from __future__ import annotations

import copy
from typing import Any

from genko.models import Episode, Frame, LayerRole, Page, StoryLine, new_id
from genko.pipeline import InkBlockedError, advance


class ApplyError(ValueError):
    """Invalid or blocked operation."""


OPS_SCHEMA: list[dict[str, Any]] = [
    {"op": "split_frame", "page": "int", "axis": "horizontal|vertical", "ratio": "float", "gutter_mm": "float", "frame_id": "optional"},
    {"op": "add_line", "page": "int", "text": "str", "speaker": "optional", "frame_id": "optional"},
    {"op": "edit_line", "id": "str", "text": "optional", "speaker": "optional"},
    {"op": "delete_line", "id": "str"},
    {"op": "name_ok", "page": "int, optional (all pages if omitted)"},
    {"op": "advance", "page": "int", "to": "name|ink|finish"},
    {"op": "add_stroke", "page": "int", "layer": "name|ink", "points": "[[x,y],...]"},
    {"op": "delete_stroke", "page": "int", "layer": "name|ink", "index": "int"},
    {"op": "add_page", "count": "int"},
    {"op": "delete_page", "page": "int"},
    {"op": "duplicate_page", "page": "int"},
    {"op": "set_note", "page": "int", "note": "str"},
    {"op": "reorder", "order": "[int]"},
    {"op": "undo"},
]


def _require_page(episode: Episode, op: dict[str, Any]) -> Page:
    try:
        index = int(op["page"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ApplyError("page (int) is required") from exc
    for page in episode.pages:
        if page.index == index:
            return page
    raise ApplyError(f"no page {index}")


def _copy_state(dst: Episode, src: Episode) -> None:
    dst.title = src.title
    dst.episode = src.episode
    dst.spec = src.spec
    dst.binding = src.binding
    dst.pages = src.pages
    dst.story = src.story
    dst.bible = src.bible


def _apply_one(episode: Episode, op: dict[str, Any]) -> None:
    name = op.get("op")
    if not name:
        raise ApplyError("op is required")

    if name == "split_frame":
        page = _require_page(episode, op)
        axis = op.get("axis")
        if axis not in ("horizontal", "vertical"):
            raise ApplyError("axis must be horizontal or vertical")
        leaves = page.leaf_frames()
        if not leaves:
            raise ApplyError("page has no frames")
        frame_id = op.get("frame_id") or page.selected_frame_id or leaves[0].id
        page.split_frame(
            frame_id,
            axis=axis,
            ratio=float(op.get("ratio", 0.5)),
            gutter_mm=float(op.get("gutter_mm", 4)),
        )
        return

    if name == "add_line":
        page = _require_page(episode, op)
        text = op.get("text")
        if not text:
            raise ApplyError("text is required")
        episode.add_line(page.index, str(text), speaker=str(op.get("speaker", "")), frame_id=op.get("frame_id"))
        return

    if name == "edit_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        for line in episode.story:
            if line.id == line_id:
                if "text" in op:
                    line.text = str(op["text"])
                if "speaker" in op:
                    line.speaker = str(op["speaker"])
                if "frame_id" in op:
                    line.frame_id = op["frame_id"]
                return
        raise ApplyError(f"no line {line_id}")

    if name == "delete_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        before = len(episode.story)
        episode.story = [line for line in episode.story if line.id != line_id]
        for page in episode.pages:
            page.texts = [line for line in page.texts if line.id != line_id]
        if len(episode.story) == before:
            raise ApplyError(f"no line {line_id}")
        return

    if name == "name_ok":
        pages = episode.pages if "page" not in op else [_require_page(episode, op)]
        for page in pages:
            page.name_ok = True
            advance(page, to="ink")
        return

    if name == "advance":
        page = _require_page(episode, op)
        to = op.get("to")
        if to not in ("name", "ink", "finish"):
            raise ApplyError("to must be name, ink, or finish")
        try:
            advance(page, to=to)
        except InkBlockedError as exc:
            raise ApplyError(str(exc)) from exc
        return

    if name == "add_stroke":
        page = _require_page(episode, op)
        layer = op.get("layer", "name")
        points_raw = op.get("points") or []
        points = [(float(x), float(y)) for x, y in points_raw]
        if len(points) < 2:
            raise ApplyError("points needs at least two [x_mm, y_mm] pairs")
        if layer == "ink":
            if not page.name_ok:
                raise ApplyError("ink strokes require name_ok")
            page.ink_strokes.append(points)
        elif layer == "name":
            page.name_strokes.append(points)
        else:
            raise ApplyError("layer must be name or ink")
        return

    if name == "delete_stroke":
        page = _require_page(episode, op)
        layer = op.get("layer", "name")
        try:
            index = int(op["index"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ApplyError("index is required") from exc
        strokes = page.ink_strokes if layer == "ink" else page.name_strokes
        if index < 0 or index >= len(strokes):
            raise ApplyError("stroke index out of range")
        strokes.pop(index)
        return

    if name == "add_page":
        count = int(op.get("count", 1))
        for _ in range(count):
            index = len(episode.pages) + 1
            page = Page(index=index, spec=episode.spec, frames=[], binding=episode.binding)
            page.frames = [Frame(id=new_id(), rect=page.inner_rect_mm())]
            episode.pages.append(page)
        return

    if name == "delete_page":
        page = _require_page(episode, op)
        if len(episode.pages) == 1:
            raise ApplyError("cannot delete the last page")
        removed = page.index
        episode.pages = [item for item in episode.pages if item.index != removed]
        episode.story = [line for line in episode.story if line.page_index != removed]
        mapping = {item.index: new for new, item in enumerate(episode.pages, start=1)}
        for line in episode.story:
            line.page_index = mapping[line.page_index]
        for new, item in enumerate(episode.pages, start=1):
            item.index = new
            for line in item.texts:
                line.page_index = new
        return

    if name == "duplicate_page":
        page = _require_page(episode, op)
        clone: Page = copy.deepcopy(page)
        clone.index = len(episode.pages) + 1
        for frame in clone.frames:
            _refresh_frame_ids(frame)
        for layer in clone.layers:
            layer.id = new_id()
        new_lines: list[StoryLine] = []
        for line in list(episode.story_for_page(page.index)):
            copied = copy.deepcopy(line)
            copied.id = new_id()
            copied.page_index = clone.index
            new_lines.append(copied)
        clone.texts = new_lines
        episode.story.extend(new_lines)
        episode.pages.append(clone)
        return

    if name == "set_note":
        page = _require_page(episode, op)
        page.note = str(op.get("note", ""))
        return

    if name == "reorder":
        order = op.get("order")
        if not isinstance(order, list) or not order:
            raise ApplyError("order must be a non-empty list of page indexes")
        episode.reorder([int(i) for i in order])
        return

    if name == "select_frame":
        page = _require_page(episode, op)
        page.selected_frame_id = op.get("frame_id")
        return

    raise ApplyError(f"unknown op: {name}")


def _refresh_frame_ids(frame: Frame) -> None:
    frame.id = new_id()
    for child in frame.children:
        _refresh_frame_ids(child)


def apply_ops(
    episode: Episode,
    ops: list[dict[str, Any]],
    dry_run: bool = False,
) -> dict[str, Any]:
    from genko.headless import snapshot

    if not isinstance(ops, list):
        raise ApplyError("ops must be a JSON array")

    if len(ops) == 1 and ops[0].get("op") == "undo":
        if dry_run:
            return {"ok": True, "applied": ["undo"], "snapshot": snapshot(episode)}
        if not episode.undo_stack:
            raise ApplyError("nothing to undo")
        previous = episode.undo_stack.pop()
        stack = episode.undo_stack
        _copy_state(episode, previous)
        episode.undo_stack = stack
        return {"ok": True, "applied": ["undo"], "snapshot": snapshot(episode)}

    work = copy.deepcopy(episode)
    work.undo_stack = []
    applied: list[str] = []
    for i, op in enumerate(ops):
        if not isinstance(op, dict):
            raise ApplyError(f"ops[{i}] must be an object")
        try:
            _apply_one(work, op)
        except ApplyError as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        except (KeyError, ValueError, TypeError) as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        applied.append(str(op.get("op")))

    if dry_run:
        return {"ok": True, "applied": applied, "snapshot": snapshot(work)}

    frozen = copy.deepcopy(episode)
    frozen.undo_stack = []
    episode.undo_stack.append(frozen)
    _copy_state(episode, work)
    return {"ok": True, "applied": applied, "snapshot": snapshot(episode)}
