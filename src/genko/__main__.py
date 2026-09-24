from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from genko.headless import OPS_SCHEMA, ApplyError, apply_ops, snapshot
from genko.io import load_episode, save_episode
from genko.migrate import UnsupportedProjectVersion
from genko.models import PageSpec, new_episode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="genko",
        description="Genko Studio — manga manuscript OS for humans and generative AI (GUI + headless JSON).",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    new = sub.add_parser("new", help="Create a .genko episode folder")
    new.add_argument("dest", type=Path)
    new.add_argument("--title", default="無題")
    new.add_argument("--episode", type=int, default=1)
    new.add_argument("--pages", type=int, default=8)
    new.add_argument("--webtoon", action="store_true")
    new.add_argument("--b4", action="store_true")
    new.add_argument("--preset", default="")
    new.add_argument("--json", action="store_true", help="Machine-readable JSON on stdout (default)")
    new.add_argument("--plain", action="store_true", help="Print only the path")

    export = sub.add_parser("export", help="Export PNG sequence (draft/name layers skipped)")
    export.add_argument("src", type=Path)
    export.add_argument("out", type=Path)
    export.add_argument("--json", action="store_true")
    export.add_argument("--dpi", type=int, default=150)
    export.add_argument("--format", dest="fmt", default="png", choices=["png", "tiff", "pdf", "strip", "psd", "epub", "pack"])

    inspect = sub.add_parser("inspect", help="Headless: dump compact JSON snapshot")
    inspect.add_argument("src", type=Path)
    inspect.add_argument("--full", action="store_true")
    inspect.add_argument("--stroke", default="")

    apply_p = sub.add_parser("apply", help="Headless: apply JSON ops (file or stdin '-')")
    apply_p.add_argument("src", type=Path)
    apply_p.add_argument("ops", help="Path to JSON array, or - for stdin")
    apply_p.add_argument("--dry-run", action="store_true")
    apply_p.add_argument(
        "--agent",
        default="",
        help="who is writing: human:<name> or ai:<name> (AI actors cannot approve; studio projects need it to approve)",
    )

    render_p = sub.add_parser("render", help="Headless: write one page PNG (name|proof|print)")
    render_p.add_argument("src", type=Path)
    render_p.add_argument("--page", type=int, default=1)
    render_p.add_argument("--mode", default="print", choices=["name", "proof", "print"])
    render_p.add_argument("--out", type=Path, required=True)
    render_p.add_argument("--dpi", type=int, default=150)

    sub.add_parser("schema", help="Headless: print operation schema as JSON")

    serve = sub.add_parser("serve", help="Headless HTTP JSON API (no GUI)")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8765)
    serve.add_argument("--root", type=Path, required=True, help="only paths inside this folder are served")
    serve.add_argument("--allow-origin", action="append", default=[], help="browser origin allowed to call the API")
    serve.add_argument("--allow-host", action="append", default=[], help="extra Host name (when not on 127.0.0.1)")

    token = sub.add_parser("token", help="Manage HTTP tokens (stored in the user config dir)")
    token.add_argument("action", choices=["add", "list", "revoke"])
    token.add_argument("--actor", default="", help="human:<name> or ai:<name> (add)")
    token.add_argument("--id", default="", help="token id prefix (revoke)")

    parser.add_argument("--ascii", action="store_true", help="Escape non-ASCII in JSON output (for legacy consoles)")
    sub.add_parser("app", help="Open the human desktop app")
    sub.add_parser("studio", help="Agent tools and human approvals (genko studio -h)", add_help=False)
    sub.add_parser("mcp", help="MCP server for agents such as Hermes Agent (genko mcp -h)", add_help=False)
    return parser


_ASCII = False


def _print_json(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=_ASCII) + "\n")


def _utf8_stdout() -> None:
    """Windows consoles default to cp932; JSON must still come out as UTF-8."""
    reconfigure = getattr(sys.stdout, "reconfigure", None)
    if reconfigure is not None and (sys.stdout.encoding or "").lower().replace("-", "") != "utf8":
        try:
            reconfigure(encoding="utf-8")
        except (ValueError, OSError):
            pass


def _read_ops(source: str) -> list:
    raw = sys.stdin.read() if source == "-" else Path(source).read_text(encoding="utf-8")
    data = json.loads(raw)
    if not isinstance(data, list):
        raise ApplyError("ops file must be a JSON array")
    return data


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    global _ASCII
    _ASCII = "--ascii" in argv
    argv = [a for a in argv if a != "--ascii"]
    if not _ASCII:
        _utf8_stdout()
    if argv and argv[0] == "studio":
        from genko.studio.cli import main as studio_main

        return studio_main(argv[1:])
    if argv and argv[0] == "mcp":
        from genko.studio.cli import mcp_main

        return mcp_main(argv[1:])
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "new":
            if args.webtoon:
                spec = PageSpec.webtoon()
            elif args.preset:
                spec = PageSpec.publisher(args.preset)
            elif args.b4:
                spec = PageSpec.b4_comic()
            else:
                spec = PageSpec.a4_mono()
            episode = new_episode(args.title, args.episode, args.pages, spec)
            save_episode(episode, args.dest)
            if args.plain:
                print(args.dest)
            else:
                _print_json({"ok": True, "path": str(args.dest), "snapshot": snapshot(episode)})
            return 0
        if args.cmd == "export":
            from genko.export import export_epub, export_print, export_psd, export_strip

            episode = load_episode(args.src)
            if args.fmt == "strip":
                path = export_strip(episode, args.out if args.out.suffix else args.out / "strip.png", dpi=args.dpi)
                paths = [path]
            elif args.fmt == "psd":
                paths = [export_psd(episode, args.out if args.out.suffix else args.out / "out.psd", dpi=args.dpi)]
            elif args.fmt == "epub":
                paths = [export_epub(episode, args.out if args.out.suffix else args.out / "out.epub", dpi=args.dpi)]
            elif args.fmt == "pack":
                from genko.pack import export_pack

                paths = export_pack(episode, args.out)
            else:
                paths = export_print(episode, args.out, fmt=args.fmt, dpi=args.dpi)
            if args.json:
                _print_json({"ok": True, "count": len(paths), "files": [str(p) for p in paths]})
            else:
                for path in paths:
                    print(path)
            return 0
        if args.cmd == "inspect":
            episode = load_episode(args.src)
            if args.stroke:
                from genko.headless import inspect_stroke

                _print_json(inspect_stroke(episode, args.stroke))
            else:
                _print_json(snapshot(episode, full=args.full))
            return 0
        if args.cmd == "apply":
            from genko.lock import ProjectLock

            ops = _read_ops(args.ops)
            agent = args.agent or ("legacy:unknown" if (args.src / "studio").is_dir() else "genko")
            with ProjectLock(args.src, agent=agent):
                # Load inside the lock so a concurrent writer's changes are never overwritten.
                episode = load_episode(args.src)
                result = apply_ops(episode, ops, dry_run=args.dry_run, agent=agent)
                if not args.dry_run:
                    save_episode(episode, args.src)
            _print_json(result)
            return 0
        if args.cmd == "render":
            from genko.render import render_page

            episode = load_episode(args.src)
            page = next(item for item in episode.pages if item.index == args.page)
            image = render_page(page, args.dpi, mode=args.mode, episode=episode)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            image.save(args.out)
            _print_json({"ok": True, "path": str(args.out), "mode": args.mode})
            return 0
        if args.cmd == "schema":
            _print_json({"ok": True, "ops": OPS_SCHEMA})
            return 0
        if args.cmd == "serve":
            from genko.server import HeadlessServer

            from genko import tokens as token_store

            known = token_store.load()
            if not known:
                first = token_store.add("human:owner")
                known = token_store.load()
                print(f"created token for human:owner: {first}", file=sys.stderr)
            server = HeadlessServer(
                host=args.host,
                port=args.port,
                root=args.root,
                tokens=known,
                allow_origins=tuple(args.allow_origin),
                allow_hosts=tuple(args.allow_host),
            )
            print(f"genko headless http://{args.host}:{server.port} root={args.root}", file=sys.stderr)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                server.shutdown()
            return 0
        if args.cmd == "token":
            from genko import tokens as token_store

            if args.action == "add":
                _print_json({"ok": True, "actor": args.actor, "token": token_store.add(args.actor)})
            elif args.action == "list":
                _print_json({"ok": True, "tokens": token_store.listing()})
            else:
                _print_json({"ok": True, "revoked": token_store.revoke(args.id)} if args.id else {"ok": False, "error": "--id is required"})
            return 0
        if args.cmd == "app":
            from genko.app.main import run_app

            return run_app()
    except ApplyError as exc:
        _print_json({"ok": False, "error": str(exc)})
        return 1
    except UnsupportedProjectVersion as exc:
        _print_json({"ok": False, "error": str(exc)})
        return 2
    except ValueError as exc:
        _print_json({"ok": False, "error": str(exc)})
        return 1
    except FileNotFoundError as exc:
        _print_json({"ok": False, "error": f"not found: {exc}"})
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
