"""Areas beyond a polygon or a mask (J2): the same language for people (the selection tools and the
選択範囲 menu) and agents (any op's "area").

An area is one of
  {"poly": [[x, y], …]}                     a polygon (mm)
  {"mask": {"box": [x, y, w, h], "png": …}} a mask over a box
  {"rect": [x, y, w, h]} / {"ellipse": [x, y, w, h]}
  {"layer": "<layer id>"}                   where that layer has something drawn
  {"color": {"x_mm", "y_mm", "tolerance"?, "contiguous"?}}  the colour found there (the page as seen)
  {"all": true}                             the whole page
  {"saved": "<name>"}                       an area kept on the page (選択範囲をストック)
  {"union": [a, b, …]} / {"intersect": [a, b, …]} / {"subtract": [a, b, …]} (a minus the rest)
and any of them may add "invert": true, "grow_mm": n (a negative n shrinks), "feather_mm": n.

resolve() turns every kind into a polygon or a mask that the drawing ops understand.
"""

from __future__ import annotations

import base64
import io
import math

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SEL_DPI = 200
PLAIN = ("poly", "mask")
EXTRA_KEYS = ("rect", "ellipse", "layer", "color", "all", "saved", "union", "intersect", "subtract", "invert", "grow_mm",
              "feather_mm")


class AreaError(ValueError):
    pass


def needs_resolving(area) -> bool:
    return isinstance(area, dict) and any(key in area for key in EXTRA_KEYS)


def _px(v: float, dpi: int = SEL_DPI) -> float:
    return v / 25.4 * dpi


def page_size(page, dpi: int = SEL_DPI) -> tuple[int, int]:
    return max(1, round(_px(page.spec.width_mm, dpi))), max(1, round(_px(page.spec.height_mm, dpi)))


def ellipse_poly(box, n: int = 72) -> list[list[float]]:
    x, y, w, h = (float(v) for v in box)
    cx, cy, rx, ry = x + w / 2, y + h / 2, w / 2, h / 2
    return [[round(cx + rx * math.cos(math.tau * k / n), 3), round(cy + ry * math.sin(math.tau * k / n), 3)] for k in range(n)]


def rect_poly(box) -> list[list[float]]:
    x, y, w, h = (float(v) for v in box)
    return [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]


def to_mask(area: dict, page, episode=None, dpi: int = SEL_DPI) -> Image.Image:
    """The area as a page-sized mask (L, 255 = inside)."""
    size = page_size(page, dpi)
    mask = _base(area, page, episode, dpi, size)
    if area.get("grow_mm"):
        mask = _grow(mask, _px(float(area["grow_mm"]), dpi))
    if area.get("invert"):
        mask = ImageChops.invert(mask)
    if area.get("feather_mm"):
        mask = mask.filter(ImageFilter.GaussianBlur(max(0.1, _px(float(area["feather_mm"]), dpi) / 2)))
    return mask


def _base(area: dict, page, episode, dpi: int, size) -> Image.Image:
    from genko import selection

    mask = Image.new("L", size, 0)
    if area.get("all"):
        return Image.new("L", size, 255)
    if area.get("rect") or area.get("ellipse") or area.get("poly"):
        points = area.get("poly") or (rect_poly(area["rect"]) if area.get("rect") else ellipse_poly(area["ellipse"]))
        if len(points) < 3:
            raise AreaError("an area needs at least three points")
        ImageDraw.Draw(mask).polygon([(_px(float(x), dpi), _px(float(y), dpi)) for x, y in points], fill=255)
        return mask
    if area.get("mask"):
        part, (x0, y0) = selection.area_mask({"mask": area["mask"]}, dpi)
        mask.paste(part, (x0, y0))
        return mask
    if area.get("layer"):
        from genko import render

        layer = next((item for item in page.layers if item.id == area["layer"]), None)
        if layer is None:
            raise AreaError(f"no layer {area['layer']}")
        alpha = render.layer_image(page, layer, dpi, episode).split()[3]
        return alpha.resize(size).point(lambda v: 255 if v > 8 else 0)
    if area.get("color"):
        return _colour_area(area["color"], page, episode, dpi, size)
    if area.get("saved"):
        saved = (page.extra.get("saved_areas") or {}).get(str(area["saved"]))
        if saved is None:
            raise AreaError(f"no saved area {area['saved']}")
        return to_mask(saved, page, episode, dpi)
    for key in ("union", "intersect", "subtract"):
        if key in area:
            parts = [to_mask(item, page, episode, dpi) for item in area[key] or []]
            if not parts:
                raise AreaError(f"{key} needs areas")
            out = parts[0]
            for other in parts[1:]:
                if key == "union":
                    out = ImageChops.lighter(out, other)
                elif key == "intersect":
                    out = ImageChops.darker(out, other)
                else:
                    out = ImageChops.subtract(out, other)
            return out
    raise AreaError("an area is poly, mask, rect, ellipse, layer, color, all, saved, union, intersect or subtract")


def _grow(mask: Image.Image, amount_px: float) -> Image.Image:
    if abs(amount_px) < 0.5:
        return mask
    radius = abs(amount_px)
    blurred = mask.filter(ImageFilter.GaussianBlur(radius / 1.5))
    # grow: whatever the blur reaches; shrink: only what stays solid
    return blurred.point(lambda v: 255 if v > 12 else 0) if amount_px > 0 else blurred.point(lambda v: 255 if v > 243 else 0)


def _colour_area(spec: dict, page, episode, dpi: int, size) -> Image.Image:
    from genko import render

    image = render.render_page(page, dpi, mode="proof", episode=episode).convert("RGB").resize(size)
    x, y = round(_px(float(spec["x_mm"]), dpi)), round(_px(float(spec["y_mm"]), dpi))
    if not (0 <= x < size[0] and 0 <= y < size[1]):
        raise AreaError("the colour point is off the page")
    tolerance = max(0, min(255, int(spec.get("tolerance", 24))))
    target = image.getpixel((x, y))
    channels = [ImageChops.difference(band, Image.new("L", size, value)) for band, value in zip(image.split(), target)]
    worst = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
    similar = worst.point(lambda v: 255 if v <= tolerance else 0)
    if not spec.get("contiguous"):
        return similar
    # only the similar colour joined to the clicked point
    ImageDraw.floodfill(similar, (x, y), 128)
    return similar.point(lambda v: 255 if v == 128 else 0)


def from_mask(mask: Image.Image, dpi: int = SEL_DPI) -> dict | None:
    """A page-sized mask back to an area ({"mask"} cropped to what it covers; None when empty)."""
    box = mask.getbbox()
    if box is None:
        return None
    crop = mask.crop(box)
    buf = io.BytesIO()
    crop.save(buf, format="PNG", optimize=True)
    mm = 25.4 / dpi
    return {"mask": {"box": [round(box[0] * mm, 3), round(box[1] * mm, 3), round(crop.width * mm, 3), round(crop.height * mm, 3)],
                     "png": base64.b64encode(buf.getvalue()).decode("ascii")}}


def resolve(area, page, episode=None) -> dict:
    """Any area to a plain one (poly or mask) the drawing ops take."""
    if not needs_resolving(area):
        return area
    if set(area) <= {"rect"}:
        return {"poly": rect_poly(area["rect"])}
    if set(area) <= {"ellipse"}:
        return {"poly": ellipse_poly(area["ellipse"])}
    out = from_mask(to_mask(area, page, episode))
    if out is None:
        raise AreaError("the area is empty")
    return out


def combine(current: dict | None, new: dict, how: str, page, episode=None) -> dict | None:
    """A selection drawn with Shift (add), Alt (take away) or both (keep the overlap)."""
    if current is None or how == "replace":
        return resolve(new, page, episode) if needs_resolving(new) else new
    key = {"add": "union", "subtract": "subtract", "intersect": "intersect"}[how]
    mask = to_mask({key: [current, new]}, page, episode)
    return from_mask(mask)


def outline(area: dict, page, episode=None) -> list[list[float]]:
    """A rough outline of an area for the screen (its bounding box when it is a mask)."""
    if area.get("poly"):
        return [list(p) for p in area["poly"]]
    x, y, w, h = area["mask"]["box"]
    return [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]


def stroke_area(points, width_mm: float) -> dict | None:
    """The area a selection pen stroke covers."""
    if not points:
        return None
    xs, ys = [float(p[0]) for p in points], [float(p[1]) for p in points]
    pad = width_mm
    x0, y0 = min(xs) - pad, min(ys) - pad
    w, h = max(xs) - min(xs) + 2 * pad, max(ys) - min(ys) + 2 * pad
    size = (max(1, round(_px(w))), max(1, round(_px(h))))
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    pts = [(_px(x - x0), _px(y - y0)) for x, y in zip(xs, ys)]
    r = max(1.0, _px(width_mm) / 2)
    if len(pts) > 1:
        draw.line(pts, fill=255, width=max(1, round(2 * r)), joint="curve")
    for x, y in (pts[0], pts[-1]):
        draw.ellipse((x - r, y - r, x + r, y + r), fill=255)
    buf = io.BytesIO()
    mask.save(buf, format="PNG")
    return {"mask": {"box": [round(x0, 3), round(y0, 3), round(w, 3), round(h, 3)], "png": base64.b64encode(buf.getvalue()).decode("ascii")}}
