"""`genko studio …` and `genko mcp …`.

Agent tools are the same as over MCP (project is a path here). Human commands
(approve, comment) require --as human:<name>. stdout is one JSON object.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from genko.ops import ApplyError
from genko.studio.service import HumanService, StudioService, ToolResult


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="genko studio", description="Agent tools and human approvals for Genko Studio")
    sub = parser.add_subparsers(dest="cmd", required=True)

    def project_cmd(name: str, help_text: str) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help_text)
        p.add_argument("project", type=Path)
        p.add_argument("--agent", default="ai:cli", help="actor for agent tools (ai:<name>)")
        return p

    init = sub.add_parser("init", help="Create a project (B4 mono by default)")
    init.add_argument("project", type=Path)
    init.add_argument("--title", default="無題")
    init.add_argument("--pages", type=int, default=8)
    init.add_argument("--spec", default="commercial-b4", choices=["commercial-b4", "a4-mono", "webtoon"])

    project_cmd("status", "Progress and approvals")
    nxt = project_cmd("next", "What to do next")
    nxt.add_argument("--limit", type=int, default=5)
    ins = project_cmd("inspect", "Read bible / script / page / schemas / rules / snapshot")
    ins.add_argument("target")
    ins.add_argument("--page", type=int)
    ren = project_cmd("render", "Write a page preview PNG")
    ren.add_argument("--page", type=int, required=True)
    ren.add_argument("--mode", default="name", choices=["name", "proof"])
    for name in ("set-bible", "set-script", "submit-name"):
        p = project_cmd(name, f"{name} from a JSON file (- for stdin)")
        p.add_argument("file")
        p.add_argument("--commit", action="store_true")
        if name == "submit-name":
            p.add_argument("--replace", action="store_true")
    rev = project_cmd("record-review", "Record a self-check of a page")
    rev.add_argument("--page", type=int, required=True)
    rev.add_argument("--notes", default="")
    rev.add_argument("--score", type=float)
    req = project_cmd("request-approval", "Ask a human to approve name pages")
    req.add_argument("--pages", required=True, help="e.g. 1,2,3")
    req.add_argument("--note", default="")
    tic = project_cmd("tickets", "Approval requests and human comments")
    tic.add_argument("--all", action="store_true")

    approve = sub.add_parser("approve", help="(human) Approve name pages")
    approve.add_argument("project", type=Path)
    approve.add_argument("gate", choices=["name"])
    approve.add_argument("--pages", required=True)
    approve.add_argument("--as", dest="actor", required=True)
    comment = sub.add_parser("comment", help="(human) Leave a fix instruction on a page")
    comment.add_argument("project", type=Path)
    comment.add_argument("text")
    comment.add_argument("--page", type=int, required=True)
    comment.add_argument("--as", dest="actor", required=True)
    review = sub.add_parser("review", help="Write review.html with previews and approve commands")
    review.add_argument("project", type=Path)
    review.add_argument("--out", type=Path, required=True)
    return parser


def _pages(text: str) -> list[int]:
    out: list[int] = []
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        elif part.strip():
            out.append(int(part))
    return out


def _load(source: str) -> dict:
    raw = sys.stdin.read() if source == "-" else Path(source).read_text(encoding="utf-8")
    return json.loads(raw)


def _emit(payload: dict) -> int:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    return 0 if payload.get("ok") else 1


def main(argv: list[str]) -> int:
    args = _parser().parse_args(argv)
    try:
        return _run(args)
    except (ApplyError, ValueError, FileNotFoundError, json.JSONDecodeError) as exc:
        return _emit({"ok": False, "error": str(exc)})


def _run(args: argparse.Namespace) -> int:
    path = args.project.resolve()
    if args.cmd == "approve":
        return _emit(HumanService(path, args.actor).approve_name(_pages(args.pages)))
    if args.cmd == "comment":
        return _emit(HumanService(path, args.actor).comment(args.page, args.text))
    if args.cmd == "review":
        from genko.studio.review import review_html

        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(review_html(path), encoding="utf-8")
        return _emit({"ok": True, "path": str(args.out)})
    service = StudioService(path.parent, getattr(args, "agent", "ai:cli"))
    name = path.name
    result: ToolResult
    if args.cmd == "init":
        result = service.create_project(name, args.title, args.pages, args.spec)
    elif args.cmd == "status":
        result = service.status(name)
    elif args.cmd == "next":
        result = service.next(name, args.limit)
    elif args.cmd == "inspect":
        result = service.inspect(name, args.target, args.page)
    elif args.cmd == "render":
        result = service.render(name, args.page, args.mode)
    elif args.cmd == "set-bible":
        result = service.set_bible(name, _load(args.file), args.commit)
    elif args.cmd == "set-script":
        result = service.set_script(name, _load(args.file), args.commit)
    elif args.cmd == "submit-name":
        result = service.submit_name(name, _load(args.file), args.commit, args.replace)
    elif args.cmd == "record-review":
        result = service.record_review(name, args.page, args.score, args.notes)
    elif args.cmd == "request-approval":
        result = service.request_approval(name, "name", _pages(args.pages), args.note)
    else:
        result = service.tickets(name, "all" if args.all else "open")
    return _emit(result.to_dict())


def mcp_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="genko mcp", description="Run the Genko MCP server over stdio")
    parser.add_argument("--root", type=Path, required=True, help="folder that holds the .genko projects")
    parser.add_argument("--agent", default="ai:agent", help="actor for every change (ai:<name>)")
    args = parser.parse_args(argv)
    try:
        from genko.mcp.server import run_stdio
    except ImportError:
        return _emit({"ok": False, "error": "install genko-studio[mcp]"})
    args.root.mkdir(parents=True, exist_ok=True)
    run_stdio(args.root, args.agent)
    return 0
