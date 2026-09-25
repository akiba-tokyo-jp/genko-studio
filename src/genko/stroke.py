from __future__ import annotations

from PIL import ImageDraw


def _mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def stabilize_points(points: list, window: int = 5) -> list:
    if window < 3 or len(points) < 3:
        return points
    half = max(1, int(window) // 2)
    out: list = []
    n = len(points)
    for i, point in enumerate(points):
        k = min(half, i, n - 1 - i)  # a window that shrinks evenly at the ends keeps them where they were drawn
        sl = points[i - k : i + k + 1]
        x = sum(float(item[0]) for item in sl) / len(sl)
        y = sum(float(item[1]) for item in sl) / len(sl)
        extra = list(point[2:]) if len(point) > 2 else []
        out.append([x, y, *extra] if extra else [x, y])
    return out


def taper_points(points: list) -> list:
    n = len(points)
    if n < 2:
        return points
    span = max(1, n * 0.25)
    out: list = []
    for i, point in enumerate(points):
        factor = min(1.0, min(i, n - 1 - i) / span)
        pressure = float(point[2]) if len(point) > 2 else 1.0
        out.append([float(point[0]), float(point[1]), pressure * max(0.15, factor)])
    return out


def apply_pressure_curve(points: list, curve: str = "gpen") -> list:
    if curve in ("", "linear"):
        return points
    out: list = []
    for point in points:
        if len(point) < 3:
            out.append(point)
            continue
        pressure = max(0.05, min(1.0, float(point[2]) ** 1.8))
        out.append([float(point[0]), float(point[1]), pressure])
    return out


def pack_point(x_mm: float, y_mm: float, pressure: float | None = None, tilt: float = 0.0) -> list[float]:
    value = 0.7 if pressure is None else max(0.05, min(1.0, float(pressure)))
    if tilt:
        value = max(0.05, min(1.0, value * (1.0 + 0.25 * float(tilt))))
    return [float(x_mm), float(y_mm), value]


def stamp_polyline(
    draw: ImageDraw.ImageDraw,
    points: list,
    dpi: int,
    width_mm: float,
    fill,
    coords: str = "px",
) -> None:
    if len(points) < 2:
        return
    for a, b in zip(points, points[1:]):
        pressure = float(a[2]) if len(a) > 2 else 1.0
        radius = max(1, round(_mm_to_px(width_mm * max(0.15, pressure), dpi) / 2))
        ax, ay = float(a[0]), float(a[1])
        bx, by = float(b[0]), float(b[1])
        if coords == "mm":
            ax, ay = _mm_to_px(ax, dpi), _mm_to_px(ay, dpi)
            bx, by = _mm_to_px(bx, dpi), _mm_to_px(by, dpi)
        distance = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
        steps = max(1, int(distance))
        for i in range(steps + 1):
            t = i / steps
            x = ax + (bx - ax) * t
            y = ay + (by - ay) * t
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=fill)


def draw_stroke_mm(draw: ImageDraw.ImageDraw, points: list, dpi: int, width_mm: float, fill, pressure_scale: bool = True,
                   floor: float = 0.15) -> None:
    """A pen line from its vector points (mm, optional pressure) at any resolution.

    Each segment is a quad between two round caps whose radii follow the pressure, so the line is
    smooth at 600 dpi and cheap at screen size (no per-pixel stamping).
    """
    import math

    if not points:
        return
    scale = dpi / 25.4
    pts = []
    for pt in points:
        pressure = float(pt[2]) if len(pt) > 2 and pressure_scale else 1.0
        radius = max(0.5, width_mm * max(floor, min(1.5, pressure)) * scale / 2)
        pts.append((float(pt[0]) * scale, float(pt[1]) * scale, radius))
    if len(pts) == 1:
        x, y, r = pts[0]
        draw.ellipse((x - r, y - r, x + r, y + r), fill=fill)
        return
    for (ax, ay, ar), (bx, by, br) in zip(pts, pts[1:]):
        dx, dy = bx - ax, by - ay
        length = math.hypot(dx, dy)
        if length > 1e-6:
            nx, ny = -dy / length, dx / length
            draw.polygon([(ax + nx * ar, ay + ny * ar), (bx + nx * br, by + ny * br),
                          (bx - nx * br, by - ny * br), (ax - nx * ar, ay - ny * ar)], fill=fill)
        draw.ellipse((bx - br, by - br, bx + br, by + br), fill=fill)
    x, y, r = pts[0]
    draw.ellipse((x - r, y - r, x + r, y + r), fill=fill)


def split_by_eraser(points: list, eraser: list, radius_mm: float) -> list[list]:
    """The pieces of a line that survive an eraser path (vector erase: the line is cut, not painted over)."""
    import math

    def near(p) -> bool:
        px, py = float(p[0]), float(p[1])
        for (ax, ay, *_), (bx, by, *_) in zip(eraser, eraser[1:] or eraser):
            dx, dy = bx - ax, by - ay
            seg = dx * dx + dy * dy
            t = 0.0 if seg == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / seg))
            if math.hypot(px - (ax + t * dx), py - (ay + t * dy)) <= radius_mm:
                return True
        return False

    # densify so a short eraser still cuts a long segment
    dense: list = []
    for a, b in zip(points, points[1:]):
        dist = math.hypot(float(b[0]) - float(a[0]), float(b[1]) - float(a[1]))
        steps = max(1, int(dist / max(0.2, radius_mm / 2)))
        for i in range(steps):
            t = i / steps
            q = [float(a[0]) + (float(b[0]) - float(a[0])) * t, float(a[1]) + (float(b[1]) - float(a[1])) * t]
            if len(a) > 2:
                q.append(float(a[2]) + ((float(b[2]) if len(b) > 2 else float(a[2])) - float(a[2])) * t)
            dense.append(q)
    if points:
        dense.append([float(v) for v in points[-1]])
    pieces: list[list] = []
    current: list = []
    for p in dense:
        if near(p):
            if len(current) >= 2:
                pieces.append(current)
            current = []
        else:
            current.append(p)
    if len(current) >= 2:
        pieces.append(current)
    return pieces


def _seg_cross(a, b, c, d):
    """The parameter t along a→b where it crosses c→d, or None."""
    rx, ry = b[0] - a[0], b[1] - a[1]
    sx, sy = d[0] - c[0], d[1] - c[1]
    den = rx * sy - ry * sx
    if abs(den) < 1e-12:
        return None
    t = ((c[0] - a[0]) * sy - (c[1] - a[1]) * sx) / den
    u = ((c[0] - a[0]) * ry - (c[1] - a[1]) * rx) / den
    return t if 0 <= t <= 1 and 0 <= u <= 1 else None


def erase_to_crossing(strokes: list, eraser: list, radius_mm: float) -> list:
    """Erase the part of each touched line between the crossings (with other lines) around the touch —
    the usual way to clean the overshoot where lines cross (交点まで消す)."""
    import math

    from genko.models import coerce_stroke

    def arc_positions(points):
        out, total = [0.0], 0.0
        for a, b in zip(points, points[1:]):
            total += math.dist(a[:2], b[:2])
            out.append(total)
        return out

    # the eraser's path, walked in small steps (a quick drag leaves far-apart points)
    step = max(0.05, radius_mm / 2)
    walked = [tuple(eraser[0][:2])] if eraser else []
    for a, b in zip(eraser, eraser[1:]):
        n = max(1, math.ceil(math.dist(a[:2], b[:2]) / step))
        walked.extend((a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n) for k in range(1, n + 1))
    eraser = walked

    result = []
    for stroke in strokes:
        pts = [tuple(p) for p in stroke.points]
        if len(pts) < 2:
            result.append(stroke)
            continue
        pos = arc_positions(pts)
        # where the eraser touches this line (arc length)
        touch = None
        for i, (a, b) in enumerate(zip(pts, pts[1:])):
            for e in eraser:
                dx, dy = b[0] - a[0], b[1] - a[1]
                seg = dx * dx + dy * dy
                t = 0.0 if seg == 0 else max(0.0, min(1.0, ((e[0] - a[0]) * dx + (e[1] - a[1]) * dy) / seg))
                if math.hypot(e[0] - (a[0] + t * dx), e[1] - (a[1] + t * dy)) <= radius_mm:
                    touch = pos[i] + t * (pos[i + 1] - pos[i])
                    break
            if touch is not None:
                break
        if touch is None:
            result.append(stroke)
            continue
        crossings = []
        for other in strokes:
            if other is stroke:
                continue
            ops = [tuple(p) for p in other.points]
            for i, (a, b) in enumerate(zip(pts, pts[1:])):
                for c, d in zip(ops, ops[1:]):
                    t = _seg_cross(a, b, c, d)
                    if t is not None:
                        crossings.append(pos[i] + t * (pos[i + 1] - pos[i]))
        before = max([c for c in crossings if c < touch], default=0.0)
        after = min([c for c in crossings if c > touch], default=pos[-1])

        def piece(lo: float, hi: float):
            out = []
            for i, (a, b) in enumerate(zip(pts, pts[1:])):
                la, lb = pos[i], pos[i + 1]
                if lb < lo or la > hi or lb == la:
                    continue
                t0, t1 = max(0.0, (lo - la) / (lb - la)), min(1.0, (hi - la) / (lb - la))
                p0 = (a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0)
                p1 = (a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1)
                if not out:
                    out.append(p0)
                out.append(p1)
            return out

        for lo, hi in ((0.0, before), (after, pos[-1])):
            part = piece(lo, hi)
            if len(part) >= 2 and math.dist(part[0], part[-1]) > 0.2:
                new = coerce_stroke(part)
                new.width_mm, new.kind, new.rgb, new.opacity = stroke.width_mm, stroke.kind, stroke.rgb, stroke.opacity
                result.append(new)
    return result
