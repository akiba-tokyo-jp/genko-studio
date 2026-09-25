from __future__ import annotations

import json

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
    coerce_stroke,
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
        panel=data.get("panel"),
    )


def _layer(data: dict, store=None) -> Layer:
    role = LayerRole(data["role"])
    layer = _layer_fields(data, role)
    if layer.kind == LayerKind.PLACED:
        layer.asset = data.get("asset")
        layer.frame_id = data.get("frame_id")
        layer.placement_mm = _rect(data["placement_mm"]) if data.get("placement_mm") else None
        layer.fit = data.get("fit") or "cover"
        layer.clip_to = data.get("clip_to") or "frame"
        layer.source = data.get("source")
        layer.finish = data.get("finish")
        return layer
    if store is not None and data.get("asset"):
        layer.raster_relpath = store.relpath(data["asset"], ".png")
        layer.raster_png = store.get_bytes(data["asset"], ".png")
    if store is not None and data.get("strokes_blob"):
        blob = store.get_bytes(data["strokes_blob"], ".strokes.json")
        if blob is not None:
            layer.strokes = [coerce_stroke(item) for item in json.loads(blob)]
    return layer


def _layer_fields(data: dict, role: LayerRole) -> Layer:
    return Layer(
        id=data.get("id") or new_id(),
        role=role,
        kind=LayerKind(data.get("kind", "strokes")),
        visible=data.get("visible", True),
        exportable=data.get("exportable", role not in (LayerRole.NAME, LayerRole.DRAFT)),
        strokes=[coerce_stroke(stroke) for stroke in data.get("strokes", [])],
        raster_relpath=data.get("raster_relpath"),
        fill_rgb=tuple(data["fill_rgb"]) if data.get("fill_rgb") else None,  # type: ignore[arg-type]
        lpi=data.get("lpi"),
        density=data.get("density"),
        region=[tuple(pt) for pt in data["region"]] if data.get("region") else None,
        opacity=float(data.get("opacity", 1)),
        material_id=data.get("material_id"),
        angle=float(data.get("angle", 45)),
        title=str(data.get("title") or ""),
        blend=str(data.get("blend") or "normal"),
        clip=bool(data.get("clip", False)),
        lock_alpha=bool(data.get("lock_alpha", False)),
        locked=bool(data.get("locked", False)),
        parent_id=data.get("parent_id"),
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
        tail=tuple(data["tail"]) if data.get("tail") else None,
        wrap=data.get("wrap", "horizontal"),
        ruby_runs=[tuple(item) for item in data.get("ruby_runs") or []],
        path=[tuple(pt) for pt in data["path"]] if data.get("path") else None,
        style=dict(data.get("style") or {}),
        tails=[dict(t) for t in data.get("tails") or []],
    )


SUPPORTED_VERSION = 3
KNOWN_TOP_KEYS = frozenset({
    "version", "revision", "title", "episode", "binding", "start_side", "strict_gates", "autosave",
    "font_path", "page_locks", "brush", "spec", "bible", "tickets", "studio", "pages", "story",
})
KNOWN_PAGE_KEYS = frozenset({
    "id", "art_ok", "plan", "index", "note", "name_ok", "stage", "spread_with", "numero", "onion_from", "lt_threshold",
    "effects", "ruler", "prims", "frames", "layers", "texts", "fills", "name_strokes", "ink_strokes",
})


class UnsupportedProjectVersion(ValueError):
    """The file was written by a newer Genko; opening it here could lose data."""


def migrate_payload(payload: dict, store=None) -> Episode:
    version = payload.get("version", 1)
    if not isinstance(version, int) or version > SUPPORTED_VERSION:
        raise UnsupportedProjectVersion(
            f"project.json version {version} is newer than this build supports ({SUPPORTED_VERSION}); update Genko"
        )
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
            id=raw.get("id") or "pg_" + new_id(),
            art_ok=bool(raw.get("art_ok", False)),
            plan=raw.get("plan"),
            index=raw["index"],
            spec=spec,
            frames=[_frame(frame) for frame in raw["frames"]],
            binding=binding,
            note=raw.get("note", ""),
            name_ok=raw.get("name_ok", False),
            stage=raw.get("stage", "name"),
            spread_with=raw.get("spread_with"),
            numero=raw.get("numero", True),
            effects=list(raw.get("effects") or []),
            ruler=raw.get("ruler"),
            prims=list(raw.get("prims") or []),
            onion_from=raw.get("onion_from"),
            lt_threshold=raw.get("lt_threshold"),
        )
        fills = {
            LayerRole(role): tuple(rgb)  # type: ignore[arg-type]
            for role, rgb in raw.get("fills", {}).items()
        }
        page.fills = fills
        if raw.get("layers"):
            page.layers = [_layer(item, store) for item in raw["layers"]]
        else:
            page.layers = default_layers()
            page.name_strokes = [[tuple(pt) for pt in stroke] for stroke in raw.get("name_strokes", [])]  # type: ignore[misc]
            page.ink_strokes = [[tuple(pt) for pt in stroke] for stroke in raw.get("ink_strokes", [])]  # type: ignore[misc]
            for role, rgb in fills.items():
                page.paint(role, rgb)  # type: ignore[arg-type]
        if story:
            # One object per line: page.texts and episode.story must not drift apart.
            page_lines = [line for line in story if line.page_index == page.index]
        else:
            page_lines = [_line(item) for item in raw.get("texts", [])]
        page.texts = page_lines
        page.extra = {k: v for k, v in raw.items() if k not in KNOWN_PAGE_KEYS}
        pages.append(page)
    if not story:
        story = [line for page in pages for line in page.texts]
    episode = Episode(
        title=payload["title"],
        episode=payload["episode"],
        spec=spec,
        binding=binding,
        pages=pages,
        story=story,
        tickets=list(payload.get("tickets") or []),
        autosave=bool(payload.get("autosave", False)),
        font_path=str(payload.get("font_path") or ""),
        page_locks=dict(payload.get("page_locks") or {}),
    )
    brush = payload.get("brush") or {}
    if brush.get("rgb"):
        episode.brush_rgb = tuple(int(v) for v in brush["rgb"])  # type: ignore[assignment]
    episode.brush_width_mm = float(brush.get("width_mm", episode.brush_width_mm))
    episode.brush_stabilize = int(brush.get("stabilize", 0) or 0)
    episode.brush_taper = bool(brush.get("taper", False))
    episode.brush_curve = str(brush.get("curve") or "linear")
    bible = payload.get("bible") or {}
    episode.bible.plot = bible.get("plot", "")
    episode.bible.characters = list(bible.get("characters") or [])
    episode.bible.constraints = list(bible.get("constraints") or [])
    episode.extra = {k: v for k, v in payload.items() if k not in KNOWN_TOP_KEYS}
    episode.revision = int(payload.get("revision") or 0)
    episode.start_side = payload.get("start_side")
    episode.strict_gates = bool(payload.get("strict_gates", False))
    episode.studio = dict(payload.get("studio") or {})
    by_index = {str(page.index): page.id for page in pages}
    # v2 keyed locks by page number; v3 by page id.
    episode.page_locks = {by_index.get(str(key), str(key)): owner for key, owner in episode.page_locks.items()}
    return episode
