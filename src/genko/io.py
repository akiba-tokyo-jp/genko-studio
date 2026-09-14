from __future__ import annotations

import json
from pathlib import Path

from genko.migrate import migrate_payload
from genko.models import Episode, Frame, Layer, Page, Rect, StoryLine


def _rect_to_dict(rect: Rect) -> dict:
    return {"x": rect.x, "y": rect.y, "width": rect.width, "height": rect.height}


def _frame_to_dict(frame: Frame) -> dict:
    return {
        "id": frame.id,
        "rect": _rect_to_dict(frame.rect),
        "split_axis": frame.split_axis,
        "clip": frame.clip,
        "bleed": frame.bleed,
        "border_mm": frame.border_mm,
        "children": [_frame_to_dict(child) for child in frame.children],
    }


def _layer_to_dict(layer: Layer) -> dict:
    return {
        "id": layer.id,
        "role": layer.role.value,
        "kind": layer.kind.value,
        "visible": layer.visible,
        "exportable": layer.exportable,
        "strokes": layer.strokes,
        "raster_relpath": layer.raster_relpath,
        "fill_rgb": list(layer.fill_rgb) if layer.fill_rgb else None,
        "lpi": layer.lpi,
        "density": layer.density,
        "region": layer.region,
    }


def _line_to_dict(line: StoryLine) -> dict:
    return {
        "id": line.id,
        "page_index": line.page_index,
        "text": line.text,
        "speaker": line.speaker,
        "frame_id": line.frame_id,
        "ruby": line.ruby,
        "x_mm": line.x_mm,
        "y_mm": line.y_mm,
        "w_mm": line.w_mm,
        "h_mm": line.h_mm,
        "balloon": line.balloon,
        "tail": list(line.tail) if line.tail else None,
    }


def save_episode(episode: Episode, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    payload = {
        "version": 2,
        "title": episode.title,
        "episode": episode.episode,
        "binding": episode.binding.value,
        "autosave": episode.autosave,
        "spec": {
            "width_mm": episode.spec.width_mm,
            "height_mm": episode.spec.height_mm,
            "dpi": episode.spec.dpi,
            "bleed_mm": episode.spec.bleed_mm,
            "inner_margin_mm": episode.spec.inner_margin_mm,
            "expression": episode.spec.expression,
            "preset": episode.spec.preset,
        },
        "bible": {
            "plot": episode.bible.plot,
            "characters": episode.bible.characters,
            "constraints": episode.bible.constraints,
        },
        "tickets": episode.tickets,
        "pages": [
            {
                "index": page.index,
                "note": page.note,
                "name_ok": page.name_ok,
                "stage": page.stage,
                "spread_with": page.spread_with,
                "numero": page.numero,
                "effects": page.effects,
                "ruler": page.ruler,
                "prims": page.prims,
                "frames": [_frame_to_dict(frame) for frame in page.frames],
                "layers": [_layer_to_dict(layer) for layer in page.layers],
                "texts": [_line_to_dict(line) for line in page.texts],
                "fills": {role.value: list(rgb) for role, rgb in page.fills.items()},
                "name_strokes": page.name_strokes,
                "ink_strokes": page.ink_strokes,
            }
            for page in episode.pages
        ],
        "story": [_line_to_dict(line) for line in episode.story],
    }
    (dest / "project.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    for page in episode.pages:
        for layer in page.layers:
            if layer.raster_png and layer.raster_relpath:
                path = dest / layer.raster_relpath
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(layer.raster_png)


def _attach_rasters(episode: Episode, src: Path) -> None:
    for page in episode.pages:
        for layer in page.layers:
            if layer.raster_relpath:
                path = src / layer.raster_relpath
                if path.is_file():
                    layer.raster_png = path.read_bytes()


def load_episode(src: Path) -> Episode:
    payload = json.loads((src / "project.json").read_text(encoding="utf-8"))
    episode = migrate_payload(payload)
    _attach_rasters(episode, src)
    return episode
