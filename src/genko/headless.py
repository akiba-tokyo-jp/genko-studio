from __future__ import annotations

from typing import Any

from genko.models import Episode, Frame, Page, StoryLine
from genko.pipeline import InkBlockedError, advance


class ApplyError(ValueError):
    """Invalid or blocked headless operation."""


OPS_SCHEMA: list[dict[str, Any]] = [
    {
        "op": "split_frame",
        "page": "int (1-based)",
        "axis": "horizontal|vertical",
        "ratio": "float 0-1, default 0.5",
        "gutter_mm": "float, default 4",
        "frame_id": "optional leaf id; default first leaf",
    },
    {
        "op": "add_line",
        "page": "int",
        "text": "str",
        "speaker": "str, optional",
        "frame_id": "str, optional",
    },
    {
        "op": "name_ok",
        "page": "int",
        "note": "marks G2 and advances that page to ink",
    },
    {
        "op": "advance",
        "page": "int",
        "to": "name|ink|finish",
    },
    {
        "op": "add_stroke",
        "page": "int",
        "layer": "name|ink",
        "points": "[[x_mm, y_mm], ...]",
    },
    {
        "op": "add_page",
        "count": "int, default 1",
    },
    {
        "op": "set_note",
        "page": "int",
        "note": "str",
    },
    {
        "op": "reorder",
        "order": "[page indexes in new order]",
    },
]


def _frame_brief(frame: Frame) -> dict[str, Any]:
    r = frame.rect
    return {
        "id": frame.id,
        "x": r.x,
        "y": r.y,
        "width": r.width,
        "height": r.height,
    }


def _line_brief(line: StoryLine) -> dict[str, Any]:
    return {
        "id": line.id,
        "text": line.text,
        "speaker": line.speaker,
        "frame_id": line.frame_id,
    }


def snapshot(episode: Episode) -> dict[str, Any]:
    """Compact episode view for humans and generative-AI agents."""
    return {
        "title": episode.title,
        "episode": episode.episode,
        "binding": episode.binding.value,
        "spec": {
            "width_mm": episode.spec.width_mm,
            "height_mm": episode.spec.height_mm,
            "dpi": episode.spec.dpi,
            "bleed_mm": episode.spec.bleed_mm,
            "inner_margin_mm": episode.spec.inner_margin_mm,
            "expression": episode.spec.expression,
        },
        "pages": [
            {
                "index": page.index,
                "name_ok": page.name_ok,
                "stage": page.stage,
                "note": page.note,
                "leaf_count": len(page.leaf_frames()),
                "leaves": [_frame_brief(frame) for frame in page.leaf_frames()],
                "story": [_line_brief(line) for line in episode.story_for_page(page.index)],
                "name_stroke_count": len(page.name_strokes),
                "ink_stroke_count": len(page.ink_strokes),
            }
            for page in episode.pages
        ],
    }


def _require_page(episode: Episode, op: dict[str, Any]) -> Page:
    try:
        index = int(op["page"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ApplyError("page (int) is required") from exc
    for page in episode.pages:
        if page.index == index:
            return page
    raise ApplyError(f"no page {index}")


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
        frame_id = op.get("frame_id") or leaves[0].id
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
        episode.add_line(
            page.index,
            str(text),
            speaker=str(op.get("speaker", "")),
            frame_id=op.get("frame_id"),
        )
        return

    if name == "name_ok":
        page = _require_page(episode, op)
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

    if name == "add_page":
        count = int(op.get("count", 1))
        for _ in range(count):
            index = len(episode.pages) + 1
            page = Page(index=index, spec=episode.spec, frames=[], binding=episode.binding)
            page.frames = [Frame(id=_new_frame_id(), rect=page.inner_rect_mm())]
            episode.pages.append(page)
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

    raise ApplyError(f"unknown op: {name}")


def _new_frame_id() -> str:
    from genko.models import _new_id

    return _new_id()


def apply_ops(episode: Episode, ops: list[dict[str, Any]]) -> dict[str, Any]:
    if not isinstance(ops, list):
        raise ApplyError("ops must be a JSON array")
    applied: list[str] = []
    for i, op in enumerate(ops):
        if not isinstance(op, dict):
            raise ApplyError(f"ops[{i}] must be an object")
        try:
            _apply_one(episode, op)
        except (KeyError, ValueError) as exc:
            if isinstance(exc, ApplyError):
                raise
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        applied.append(str(op.get("op")))
    return {"ok": True, "applied": applied, "snapshot": snapshot(episode)}
