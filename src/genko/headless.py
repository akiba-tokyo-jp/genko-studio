from __future__ import annotations

from typing import Any

from genko.models import Episode, Frame, LayerRole, StoryLine, stroke_points
from genko.ops import OPS_SCHEMA, ApplyError, apply_ops

__all__ = ["OPS_SCHEMA", "ApplyError", "apply_ops", "snapshot", "inspect_stroke"]


def _frame_brief(frame: Frame) -> dict[str, Any]:
    r = frame.rect
    return {"id": frame.id, "x": r.x, "y": r.y, "width": r.width, "height": r.height}


def _line_brief(line: StoryLine) -> dict[str, Any]:
    return {
        "id": line.id,
        "text": line.text,
        "speaker": line.speaker,
        "frame_id": line.frame_id,
        "x_mm": line.x_mm,
        "y_mm": line.y_mm,
    }


def _count(page, role) -> int:
    return sum(len(layer.strokes) for layer in page.layers if layer.role == role)


def snapshot(episode: Episode, full: bool = False) -> dict[str, Any]:
    pages = []
    for page in episode.pages:
        item: dict[str, Any] = {
            "index": page.index,
            "name_ok": page.name_ok,
            "stage": page.stage,
            "note": page.note,
            "leaf_count": len(page.leaf_frames()),
            "leaves": [_frame_brief(frame) for frame in page.leaf_frames()],
            "story": [_line_brief(line) for line in episode.story_for_page(page.index)],
            "name_stroke_count": _count(page, LayerRole.NAME),
            "ink_stroke_count": _count(page, LayerRole.INK),
            "selected_frame_id": page.selected_frame_id,
            "layers": [
                {
                    "id": layer.id,
                    "role": layer.role.value,
                    "exportable": layer.exportable,
                    "visible": layer.visible,
                    "stroke_count": len(layer.strokes),
                }
                for layer in page.layers
            ],
        }
        if full:
            item["name_strokes"] = page.name_strokes
            item["ink_strokes"] = page.ink_strokes
        pages.append(item)
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
            "preset": episode.spec.preset,
        } | ({"trim_w_mm": episode.spec.trim_w_mm, "trim_h_mm": episode.spec.trim_h_mm} if episode.spec.trim_w_mm else {})
          | ({"margins_mm": list(episode.spec.margins_mm)} if episode.spec.margins_mm else {}),
        "pages": pages,
        "tickets": episode.tickets,
        "autosave": episode.autosave,
    }


def inspect_stroke(episode: Episode, stroke_id: str) -> dict[str, Any]:
    for page in episode.pages:
        for layer in page.layers:
            for stroke in layer.strokes:
                if getattr(stroke, "id", None) == stroke_id:
                    return {
                        "id": stroke.id,
                        "page": page.index,
                        "layer": layer.role.value,
                        "points": stroke_points(stroke),
                        "kind": getattr(stroke, "kind", "gpen"),
                    }
    raise ApplyError(f"no stroke {stroke_id}")
