"""Synthetic hand-drawn names (atari) with their true panel rectangles, for M8 tests and D8 dry runs.

Pen lines wobble, break and overshoot at corners; panels hold stick figures and
vertical scribbles of dialogue; the paper has specks. The truth is the panel
rectangle the artist meant (the middle of the border lines), in page mm.
"""

from __future__ import annotations

import io
import math
import random

from PIL import Image, ImageDraw

PAGE_MM = (257.0, 364.0)   # B4 comic
LIVE_MM = (13.0, 13.0, 231.0, 338.0)  # its live area (bleed 3 + margin 10)

LAYOUTS = [
    # tiers of (height share, [column width shares right to left])
    [(0.3, [1.0]), (0.4, [0.55, 0.45]), (0.3, [1.0])],
    [(0.25, [0.5, 0.5]), (0.5, [1.0]), (0.25, [0.35, 0.3, 0.35])],
    [(0.35, [1.0]), (0.3, [0.4, 0.6]), (0.35, [0.6, 0.4])],
    [(1.0, [1.0])],
    [(0.2, [1.0]), (0.4, [0.5, 0.5]), (0.4, [0.3, 0.7])],
    [(0.5, [0.6, 0.4]), (0.5, [0.4, 0.6])],
]


def true_panels(layout, h_gutter=6.0, v_gutter=3.0) -> list[list[float]]:
    x, y, w, h = LIVE_MM
    rects = []
    usable_h = h - h_gutter * (len(layout) - 1)
    ty = y
    for share, cols in layout:
        th = usable_h * share
        usable_w = w - v_gutter * (len(cols) - 1)
        cx = x + w  # columns are listed right to left
        for cw in cols:
            width = usable_w * cw
            cx -= width
            rects.append([round(cx, 2), round(ty, 2), round(width, 2), round(th, 2)])
            cx -= v_gutter
        ty += th + h_gutter
    return rects


def _wobbly(draw, a, b, rng, px_per_mm, width, breaks=True):
    length = math.dist(a, b)
    steps = max(4, int(length / (3 * px_per_mm)))
    jitter = 0.35 * px_per_mm
    points = []
    for i in range(steps + 1):
        t = i / steps
        points.append((a[0] + (b[0] - a[0]) * t + rng.uniform(-jitter, jitter), a[1] + (b[1] - a[1]) * t + rng.uniform(-jitter, jitter)))
    segment = []
    for p in points:
        if breaks and rng.random() < 0.03:  # the pen skips
            if len(segment) > 1:
                draw.line(segment, fill=30, width=width)
            segment = []
            continue
        segment.append(p)
    if len(segment) > 1:
        draw.line(segment, fill=30, width=width)


def draw_page(layout, dpi: int = 150, seed: int = 0, margin_mm: float = 0.0) -> tuple[bytes, list[list[float]]]:
    """(PNG of the scan, true panel rects in mm). `margin_mm` adds extra paper around the page."""
    rng = random.Random(seed)
    px_per_mm = dpi / 25.4
    width_mm, height_mm = PAGE_MM[0] + 2 * margin_mm, PAGE_MM[1] + 2 * margin_mm
    image = Image.new("L", (round(width_mm * px_per_mm), round(height_mm * px_per_mm)), 238)
    draw = ImageDraw.Draw(image)

    def px(x_mm, y_mm):
        return ((x_mm + margin_mm) * px_per_mm, (y_mm + margin_mm) * px_per_mm)

    rects = true_panels(layout, h_gutter=rng.uniform(5, 8), v_gutter=rng.uniform(2.5, 4))
    pen = max(2, round(0.6 * px_per_mm))
    for x, y, w, h in rects:
        over = lambda: rng.uniform(-1.0, 1.2)  # noqa: E731 - corners overshoot or fall short
        corners = [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
        for (ax, ay), (bx, by) in zip(corners, corners[1:] + corners[:1]):
            if ax == bx:
                a, b = px(ax, ay - over() * (1 if by > ay else -1)), px(bx, by + over() * (1 if by > ay else -1))
            else:
                a, b = px(ax - over() * (1 if bx > ax else -1), ay), px(bx + over() * (1 if bx > ax else -1), by)
            _wobbly(draw, a, b, rng, px_per_mm, pen)
        # a stick figure
        if w > 30 and h > 40:
            cx, cy = x + w * rng.uniform(0.3, 0.7), y + h * rng.uniform(0.35, 0.55)
            r = min(w, h) * 0.08
            draw.ellipse([*px(cx - r, cy - r), *px(cx + r, cy + r)], outline=60, width=max(1, pen - 1))
            _wobbly(draw, px(cx, cy + r), px(cx, cy + h * 0.3), rng, px_per_mm, max(1, pen - 1), breaks=False)
        # vertical scribbles of dialogue near the top right
        for k in range(rng.randint(1, 3)):
            sx = x + w - 6 - k * 5
            if sx < x + 4:
                break
            top = y + 5
            for c in range(rng.randint(3, 7)):
                cy0 = top + c * 4.5
                if cy0 + 3 > y + h - 4:
                    break
                _wobbly(draw, px(sx - 1.5, cy0), px(sx + 1.5, cy0 + 3), rng, px_per_mm, max(1, pen - 1), breaks=False)
    for _ in range(80):  # specks and eraser crumbs
        sx, sy = rng.uniform(0, width_mm), rng.uniform(0, height_mm)
        r = rng.uniform(0.1, 0.35) * px_per_mm
        cx, cy = sx * px_per_mm, sy * px_per_mm
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=rng.randint(60, 160))
    buf = io.BytesIO()
    image.convert("RGB").save(buf, format="PNG")
    return buf.getvalue(), rects
