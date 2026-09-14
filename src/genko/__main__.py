from __future__ import annotations

import argparse
import sys
from pathlib import Path

from genko.export import export_png_sequence
from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="genko", description="Genko Studio — manga manuscript OS")
    sub = parser.add_subparsers(dest="cmd", required=True)

    new = sub.add_parser("new", help="Create a .genko episode folder")
    new.add_argument("dest", type=Path)
    new.add_argument("--title", default="無題")
    new.add_argument("--episode", type=int, default=1)
    new.add_argument("--pages", type=int, default=8)
    new.add_argument("--webtoon", action="store_true")

    export = sub.add_parser("export", help="Export PNG sequence (draft/name layers skipped)")
    export.add_argument("src", type=Path)
    export.add_argument("out", type=Path)

    sub.add_parser("app", help="Open the desktop app")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cmd == "new":
        spec = PageSpec.webtoon() if args.webtoon else PageSpec.a4_mono()
        episode = new_episode(args.title, args.episode, args.pages, spec)
        save_episode(episode, args.dest)
        print(args.dest)
        return 0
    if args.cmd == "export":
        episode = load_episode(args.src)
        paths = export_png_sequence(episode, args.out)
        for path in paths:
            print(path)
        return 0
    if args.cmd == "app":
        from genko.app.main import run_app

        return run_app()
    return 2


if __name__ == "__main__":
    sys.exit(main())
