"""Tone layers: where the tone is (the mask) and how it looks (the pattern), drawn at any resolution.

Where: the layer's `region` (a polygon, old books), its patches (masks: an area, a panel, a fill) and
its pen lines in order — lines paint tone, `scrape` lines take it away (削り; `scrape_soft` fades it).
A tone layer with none of these covers every panel (as old books did).

How (`layer.tone`, with `layer.lpi`, `layer.density` (black share 0..1) and `layer.angle`):
{"pattern": dot | line | cross | noise | flat, "gradient": {"shape": linear | radial, "angle": deg,
"start": density, "end": density} | None}. Gradients run across the mask's box along `angle`
(linear) or from its middle outward (radial). Print renders show the pattern in pure black and
white; proofs and the screen show the grey it will read as.
"""

from __future__ import annotations

import io
import math

import numpy as np
from PIL import Image, ImageChops, ImageDraw

PATTERNS = ("dot", "line", "cross", "noise", "flat", "check", "brick", "wave", "grid", "hatch", "star", "sand", "image")
MOTIFS = ("check", "brick", "wave", "grid", "hatch", "star", "sand", "image")  # 柄トーン: a picture repeated, `scale_mm` apart
SHAPES = ("linear", "radial")


def settings(layer) -> dict:
    tone = dict(getattr(layer, "tone", None) or {})
    pattern = tone.get("pattern")
    if not pattern:
        pattern = "noise" if _legacy_noise(layer) else "dot"
    return {**{k: tone[k] for k in ("scale_mm", "tile_png", "seed") if tone.get(k) is not None},
            "pattern": pattern, "gradient": tone.get("gradient"), "lpi": float(layer.lpi or 60),
            "density": max(0.0, min(1.0, float(layer.density if layer.density is not None else 0.3))),
            "angle": float(layer.angle if layer.angle is not None else 45)}


def _legacy_noise(layer) -> bool:
    if not getattr(layer, "material_id", None):
        return False
    try:
        from genko.materials import get_material

        return get_material(str(layer.material_id)).get("kind") == "noise"
    except Exception:
        return False


def validate(tone: dict) -> None:
    if tone.get("pattern") and tone["pattern"] not in PATTERNS:
        raise ValueError(f"pattern must be one of {', '.join(PATTERNS)}")
    if tone.get("pattern") == "image":
        if not tone.get("tile_png") or _tile(tone["tile_png"]) is None:
            raise ValueError("an image tone needs its picture (tile_png, a base64 PNG)")
    if tone.get("scale_mm") is not None and not 0.3 <= float(tone["scale_mm"]) <= 50:
        raise ValueError("scale_mm is 0.3 to 50")
    gradient = tone.get("gradient")
    if gradient:
        if gradient.get("shape", "linear") not in SHAPES:
            raise ValueError("gradient shape must be linear or radial")
        for key in ("start", "end"):
            if not 0 <= float(gradient.get(key, 0)) <= 1:
                raise ValueError(f"gradient {key} is a density from 0 to 1")


# --- where -----------------------------------------------------------------------------------------------


def mask(layer, page, size: tuple[int, int], dpi: int) -> Image.Image:
    """Where the layer has tone (L, 255 = tone)."""
    from genko import brushes
    from genko.models import stroke_points
    from genko.render import _paint_patch, _xy, rect_px

    out = Image.new("L", size, 0)
    region = getattr(layer, "region", None)
    patches = [p for p in (getattr(layer, "patches", None) or []) if p.get("png")]
    strokes = getattr(layer, "strokes", None) or []
    if region:
        ImageDraw.Draw(out).polygon([_xy(pt, dpi) for pt in region], fill=255)
    if patches:
        rgba = Image.new("RGBA", size, (0, 0, 0, 0))
        for patch in patches:
            _paint_patch(rgba, {**patch, "rgb": [0, 0, 0], "opacity": 1.0}, dpi)
        out = ImageChops.lighter(out, rgba.split()[3])
    painted = any(not str(getattr(s, "kind", "")).startswith("scrape") for s in strokes)
    if not region and not patches and not painted:
        draw = ImageDraw.Draw(out)
        for frame in page.leaf_frames():
            draw.rectangle(rect_px(frame.rect, dpi), fill=255)
    for stroke in strokes:
        kind = str(getattr(stroke, "kind", "") or "")
        scrape = kind.startswith("scrape")
        drawn = brushes.draw(size, stroke_points(stroke), dpi, float(getattr(stroke, "width_mm", 1.0) or 1.0),
                             "airbrush" if kind == "scrape_soft" else ("mili" if scrape else kind), seed=str(getattr(stroke, "id", "")))
        if drawn is None:
            continue
        cover, (x0, y0) = drawn
        region_now = out.crop((x0, y0, x0 + cover.width, y0 + cover.height))
        if scrape:
            region_now = ImageChops.subtract(region_now, cover)
        else:
            region_now = ImageChops.lighter(region_now, cover)
        out.paste(region_now, (x0, y0))
    return out


# --- how -------------------------------------------------------------------------------------------------


def coverage_map(tone: dict, where: Image.Image) -> np.ndarray:
    """The black share at each pixel (float32, 0..1) over the mask's size."""
    h, w = where.height, where.width
    gradient = tone.get("gradient")
    if not gradient:
        return np.full((h, w), tone["density"], dtype=np.float32)
    box = where.getbbox() or (0, 0, w, h)
    x0, y0, x1, y1 = box
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    start, end = float(gradient.get("start", 0.0)), float(gradient.get("end", tone["density"]))
    if gradient.get("shape") == "radial":
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        radius = max(1.0, math.hypot(x1 - x0, y1 - y0) / 2)
        t = np.hypot(xs - cx, ys - cy) / radius
    else:
        a = math.radians(float(gradient.get("angle", 90)))
        dx, dy = math.cos(a), math.sin(a)
        corners = [(x0, y0), (x1, y0), (x0, y1), (x1, y1)]
        proj = [cx * dx + cy * dy for cx, cy in corners]
        lo, hi = min(proj), max(proj)
        t = (xs * dx + ys * dy - lo) / max(1.0, hi - lo)
    t = np.clip(t, 0.0, 1.0)
    return (start + (end - start) * t).astype(np.float32)


def _screen(pattern: str, size: tuple[int, int], dpi: int, lpi: float, angle: float) -> np.ndarray:
    """The threshold (0..1) a pixel's coverage must pass to be black."""
    w, h = size
    if pattern == "dot":
        from genko.screentone import tiled_threshold

        return np.asarray(tiled_threshold(size, dpi, lpi, angle), dtype=np.float32) / 256.0
    period = max(2.0, dpi / max(1.0, lpi))
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(angle)

    def lines(theta: float) -> np.ndarray:
        u = (xs * math.cos(theta) + ys * math.sin(theta)) / period
        return np.abs(u - np.floor(u) - 0.5) * 2  # 0 on a line's middle, 1 between lines

    if pattern == "line":
        return lines(a + math.pi / 2)
    return np.minimum(lines(a + math.pi / 2), lines(a))  # cross: either family


_TILES: dict = {}


def _tile(data: str):
    import base64

    key = hash(data)
    if key not in _TILES:
        try:
            grey = Image.open(io.BytesIO(base64.b64decode(data))).convert("LA")
            ink = ImageChops.multiply(ImageChops.invert(grey.getchannel(0)), grey.getchannel(1))
            _TILES[key] = ink
        except Exception:
            _TILES[key] = None
    return _TILES[key]


def _motif(tone: dict, size: tuple[int, int], dpi: int, cover: np.ndarray) -> np.ndarray:
    """柄トーン: 1 where a motif is black. The motif repeats every `scale_mm` (3 mm), turned by the angle;
    the density thickens its lines (check, brick, wave, grid, hatch) or grows its shapes (star, sand)."""
    w, h = size
    period = max(3.0, float(tone.get("scale_mm") or 3.0) / 25.4 * dpi)
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(float(tone.get("angle", 0) if tone.get("pattern") != "hatch" else tone.get("angle", 45)))
    u = (xs * math.cos(a) + ys * math.sin(a)) / period
    v = (-xs * math.sin(a) + ys * math.cos(a)) / period
    fu, fv = u - np.floor(u), v - np.floor(v)
    pattern = tone["pattern"]
    if pattern == "check":
        return ((np.floor(u) + np.floor(v)) % 2 == 0).astype(np.float32)
    if pattern == "grid":
        return ((fu < cover / 2) | (fv < cover / 2)).astype(np.float32)
    if pattern == "hatch":
        return (fu < cover).astype(np.float32)
    if pattern == "brick":
        row = np.floor(v * 2)
        shifted = (u + (row % 2) * 0.5) % 1.0
        return (((v * 2) % 1.0 < cover / 2) | (shifted < cover / 4)).astype(np.float32)
    if pattern == "wave":
        wave = (v + 0.18 * np.sin(u * 2 * math.pi)) % 1.0
        return (wave < cover).astype(np.float32)
    if pattern == "star":
        dx, dy = fu - 0.5, fv - 0.5
        r = np.hypot(dx, dy)
        theta = np.arctan2(dy, dx)
        edge = (0.18 + 0.5 * math.sqrt(max(0.01, float(cover.mean())))) * (0.55 + 0.45 * np.cos(5 * theta))
        return (r < edge * 0.5).astype(np.float32)
    if pattern == "sand":
        rng = np.random.default_rng(int(tone.get("seed", 7)))
        grain = max(1, round(period / 6))
        small = rng.random((h // grain + 1, w // grain + 1)).astype(np.float32)
        field = np.kron(small, np.ones((grain, grain), dtype=np.float32))[:h, :w]
        return (field < cover).astype(np.float32)
    tile = _tile(tone.get("tile_png") or "")
    if tile is None:
        return np.zeros((h, w), dtype=np.float32)
    side = max(2, round(period))
    scaled = np.asarray(tile.resize((side, max(2, round(side * tile.height / max(1, tile.width))))), dtype=np.float32) / 255.0
    th, tw = scaled.shape
    iu = (np.floor(fu * tw)).astype(np.int64) % tw
    iv = (np.floor(fv * th)).astype(np.int64) % th
    return (scaled[iv, iu] > 0.5).astype(np.float32)


def pattern_image(tone: dict, where: Image.Image, dpi: int, print_mode: bool) -> Image.Image:
    """An RGBA layer: the tone inside `where`."""
    cover = coverage_map(tone, where)
    pattern = tone["pattern"]
    alpha_in = np.asarray(where, dtype=np.float32) / 255.0
    if pattern in MOTIFS:  # (a pattern shows as itself on screen too)
        black = _motif(tone, where.size, dpi, cover)
    elif not print_mode or pattern == "flat":
        black = cover
    elif pattern == "noise":
        grey = Image.fromarray(np.clip(255 * (1 - cover), 0, 255).astype(np.uint8), "L")
        black = (np.asarray(grey.convert("1").convert("L")) < 128).astype(np.float32)
    else:
        share = cover
        if pattern == "cross":  # two families together reach the asked share
            share = 1 - np.sqrt(np.clip(1 - cover, 0, 1))
        black = (_screen(pattern, where.size, dpi, tone["lpi"], tone["angle"]) < share).astype(np.float32)
    alpha = np.clip(black * alpha_in * 255, 0, 255).astype(np.uint8)
    layer = Image.new("RGBA", where.size, (0, 0, 0, 0))
    layer.putalpha(Image.fromarray(alpha, "L"))
    return layer


def draw_layer(image: Image.Image, layer, page, dpi: int, print_mode: bool) -> Image.Image:
    where = mask(layer, page, image.size, dpi)
    if getattr(layer, "panel_clip", True):
        from genko.render import _clip_mask

        panels = _clip_mask(page, image.size, dpi)
        if panels is not None:
            where = ImageChops.multiply(where, panels)
    if where.getbbox() is None:
        return image
    tone = pattern_image(settings(layer), where, dpi, print_mode)
    opacity = float(getattr(layer, "opacity", 1.0) if getattr(layer, "opacity", None) is not None else 1.0)
    if opacity < 1:
        tone.putalpha(tone.split()[3].point(lambda v, o=opacity: round(v * o)))
    return Image.alpha_composite(image.convert("RGBA"), tone).convert(image.mode)


def screened(raster: Image.Image, spec: dict, dpi: int) -> Image.Image:
    """レイヤーのトーン化: a layer's greys as a halftone in pure black (dots, lines or noise at the lpi and angle);
    what is darker than `black` stays solid, lighter than `white` goes."""
    rgba = raster.convert("RGBA")
    grey = np.asarray(rgba.convert("L"), dtype=np.float32) / 255.0
    alpha = np.asarray(rgba.getchannel(3), dtype=np.float32) / 255.0
    black_at = float(spec.get("black", 0.1))
    white_at = float(spec.get("white", 0.95))
    cover = np.clip((white_at - grey) / max(0.01, white_at - black_at), 0, 1) * alpha
    pattern = str(spec.get("pattern") or "dot")
    lpi, angle = float(spec.get("lpi", 60)), float(spec.get("angle", 45))
    if pattern == "noise":
        img = Image.fromarray(np.clip(255 * (1 - cover), 0, 255).astype(np.uint8), "L")
        black = (np.asarray(img.convert("1").convert("L")) < 128).astype(np.float32)
    else:
        black = (_screen(pattern if pattern in ("dot", "line", "cross") else "dot", rgba.size, dpi, lpi, angle) < cover).astype(np.float32)
    out = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
    out.putalpha(Image.fromarray((black * 255).astype(np.uint8), "L"))
    return out


def swatch(tone: dict, size: tuple[int, int] = (96, 96), dpi: int = 150, print_mode: bool = True) -> Image.Image:
    """A small sample of a tone (for the materials panel)."""
    where = Image.new("L", size, 255)
    base = Image.new("RGB", size, (255, 255, 255))
    full = {"pattern": "dot", "gradient": None, "lpi": 60.0, "density": 0.3, "angle": 45.0, **tone}
    out = Image.alpha_composite(base.convert("RGBA"), pattern_image(full, where, dpi, print_mode))
    return out.convert("RGB")


def png_bytes(image: Image.Image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


