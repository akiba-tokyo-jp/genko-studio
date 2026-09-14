from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from genko.export import export_png_sequence
from genko.headless import OPS_SCHEMA, ApplyError, apply_ops, snapshot
from genko.io import load_episode, save_episode
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
    new.add_argument("--json", action="store_true", help="Machine-readable JSON on stdout")

    export = sub.add_parser("export", help="Export PNG sequence (draft/name layers skipped)")
    export.add_argument("src", type=Path)
    export.add_argument("out", type=Path)
    export.add_argument("--json", action="store_true")
    export.add_argument("--dpi", type=int, default=150)

    inspect = sub.add_parser("inspect", help="Headless: dump compact JSON snapshot")
    inspect.add_argument("src", type=Path)
    inspect.add_argument("--full", action="store_true")

    apply_p = sub.add_parser("apply", help="Headless: apply JSON ops (file or stdin '-')")
    apply_p.add_argument("src", type=Path)
    apply_p.add_argument("ops", help="Path to JSON array, or - for stdin")
    apply_p.add_argument("--dry-run", action="store_true")

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

    sub.add_parser("app", help="Open the human desktop app")
    return parser


def _print_json(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")


def _read_ops(source: str) -> list:
    raw = sys.stdin.read() if source == "-" else Path(source).read_text(encoding="utf-8")
    data = json.loads(raw)
    if not isinstance(data, list):
        raise ApplyError("ops file must be a JSON array")
    return data


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "new":
            spec = PageSpec.webtoon() if args.webtoon else PageSpec.a4_mono()
            episode = new_episode(args.title, args.episode, args.pages, spec)
            save_episode(episode, args.dest)
            if args.json:
                _print_json({"ok": True, "path": str(args.dest), "snapshot": snapshot(episode)})
            else:
                print(args.dest)
            return 0
        if args.cmd == "export":
            episode = load_episode(args.src)
            paths = export_png_sequence(episode, args.out, working_dpi=args.dpi)
            if args.json:
                _print_json({"ok": True, "count": len(paths), "files": [str(p) for p in paths]})
            else:
                for path in paths:
                    print(path)
            return 0
        if args.cmd == "inspect":
            _print_json(snapshot(load_episode(args.src), full=args.full))
            return 0
        if args.cmd == "apply":
            from genko.lock import ProjectLock

            episode = load_episode(args.src)
            with ProjectLock(args.src):
                result = apply_ops(episode, _read_ops(args.ops), dry_run=args.dry_run)
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

            server = HeadlessServer(host=args.host, port=args.port)
            print(f"genko headless http://{args.host}:{server.port}", file=sys.stderr)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                server.shutdown()
            return 0
        if args.cmd == "app":
            from genko.app.main import run_app

            return run_app()
    except ApplyError as exc:
        _print_json({"ok": False, "error": str(exc)})
        return 1
    except FileNotFoundError as exc:
        _print_json({"ok": False, "error": f"not found: {exc}"})
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
