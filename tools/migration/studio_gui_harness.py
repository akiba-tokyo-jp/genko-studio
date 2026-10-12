#!/usr/bin/env python3
"""The Python reference for the app's studio contract test (native/tests/contract/test_contract_studio_gui.cpp): what the
approval box, the process bar and the AI dialog compute without Qt — genko/app/review_model.py (inbox, progress, the
ops of 承認 and 差し戻し), the sheet candidates as the approval box numbers them (studio_widgets.ApprovalBox.
_numbered_sheets) and the Qt-free functions of genko/app/ai_link.py and links.py. Development only; nothing here goes
into the product.

    PYTHONPATH=src python3 tools/migration/studio_gui_harness.py review BOOK OUT
    PYTHONPATH=src python3 tools/migration/studio_gui_harness.py ai CASES OUT

review: BOOK is the studio book (studio_harness.py make-studiobook) — first given the requests it lacks, as the agent
(ai:hermes) asks for them (an art approval of page 1, an export approval, a question about the whole book, a sheet of a
character the book does not have; saved as v3) —, then for each request in the box: its fields and title, the
numbered sheet candidates, the ops of 承認 (a sheet with the first candidate the box lists; once more without any) and
of 差し戻し with "", "  " and a reason in two lines. "progress" is the process bar's counts.
ai: CASES is a JSON list of {"fn", ...}: snippet (client, root, base: the command that starts Genko), first_request
(book or null), ai_name (actor), ago (minutes), recent_ai / status_words (book, now), root_text (typed: str(Path(typed
.strip() or "."))), is_link (text), link_name (user: getpass.getuser()). Each answer, or {"raised", "message"}.
"""
from __future__ import annotations

import argparse
import json
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for path in (str(ROOT / "src"), str(HERE)):
    if path not in sys.path:
        sys.path.insert(0, path)


def _stub_qt() -> None:
    """genko/app/ai_link.py and links.py import PySide6 (not installed with the reference): a stand-in whose every name
    is a class that takes anything, for the Qt-free functions only."""

    class _Meta(type):
        def __getattr__(cls, name):
            return _Any()

        def __or__(cls, other):
            return cls

    class _Any(metaclass=_Meta):
        def __init__(self, *args, **kwargs):
            pass

        def __getattr__(self, name):
            return _Any()

        def __call__(self, *args, **kwargs):
            return _Any()

        def __or__(self, other):
            return self

        def __iter__(self):
            return iter(())

    def module(name: str) -> types.ModuleType:
        made = types.ModuleType(name)
        made.__getattr__ = lambda attr: type(attr, (_Any,), {})  # type: ignore[attr-defined]
        made.__path__ = []  # type: ignore[attr-defined]
        return made

    for name in ("PySide6", "PySide6.QtCore", "PySide6.QtGui", "PySide6.QtWidgets", "PySide6.QtNetwork", "PySide6.QtSvg"):
        sys.modules.setdefault(name, module(name))


def _dump(value) -> str:
    return json.dumps(value, ensure_ascii=False)


def _raised(exc: BaseException) -> dict:
    return {"raised": type(exc).__name__, "message": str(exc)}


def numbered_sheets(episode, item) -> list:
    """studio_widgets.ApprovalBox._numbered_sheets (the window's episode is the book)."""
    changed = int((episode.studio.get("style") or {}).get("changed_rev") or 0)
    every = (episode.studio.get("character_candidates") or {}).get(item.character_id, [])
    out = []
    for n, cand in enumerate(every, 1):
        if cand.get("status") in ("rejected", "withdrawn"):
            continue
        old = changed and int(cand.get("rev") or 0) < changed
        out.append((f"候補 {n}" + ("（前の絵柄）" if old else ""), cand))
    fresh = [pair for pair in reversed(out) if "前の絵柄" not in pair[0]]
    return fresh + [pair for pair in reversed(out) if "前の絵柄" in pair[0]]


def review(book: str, out: str) -> None:
    import pyref_harness as base
    from genko.app import review_model
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode

    base.fresh_process_state(True)
    base.counting_ids(0xE00)
    path = Path(book)
    episode = load_episode(path)
    extra = [{"op": "request_approval", "gate": "art", "pages": [1], "note": "絵を見てください"},
             {"op": "request_approval", "gate": "export", "pages": [], "note": ""},
             {"op": "ask_human", "text": "全体の流れはこれでいいですか\n二行目", "item": "plan"},
             {"op": "request_approval", "gate": "sheet", "character_id": "c-mio"}]
    reply = apply_ops(episode, extra, agent="ai:hermes")
    if not reply.get("ok"):
        raise SystemExit(f"review: {reply}")
    save_episode(episode, path, actor="ai:hermes")
    episode = load_episode(path)
    items = review_model.inbox(episode)
    shown = []
    for item in items:
        entry = {"ticket_id": item.ticket_id, "kind": item.kind, "gate": item.gate, "pages": item.pages,
                 "character_id": item.character_id, "frame_id": item.frame_id, "text": item.text, "by": item.by,
                 "name": item.name, "title": item.title}
        sheets = numbered_sheets(episode, item) if item.gate == "sheet" and item.kind == "gate" else []
        entry["sheets"] = [[label, cand.get("id")] for label, cand in sheets]
        choice = sheets[0][1].get("id") if sheets else None
        try:
            entry["approve"] = {"ops": review_model.approve_ops(episode, item, project=path, candidate_id=choice)}
        except Exception as exc:  # noqa: BLE001 (compared)
            entry["approve"] = _raised(exc)
        if item.gate == "sheet" and item.kind == "gate":
            try:
                entry["approve_none"] = {"ops": review_model.approve_ops(episode, item, project=path, candidate_id=None)}
            except Exception as exc:  # noqa: BLE001
                entry["approve_none"] = _raised(exc)
        entry["send_back"] = []
        for text in ("", "  ", "  直してください\n二行目も  "):
            try:
                entry["send_back"].append({"ops": review_model.send_back_ops(episode, item, text)})
            except Exception as exc:  # noqa: BLE001
                entry["send_back"].append(_raised(exc))
        shown.append(entry)
    result = {"inbox": shown, "progress": [list(pair) for pair in review_model.progress(episode)]}
    Path(out).write_text(_dump(result), encoding="utf-8")


def ai(cases_file: str, out: str) -> None:
    import getpass
    import os
    import time

    _stub_qt()
    from genko.app import ai_link, links

    results = []
    for case in json.loads(Path(cases_file).read_text(encoding="utf-8")):
        fn = case["fn"]
        try:
            if fn == "snippet":
                saved = ai_link.command
                ai_link.command = lambda base=case["base"]: list(base)
                try:
                    where, text = ai_link.snippet(case["client"], Path(case["root"]))
                finally:
                    ai_link.command = saved
                results.append({"where": where, "text": text})
            elif fn == "first_request":
                results.append(ai_link.first_request(Path(case["book"]) if case["book"] else None))
            elif fn == "ai_name":
                results.append(ai_link.ai_name(case["actor"]))
            elif fn == "ago":
                results.append(ai_link._ago(case["minutes"]))
            elif fn in ("recent_ai", "status_words"):
                real = time.time
                time.time = lambda now=case["now"]: now
                try:
                    book = Path(case["book"]) if case["book"] else None
                    results.append(ai_link.recent_ai(book) if fn == "recent_ai" else list(ai_link.status_words(book)))
                finally:
                    time.time = real
            elif fn == "root_text":
                results.append(str(Path(case["typed"].strip() or ".")))
            elif fn == "is_link":
                results.append(links.is_link(case["text"]))
            elif fn == "link_name":
                saved_user = getpass.getuser
                getpass.getuser = lambda user=case["user"]: user
                try:
                    results.append(links._name())
                finally:
                    getpass.getuser = saved_user
            else:
                raise SystemExit(f"unknown fn {fn}")
        except Exception as exc:  # noqa: BLE001 (compared)
            results.append(_raised(exc))
    del os
    Path(out).write_text(_dump(results), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("review")
    p.add_argument("book")
    p.add_argument("out")
    p = sub.add_parser("ai")
    p.add_argument("cases")
    p.add_argument("out")
    args = parser.parse_args()
    if args.cmd == "review":
        review(args.book, args.out)
    else:
        ai(args.cases, args.out)


if __name__ == "__main__":
    main()
