"""J9 ops over the whole book: covers (表紙・裏表紙・カバー), find and replace in every line (一括置換), the
same ops on many pages (全ページに), and who draws which page (担当)."""

from __future__ import annotations

import copy
import re
from typing import Any

from genko import covers
from genko.models import Frame, Page, new_id
from genko.ops import ApplyError

OPS = ("add_cover", "replace_text", "set_assignee")  # (for_pages is expanded by apply_ops: see expand)
# ops that for_pages must not repeat (they change the pages themselves or the whole book)
NOT_PER_PAGE = {"add_page", "delete_page", "duplicate_page", "reorder", "move_page", "set_page_spec", "add_cover", "for_pages",
                "replace_text", "approve", "revoke", "name_ok", "advance", "set_bible", "set_script", "define_brush", "set_brush"}


def add_cover(episode, op: dict) -> None:
    kind = str(op.get("kind") or "front")
    if kind not in covers.KINDS:
        raise ApplyError("kind must be front, back or jacket")
    if any((covers.cover_of(p) or {}).get("kind") == kind for p in episode.pages):
        raise ApplyError(f"the book already has a {kind} cover")
    cover = {"kind": kind}
    if kind == "jacket":
        spine = float(op.get("spine_mm") or 0)
        flap = float(op.get("flap_mm") or 0)
        if not 0 < spine <= 100:
            raise ApplyError("spine_mm is the spine's width (0..100 mm)")
        if not 0 <= flap <= 200:
            raise ApplyError("flap_mm is 0..200 mm")
        cover.update(spine_mm=spine, flap_mm=flap)
    spec = covers.spec_for(episode.spec, cover)
    page = Page(index=len(episode.pages) + 1, spec=spec, frames=[], binding=episode.binding)
    page.numero = False
    page.extra["cover"] = cover
    t = page.trim_rect_mm()
    page.frames = [Frame(id=new_id(), rect=page.bleed_rect_mm() if op.get("bleed", True) else t, bleed=bool(op.get("bleed", True)),
                         border_mm=0.0)]
    episode.pages.append(page)


def replace_text(episode, op: dict) -> None:
    """Every line's words (and speakers, if asked) with `find` swapped for `replace`; `regex` for patterns.
    `pages` limits it; the count found is left in op["_count"] for the caller."""
    find = str(op.get("find") or "")
    if not find:
        raise ApplyError("find is the words to look for")
    replace = str(op.get("replace") or "")
    flags = 0 if op.get("case", True) else re.IGNORECASE
    try:
        pattern = re.compile(find if op.get("regex") else re.escape(find), flags)
    except re.error as exc:
        raise ApplyError(f"the pattern cannot be read: {exc}") from exc
    pages = {int(p) for p in op.get("pages") or []}
    count = 0
    for line in episode.story:
        if pages and line.page_index not in pages:
            continue
        new, n = pattern.subn(replace, line.text or "")
        if n:
            line.text = new
            count += n
            # (ruby, dots and styled parts keep pointing at words that are still there)
            line.ruby_runs = [r for r in line.ruby_runs or [] if r and r[0] in new]
            line.emphasis_runs = [w for w in getattr(line, "emphasis_runs", None) or [] if w in new]
            line.style_runs = [r for r in getattr(line, "style_runs", None) or [] if r and r[0] in new]
        if op.get("speakers"):
            new, n = pattern.subn(replace, line.speaker or "")
            if n:
                line.speaker = new
                count += n
    if count == 0 and op.get("must_find"):
        raise ApplyError("nothing matched")
    op["_count"] = count


def expand(episode, op: dict) -> list[dict]:
    """for_pages as the ops it stands for: the same ops on every page asked for (`pages`: [n…] | "all" |
    "body" (not covers)), each op's page set to the page it runs on (apply_ops then checks each one)."""
    ops = op.get("ops")
    if not isinstance(ops, list) or not ops:
        raise ApplyError("ops is the list of ops to run on each page")
    names = [str(o.get("op")) for o in ops if isinstance(o, dict)]
    if len(names) != len(ops):
        raise ApplyError("ops is the list of ops to run on each page")
    bad = [n for n in names if n in NOT_PER_PAGE]
    if bad:
        raise ApplyError(f"{bad[0]} cannot be repeated page by page")
    wanted = op.get("pages") or "body"
    if wanted in ("all", "body"):
        indexes = [p.index for p in episode.pages if wanted == "all" or not covers.is_cover(p)]
    else:
        indexes = [int(v) for v in wanted]
        known = {p.index for p in episode.pages}
        missing = [i for i in indexes if i not in known]
        if missing:
            raise ApplyError(f"no page {missing[0]}")
    return [{**copy.deepcopy(item), "page": index} for index in indexes for item in ops]


def set_assignee(episode, op: dict) -> None:
    """担当: who draws a page (a name, or none), for sharing the work out."""
    pages = [int(v) for v in op.get("pages") or ([op["page"]] if op.get("page") else [])]
    if not pages:
        raise ApplyError("pages is the list of pages")
    who = str(op.get("who") or "").strip()
    for index in pages:
        page = next((p for p in episode.pages if p.index == index), None)
        if page is None:
            raise ApplyError(f"no page {index}")
        if who:
            page.extra["assignee"] = who[:40]
        else:
            page.extra.pop("assignee", None)


def apply(episode, op: dict[str, Any], name: str) -> None:
    {"add_cover": add_cover, "replace_text": replace_text, "set_assignee": set_assignee}[name](episode, op)
