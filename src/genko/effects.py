"""Effect lines (効果線), drawn from a few settings: the same effect always gives the same lines.

Kinds and their settings (`params`, page mm; everything is optional):

- focus (集中線): `center` [x, y], `inner` [rx, ry] (the clear middle), `count`, `jitter` (0..1, how
  unevenly the lines stop), `width_mm`, `taper` (thin toward the middle).
- speed (流線): `angle` (degrees, the direction of motion), `count`, `length` (share of the panel, 0..1),
  `jitter`, `width_mm`, `curve` (mm the lines bow by), `taper`.
- uni_flash (ウニフラッシュ): `center`, `inner` [rx, ry], `count`, `length_mm`, `jitter`, `width_mm`.
- beta_flash (ベタフラッシュ): `center`, `inner` [rx, ry], `spikes`, `depth` (0..1, how far the white
  spikes reach into the black), `jitter`.
- white: the panel in plain white.

`rgb` sets the colour (black by default; white lines over black work too). Effects stay inside their
panel (or the page) and are kept as settings, so they can be changed later, or turned into pen
lines and fills on a layer to finish by hand.
"""

from __future__ import annotations

import math
import random

KINDS = ("focus", "speed", "uni_flash", "beta_flash", "white")
LABELS = {"focus": "集中線", "speed": "流線", "uni_flash": "ウニフラッシュ", "beta_flash": "ベタフラッシュ", "white": "白で塗る"}
INK = (15, 15, 15)


def validate(kind: str, params: dict) -> None:
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {', '.join(KINDS)}")
    if int(params.get("count", 1) or 1) > 2000 or int(params.get("spikes", 1) or 1) > 2000:
        raise ValueError("too many lines (at most 2000)")
    for key in ("jitter", "depth", "length"):
        if key in params and not 0 <= float(params[key]) <= 1:
            raise ValueError(f"{key} is 0 to 1")


def area(effect: dict, page) -> tuple[list[tuple[float, float]], tuple[float, float, float, float]]:
    """The effect's panel outline (mm) and its box."""
    from genko import frames as geo

    frame = None
    if effect.get("frame_id"):
        try:
            frame = page._find(effect["frame_id"])
        except (KeyError, IndexError):
            frame = None
    if frame is not None:
        outline = [(float(x), float(y)) for x, y in geo.shape(frame)]
    else:
        w, h = page.spec.width_mm, page.spec.height_mm
        outline = [(0.0, 0.0), (w, 0.0), (w, h), (0.0, h)]
    xs, ys = [p[0] for p in outline], [p[1] for p in outline]
    return outline, (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))


def _centre(params: dict, box) -> tuple[float, float]:
    x, y, w, h = box
    c = params.get("center")
    return (float(c[0]), float(c[1])) if c else (x + w / 2, y + h / 2)


def _inner(params: dict, box) -> tuple[float, float]:
    x, y, w, h = box
    if params.get("inner"):
        rx, ry = params["inner"]
        return max(0.5, float(rx)), max(0.5, float(ry))
    share = float(params.get("clear", 0.4))  # old books: the clear middle as a share of the panel
    return max(0.5, w / 2 * share), max(0.5, h / 2 * share)


def _line(a, b, taper: str, n: int = 6) -> list[list[float]]:
    """Points from a to b with pressures for the taper: "in" (thin at b), "both", or "" (even)."""
    out = []
    for i in range(n):
        t = i / (n - 1)
        if taper == "in":
            p = 1 - t * 0.97
        elif taper == "both":
            p = 0.03 + 0.97 * math.sin(math.pi * t)
        else:
            p = 1.0
        out.append([round(a[0] + (b[0] - a[0]) * t, 3), round(a[1] + (b[1] - a[1]) * t, 3), round(p, 3)])
    return out


def geometry(effect: dict, page) -> dict:
    """{"lines": [{"points", "width_mm"}], "fills": [{"points", "rgb"}], "rgb", "outline"} in page mm."""
    kind = effect.get("kind")
    params = dict(effect.get("params") or {})
    outline, box = area(effect, page)
    x, y, w, h = box
    rng = random.Random(str(params.get("seed", effect.get("id"))))
    rgb = tuple(int(v) for v in params.get("rgb") or INK)
    lines: list[dict] = []
    fills: list[dict] = []
    jitter = float(params.get("jitter", 0.25))
    if kind == "focus":
        cx, cy = _centre(params, box)
        rx, ry = _inner(params, box)
        count = int(params.get("count", 90))
        width = float(params.get("width_mm", 0.8))
        outer = math.hypot(w, h) + math.hypot(cx - (x + w / 2), cy - (y + h / 2))
        taper = "in" if params.get("taper", True) else ""
        for i in range(count):
            a = 2 * math.pi * (i + rng.random() * 0.7) / count
            stop = 1 + jitter * rng.random() * 1.2
            inner_pt = (cx + rx * stop * math.cos(a), cy + ry * stop * math.sin(a))
            outer_pt = (cx + outer * math.cos(a), cy + outer * math.sin(a))
            lines.append({"points": _line(outer_pt, inner_pt, taper), "width_mm": width * (0.6 + rng.random() * 0.8)})
    elif kind == "speed":
        angle = math.radians(float(params.get("angle", 0)))
        d = (math.cos(angle), math.sin(angle))
        n = (-d[1], d[0])
        count = int(params.get("count", 40))
        width = float(params.get("width_mm", 0.5))
        share = float(params.get("length", 0.7))
        curve = float(params.get("curve", 0))
        cx, cy = x + w / 2, y + h / 2
        corners = [(px - cx, py - cy) for px, py in outline]
        across = [c[0] * n[0] + c[1] * n[1] for c in corners]
        along = [c[0] * d[0] + c[1] * d[1] for c in corners]
        lo, hi = min(across), max(across)
        a0, a1 = min(along), max(along)
        span = a1 - a0
        taper = str(params.get("taper", "both")) if params.get("taper", True) is not False else ""
        taper = "both" if taper in ("True", "both") else taper
        for i in range(count):
            offset = lo + (hi - lo) * (i + rng.random()) / count
            length = span * share * (1 - jitter * rng.random() * 0.8)
            start = a0 - span * 0.05 + (span * 1.1 - length) * rng.random()
            pts = []
            steps = 24 if curve else 8
            for k in range(steps):
                t = k / (steps - 1)
                along_t = start + length * t
                bow = curve * 4 * t * (1 - t)
                px = cx + d[0] * along_t + n[0] * (offset + bow)
                py = cy + d[1] * along_t + n[1] * (offset + bow)
                if taper == "both":
                    p = 0.03 + 0.97 * math.sin(math.pi * t)
                elif taper == "in":
                    p = 1 - 0.97 * t
                else:
                    p = 1.0
                pts.append([round(px, 3), round(py, 3), round(p, 3)])
            lines.append({"points": pts, "width_mm": width * (0.5 + rng.random())})
    elif kind == "uni_flash":
        cx, cy = _centre(params, box)
        rx, ry = _inner(params, box)
        count = int(params.get("count", 140))
        width = float(params.get("width_mm", 0.35))
        length = float(params.get("length_mm", max(8.0, min(w, h) * 0.18)))
        for i in range(count):
            a = 2 * math.pi * (i + rng.random() * 0.8) / count
            start = 1 + jitter * (rng.random() - 0.5) * 0.3
            size = length * (1 - jitter * rng.random() * 0.7)
            p0 = (cx + rx * start * math.cos(a), cy + ry * start * math.sin(a))
            k = size / max(1e-6, math.hypot(rx * math.cos(a), ry * math.sin(a)))
            p1 = (p0[0] + rx * k * math.cos(a), p0[1] + ry * k * math.sin(a))
            lines.append({"points": _line(p0, p1, "both"), "width_mm": width * (0.7 + rng.random() * 0.6)})
    elif kind == "beta_flash":
        cx, cy = _centre(params, box)
        rx, ry = _inner(params, box)
        spikes = int(params.get("spikes", 70))
        depth = float(params.get("depth", 0.45))
        fills.append({"points": outline, "rgb": list(rgb)})
        star = []
        reach = math.hypot(w, h) / 2
        for i in range(spikes * 2):
            a = math.pi * i / spikes
            if i % 2:  # a white spike's tip, out in the black
                r = 1 + (reach / max(rx, ry) - 1) * depth * (0.55 + rng.random() * 0.9 * (0.5 + jitter))
            else:
                r = 1 + jitter * 0.15 * rng.random()
            star.append((round(cx + rx * r * math.cos(a), 3), round(cy + ry * r * math.sin(a), 3)))
        fills.append({"points": star, "rgb": [255, 255, 255]})
    elif kind == "white":
        fills.append({"points": outline, "rgb": [255, 255, 255]})
    return {"lines": lines, "fills": fills, "rgb": rgb, "outline": outline}


def draw(image, effect: dict, page, dpi: int):
    """The effect onto an RGB(A) page image, clipped to its panel."""
    from PIL import Image, ImageChops, ImageDraw

    from genko import brushes
    from genko.render import _xy

    geo = geometry(effect, page)
    layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw_ = ImageDraw.Draw(layer)
    for fill in geo["fills"]:
        draw_.polygon([_xy(p, dpi) for p in fill["points"]], fill=(*[int(v) for v in fill["rgb"]], 255))
    colour = Image.new("RGBA", image.size, (*geo["rgb"], 255))
    cover = Image.new("L", image.size, 0)
    for line in geo["lines"]:
        drawn = brushes.draw(image.size, line["points"], dpi, line["width_mm"], "fx")
        if drawn is None:
            continue
        mask, (x0, y0) = drawn
        region = cover.crop((x0, y0, x0 + mask.width, y0 + mask.height))
        cover.paste(ImageChops.lighter(region, mask), (x0, y0))
    layer = Image.composite(colour, layer, cover)
    clip = Image.new("L", image.size, 0)
    ImageDraw.Draw(clip).polygon([_xy(p, dpi) for p in geo["outline"]], fill=255)
    layer.putalpha(ImageChops.multiply(layer.split()[3], clip))
    return Image.alpha_composite(image.convert("RGBA"), layer).convert(image.mode)


def to_layer(effect: dict, page, layer) -> None:
    """Turn an effect into pen lines (効果線ペン) and fills on a layer, to finish by hand."""
    from genko.fill import polygon_patch
    from genko.models import coerce_stroke

    geo = geometry(effect, page)
    for fill in geo["fills"]:
        patch = polygon_patch(fill["points"], fill["rgb"])
        if patch:
            layer.patches.append(patch)
    for line in geo["lines"]:
        stroke = coerce_stroke(line["points"])
        stroke.kind = "fx"
        stroke.width_mm = round(float(line["width_mm"]), 3)
        stroke.rgb = tuple(geo["rgb"]) if tuple(geo["rgb"]) != INK else None
        layer.strokes.append(stroke)
