"""Working on part of a layer: an area (a polygon, or a mask over a box), and what lies in it — pen lines,
fills and pasted pixels (patches), and the layer's pixels — lifted, moved, scaled, turned, flipped,
deleted, copied, pasted.

Areas: {"poly": [[x, y], …]} in page mm, or {"mask": {"box": [x, y, w, h], "png": base64}} (from the
auto-select). Matrices are affine [a, b, c, d, e, f]: x' = a·x + c·y + e, y' = b·x + d·y + f (mm).
"""

from __future__ import annotations

import base64
import io
import math

from PIL import Image, ImageChops, ImageDraw

from genko.fill import FILL_DPI, mask_patch
from genko.models import coerce_stroke, new_id, stroke_points

Matrix = tuple[float, float, float, float, float, float]
IDENTITY: Matrix = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def _mm2px(v: float, dpi: int = FILL_DPI) -> float:
    return v / 25.4 * dpi


def area_mask(area: dict, dpi: int = FILL_DPI) -> tuple[Image.Image, tuple[int, int]]:
    """The area as a mask (L) and its top-left in px at `dpi`."""
    if area.get("poly"):
        pts = [(_mm2px(float(x), dpi), _mm2px(float(y), dpi)) for x, y in area["poly"]]
        x0, y0 = int(min(p[0] for p in pts)) - 1, int(min(p[1] for p in pts)) - 1
        x1, y1 = int(max(p[0] for p in pts)) + 2, int(max(p[1] for p in pts)) + 2
        mask = Image.new("L", (max(1, x1 - x0), max(1, y1 - y0)), 0)
        ImageDraw.Draw(mask).polygon([(x - x0, y - y0) for x, y in pts], fill=255)
        return mask, (x0, y0)
    spec = area.get("mask") or {}
    x, y, w, h = (float(v) for v in spec["box"])
    data = spec["png"] if isinstance(spec["png"], bytes) else base64.b64decode(spec["png"])
    image = Image.open(io.BytesIO(data)).convert("L")
    size = (max(1, round(_mm2px(w, dpi))), max(1, round(_mm2px(h, dpi))))
    return image.resize(size, Image.Resampling.NEAREST), (round(_mm2px(x, dpi)), round(_mm2px(y, dpi)))


def contains(area: dict, x: float, y: float) -> bool:
    if area.get("poly"):
        pts = [(float(a), float(b)) for a, b in area["poly"]]
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-9) + xi:
                inside = not inside
            j = i
        return inside
    mask, (x0, y0) = area_mask(area)
    px, py = round(_mm2px(x)) - x0, round(_mm2px(y)) - y0
    return 0 <= px < mask.width and 0 <= py < mask.height and mask.getpixel((px, py)) > 127


def stroke_inside(stroke, area: dict) -> bool:
    """A line belongs to the area when most of its points are in it."""
    pts = stroke.points
    return bool(pts) and sum(1 for x, y in pts if contains(area, x, y)) * 2 >= len(pts)


def apply(m: Matrix, x: float, y: float) -> tuple[float, float]:
    a, b, c, d, e, f = m
    return a * x + c * y + e, b * x + d * y + f


def _invert(m: Matrix) -> Matrix:
    a, b, c, d, e, f = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        raise ValueError("the transform squashes the selection flat")
    ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
    return ia, ib, ic, id_, -(ia * e + ic * f), -(ib * e + id_ * f)


def transform_stroke(stroke, m: Matrix):
    out = coerce_stroke([(*apply(m, x, y), *( [p] if p is not None else [])) for (x, y), p in
                         zip(stroke.points, stroke.pressure or [None] * len(stroke.points))])
    a, b, c, d, _, _ = m
    out.width_mm = round(stroke.width_mm * math.sqrt(abs(a * d - b * c)), 4)
    out.kind, out.rgb, out.opacity = stroke.kind, stroke.rgb, stroke.opacity
    return out


def _patch_image(patch: dict) -> Image.Image:
    image = Image.open(io.BytesIO(patch["png"]))
    return image.convert("L") if patch.get("mode", "mask") == "mask" else image.convert("RGBA")


def _patch_px(patch: dict, dpi: int = FILL_DPI) -> tuple[Image.Image, tuple[int, int]]:
    x, y, w, h = (float(v) for v in patch["box"])
    size = (max(1, round(_mm2px(w, dpi))), max(1, round(_mm2px(h, dpi))))
    return _patch_image(patch).resize(size, Image.Resampling.LANCZOS), (round(_mm2px(x, dpi)), round(_mm2px(y, dpi)))


def _to_patch(image: Image.Image, origin: tuple[int, int], template: dict, dpi: int = FILL_DPI) -> dict | None:
    """A patch from a page-aligned image (L mask or RGBA) cropped to what it shows."""
    alpha = image if image.mode == "L" else image.split()[3]
    box = alpha.getbbox()
    if box is None:
        return None
    crop = image.crop(box)
    buf = io.BytesIO()
    crop.save(buf, format="PNG", optimize=True)
    mm = 25.4 / dpi
    return {**{k: v for k, v in template.items() if k not in ("png", "asset", "box", "id")}, "id": new_id(),
            "box": [round((origin[0] + box[0]) * mm, 3), round((origin[1] + box[1]) * mm, 3),
                    round(crop.width * mm, 3), round(crop.height * mm, 3)],
            "png": buf.getvalue()}


def _split_patch(patch: dict, area: dict) -> tuple[dict | None, dict | None]:
    """(the part outside the area, the part inside)."""
    image, origin = _patch_px(patch)
    mask, (mx, my) = area_mask(area)
    local = Image.new("L", image.size, 0)
    local.paste(mask, (mx - origin[0], my - origin[1]))
    alpha = image if image.mode == "L" else image.split()[3]
    inside_alpha = ImageChops.multiply(alpha, local)
    outside_alpha = ImageChops.subtract(alpha, inside_alpha)

    def with_alpha(a: Image.Image) -> Image.Image:
        if image.mode == "L":
            return a
        out = image.copy()
        out.putalpha(a)
        return out

    return _to_patch(with_alpha(outside_alpha), origin, patch), _to_patch(with_alpha(inside_alpha), origin, patch)


def transform_patch(patch: dict, m: Matrix, resample=Image.Resampling.BILINEAR) -> dict | None:
    image, (ox, oy) = _patch_px(patch)
    corners = [(ox, oy), (ox + image.width, oy), (ox, oy + image.height), (ox + image.width, oy + image.height)]
    scale = FILL_DPI / 25.4

    def page_px(x: float, y: float) -> tuple[float, float]:  # a transform in mm applied to px
        tx, ty = apply(m, x / scale, y / scale)
        return tx * scale, ty * scale

    moved = [page_px(x, y) for x, y in corners]
    nx0, ny0 = math.floor(min(p[0] for p in moved)), math.floor(min(p[1] for p in moved))
    nx1, ny1 = math.ceil(max(p[0] for p in moved)), math.ceil(max(p[1] for p in moved))
    inv = _invert(m)

    def back(x: float, y: float) -> tuple[float, float]:
        tx, ty = apply(inv, x / scale, y / scale)
        return tx * scale, ty * scale

    # PIL's affine maps output (x, y) → input: express back(x + nx0, y + ny0) − (ox, oy) as coefficients
    p00 = back(nx0, ny0)
    p10 = back(nx0 + 1, ny0)
    p01 = back(nx0, ny0 + 1)
    coeffs = (p10[0] - p00[0], p01[0] - p00[0], p00[0] - ox, p10[1] - p00[1], p01[1] - p00[1], p00[1] - oy)
    size = (max(1, nx1 - nx0), max(1, ny1 - ny0))
    out = image.transform(size, Image.Transform.AFFINE, coeffs, resample=resample)
    return _to_patch(out, (nx0, ny0), patch)


def lift(layer, area: dict, page=None) -> dict:
    """Take what lies in the area off the layer: {"strokes": [...], "patches": [...]}; the layer keeps the rest.
    The layer's own pixels (raster) in the area become an image patch."""
    strokes_in, strokes_out = [], []
    for stroke in layer.strokes:
        (strokes_in if stroke_inside(stroke, area) else strokes_out).append(stroke)
    patches_in, patches_out = [], []
    for patch in layer.patches:
        if not patch.get("png"):
            patches_out.append(patch)
            continue
        outside, inside = _split_patch(patch, area)
        if outside:
            patches_out.append(outside)
        if inside:
            patches_in.append(inside)
    if layer.raster_png and page is not None:
        raster = Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
        from genko.raster import WORKING_DPI

        mask, (mx, my) = area_mask(area, WORKING_DPI)
        local = Image.new("L", raster.size, 0)
        local.paste(mask, (mx, my))
        inside_alpha = ImageChops.multiply(raster.split()[3], local)
        if inside_alpha.getbbox():
            piece = raster.copy()
            piece.putalpha(inside_alpha)
            kept = raster.copy()
            kept.putalpha(ImageChops.subtract(raster.split()[3], inside_alpha))
            buf = io.BytesIO()
            kept.save(buf, format="PNG")
            layer.raster_png = buf.getvalue()
            patch = _to_patch(piece, (0, 0), {"mode": "image", "opacity": 1.0}, WORKING_DPI)
            if patch:
                patches_in.append(patch)
    layer.strokes, layer.patches = strokes_out, patches_out
    return {"strokes": strokes_in, "patches": patches_in}


def drop(layer, items: dict, m: Matrix = IDENTITY, fresh_ids: bool = False, resample=Image.Resampling.BILINEAR) -> None:
    """Put lifted (or copied) items back on the layer, transformed by `m`."""
    for stroke in items.get("strokes", []):
        moved = transform_stroke(stroke, m) if m != IDENTITY else stroke
        if fresh_ids or moved is not stroke:
            moved.id = new_id() if fresh_ids else stroke.id
        layer.strokes.append(moved)
    for patch in items.get("patches", []):
        moved = transform_patch(patch, m, resample) if m != IDENTITY else dict(patch)
        if moved:
            if fresh_ids:
                moved["id"] = new_id()
            layer.patches.append(moved)


def drop_warped(layer, items: dict, go, resample=Image.Resampling.BILINEAR) -> None:
    """Put lifted items back through a free transform (genko.warp): lines point by point, pixels piece by piece."""
    from genko import warp

    for stroke in items.get("strokes", []):
        layer.strokes.append(warp.warp_stroke(stroke, go))
    for patch in items.get("patches", []):
        image, origin = _patch_px(patch)
        warped = warp.warp_image(image, origin, go, FILL_DPI, resample=resample)
        if warped is not None:
            moved = _to_patch(warped[0], warped[1], patch)
            if moved:
                layer.patches.append(moved)


def items_to_json(items: dict) -> dict:
    """Copied items in a form that travels in an op (the clipboard)."""
    from genko.models import stroke_to_dict

    return {"strokes": [stroke_to_dict(s) for s in items.get("strokes", [])],
            "patches": [{**{k: v for k, v in p.items() if k not in ("png", "asset")},
                         "png": base64.b64encode(p["png"]).decode("ascii")} for p in items.get("patches", []) if p.get("png")]}


def items_from_json(data: dict) -> dict:
    return {"strokes": [coerce_stroke(s) for s in data.get("strokes", [])],
            "patches": [{**p, "png": base64.b64decode(p["png"])} for p in data.get("patches", []) if p.get("png")]}


def area_bbox(area: dict) -> tuple[float, float, float, float]:
    if area.get("poly"):
        xs, ys = [float(p[0]) for p in area["poly"]], [float(p[1]) for p in area["poly"]]
        return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)
    return tuple(float(v) for v in area["mask"]["box"])  # type: ignore[return-value]


def wand_area(mask: Image.Image, dpi: int) -> dict | None:
    """An auto-selected region as an area."""
    patch = mask_patch(mask, dpi, (0, 0, 0))
    if patch is None:
        return None
    return {"mask": {"box": patch["box"], "png": base64.b64encode(patch["png"]).decode("ascii")}}


def stroke_points_of(stroke) -> list:
    return stroke_points(stroke)
