"""Monochrome finishing for print: levels, a line mask, flat tone steps and AM dots (Pillow only).

AM dots use a threshold array on an integer screen lattice: the screen vector
(m, n) is the cell edge in pixels at the requested angle, so the pattern
repeats on an (m²+n²)-pixel square tile. Pixels of the tile are ranked by their
distance to the nearest dot centre; a coverage c blackens the first c·N of
them. Dots grow round up to 50% and then turn into white holes on black, and
the black share equals the requested density at any dpi (±1/256).

`finish_gray` turns a greyscale image into print (pure black and white) or a
proof (the quantized greys without dots, to avoid moiré on screens).
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from functools import lru_cache

from PIL import Image, ImageChops

DEFAULT_STEPS = (0.1, 0.2, 0.3)


@dataclass(frozen=True)
class Finish:
    black: int = 40            # L below this is solid black (beta)
    white: int = 225           # L above this is paper white
    line_threshold: int = 110  # dark enough to be a line …
    line_contrast: int = 45    # … and darker than its surroundings by this much
    steps: tuple[float, ...] = DEFAULT_STEPS  # the flat tones used for the mid greys
    lpi: float = 60.0
    angle: float = 45.0
    screen: str = "am"         # am (dots) or fm (noise)

    @classmethod
    def from_dict(cls, data: dict | None) -> "Finish":
        if not data:
            return cls()
        known = {f for f in cls.__dataclass_fields__}
        values = {k: v for k, v in data.items() if k in known and v is not None}
        if "steps" in values:
            values["steps"] = tuple(sorted(float(s) for s in values["steps"] if 0 < float(s) < 1)) or DEFAULT_STEPS
        return cls(**values)


# --- the screen ------------------------------------------------------------------------


def screen_vector(dpi: int, lpi: float, angle: float) -> tuple[int, int]:
    cell = max(2.0, dpi / max(1.0, lpi))
    theta = math.radians(angle)
    m, n = round(cell * math.cos(theta)), round(cell * math.sin(theta))
    if m == 0 and n == 0:
        m = 2
    return m, n


def effective_lpi(dpi: int, lpi: float, angle: float) -> float:
    m, n = screen_vector(dpi, lpi, angle)
    return dpi / math.hypot(m, n)


@lru_cache(maxsize=32)
def threshold_tile(m: int, n: int) -> Image.Image:
    """L image of side m²+n²: each pixel's rank (0..255) by distance to its dot centre."""
    size = m * m + n * n
    norm = float(size)
    order = []
    for y in range(size):
        for x in range(size):
            # coordinates in the lattice basis v=(m,n), w=(-n,m); dot centres sit at cell centres
            u = (x * m + y * n) / norm
            v = (-x * n + y * m) / norm
            du = u - math.floor(u) - 0.5
            dv = v - math.floor(v) - 0.5
            dist = du * du + dv * dv
            # ties (the same spot in every cell) are broken by position so coverage stays exact
            order.append((round(dist, 9), (x * 7 + y * 13) % size, y, x))
    order.sort()
    total = len(order)
    data = bytearray(total)
    for rank, (_, _, y, x) in enumerate(order):
        data[y * size + x] = min(255, rank * 256 // total)
    return Image.frombytes("L", (size, size), bytes(data))


def tiled_threshold(size: tuple[int, int], dpi: int, lpi: float, angle: float, origin: tuple[int, int] = (0, 0)) -> Image.Image:
    """The threshold array over an area whose top-left is `origin` on the page (so screens line up across layers)."""
    tile = threshold_tile(*screen_vector(dpi, lpi, angle))
    t = tile.width
    ox, oy = origin[0] % t, origin[1] % t
    out = Image.new("L", size)
    for y in range(-oy, size[1], t):
        for x in range(-ox, size[0], t):
            out.paste(tile, (x, y))
    return out


def dots(coverage: float, size: tuple[int, int], dpi: int, lpi: float = 60.0, angle: float = 45.0,
         origin: tuple[int, int] = (0, 0)) -> Image.Image:
    """L image, 0 where ink goes, 255 elsewhere, with the given black share."""
    level = max(0, min(256, round(coverage * 256)))
    thresh = tiled_threshold(size, dpi, lpi, angle, origin)
    return thresh.point(lambda t, lv=level: 0 if t < lv else 255)


def noise(coverage: float, size: tuple[int, int]) -> Image.Image:
    """FM (error-diffused) tone with the given black share."""
    grey = Image.new("L", size, round(255 * (1 - coverage)))
    return grey.convert("1").convert("L")  # Floyd–Steinberg keeps the black share


def coverage(image: Image.Image) -> float:
    grey = image.convert("L")
    hist = grey.histogram()
    return sum(hist[:128]) / max(1, grey.width * grey.height)


# --- finishing ------------------------------------------------------------------------


def levels(grey: Image.Image, finish: Finish) -> Image.Image:
    lo, hi = finish.black, max(finish.black + 1, finish.white)
    return grey.point(lambda v: 0 if v <= lo else (255 if v >= hi else round((v - lo) * 255 / (hi - lo))))


def _max_filter(grey: Image.Image, size: int) -> Image.Image:
    """The brightest value in a size×size square around each pixel (as ImageFilter.MaxFilter), done as a row pass
    and a column pass: at 600 dpi a square filter is many times slower."""
    import numpy as np

    pixels = np.asarray(grey.convert("L"))
    r = size // 2
    height, width = pixels.shape
    padded = np.pad(pixels, ((0, 0), (r, r)), mode="edge")
    rows = padded[:, 0:width].copy()
    for k in range(1, 2 * r + 1):
        np.maximum(rows, padded[:, k:k + width], out=rows)
    padded = np.pad(rows, ((r, r), (0, 0)), mode="edge")
    out = padded[0:height].copy()
    for k in range(1, 2 * r + 1):
        np.maximum(out, padded[k:k + height], out=out)
    return Image.fromarray(out, "L")


def line_mask(grey: Image.Image, finish: Finish, dpi: int) -> Image.Image:
    """255 where a pixel is part of a drawn line: dark, and darker than its neighbourhood."""
    size = max(3, (round(dpi / 100) * 2 + 1))
    local_max = _max_filter(grey, size)
    contrast = ImageChops.subtract(local_max, grey)
    dark = grey.point(lambda v: 255 if v < finish.line_threshold else 0)
    strong = contrast.point(lambda v: 255 if v >= finish.line_contrast else 0)
    return ImageChops.multiply(dark, strong)


def quantize(grey: Image.Image, finish: Finish) -> tuple[Image.Image, list[tuple[float, Image.Image]]]:
    """(the proof grey, [(coverage, region mask)] for each tone step)."""
    steps = sorted(finish.steps)
    lo, hi = finish.black, finish.white
    # the mid greys (between solid black and paper) are split evenly among the steps, darkest to the densest step
    bands: list[tuple[float, int, int]] = []
    span = (hi - lo) / len(steps)
    for i, cov in enumerate(reversed(steps)):
        bands.append((cov, round(lo + i * span), round(lo + (i + 1) * span)))
    table = []
    for v in range(256):
        if v <= lo:
            table.append(0)
        elif v >= hi:
            table.append(255)
        else:
            cov = next((c for c, a, b in bands if a < v <= b), steps[0])
            table.append(round(255 * (1 - cov)))
    proof = grey.point(table)
    regions = []
    for cov, a, b in bands:
        regions.append((cov, grey.point(lambda v, a=a, b=b: 255 if a < v <= b else 0)))
    return proof, regions


def finish_gray(grey: Image.Image, finish: Finish, dpi: int, *, screen: bool, origin: tuple[int, int] = (0, 0)) -> Image.Image:
    """Greyscale art → print (screen=True: only 0 and 255) or proof (quantized greys)."""
    grey = grey.convert("L")
    lines = line_mask(grey, finish, dpi)
    proof, regions = quantize(grey, finish)
    if not screen:
        return ImageChops.darker(proof, ImageChops.invert(lines))
    out = proof.point(lambda v: 0 if v == 0 else 255)  # solid black and paper
    for cov, region in regions:
        tone = noise(cov, grey.size) if finish.screen == "fm" else dots(cov, grey.size, dpi, finish.lpi, finish.angle, origin)
        # where the region is on, take the tone
        out = Image.composite(tone, out, region)
    return ImageChops.darker(out, ImageChops.invert(lines))


def tone_area(mask: Image.Image, density: float, dpi: int, lpi: float = 60.0, angle: float = 45.0, fm: bool = False) -> Image.Image:
    """An RGBA layer of black tone (density = black share) inside `mask` (L, 255 = inside)."""
    size = mask.size
    tone = noise(density, size) if fm else dots(density, size, dpi, lpi, angle)
    alpha = ImageChops.multiply(ImageChops.invert(tone), mask)
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    layer.putalpha(alpha)
    return layer

