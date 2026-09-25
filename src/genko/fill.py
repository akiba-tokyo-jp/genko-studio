"""Fills: find the region under a click (closing small gaps in the lines), or take a drawn area, and keep
it as a patch (a mask over its box, at FILL_DPI) that the renderer colours at any resolution.
"""

from __future__ import annotations

import io

import numpy as np
from PIL import Image, ImageDraw

from genko.models import new_id

FILL_DPI = 300


def px(mm: float, dpi: int = FILL_DPI) -> int:
    return round(mm / 25.4 * dpi)


def region(free: np.ndarray, seed: tuple[int, int]) -> np.ndarray:
    """Pixels connected to `seed` through `free` pixels (4-neighbour), by spans (fast in numpy)."""
    h, w = free.shape
    sx, sy = seed
    filled = np.zeros_like(free)
    if not (0 <= sx < w and 0 <= sy < h) or not free[sy, sx]:
        return filled
    stack = [(sy, sx)]
    while stack:
        y, x = stack.pop()
        if filled[y, x] or not free[y, x]:
            continue
        row = free[y]
        left = x
        blocked = np.flatnonzero(~row[:x])
        left = blocked[-1] + 1 if blocked.size else 0
        blocked = np.flatnonzero(~row[x:])
        right = x + blocked[0] - 1 if blocked.size else w - 1
        filled[y, left:right + 1] = True
        for ny in (y - 1, y + 1):
            if 0 <= ny < h:
                open_ = free[ny, left:right + 1] & ~filled[ny, left:right + 1]
                if not open_.any():
                    continue
                starts = np.flatnonzero(open_ & ~np.concatenate(([False], open_[:-1])))
                stack.extend((ny, left + int(s)) for s in starts)
    return filled


def dilate(arr: np.ndarray, r: int) -> np.ndarray:
    """Grow True pixels by r (a square), in two separable passes."""
    if r <= 0:
        return arr
    out = arr.copy()
    for axis in (0, 1):
        grown = out.copy()
        for k in range(1, r + 1):
            if axis == 0:
                grown[k:] |= out[:-k]
                grown[:-k] |= out[k:]
            else:
                grown[:, k:] |= out[:, :-k]
                grown[:, :-k] |= out[:, k:]
        out = grown
    return out


def region_mask(reference: Image.Image, at: tuple[int, int], gap_px: int = 0, threshold: int = 160,
                expand_px: int = 1, window: tuple[int, int, int, int] | None = None) -> Image.Image | None:
    """The fill region of a rendered reference (L) at pixel `at`: dark pixels are walls, gaps up to
    `gap_px` are closed, and the region grows `expand_px` under the lines around it. `window` (px box)
    limits the search (the clicked panel); the result is page-sized."""
    grey = reference.convert("L")
    x0, y0, x1, y1 = window or (0, 0, grey.width, grey.height)
    crop = np.asarray(grey.crop((x0, y0, x1, y1)))
    walls = crop < threshold
    # the window's edge is a wall too (a fill never leaves its panel's box)
    walls[0, :] = walls[-1, :] = True
    walls[:, 0] = walls[:, -1] = True
    free = ~dilate(walls, gap_px)
    filled = region(free, (at[0] - x0, at[1] - y0))
    if not filled.any():
        return None
    filled = dilate(filled, expand_px + gap_px)  # under the lines (and into the closed gaps)
    page = Image.new("L", grey.size, 0)
    page.paste(Image.fromarray((filled * 255).astype("uint8"), "L"), (x0, y0))
    return page


def mask_patch(mask: Image.Image, dpi: int, rgb, opacity: float = 1.0, offset_px: tuple[int, int] = (0, 0)) -> dict | None:
    """A patch dict from a page-aligned mask (cropped to what it covers)."""
    box = mask.getbbox()
    if box is None:
        return None
    crop = mask.crop(box)
    buf = io.BytesIO()
    crop.save(buf, format="PNG", optimize=True)
    x0, y0 = box[0] + offset_px[0], box[1] + offset_px[1]
    mm = 25.4 / dpi
    return {"id": new_id(), "box": [round(x0 * mm, 3), round(y0 * mm, 3), round(crop.width * mm, 3), round(crop.height * mm, 3)],
            "mode": "mask", "png": buf.getvalue(), "rgb": [int(v) for v in rgb], "opacity": float(opacity)}


def polygon_patch(points_mm: list, rgb, opacity: float = 1.0, dpi: int = FILL_DPI) -> dict | None:
    """A drawn area (囲って塗る, or a selection) filled in one colour."""
    if len(points_mm) < 3:
        return None
    xs, ys = [float(p[0]) for p in points_mm], [float(p[1]) for p in points_mm]
    x0, y0 = px(min(xs), dpi), px(min(ys), dpi)
    w, h = px(max(xs), dpi) - x0 + 2, px(max(ys), dpi) - y0 + 2
    if w < 2 or h < 2:
        return None
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).polygon([(px(x, dpi) - x0, px(y, dpi) - y0) for x, y in zip(xs, ys)], fill=255)
    return mask_patch(mask, dpi, rgb, opacity, (x0, y0))
