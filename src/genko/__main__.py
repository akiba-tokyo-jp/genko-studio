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
    new.add_argument("--paper", default="", help="b4 (magazines, contests) | b5 | a5 (doujinshi) | a4 (practice) | webtoon")
    new.add_argument("--json", action="store_true", help="Machine-readable JSON on stdout (default)")
    new.add_argument("--plain", action="store_true", help="Print only the path")

    export = sub.add_parser("export", help="Export PNG sequence (draft/name layers skipped)")
    export.add_argument("src", type=Path)
    export.add_argument("out", type=Path)
    export.add_argument("--json", action="store_true")
    export.add_argument("--dpi", type=int, default=None, help="default: the page spec dpi for print formats, 150 for strip/epub")
    export.add_argument("--format", dest="fmt", default="png", choices=["png", "tiff", "pdf", "strip", "psd", "epub", "pack", "webtoon", "sns",
                                                                         "cmyk", "layers", "kindle", "timelapse", "animation"])
    export.add_argument("--width", type=int, default=800, help="webtoon: strip width in px")
    export.add_argument("--max-height", type=int, default=1280, help="webtoon: slice height limit in px")
    export.add_argument("--long-edge", type=int, default=2048, help="sns: long edge in px")
    export.add_argument("--jpeg", action="store_true", help="webtoon/sns: JPEG instead of PNG")
    export.add_argument("--spreads", action="store_true", help="sns: also one image per spread")
    export.add_argument("--color", default="rgb", choices=["rgb", "cmyk", "gray"], help="pdf/tiff/png: colour of the pages")
    export.add_argument("--icc", default=None, help="cmyk: the printer's CMYK ICC profile")
    export.add_argument("--area", default="paper", choices=["paper", "bleed", "trim"])
    export.add_argument("--fps", type=float, default=12, help="timelapse: pictures per second")
    export.add_argument("--seconds", type=float, default=None, help="timelapse: fit the whole recording into this time")
    export.add_argument("--page", type=int, default=None, help="timelapse: only this page; animation: the page")

    inspect = sub.add_parser("inspect", help="Headless: dump compact JSON snapshot")
    inspect.add_argument("src", type=Path)
    inspect.add_argument("--full", action="store_true")
    inspect.add_argument("--stroke", default="")

    apply_p = sub.add_parser("apply", help="Headless: apply JSON ops (file or stdin '-')")
    apply_p.add_argument("src", type=Path)
    apply_p.add_argument("ops", help="Path to JSON array, or - for stdin")
    apply_p.add_argument("--dry-run", action="store_true")
    apply_p.add_argument("--expect-revision", type=int, default=None, help="fail if someone saved since")
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

    undo = sub.add_parser("undo", help="Undo the latest saved change (works across processes)")
    undo.add_argument("src", type=Path)
    undo.add_argument("--as", dest="actor", default="genko")
    undo.add_argument("--force", action="store_true", help="undo another actor's change / ignore outside edits")
    redo = sub.add_parser("redo", help="Redo the latest undone change")
    redo.add_argument("src", type=Path)
    redo.add_argument("--as", dest="actor", default="genko")
    redo.add_argument("--force", action="store_true")
    gc_p = sub.add_parser("gc", help="Delete assets nothing refers to")
    gc_p.add_argument("src", type=Path)
    gc_p.add_argument("--dry-run", action="store_true")
    gc_p.add_argument("--legacy", action="store_true", help="also remove the v2 pages/ folder")
    doctor_p = sub.add_parser("doctor", help="Check assets, font and paths")
    doctor_p.add_argument("src", type=Path)

    token = sub.add_parser("token", help="Manage HTTP tokens (stored in the user config dir)")
    token.add_argument("action", choices=["add", "list", "revoke"])
    token.add_argument("--actor", default="", help="human:<name> or ai:<name> (add)")
    token.add_argument("--id", default="", help="token id prefix (revoke)")

    parser.add_argument("--ascii", action="store_true", help="Escape non-ASCII in JSON output (for legacy consoles)")
    app_p = sub.add_parser("app", help="Open the human desktop app (review, approve, edit)")
    app_p.add_argument("project", type=Path, nargs="?", help="a .genko folder; without it a start screen lists recent projects")
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


def _is_studio_project(src: Path) -> bool:
    """Agent projects (strict gates, studio state, or M0 sidecars): an unnamed caller is not trusted as a person."""
    if (src / "studio" / "drafts").is_dir():
        return True
    try:
        payload = json.loads((src / "project.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return bool(payload.get("strict_gates") or payload.get("studio"))


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
            if args.paper:
                from genko.models import PAPER_PRESETS

                if args.paper not in PAPER_PRESETS:
                    raise SystemExit(f"--paper must be one of {', '.join(PAPER_PRESETS)}")
                spec = PAPER_PRESETS[args.paper][1]()
            elif args.webtoon:
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
                path = export_strip(episode, args.out if args.out.suffix else args.out / "strip.png", dpi=args.dpi or 150)
                paths = [path]
            elif args.fmt == "psd":
                from genko.psd import export_psd_pages

                # a folder gets one layered PSD per page; a .psd path gets the first page
                paths = [export_psd(episode, args.out, dpi=args.dpi or episode.spec.dpi)] if args.out.suffix else export_psd_pages(episode, args.out, dpi=args.dpi)
            elif args.fmt == "epub":
                paths = [export_epub(episode, args.out if args.out.suffix else args.out / "out.epub", dpi=args.dpi or 150)]
            elif args.fmt in ("webtoon", "sns"):
                from genko import profiles

                if args.fmt == "webtoon":
                    paths = profiles.export_webtoon(episode, args.out, args.width, args.max_height, fmt="jpeg" if args.jpeg else "png")
                else:
                    paths = profiles.export_sns(episode, args.out, args.long_edge, fmt="jpeg" if args.jpeg else "png", spreads=args.spreads)
            elif args.fmt == "pack":
                from genko.pack import export_pack

                paths = export_pack(episode, args.out, dpi=args.dpi)
            elif args.fmt == "layers":
                from genko.export import export_layers

                paths = export_layers(episode, args.out, dpi=args.dpi or episode.spec.dpi, area=args.area)
            elif args.fmt == "kindle":
                from genko.export import KINDLE_LONG_EDGE, export_kindle

                long_edge = args.long_edge if args.long_edge != 2048 else KINDLE_LONG_EDGE
                paths = [export_kindle(episode, args.out if args.out.suffix else args.out / "kindle.epub", long_edge=long_edge)]
            elif args.fmt == "animation":
                from genko import anim

                page = next((p for p in episode.pages if p.index == (args.page or 1)), None)
                if page is None:
                    raise SystemExit(f"no page {args.page}")
                written = anim.export(page, args.out if args.out.suffix else args.out / f"p{page.index:03d}.gif", episode=episode,
                                      dpi=args.dpi or 100)
                paths = written if isinstance(written, list) else [written]
            elif args.fmt == "timelapse":
                from genko import timelapse

                paths = [timelapse.export(args.src, args.out if args.out.suffix else args.out / "timelapse.webp", page=args.page,
                                          fps=args.fps, seconds=args.seconds)]
            else:
                paths = export_print(episode, args.out, fmt=args.fmt, dpi=args.dpi, color="cmyk" if args.fmt == "cmyk" else args.color,
                                     icc=args.icc, area=args.area)
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
            agent = args.agent or ("legacy:unknown" if _is_studio_project(args.src) else "genko")
            with ProjectLock(args.src, agent=agent):
                # Load inside the lock so a concurrent writer's changes are never overwritten.
                episode = load_episode(args.src)
                if args.expect_revision is not None and episode.revision != args.expect_revision:
                    raise ApplyError(f"revision conflict: expected {args.expect_revision}, found {episode.revision}")
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
        if args.cmd in ("undo", "redo"):
            from genko.journal import restore
            from genko.lock import ProjectLock

            with ProjectLock(args.src, agent=args.actor):
                _print_json(restore(args.src, actor=args.actor, redo=args.cmd == "redo", force=args.force))
            return 0
        if args.cmd == "gc":
            from genko.maintenance import gc

            _print_json(gc(args.src, dry_run=args.dry_run, legacy=args.legacy))
            return 0
        if args.cmd == "doctor":
            from genko.maintenance import doctor

            report = doctor(args.src)
            _print_json(report)
            return 0 if report["ok"] else 1
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

            return run_app(args.project)
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
