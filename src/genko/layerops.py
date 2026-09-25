"""J5 layer work on several layers at once and the liquify tool.

Merging chosen layers (結合) and the visible ones (表示レイヤーの結合 / のコピー), putting layers in a
new folder (グループ化), moving several at once, changing a layer's kind (ペン ⇄ 描画), setting many
layers in one go (まとめて設定), the paper's colour (用紙色) and liquify (ゆがみツール: push, pinch,
bloat, twirl on both pixels and pen lines).
"""

from __future__ import annotations

import math
from typing import Any

from PIL import Image

from genko.models import Layer, LayerKind, LayerRole, new_id
from genko.ops import ApplyError

OPS = ("merge_layers", "merge_visible", "group_layers", "move_layers", "convert_layer", "set_layers", "set_paper", "liquify")
CORE = (LayerRole.NAME, LayerRole.INK, LayerRole.BG, LayerRole.FINISH)
LIQUIFY = ("push", "pinch", "bloat", "twirl_cw", "twirl_ccw")


def _ids(op: dict) -> list[str]:
    ids = [str(v) for v in op.get("ids") or []]
    if not ids:
        raise ApplyError("ids is the list of layer ids")
    return ids


def _layers(page, ids: list[str]) -> list[Layer]:
    """The layers named, in page order (bottom first)."""
    by_id = {layer.id: layer for layer in page.layers}
    missing = [i for i in ids if i not in by_id]
    if missing:
        raise ApplyError(f"no layer {missing[0]}")
    wanted = set(ids)
    return [layer for layer in page.layers if layer.id in wanted]


def _composite(page, layers: list[Layer], episode, dpi: int) -> Image.Image:
    """The layers drawn over each other as they show (blend, opacity, clip), over transparency."""
    from genko import render

    size = (render.mm_to_px(page.spec.width_mm, dpi), render.mm_to_px(page.spec.height_mm, dpi))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    prev = None
    for layer in layers:
        if layer.kind == LayerKind.FOLDER:
            continue
        if layer.kind == LayerKind.ADJUST:
            out = render._adjusted(out, layer, prev if layer.clip else None)
            continue
        picture = render.layer_image(page, layer, dpi, episode)
        opacity = 1.0 if layer.opacity is None else float(layer.opacity)
        clip = prev if layer.clip else None
        below_alpha = out.split()[3]
        out = render._blend_over(out, picture, layer.blend or "normal", opacity, clip)
        if (layer.blend or "normal") != "normal":  # where only this layer is, it still shows
            from PIL import ImageChops

            shown = picture.split()[3]
            if clip is not None:
                shown = ImageChops.multiply(shown, clip)
            out.putalpha(ImageChops.lighter(below_alpha, shown.point(lambda v, o=opacity: int(v * o))))
        prev = picture.split()[3]
    return out


def _into_pixels(page, target: Layer, picture: Image.Image) -> None:
    from genko import raster as rasters

    target.strokes, target.patches, target.mask = [], [], None
    target.kind = LayerKind.RASTER
    target.fill = target.adjust = target.effect = None
    target.fill_rgb = None
    target.blend, target.opacity, target.clip = "normal", 1.0, False
    rasters.save_raster(page, target, picture)


def _check_mergeable(layers: list[Layer]) -> None:
    for layer in layers:
        if layer.kind == LayerKind.PLACED or layer.tone or layer.kind == LayerKind.TONE:
            raise ApplyError("placed images and tones cannot be merged")
        if layer.locked:
            raise ApplyError("the layer is locked")


def _empty(layer: Layer) -> None:
    layer.strokes, layer.patches, layer.raster_png = [], [], None


def merge_layers(episode, page, op: dict) -> None:
    """The chosen layers into the lowest of them, drawn as they show; the others go (a core layer is emptied)."""
    from genko import raster as rasters

    layers = [layer for layer in _layers(page, _ids(op)) if layer.kind != LayerKind.FOLDER]
    if len(layers) < 2:
        raise ApplyError("choose two or more layers to merge")
    _check_mergeable(layers)
    target = layers[0]
    picture = _composite(page, layers, episode, rasters.WORKING_DPI)
    _into_pixels(page, target, picture)
    if op.get("name"):
        target.title = str(op["name"])
    for layer in layers[1:]:
        if layer.role in CORE:
            _empty(layer)
        else:
            page.layers.remove(layer)


def merge_visible(episode, page, op: dict) -> None:
    """copy (default): a new paint layer on top holding what the visible layers show together.
    Otherwise the visible layers are merged into the lowest of them."""
    from genko import raster as rasters

    shown = [layer for layer in page.layers if layer.visible and layer.kind != LayerKind.FOLDER
             and layer.role not in (LayerRole.NAME, LayerRole.DRAFT) and _parents_visible(page, layer)]
    if not shown:
        raise ApplyError("no layer is showing")
    picture = _composite(page, shown, episode, rasters.WORKING_DPI)
    if op.get("copy", True):
        layer = Layer(id=str(op.get("id") or new_id()), role=LayerRole.USER, kind=LayerKind.RASTER,
                      title=str(op.get("name") or "表示レイヤーのコピー"), exportable=True)
        if any(item.id == layer.id for item in page.layers):
            raise ApplyError(f"layer {layer.id} exists")
        page.layers.append(layer)
        rasters.save_raster(page, layer, picture)
        return
    _check_mergeable(shown)
    target = shown[0]
    _into_pixels(page, target, picture)
    for layer in shown[1:]:
        if layer.role in CORE:
            _empty(layer)
        else:
            page.layers.remove(layer)


def _parents_visible(page, layer: Layer) -> bool:
    by_id = {item.id: item for item in page.layers}
    parent = by_id.get(layer.parent_id or "")
    seen = set()
    while parent is not None and parent.id not in seen:
        if not parent.visible:
            return False
        seen.add(parent.id)
        parent = by_id.get(parent.parent_id or "")
    return True


def group_layers(episode, page, op: dict) -> None:
    """A new folder holding the chosen layers, where the topmost of them was."""
    layers = _layers(page, _ids(op))
    folder = Layer(id=str(op.get("id") or new_id()), role=LayerRole.USER, kind=LayerKind.FOLDER,
                   title=str(op.get("name") or "フォルダー"), exportable=True, parent_id=layers[-1].parent_id)
    if any(item.id == folder.id for item in page.layers):
        raise ApplyError(f"layer {folder.id} exists")
    if folder.id in {layer.id for layer in layers}:
        raise ApplyError("a folder cannot hold itself")
    page.layers.insert(page.layers.index(layers[-1]) + 1, folder)
    moved = [layer for layer in layers]
    for layer in moved:
        page.layers.remove(layer)
        layer.parent_id = folder.id
    at = page.layers.index(folder)
    page.layers[at:at] = moved


def move_layers(episode, page, op: dict) -> None:
    """The chosen layers together: into a folder (parent; null: out of any) and/or just above a layer
    (after; "bottom": to the bottom)."""
    layers = _layers(page, _ids(op))
    if "parent" in op:
        parent = op.get("parent")
        if parent:
            folder = next((item for item in page.layers if item.id == parent), None)
            if folder is None or folder.kind != LayerKind.FOLDER:
                raise ApplyError("parent must be a folder")
            if folder in layers or _inside(page, folder, {layer.id for layer in layers}):
                raise ApplyError("a folder cannot hold itself")
        for layer in layers:
            layer.parent_id = str(parent) if parent else None
    after = op.get("after")
    if after is None and "parent" in op and op.get("parent"):
        after = op["parent"]  # (into a folder: just above it, which is where its layers sit)
        folder = next(item for item in page.layers if item.id == after)
        kids = [item for item in page.layers if item.parent_id == folder.id and item not in layers]
        after = kids[-1].id if kids else "__before__" + folder.id
    if after is not None:
        for layer in layers:
            page.layers.remove(layer)
        if after == "bottom":
            page.layers[0:0] = layers
        elif str(after).startswith("__before__"):
            at = next(i for i, item in enumerate(page.layers) if item.id == str(after)[10:])
            page.layers[at:at] = layers
        else:
            anchor = next((i for i, item in enumerate(page.layers) if item.id == after), None)
            if anchor is None:
                raise ApplyError(f"no layer {after}")
            page.layers[anchor + 1:anchor + 1] = layers


def _inside(page, folder: Layer, ids: set[str]) -> bool:
    by_id = {item.id: item for item in page.layers}
    parent = by_id.get(folder.parent_id or "")
    seen = set()
    while parent is not None and parent.id not in seen:
        if parent.id in ids:
            return True
        seen.add(parent.id)
        parent = by_id.get(parent.parent_id or "")
    return False


def convert_layer(episode, page, op: dict) -> None:
    """to paint: the lines become pixels. to pen: the pixels are traced into pen lines (their middle
    lines, each as wide as the mark). to paint also turns a fill layer into its pixels."""
    from genko import raster as rasters
    from genko import render

    layer = _layers(page, [str(op.get("id") or "")])[0]
    to = str(op.get("to") or "")
    if layer.locked:
        raise ApplyError("the layer is locked")
    if to == "paint":
        if layer.kind in (LayerKind.FOLDER, LayerKind.ADJUST, LayerKind.TONE) or layer.tone:
            raise ApplyError("this layer cannot become a paint layer")
        picture = render.layer_image(page, layer, rasters.WORKING_DPI, episode)
        if layer.kind == LayerKind.FILL and (layer.fill or layer.fill_rgb):
            picture = render.fill_layer_image(layer, picture.size, rasters.WORKING_DPI)
        blend, opacity, clip = layer.blend, layer.opacity, layer.clip
        _into_pixels(page, layer, picture)  # (the mask is drawn into the pixels)
        layer.blend, layer.opacity, layer.clip = blend, opacity, clip
        layer.asset = None
        return
    if to == "pen":
        if layer.kind not in (LayerKind.RASTER, LayerKind.STROKES):
            raise ApplyError("only a paint layer can become a pen layer")
        from genko.vectorize import trace_layer

        picture = render.layer_image(page, layer, rasters.WORKING_DPI, episode)
        strokes = trace_layer(picture, rasters.WORKING_DPI, float(op.get("min_mm", 0.8)))
        if not strokes:
            raise ApplyError("the layer has no marks to trace")
        layer.strokes = strokes
        layer.patches, layer.raster_png = [], None
        layer.kind = LayerKind.STROKES
        return
    raise ApplyError("to must be paint or pen")


SETTABLE = ("visible", "opacity", "blend", "clip", "lock_alpha", "locked", "panel_clip", "color", "reference", "exportable",
            "color_prints", "effect")


def set_layers(episode, page, op: dict) -> None:
    """The same settings on many layers: {ids | all, ...set_layer fields}."""
    from genko.ops import _apply_one

    if op.get("all"):
        ids = [layer.id for layer in page.layers if layer.kind != LayerKind.FOLDER or "visible" in op]
    else:
        ids = [layer.id for layer in _layers(page, _ids(op))]
    fields = {key: op[key] for key in SETTABLE if key in op}
    if not fields:
        raise ApplyError(f"set_layers needs something to set ({', '.join(SETTABLE)})")
    for layer_id in ids:
        _apply_one(episode, {"op": "set_layer", "page": page.index, "id": layer_id, **fields})


def set_paper(episode, op: dict) -> None:
    """The paper's colour (用紙色): one page, or every page with no page given. null: white again."""
    rgb = op.get("rgb")
    value = [int(v) for v in rgb][:3] if rgb else None
    if value is not None and (len(value) != 3 or any(not 0 <= v <= 255 for v in value)):
        raise ApplyError("rgb is [r, g, b], each 0..255")
    pages = episode.pages if op.get("page") in (None, "", 0) else [p for p in episode.pages if p.index == int(op["page"])]
    if not pages:
        raise ApplyError("page not found")
    for page in pages:
        if value is None:
            page.extra.pop("paper_rgb", None)
        else:
            page.extra["paper_rgb"] = value


# --- liquify -----------------------------------------------------------------------------------------------------


def _displace(x, y, cx, cy, dx, dy, radius, mode, strength):
    """Where a point goes under one dab (numbers or numpy arrays, mm)."""
    import numpy as np

    rx, ry = x - cx, y - cy
    d = np.hypot(rx, ry)
    fall = np.clip(1 - d / radius, 0, 1) ** 2 * strength
    if mode == "push":
        return x + dx * fall, y + dy * fall
    if mode in ("pinch", "bloat"):
        k = -0.5 if mode == "pinch" else 0.5
        return x + rx * fall * k, y + ry * fall * k
    angle = fall * (0.6 if mode == "twirl_cw" else -0.6)
    c, s = np.cos(angle), np.sin(angle)
    return cx + rx * c - ry * s, cy + rx * s + ry * c


def _dabs(points: list, radius: float) -> list[tuple[float, float, float, float]]:
    """(x, y, dx, dy) along the drag, a quarter radius apart."""
    out = []
    if len(points) == 1:
        return [(points[0][0], points[0][1], 0.0, 0.0)]
    step = max(0.2, radius / 4)
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        length = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(length / step))
        for k in range(n):
            t = k / n
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, (x1 - x0) / n, (y1 - y0) / n))
    return out


def liquify(episode, page, op: dict) -> None:
    import numpy as np

    from genko import raster as rasters
    from genko.ops import _paint_target

    layer = _paint_target(page, op)
    mode = str(op.get("mode") or "push")
    if mode not in LIQUIFY:
        raise ApplyError(f"mode must be one of {', '.join(LIQUIFY)}")
    try:
        points = [(float(p[0]), float(p[1])) for p in op.get("points") or []]
    except (TypeError, ValueError, IndexError) as exc:
        raise ApplyError("points is [[x, y], ...] in mm") from exc
    if not points:
        raise ApplyError("points is [[x, y], ...] in mm")
    radius = max(0.5, float(op.get("width_mm", 10)) / 2)
    strength = max(0.05, min(1.0, float(op.get("strength", 0.6))))
    dabs = _dabs(points, radius)
    if mode != "push":
        dabs = [(x, y, 0.0, 0.0) for x, y, _dx, _dy in dabs[:: max(1, len(dabs) // 40)]]
    reach_box = (min(x for x, *_ in dabs) - radius, min(y for _x, y, *_ in dabs) - radius,
                 max(x for x, *_ in dabs) + radius, max(y for _x, y, *_ in dabs) + radius)
    for stroke in layer.strokes:  # pen lines: their points move (more points first where the tool reaches)
        _densify(stroke, reach_box, radius / 4)
        xs = np.array([p[0] for p in stroke.points], dtype=float)
        ys = np.array([p[1] for p in stroke.points], dtype=float)
        for cx, cy, dx, dy in dabs:
            xs, ys = _displace(xs, ys, cx, cy, dx, dy, radius, mode, strength)
        stroke.points = [(round(float(a), 3), round(float(b), 3)) for a, b in zip(xs, ys)]
    if layer.raster_png or layer.patches:  # pixels: each place fetches from where the dabs pulled it
        from genko import render
        from genko.filters import _remap

        dpi = rasters.WORKING_DPI
        picture = render.layer_image(page, _pixels_only(layer), dpi, episode)
        box = picture.getbbox()
        if box:
            scale = dpi / 25.4
            reach = (min(x for x, *_ in dabs) - radius, min(y for _x, y, *_ in dabs) - radius,
                     max(x for x, *_ in dabs) + radius, max(y for _x, y, *_ in dabs) + radius)
            x0 = max(0, int(reach[0] * scale) - 2)
            y0 = max(0, int(reach[1] * scale) - 2)
            x1 = min(picture.width, int(reach[2] * scale) + 3)
            y1 = min(picture.height, int(reach[3] * scale) + 3)
            if x1 > x0 and y1 > y0:
                gx, gy = np.meshgrid((np.arange(x0, x1) + 0.5) / scale, (np.arange(y0, y1) + 0.5) / scale)
                sx, sy = gx.copy(), gy.copy()
                for cx, cy, dx, dy in reversed(dabs):  # (backwards: where did this place's pixel come from)
                    sx, sy = _undo(sx, sy, cx, cy, dx, dy, radius, mode, strength)
                picture.paste(_remap(picture, sx * scale - 0.5, sy * scale - 0.5), (x0, y0))
            layer.patches = []
            rasters.save_raster(page, layer, picture)


def _densify(stroke, box, step: float) -> None:
    """Points added along the parts of a line inside the box, `step` apart (pressure follows)."""
    pts = [(float(p[0]), float(p[1])) for p in stroke.points]
    if len(pts) < 2:
        return
    pressure = list(stroke.pressure) if len(stroke.pressure) == len(pts) else [1.0] * len(pts)
    x0, y0, x1, y1 = box
    out, out_p = [pts[0]], [pressure[0]]
    for k in range(1, len(pts)):
        (ax, ay), (bx, by) = pts[k - 1], pts[k]
        near = not (max(ax, bx) < x0 or min(ax, bx) > x1 or max(ay, by) < y0 or min(ay, by) > y1)
        n = max(1, int(math.hypot(bx - ax, by - ay) / step)) if near else 1
        for i in range(1, n + 1):
            t = i / n
            out.append((ax + (bx - ax) * t, ay + (by - ay) * t))
            out_p.append(pressure[k - 1] + (pressure[k] - pressure[k - 1]) * t)
    if len(out) != len(pts):
        stroke.points, stroke.pressure = out, out_p
        stroke.handles = None


def _undo(tx, ty, cx, cy, dx, dy, radius, mode, strength):
    """The place one dab moved to (tx, ty): x solving x + D(x) = t, found by a few fixed-point steps."""
    x, y = tx, ty
    for _ in range(5):
        fx, fy = _displace(x, y, cx, cy, dx, dy, radius, mode, strength)
        x, y = tx - (fx - x), ty - (fy - y)
    return x, y


def _pixels_only(layer: Layer) -> Layer:
    import copy

    twin = copy.copy(layer)
    twin.strokes, twin.mask = [], None
    return twin


def apply(episode, op: dict[str, Any], name: str) -> None:
    from genko.ops import _require_page

    if name == "set_paper":
        set_paper(episode, op)
        return
    page = _require_page(episode, op)
    {"merge_layers": merge_layers, "merge_visible": merge_visible, "group_layers": group_layers, "move_layers": move_layers,
     "convert_layer": convert_layer, "set_layers": set_layers, "liquify": liquify}[name](episode, page, op)
