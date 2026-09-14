from __future__ import annotations

import json
from pathlib import Path

from genko.models import (
    Binding,
    Episode,
    Frame,
    LayerRole,
    Page,
    PageSpec,
    Rect,
    StoryLine,
)


def _rect_to_dict(rect: Rect) -> dict:
    return {"x": rect.x, "y": rect.y, "width": rect.width, "height": rect.height}


def _rect_from_dict(data: dict) -> Rect:
    return Rect(data["x"], data["y"], data["width"], data["height"])


def _frame_to_dict(frame: Frame) -> dict:
    return {
        "id": frame.id,
        "rect": _rect_to_dict(frame.rect),
        "split_axis": frame.split_axis,
        "children": [_frame_to_dict(child) for child in frame.children],
    }


def _frame_from_dict(data: dict) -> Frame:
    return Frame(
        id=data["id"],
        rect=_rect_from_dict(data["rect"]),
        split_axis=data.get("split_axis"),
        children=[_frame_from_dict(child) for child in data.get("children", [])],
    )


def save_episode(episode: Episode, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    payload = {
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
                "note": page.note,
                "name_ok": page.name_ok,
                "stage": page.stage,
                "frames": [_frame_to_dict(frame) for frame in page.frames],
                "fills": {role.value: list(rgb) for role, rgb in page.fills.items()},
                "name_strokes": page.name_strokes,
                "ink_strokes": page.ink_strokes,
            }
            for page in episode.pages
        ],
        "story": [
            {
                "id": line.id,
                "page_index": line.page_index,
                "text": line.text,
                "speaker": line.speaker,
                "frame_id": line.frame_id,
            }
            for line in episode.story
        ],
    }
    (dest / "project.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def load_episode(src: Path) -> Episode:
    payload = json.loads((src / "project.json").read_text(encoding="utf-8"))
    spec = PageSpec(**payload["spec"])
    binding = Binding(payload["binding"])
    pages: list[Page] = []
    for raw in payload["pages"]:
        fills = {
            LayerRole(role): tuple(rgb)  # type: ignore[arg-type]
            for role, rgb in raw.get("fills", {}).items()
        }
        page = Page(
            index=raw["index"],
            spec=spec,
            frames=[_frame_from_dict(frame) for frame in raw["frames"]],
            binding=binding,
            note=raw.get("note", ""),
            name_ok=raw.get("name_ok", False),
            stage=raw.get("stage", "name"),
            fills=fills,
            name_strokes=[
                [tuple(pt) for pt in stroke]  # type: ignore[misc]
                for stroke in raw.get("name_strokes", [])
            ],
            ink_strokes=[
                [tuple(pt) for pt in stroke]  # type: ignore[misc]
                for stroke in raw.get("ink_strokes", [])
            ],
        )
        pages.append(page)
    story = [
        StoryLine(
            id=line["id"],
            page_index=line["page_index"],
            text=line["text"],
            speaker=line.get("speaker", ""),
            frame_id=line.get("frame_id"),
        )
        for line in payload.get("story", [])
    ]
    return Episode(
        title=payload["title"],
        episode=payload["episode"],
        spec=spec,
        binding=binding,
        pages=pages,
        story=story,
    )
