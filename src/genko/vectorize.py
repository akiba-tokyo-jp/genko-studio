"""Pixels into pen lines (ラスター → ベクター): the marks are thinned to their middle lines, which are
followed into polylines, simplified, and given the marks' width and colour."""

from __future__ import annotations

import math

from PIL import Image, ImageFilter

TRACE_DPI = 150
MAX_DEPTH = 24


def _thin(on):
    """Zhang–Suen thinning of a bool array (numpy), one pixel wide lines left."""
    import numpy as np

    img = np.pad(on.astype(np.uint8), 1)
    while True:
        changed = False
        for step in (0, 1):
            p2, p3, p4 = img[:-2, 1:-1], img[:-2, 2:], img[1:-1, 2:]
            p5, p6, p7 = img[2:, 2:], img[2:, 1:-1], img[2:, :-2]
            p8, p9 = img[1:-1, :-2], img[:-2, :-2]
            ring = [p2, p3, p4, p5, p6, p7, p8, p9, p2]
            b = sum(ring[:8])
            a = sum(((ring[k] == 0) & (ring[k + 1] == 1)).astype(np.uint8) for k in range(8))
            if step == 0:
                c1, c2 = p2 * p4 * p6, p4 * p6 * p8
            else:
                c1, c2 = p2 * p4 * p8, p2 * p6 * p8
            core = img[1:-1, 1:-1]
            gone = (core == 1) & (b >= 2) & (b <= 6) & (a == 1) & (c1 == 0) & (c2 == 0)
            if gone.any():
                core[gone] = 0
                changed = True
        if not changed:
            return img[1:-1, 1:-1].astype(bool)


def _depth(alpha: Image.Image):
    """How deep inside the marks each pixel is (erosion steps, capped)."""
    import numpy as np

    depth = np.zeros((alpha.height, alpha.width), dtype=np.float32)
    current = alpha
    for _ in range(MAX_DEPTH):
        on = np.asarray(current) > 127
        if not on.any():
            break
        depth += on
        current = current.filter(ImageFilter.MinFilter(3))
    return depth


def _simplify(points: list, epsilon: float) -> list:
    if len(points) < 3:
        return points
    (x0, y0), (x1, y1) = points[0], points[-1]
    length = math.hypot(x1 - x0, y1 - y0)
    far, far_d = 0, -1.0
    for k in range(1, len(points) - 1):
        px, py = points[k]
        d = math.hypot(px - x0, py - y0) if length == 0 else abs((x1 - x0) * (y0 - py) - (x0 - px) * (y1 - y0)) / length
        if d > far_d:
            far, far_d = k, d
    if far_d <= epsilon:
        return [points[0], points[-1]]
    return _simplify(points[:far + 1], epsilon)[:-1] + _simplify(points[far:], epsilon)


def _chains(skeleton) -> list[list[tuple[int, int]]]:
    """The skeleton's pixels followed into chains, from the ends first, then the loops."""
    import numpy as np

    ys, xs = np.nonzero(skeleton)
    left = set(zip(ys.tolist(), xs.tolist()))
    steps = [(-1, 0), (0, 1), (1, 0), (0, -1), (-1, 1), (1, 1), (1, -1), (-1, -1)]

    def near(p):
        return [(p[0] + dy, p[1] + dx) for dy, dx in steps if (p[0] + dy, p[1] + dx) in left]

    out = []
    ends = [p for p in left if len(near(p)) == 1]
    for start in ends + sorted(left):
        if start not in left:
            continue
        chain = [start]
        left.discard(start)
        p = start
        while True:
            ahead = near(p)
            if not ahead:
                break
            p = ahead[0]
            left.discard(p)
            chain.append(p)
        out.append(chain)
    return out


def trace_layer(picture: Image.Image, dpi: int, min_mm: float = 0.8) -> list:
    """Pen lines (Stroke) for the marks of an RGBA picture drawn at dpi."""
    import numpy as np

    from genko.models import Stroke, new_id

    if picture.width * picture.height == 0:
        return []
    factor = TRACE_DPI / dpi
    small = picture.convert("RGBA").resize((max(1, round(picture.width * factor)), max(1, round(picture.height * factor))),
                                           Image.Resampling.BILINEAR) if factor < 1 else picture.convert("RGBA")
    scale = (TRACE_DPI if factor < 1 else dpi) / 25.4
    box = small.split()[3].point(lambda v: 255 if v > 90 else 0).getbbox()
    if not box:
        return []
    x0, y0 = max(0, box[0] - 2), max(0, box[1] - 2)
    crop = small.crop((x0, y0, min(small.width, box[2] + 2), min(small.height, box[3] + 2)))
    alpha = crop.split()[3].point(lambda v: 255 if v > 90 else 0)
    depth = _depth(alpha)
    skeleton = _thin(np.asarray(alpha) > 127)
    colours = np.asarray(crop.convert("RGB"), dtype=np.float32)
    strokes = []
    for chain in _chains(skeleton):
        if len(chain) * 1.0 / scale < min_mm and len(chain) < 3:
            continue
        pts = [((x + x0 + 0.5) / scale, (y + y0 + 0.5) / scale) for y, x in chain]
        length = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
        if length < min_mm:
            continue
        ys = np.array([p[0] for p in chain])
        xs = np.array([p[1] for p in chain])
        width = max(0.1, float(np.median(depth[ys, xs])) * 2 / scale)
        rgb = tuple(int(v) for v in colours[ys, xs].mean(axis=0).round())
        simple = _simplify(pts, 0.4 / scale)
        strokes.append(Stroke(id=new_id(), points=[(round(x, 3), round(y, 3)) for x, y in simple],
                              pressure=[1.0] * len(simple), width_mm=round(width, 3), kind="mili", rgb=rgb))
    return strokes
