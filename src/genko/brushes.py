"""Pen and brush kinds: how a vector line is drawn, and the presets people pick.

Every line keeps its points (with pressure) and its `kind`; the look is made at render time, at the
render's resolution, so a line can be redrawn in another kind and prints sharp at 600 dpi.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from genko.stroke import draw_stroke_mm


@dataclass(frozen=True)
class Brush:
    key: str
    label: str
    width_mm: float  # the default size
    min_pressure: float = 0.15  # how thin a light touch gets (share of the width)
    gamma: float = 1.0  # pressure curve: >1 needs more force for a thick line
    opacity: float = 1.0
    stabilize: int = 3
    taper: bool = True
    texture: str = ""  # "" | grain (pencil) | soft (airbrush) | dry (brush)
    rgb: tuple[int, int, int] | None = None  # a fixed colour (white)
    fixed_width: bool = False  # ignores pressure (technical pens, markers)


BRUSHES: dict[str, Brush] = {b.key: b for b in (
    Brush("gpen", "G ペン", 0.5, min_pressure=0.1, gamma=1.4, stabilize=3, taper=True),
    Brush("maru", "丸ペン", 0.25, min_pressure=0.2, gamma=1.2, stabilize=3, taper=True),
    Brush("kabura", "かぶらペン", 0.6, min_pressure=0.35, gamma=1.0, stabilize=2, taper=True),
    Brush("mili", "ミリペン", 0.3, fixed_width=True, stabilize=4, taper=False),
    Brush("pencil", "鉛筆", 0.5, min_pressure=0.4, gamma=1.0, opacity=0.85, stabilize=1, taper=False, texture="grain"),
    Brush("fude", "筆", 1.6, min_pressure=0.05, gamma=1.8, stabilize=2, taper=True, texture="dry"),
    Brush("marker", "マーカー", 1.5, fixed_width=True, opacity=0.6, stabilize=2, taper=False),
    Brush("airbrush", "エアブラシ", 8.0, min_pressure=0.5, opacity=0.5, stabilize=1, taper=False, texture="soft"),
    Brush("fill_pen", "ベタ塗りペン", 3.0, fixed_width=True, stabilize=1, taper=False),
    Brush("white", "ホワイト（修正）", 1.0, min_pressure=0.3, stabilize=2, taper=False, rgb=(255, 255, 255)),
    Brush("fx", "効果線ペン", 0.5, min_pressure=0.0, gamma=1.0, stabilize=0, taper=False),
)}
DEFAULT = "gpen"
LEGACY = {"oil": "marker"}


def brush(key: str | None) -> Brush:
    key = LEGACY.get(key or "", key or DEFAULT)
    return BRUSHES.get(key, BRUSHES[DEFAULT])


def _pressured(points: list, b: Brush) -> list:
    out = []
    for pt in points:
        if b.fixed_width or len(pt) < 3:
            out.append((pt[0], pt[1], 1.0))
            continue
        p = max(0.0, min(1.0, float(pt[2]))) ** b.gamma
        out.append((pt[0], pt[1], b.min_pressure + (1 - b.min_pressure) * p))
    return out


def draw(size: tuple[int, int], points: list, dpi: int, width_mm: float, kind: str | None, seed: str = ""):
    """The line's coverage at this resolution, only around the line: (L image, (x0, y0)) or None."""
    b = brush(kind)
    pts = _pressured(points, b)
    if not pts:
        return None
    scale = dpi / 25.4
    pad = width_mm * scale * (1.5 if b.texture == "soft" else 0.75) + 3
    xs = [p[0] * scale for p in pts]
    ys = [p[1] * scale for p in pts]
    x0, y0 = max(0, int(min(xs) - pad)), max(0, int(min(ys) - pad))
    x1, y1 = min(size[0], int(max(xs) + pad) + 1), min(size[1], int(max(ys) + pad) + 1)
    if x1 <= x0 or y1 <= y0:
        return None
    shift = [(p[0] - x0 / scale, p[1] - y0 / scale, p[2]) for p in pts]
    mask = Image.new("L", (x1 - x0, y1 - y0), 0)
    if b.texture == "soft":
        # an airbrush: a wide soft spray (the width is its diameter)
        draw_stroke_mm(ImageDraw.Draw(mask), shift, dpi, width_mm * 0.5, 255)
        return mask.filter(ImageFilter.GaussianBlur(max(1.0, width_mm * scale / 3))), (x0, y0)
    draw_stroke_mm(ImageDraw.Draw(mask), shift, dpi, width_mm, 255, floor=0.03 if b.min_pressure < 0.05 else 0.15)
    if b.texture in ("grain", "dry"):
        rng = random.Random(seed or "genko")
        grain_px = max(1, round(dpi / 150)) if b.texture == "grain" else max(1, round(dpi / 60))
        small = Image.new("L", (max(1, mask.width // grain_px), max(1, mask.height // grain_px)))
        keep = 0.72 if b.texture == "grain" else 0.9
        small.putdata([255 if rng.random() < keep else 70 for _ in range(small.width * small.height)])
        mask = ImageChops.multiply(mask, small.resize(mask.size, Image.Resampling.NEAREST))
    return mask, (x0, y0)
