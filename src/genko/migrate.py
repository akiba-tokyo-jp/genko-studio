from __future__ import annotations

from genko.models import (
    Binding,
    Episode,
    Frame,
    Layer,
    LayerKind,
    LayerRole,
    Page,
    PageSpec,
    Rect,
    StoryLine,
    default_layers,
    new_id,
)


def _rect(data: dict) -> Rect:
    return Rect(data["x"], data["y"], data["width"], data["height"])


def _frame(data: dict) -> Frame:
    return Frame(
        id=data["id"],
        rect=_rect(data["rect"]),
        split_axis=data.get("split_axis"),
        children=[_frame(child) for child in data.get("children", [])],
        clip=data.get("clip", True),
        bleed=data.get("bleed", False),
        border_mm=float(data.get("border_mm", 0.8)),
    )


def _layer(data: dict) -> Layer:
    role = LayerRole(data["role"])
    return Layer(
        id=data.get("id") or new_id(),
        role=role,
        kind=LayerKind(data.get("kind", "strokes")),
        visible=data.get("visible", True),
        exportable=data.get("exportable", role not in (LayerRole.NAME, LayerRole.DRAFT)),
        strokes=[[tuple(pt) for pt in stroke] for stroke in data.get("strokes", [])],  # type: ignore[misc]
        raster_relpath=data.get("raster_relpath"),
        fill_rgb=tuple(data["fill_rgb"]) if data.get("fill_rgb") else None,  # type: ignore[arg-type]
    )


def _line(data: dict) -> StoryLine:
    return StoryLine(
        id=data["id"],
        page_index=data["page_index"],
        text=data["text"],
        speaker=data.get("speaker", ""),
        frame_id=data.get("frame_id"),
        ruby=data.get("ruby", ""),
        x_mm=float(data.get("x_mm", 0)),
        y_mm=float(data.get("y_mm", 0)),
        w_mm=float(data.get("w_mm", 40)),
        h_mm=float(data.get("h_mm", 20)),
        balloon=data.get("balloon", "speech"),
    )


def migrate_payload(payload: dict) -> Episode:
    spec_raw = payload["spec"]
    spec = PageSpec(
        width_mm=spec_raw["width_mm"],
        height_mm=spec_raw["height_mm"],
        dpi=spec_raw["dpi"],
        bleed_mm=spec_raw["bleed_mm"],
        inner_margin_mm=spec_raw["inner_margin_mm"],
        expression=spec_raw.get("expression", "mono"),
        preset=spec_raw.get("preset"),
    )
    binding = Binding(payload.get("binding", "right"))
    story = [_line(item) for item in payload.get("story", [])]
    pages: list[Page] = []
    for raw in payload["pages"]:
        page = Page(
            index=raw["index"],
            spec=spec,
            frames=[_frame(frame) for frame in raw["frames"]],
            binding=binding,
            note=raw.get("note", ""),
            name_ok=raw.get("name_ok", False),
            stage=raw.get("stage", "name"),
            spread_with=raw.get("spread_with"),
        )
        fills = {
            LayerRole(role): tuple(rgb)  # type: ignore[arg-type]
            for role, rgb in raw.get("fills", {}).items()
        }
        page.fills = fills
        if raw.get("layers"):
            page.layers = [_layer(item) for item in raw["layers"]]
        else:
            page.layers = default_layers()
            page.name_strokes = [[tuple(pt) for pt in stroke] for stroke in raw.get("name_strokes", [])]  # type: ignore[misc]
            page.ink_strokes = [[tuple(pt) for pt in stroke] for stroke in raw.get("ink_strokes", [])]  # type: ignore[misc]
            for role, rgb in fills.items():
                page.paint(role, rgb)  # type: ignore[arg-type]
        page_lines = [_line(item) for item in raw.get("texts", [])]
        if not page_lines:
            page_lines = [line for line in story if line.page_index == page.index]
        page.texts = page_lines
        pages.append(page)
    if not story:
        story = [line for page in pages for line in page.texts]
    return Episode(
        title=payload["title"],
        episode=payload["episode"],
        spec=spec,
        binding=binding,
        pages=pages,
        story=story,
    )
