"""3D shapes on the page (drawing guides), turned in space and seen in perspective: boxes, cylinders,
stairs and floors (a perspective grid on the ground).

A box is {"kind": "box", "pos": [x, y, z] (its centre: page mm, z = depth toward the back),
"size": [w, h, d] (mm), "rot": [tip, turn, lean] (radians: about the page's across, upright and
out-of-page axes), "focal_mm": the camera distance (smaller = stronger perspective; 400 by default)}.
"""

from __future__ import annotations

import math

EDGES = [(0, 1), (1, 3), (3, 2), (2, 0), (4, 5), (5, 7), (7, 6), (6, 4), (0, 4), (1, 5), (2, 6), (3, 7)]
# faces by corner index (corner i = (x, y, z) signs from bits 2, 1, 0: x = bit 2, y = bit 1, z = bit 0)
FACES = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]


def _size(prim: dict) -> tuple[float, float, float]:
    size = prim.get("size") or [40, 40, 40]
    if not isinstance(size, (list, tuple)):
        size = [size, size, size]
    size = list(size) + [size[-1]] * (3 - len(size))
    return tuple(max(0.1, float(v)) for v in size[:3])  # type: ignore[return-value]


def _rotate(p, rot) -> tuple[float, float, float]:
    tip, turn, lean = (list(rot) + [0, 0, 0])[:3]
    x, y, z = p
    # turn (about the upright axis), then tip (about the across axis), then lean (in the page)
    x, z = x * math.cos(turn) - z * math.sin(turn), x * math.sin(turn) + z * math.cos(turn)
    y, z = y * math.cos(tip) - z * math.sin(tip), y * math.sin(tip) + z * math.cos(tip)
    x, y = x * math.cos(lean) - y * math.sin(lean), x * math.sin(lean) + y * math.cos(lean)
    return x, y, z


def corners3d(prim: dict) -> list[tuple[float, float, float]]:
    w, h, d = _size(prim)
    rot = prim.get("rot") or [0.3, 0.6, 0]
    out = []
    for dx in (-w / 2, w / 2):
        for dy in (-h / 2, h / 2):
            for dz in (-d / 2, d / 2):
                out.append(_rotate((dx, dy, dz), rot))
    return out


def project(prim: dict) -> list[tuple[float, float]]:
    """The eight corners on the page (mm)."""
    cx, cy, cz = (list(prim.get("pos") or [100, 150, 0]) + [0, 0, 0])[:3]
    focal = float(prim.get("focal_mm", 400) or 400)
    out = []
    for x, y, z in corners3d(prim):
        scale = focal / max(focal * 0.2, focal + float(cz) + z)
        out.append((float(cx) + x * scale, float(cy) + y * scale))
    return out


KINDS = ("box", "cylinder", "stairs", "floor")


def _segments3d(prim: dict) -> list[tuple[tuple[float, float, float], tuple[float, float, float]]]:
    """The lines of a cylinder, stairs or floor in its own space (centred, before turning)."""
    w, h, d = _size(prim)
    kind = prim.get("kind")
    out = []
    if kind == "cylinder":
        n = 32
        rx, rz = w / 2, d / 2
        for y in (-h / 2, h / 2):
            ring = [(rx * math.cos(math.tau * k / n), y, rz * math.sin(math.tau * k / n)) for k in range(n + 1)]
            out += list(zip(ring, ring[1:]))
        for k in range(4):
            a = math.tau * k / 4
            out.append(((rx * math.cos(a), -h / 2, rz * math.sin(a)), (rx * math.cos(a), h / 2, rz * math.sin(a))))
    elif kind == "stairs":
        steps = max(2, min(30, int(prim.get("steps") or 6)))
        rise, run = h / steps, d / steps
        profile = [(h / 2, -d / 2)]  # (y, z): up is −y; the stairs climb toward the back
        for k in range(steps):
            y, z = profile[-1]
            profile.append((y - rise, z))
            profile.append((y - rise, z + run))
        profile.append((h / 2, d / 2))
        profile.append(profile[0])
        for x in (-w / 2, w / 2):
            pts = [(x, y, z) for y, z in profile]
            out += list(zip(pts, pts[1:]))
        for y, z in profile[:-1]:
            out.append(((-w / 2, y, z), (w / 2, y, z)))
    else:  # floor: a grid on the ground
        n = max(2, min(40, int(prim.get("lines") or 8)))
        for k in range(n + 1):
            x = -w / 2 + w * k / n
            z = -d / 2 + d * k / n
            out.append(((x, 0.0, -d / 2), (x, 0.0, d / 2)))
            out.append(((-w / 2, 0.0, z), (w / 2, 0.0, z)))
    return out


def _to_page(prim: dict, p) -> tuple[float, float]:
    cx, cy, cz = (list(prim.get("pos") or [100, 150, 0]) + [0, 0, 0])[:3]
    focal = float(prim.get("focal_mm", 400) or 400)
    x, y, z = _rotate(p, prim.get("rot") or [0.3, 0.6, 0])
    scale = focal / max(focal * 0.2, focal + float(cz) + z)
    return float(cx) + x * scale, float(cy) + y * scale


def edges(prim: dict) -> list[tuple[tuple[float, float], tuple[float, float], bool]]:
    """(a, b, seen) for each edge: seen is False for the edges at the back (boxes; the other shapes show all)."""
    if prim.get("kind") in ("cylinder", "stairs", "floor"):
        return [(_to_page(prim, a), _to_page(prim, b), True) for a, b in _segments3d(prim)]
    pts3 = corners3d(prim)
    pts = project(prim)
    focal = float(prim.get("focal_mm", 400) or 400)
    cz = float((list(prim.get("pos") or [0, 0, 0]) + [0, 0, 0])[2])
    seen_faces = []
    for face in FACES:
        a, b, c = (pts3[i] for i in face[:3])
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        centre = [sum(pts3[i][k] for i in face) / 4 for k in range(3)]
        centre[2] += cz + focal  # from the camera (at -focal) to the face
        seen_faces.append(n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] < 0)
    # the face winding decides the sign; if every face or none is "seen", flip the test
    if sum(seen_faces) > 3:
        seen_faces = [not s for s in seen_faces]
    out = []
    for i, j in EDGES:
        seen = any(seen_faces[k] and i in face and j in face for k, face in enumerate(FACES))
        out.append((pts[i], pts[j], seen))
    return out


def bbox(prim: dict) -> tuple[float, float, float, float]:
    pts = project(prim) if prim.get("kind", "box") == "box" else [p for a, b, _ in edges(prim) for p in (a, b)]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)


def prim_bbox(prim: dict) -> tuple[float, float, float, float]:
    if prim.get("kind") == "mannequin":
        from genko import mannequin

        return mannequin.skeleton(prim)["bbox"]
    return bbox(prim)


def trace(prim: dict) -> list[list[tuple[float, float]]]:
    """Lines to draw the prim with a pen (its seen edges, or the figure's bones and head)."""
    if prim.get("kind") == "mannequin":
        from genko import mannequin

        bone = mannequin.skeleton(prim)
        lines = [[a, b] for a, b, _ in bone["segments"]]
        (hx, hy), r = bone["head"]
        lines.append([(hx + r * math.cos(k * math.pi / 12), hy + r * math.sin(k * math.pi / 12)) for k in range(25)])
        return lines
    return [[a, b] for a, b, seen in edges(prim) if seen]
