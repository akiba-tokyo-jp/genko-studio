"""Free transform: an area's corners pulled anywhere (遠近, perspective) or a 3×3 grid of points pulled to
bend it (メッシュ). Pen lines move point by point (long segments are split first so they bend with the
area); fills and pixels are redrawn through the same mapping, piece by piece.

A warp is {"perspective": [[x, y] × 4]} — where the top-left, top-right, bottom-right and bottom-left of
the area's box go — or {"mesh": [[x, y] × 9]} — where the 3×3 grid over the box goes, row by row (mm).
"""

from __future__ import annotations

import math

import numpy as np
from PIL import Image, ImageDraw

from genko.models import coerce_stroke


class WarpError(ValueError):
    pass


def _homography(src: list, dst: list) -> np.ndarray:
    rows = []
    for (x, y), (u, v) in zip(src, dst):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y, -u])
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y, -v])
    _, _, vt = np.linalg.svd(np.array(rows, dtype=float))
    h = vt[-1].reshape(3, 3)
    return h / h[2, 2]


def mapping(box: tuple[float, float, float, float], warp: dict):
    """The warp as a function (x, y) mm → (x', y') mm over the area's box (x, y, w, h)."""
    x0, y0, w, h = box
    if w <= 0 or h <= 0:
        raise WarpError("the area has no size")
    if warp.get("perspective"):
        corners = [(float(p[0]), float(p[1])) for p in warp["perspective"]]
        if len(corners) != 4:
            raise WarpError("perspective takes four corners: top-left, top-right, bottom-right, bottom-left")
        src = [(x0, y0), (x0 + w, y0), (x0 + w, y0 + h), (x0, y0 + h)]
        area = 0.0
        for (ax, ay), (bx, by) in zip(corners, corners[1:] + corners[:1]):
            area += ax * by - bx * ay
        if abs(area) < 1e-3:
            raise WarpError("the four corners must enclose an area")
        hm = _homography(src, corners)

        def go(x: float, y: float) -> tuple[float, float]:
            u, v, s = hm @ np.array([x, y, 1.0])
            return float(u / s), float(v / s)

        return go
    if warp.get("mesh"):
        grid = [(float(p[0]), float(p[1])) for p in warp["mesh"]]
        if len(grid) != 9:
            raise WarpError("mesh takes nine points, a 3×3 grid row by row")

        def go(x: float, y: float) -> tuple[float, float]:
            fx = min(2.0, max(0.0, (x - x0) / w * 2))
            fy = min(2.0, max(0.0, (y - y0) / h * 2))
            cx, cy = min(1, int(fx)), min(1, int(fy))
            tx, ty = fx - cx, fy - cy
            p00, p10 = grid[cy * 3 + cx], grid[cy * 3 + cx + 1]
            p01, p11 = grid[(cy + 1) * 3 + cx], grid[(cy + 1) * 3 + cx + 1]
            u = (1 - tx) * (1 - ty) * p00[0] + tx * (1 - ty) * p10[0] + (1 - tx) * ty * p01[0] + tx * ty * p11[0]
            v = (1 - tx) * (1 - ty) * p00[1] + tx * (1 - ty) * p10[1] + (1 - tx) * ty * p01[1] + tx * ty * p11[1]
            # beyond the box (a line that reaches out of it) the edge's pull carries on
            u += (x - x0 - fx * w / 2) if fx in (0.0, 2.0) else 0.0
            v += (y - y0 - fy * h / 2) if fy in (0.0, 2.0) else 0.0
            return u, v

        return go
    raise WarpError("a warp is perspective (four corners) or mesh (nine points)")


def _dense(points: list, step_mm: float = 1.5) -> list:
    out = [points[0]]
    for a, b in zip(points, points[1:]):
        n = max(1, int(math.dist(a[:2], b[:2]) / step_mm))
        for k in range(1, n + 1):
            t = k / n
            p = [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]
            if len(a) > 2 and len(b) > 2:
                p.append(a[2] + (b[2] - a[2]) * t)
            out.append(p)
    return out


def _scale_at(go, x: float, y: float) -> float:
    """How much the warp grows things around (x, y) (square root of the area change)."""
    d = 0.5
    a, b, c = go(x, y), go(x + d, y), go(x, y + d)
    det = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return math.sqrt(abs(det)) / d


def warp_stroke(stroke, go):
    pts = [(x, y, p) if p is not None else (x, y) for (x, y), p in
           zip(stroke.points, stroke.pressure or [None] * len(stroke.points))]
    dense = _dense([list(p) for p in pts]) if len(pts) > 1 else [list(p) for p in pts]
    out = coerce_stroke([(*go(p[0], p[1]), *p[2:]) for p in dense])
    mid = stroke.points[len(stroke.points) // 2]
    out.width_mm = round(stroke.width_mm * max(0.1, min(10.0, _scale_at(go, *mid))), 4)
    out.kind, out.rgb, out.opacity, out.id = stroke.kind, stroke.rgb, stroke.opacity, stroke.id
    return out


def _affine_back(dst: list, src: list) -> tuple:
    """PIL affine coefficients mapping each output pixel of triangle `dst` back to triangle `src`."""
    a = np.array([[dst[i][0], dst[i][1], 1.0] for i in range(3)])
    try:
        cx = np.linalg.solve(a, np.array([src[i][0] for i in range(3)]))
        cy = np.linalg.solve(a, np.array([src[i][1] for i in range(3)]))
    except np.linalg.LinAlgError:
        return None
    return (*cx, *cy)


def warp_image(image: Image.Image, origin: tuple[int, int], go, dpi: int, cells: int = 14, resample=Image.Resampling.BILINEAR) -> tuple[Image.Image, tuple[int, int]] | None:
    """An image lying at `origin` (px at dpi) redrawn through the warp: (image, its new origin) or None."""
    scale = dpi / 25.4
    ox, oy = origin

    def fwd(px: float, py: float) -> tuple[float, float]:
        u, v = go((px + ox) / scale, (py + oy) / scale)
        return u * scale, v * scale

    w, h = image.size
    grid = [[fwd(w * i / cells, h * j / cells) for i in range(cells + 1)] for j in range(cells + 1)]
    xs = [p[0] for row in grid for p in row]
    ys = [p[1] for row in grid for p in row]
    nx0, ny0 = math.floor(min(xs)), math.floor(min(ys))
    nx1, ny1 = math.ceil(max(xs)) + 1, math.ceil(max(ys)) + 1
    if nx1 - nx0 > 20000 or ny1 - ny0 > 20000:
        raise WarpError("the transform stretches the area too far")
    mode = image.mode
    out = Image.new(mode, (max(1, nx1 - nx0), max(1, ny1 - ny0)), 0 if mode == "L" else (0, 0, 0, 0))
    for j in range(cells):
        for i in range(cells):
            s00 = (w * i / cells, h * j / cells)
            s10 = (w * (i + 1) / cells, h * j / cells)
            s01 = (w * i / cells, h * (j + 1) / cells)
            s11 = (w * (i + 1) / cells, h * (j + 1) / cells)
            d00, d10 = grid[j][i], grid[j][i + 1]
            d01, d11 = grid[j + 1][i], grid[j + 1][i + 1]
            for src, dst in (((s00, s10, s11), (d00, d10, d11)), ((s00, s11, s01), (d00, d11, d01))):
                local = [(x - nx0, y - ny0) for x, y in dst]
                bx0 = max(0, math.floor(min(p[0] for p in local)) - 1)
                by0 = max(0, math.floor(min(p[1] for p in local)) - 1)
                bx1 = min(out.width, math.ceil(max(p[0] for p in local)) + 2)
                by1 = min(out.height, math.ceil(max(p[1] for p in local)) + 2)
                if bx1 <= bx0 or by1 <= by0:
                    continue
                coeffs = _affine_back([(x - bx0, y - by0) for x, y in local], list(src))
                if coeffs is None:
                    continue
                piece = image.transform((bx1 - bx0, by1 - by0), Image.Transform.AFFINE, coeffs, resample=resample)
                mask = Image.new("L", piece.size, 0)
                # (a little wider than the triangle, so neighbouring pieces leave no seam)
                ImageDraw.Draw(mask).polygon([(x - bx0, y - by0) for x, y in local], fill=255, outline=255)
                out.paste(piece, (bx0, by0), mask)
    return out, (nx0, ny0)
