"""Rulers: guides a pen line snaps to, kept on the page (page mm).

Kinds (`kind`) and what they use:

- line: `points` [a, b] — lines drawn near it run along it (a straight ruler).
- curve: `points` (2 or more) — lines drawn near it run along the smooth curve through them.
- parallel: `angle` (degrees, 0 = across the page) — every line is straight at that angle.
- concentric: `points` [centre], `ratio` (height / width, 1 = circles), `angle` — lines follow the
  circle or ellipse through where they start.
- radial: `points` [centre] — lines run toward the centre (focus lines).
- perspective: `points` = 1 to 3 vanishing points — lines run toward the vanishing point that fits the
  stroke's direction (and level / upright for one and two points).
- guide: `axis` ("h" across / "v" up and down) and `at` (mm) — a guide line pulled out of the rulers at
  the page's edges; lines started within 3 mm of it run along it.
- symmetry: `points` [a, b] (the axis), `copies` (2 = a mirror; 3 or more = turned copies around a),
  `mirror` (with copies > 2, mirrored too) — every line is drawn again in the other places.

Every ruler has `id`, `active` (it snaps / copies), `visible` and may have `frame_id` (only for lines that
start in that panel). `reach_mm` is how near a line / curve ruler a stroke has to start to snap (10 mm).
"""

from __future__ import annotations

import math

KINDS = ("line", "curve", "parallel", "concentric", "radial", "perspective", "symmetry", "guide")
POINTS_NEEDED = {"line": 2, "curve": 2, "parallel": 0, "concentric": 1, "radial": 1, "perspective": 1, "symmetry": 2, "guide": 0}
GUIDE_REACH_MM = 3.0
REACH_MM = 10.0


def validate(ruler: dict) -> None:
    kind = ruler.get("kind")
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {', '.join(KINDS)}")
    points = ruler.get("points") or []
    if len(points) < POINTS_NEEDED[kind]:
        raise ValueError(f"a {kind} ruler needs {POINTS_NEEDED[kind]} point(s)")
    if kind == "perspective" and len(points) > 3:
        raise ValueError("a perspective ruler has 1 to 3 vanishing points")
    if kind == "concentric" and float(ruler.get("ratio", 1) or 1) <= 0:
        raise ValueError("ratio must be above 0")
    if kind == "guide":
        if ruler.get("axis") not in ("h", "v"):
            raise ValueError("a guide's axis is h or v")
        float(ruler.get("at"))
    if kind == "symmetry" and not 2 <= int(ruler.get("copies", 2) or 2) <= 32:
        raise ValueError("copies is 2 to 32")


# --- small geometry ------------------------------------------------------------------------------------


def _xy(p) -> tuple[float, float]:
    return float(p[0]), float(p[1])


def _pressure(p, default=None):
    return float(p[2]) if len(p) > 2 else default


def _with(points_xy: list, source: list) -> list:
    """New positions, with the pressures of the stroke they came from (spread along the new points)."""
    pressures = [_pressure(p) for p in source]
    if all(p is None for p in pressures):
        return [[round(x, 4), round(y, 4)] for x, y in points_xy]
    pressures = [p if p is not None else 0.7 for p in pressures]
    n, m = len(points_xy), len(pressures)
    out = []
    for i, (x, y) in enumerate(points_xy):
        t = i / max(1, n - 1) * (m - 1)
        k = min(m - 2, int(t)) if m > 1 else 0
        f = t - k
        p = pressures[k] * (1 - f) + pressures[min(m - 1, k + 1)] * f
        out.append([round(x, 4), round(y, 4), round(p, 4)])
    return out


def _segment(a, b, source: list) -> list:
    n = max(2, len(source), int(math.dist(a, b) / 1.0) + 2)
    return _with([(a[0] + (b[0] - a[0]) * i / (n - 1), a[1] + (b[1] - a[1]) * i / (n - 1)) for i in range(n)], source)


def _project(p, a, d) -> tuple[float, tuple[float, float]]:
    """(t, point) of p projected on the line a + t·d (d a unit vector)."""
    t = (p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]
    return t, (a[0] + t * d[0], a[1] + t * d[1])


def _unit(dx, dy) -> tuple[float, float] | None:
    n = math.hypot(dx, dy)
    return (dx / n, dy / n) if n > 1e-9 else None


def _straight_from(start, end, direction, source) -> list:
    d = _unit(*direction)
    if d is None:
        return _with([start, end], source)
    t, _ = _project(end, start, d)
    return _segment(start, (start[0] + t * d[0], start[1] + t * d[1]), source)


def smooth_curve(points: list, per_mm: float = 1.0) -> list[tuple[float, float]]:
    """A Catmull-Rom curve through the points, about one point per mm."""
    pts = [_xy(p) for p in points]
    if len(pts) < 3:
        return pts
    out = [pts[0]]
    ext = [pts[0], *pts, pts[-1]]
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        n = max(2, int(math.dist(p1, p2) * per_mm))
        for k in range(1, n + 1):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * p1[j] + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in (0, 1)))
    return out


def _nearest_on_polyline(p, poly) -> tuple[float, float, tuple[float, float]]:
    """(distance, arc position, point) of the nearest place on a polyline."""
    best = (math.inf, 0.0, poly[0])
    pos = 0.0
    for a, b in zip(poly, poly[1:]):
        seg = math.dist(a, b)
        if seg > 1e-9:
            t = max(0.0, min(1.0, ((p[0] - a[0]) * (b[0] - a[0]) + (p[1] - a[1]) * (b[1] - a[1])) / (seg * seg)))
            q = (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))
            d = math.dist(p, q)
            if d < best[0]:
                best = (d, pos + t * seg, q)
        pos += seg
    return best


def _at_arc(poly, s: float) -> tuple[float, float]:
    pos = 0.0
    for a, b in zip(poly, poly[1:]):
        seg = math.dist(a, b)
        if pos + seg >= s and seg > 1e-9:
            t = (s - pos) / seg
            return a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
        pos += seg
    return poly[-1]


# --- snapping ------------------------------------------------------------------------------------------


def directions(ruler: dict, at) -> list[tuple[float, float]]:
    """The straight directions a ruler offers at a point (for straight kinds)."""
    kind = ruler["kind"]
    if kind == "parallel":
        a = math.radians(float(ruler.get("angle", 0) or 0))
        return [(math.cos(a), math.sin(a))]
    if kind == "radial":
        c = _xy(ruler["points"][0])
        d = _unit(c[0] - at[0], c[1] - at[1])
        return [d] if d else []
    if kind == "perspective":
        vps = [_xy(p) for p in ruler["points"]]
        out = [d for d in (_unit(v[0] - at[0], v[1] - at[1]) for v in vps) if d]
        if len(vps) == 1:
            out += [(1.0, 0.0), (0.0, 1.0)]
        elif len(vps) == 2:
            h = _unit(vps[1][0] - vps[0][0], vps[1][1] - vps[0][1]) or (1.0, 0.0)
            out.append((-h[1], h[0]))  # upright: square to the horizon
        return out
    return []


def horizon(ruler: dict):
    """The eye level of a perspective ruler: (a point, a unit direction), or None for three points' third."""
    vps = [_xy(p) for p in ruler.get("points") or []]
    if not vps:
        return None
    if len(vps) == 1:
        return vps[0], (1.0, 0.0)
    return vps[0], _unit(vps[1][0] - vps[0][0], vps[1][1] - vps[0][1]) or (1.0, 0.0)


def _snap_one(ruler: dict, points: list):
    """The stroke along this ruler, or None when the ruler does not take it."""
    kind = ruler["kind"]
    if kind == "guide":
        at = float(ruler["at"])
        line = [(-1e4, at), (1e4, at)] if ruler.get("axis") == "h" else [(at, -1e4), (at, 1e4)]
        ruler = {"kind": "line", "points": line, "reach_mm": ruler.get("reach_mm", GUIDE_REACH_MM)}
        kind = "line"
    start, end = _xy(points[0]), _xy(points[-1])
    reach = float(ruler.get("reach_mm", REACH_MM) or REACH_MM)
    if kind == "line":
        a, b = _xy(ruler["points"][0]), _xy(ruler["points"][1])
        d = _unit(b[0] - a[0], b[1] - a[1])
        if d is None:
            return None
        t0, p0 = _project(start, a, d)
        if math.dist(start, p0) > reach:
            return None
        t1, p1 = _project(end, a, d)
        return _segment(p0, p1, points)
    if kind == "curve":
        poly = smooth_curve(ruler["points"])
        d0, s0, _ = _nearest_on_polyline(start, poly)
        if d0 > reach:
            return None
        _, s1, _ = _nearest_on_polyline(end, poly)
        n = max(2, len(points), int(abs(s1 - s0)) + 2)
        return _with([_at_arc(poly, s0 + (s1 - s0) * i / (n - 1)) for i in range(n)], points)
    if kind == "concentric":
        c = _xy(ruler["points"][0])
        ratio = float(ruler.get("ratio", 1) or 1)
        rot = math.radians(float(ruler.get("angle", 0) or 0))
        cos_r, sin_r = math.cos(rot), math.sin(rot)

        def to_local(p):
            x, y = p[0] - c[0], p[1] - c[1]
            return x * cos_r + y * sin_r, (-x * sin_r + y * cos_r) / ratio

        def to_page(x, y):
            y *= ratio
            return c[0] + x * cos_r - y * sin_r, c[1] + x * sin_r + y * cos_r

        lx, ly = to_local(start)
        radius = math.hypot(lx, ly)
        if radius < 0.2:
            return None
        angles, last = [], None
        for p in points:
            x, y = to_local(_xy(p))
            a = math.atan2(y, x)
            if last is not None:
                while a - last > math.pi:
                    a -= 2 * math.pi
                while a - last < -math.pi:
                    a += 2 * math.pi
            angles.append(a)
            last = a
        # about one point per mm of arc
        out = []
        for a, b in zip(angles, angles[1:]):
            n = max(1, int(abs(b - a) * radius))
            out += [to_page(radius * math.cos(a + (b - a) * k / n), radius * math.sin(a + (b - a) * k / n)) for k in range(n)]
        out.append(to_page(radius * math.cos(angles[-1]), radius * math.sin(angles[-1])))
        return _with(out, points)
    if kind in ("parallel", "radial", "perspective"):
        options = directions(ruler, start)
        if not options:
            return None
        drawn = _unit(end[0] - start[0], end[1] - start[1])
        if drawn is None:
            return None
        best = max(options, key=lambda d: abs(d[0] * drawn[0] + d[1] * drawn[1]))
        return _straight_from(start, end, best, points)
    return None


def _applies(ruler: dict, start, frame_contains) -> bool:
    if not ruler.get("active", True) or ruler.get("kind") == "symmetry":
        return False
    if ruler.get("frame_id") and frame_contains is not None:
        return bool(frame_contains(ruler["frame_id"], *start))
    return True


def snap(points: list, rulers: list, frame_contains=None, only: str | None = None) -> list:
    """The stroke snapped to the ruler that suits it best (the one whose line stays nearest to what was
    drawn); unchanged when no ruler takes it. `frame_contains(frame_id, x, y)` limits panel rulers;
    `only` picks one ruler by id."""
    if len(points) < 2:
        return points
    start = _xy(points[0])
    best, best_cost = None, math.inf
    for ruler in rulers:
        if only and ruler.get("id") != only:
            continue
        if not _applies(ruler, start, frame_contains):
            continue
        snapped = _snap_one(ruler, points)
        if not snapped:
            continue
        cost = sum(_nearest_on_polyline(_xy(p), [_xy(q) for q in snapped])[0] for p in points[:: max(1, len(points) // 12)])
        if cost < best_cost:
            best, best_cost = snapped, cost
    return best if best is not None else [list(p) for p in points]


# --- symmetry ------------------------------------------------------------------------------------------


def symmetry_copies(points: list, rulers: list, frame_contains=None) -> list[list]:
    """The extra strokes the active symmetry rulers make from one stroke."""
    out: list[list] = []
    start = _xy(points[0]) if points else (0, 0)
    for ruler in rulers:
        if ruler.get("kind") != "symmetry" or not ruler.get("active", True):
            continue
        if ruler.get("frame_id") and frame_contains is not None and not frame_contains(ruler["frame_id"], *start):
            continue
        a, b = _xy(ruler["points"][0]), _xy(ruler["points"][1])
        axis = math.atan2(b[1] - a[1], b[0] - a[0])
        copies = int(ruler.get("copies", 2) or 2)
        transforms = []
        if copies == 2:
            transforms.append(("mirror", axis))
        else:
            step = 2 * math.pi / copies
            for k in range(1, copies):
                transforms.append(("turn", k * step))
            if ruler.get("mirror"):
                transforms += [("mirror", axis + k * step / 2) for k in range(copies)]
        for kind, angle in transforms:
            c, s = math.cos(angle), math.sin(angle)
            moved = []
            for p in points:
                x, y = float(p[0]) - a[0], float(p[1]) - a[1]
                if kind == "turn":
                    nx, ny = x * c - y * s, x * s + y * c
                else:  # reflect across the line through a at `angle`
                    c2, s2 = math.cos(2 * angle), math.sin(2 * angle)
                    nx, ny = x * c2 + y * s2, x * s2 - y * c2
                moved.append([round(a[0] + nx, 4), round(a[1] + ny, 4), *([float(p[2])] if len(p) > 2 else [])])
            out.append(moved)
    return out


# --- the grid ------------------------------------------------------------------------------------------


def snap_to_grid(point, spacing_mm: float, origin=(0.0, 0.0)) -> tuple[float, float]:
    if spacing_mm <= 0:
        return _xy(point)
    return (origin[0] + round((float(point[0]) - origin[0]) / spacing_mm) * spacing_mm,
            origin[1] + round((float(point[1]) - origin[1]) / spacing_mm) * spacing_mm)
