"""3D shapes on the page (drawing guides), turned in space and seen in perspective: boxes, cylinders,
stairs and floors (a perspective grid on the ground), and whole background scenes (a room, a classroom,
a corridor, a street) made of those parts and moved, turned and traced as one.

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


KINDS = ("box", "cylinder", "stairs", "floor", "scene")
SCENES = ("room", "classroom", "corridor", "street")
SCENE_LABELS = {"room": "部屋", "classroom": "教室", "corridor": "廊下", "street": "街並み"}
# where the camera looks from, per scene: rooms from a corner, a slight view down; corridors and streets
# straight down their length (one-point perspective) from inside, near end just in front of the camera
SCENE_VIEWS = {"room": ([-0.38, 0.28, 0], -0.1), "classroom": ([-0.36, 0.2, 0], -0.15),
               "corridor": ([-0.06, 0.0, 0], -0.4), "street": ([-0.04, 0.08, 0], -0.4)}
SCENE_SIZES = {"room": [160, 90, 140], "classroom": [200, 90, 220], "corridor": [70, 80, 320], "street": [220, 120, 360]}


def _box_edges(cx, cy, cz, w, h, d) -> list:
    pts = [(cx + sx * w / 2, cy + sy * h / 2, cz + sz * d / 2) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    return [(pts[i], pts[j]) for i, j in EDGES]


def _rect_edges(corners) -> list:
    return list(zip(corners, corners[1:] + corners[:1]))


def scene_parts(kind: str, size) -> list:
    """The lines of a background scene in its own space (centred; the ground is at y = h/2, up is −y).
    Walls are open toward the viewer (the side at −z), so the inside shows."""
    w, h, d = size
    g = h / 2  # the ground
    top = -h / 2
    back = d / 2
    out: list = []

    def grid(x0, x1, z0, z1, nx, nz, y=g):
        for k in range(nx + 1):
            x = x0 + (x1 - x0) * k / nx
            out.append(((x, y, z0), (x, y, z1)))
        for k in range(nz + 1):
            z = z0 + (z1 - z0) * k / nz
            out.append(((x0, y, z), (x1, y, z)))

    def wall_x(x, z0, z1, y0=top, y1=g):  # a wall facing across (left or right)
        out.extend(_rect_edges([(x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0)]))

    def wall_z(z, x0, x1, y0=top, y1=g):  # a wall facing the viewer (at the back)
        out.extend(_rect_edges([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]))

    def hole_x(x, z0, z1, y0, y1):  # a window or door in a side wall
        out.extend(_rect_edges([(x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0)]))

    def hole_z(z, x0, x1, y0, y1):
        out.extend(_rect_edges([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]))

    if kind == "room":
        grid(-w / 2, w / 2, -d / 2, back, 6, 6)
        wall_z(back, -w / 2, w / 2)
        wall_x(-w / 2, -d / 2, back)
        wall_x(w / 2, -d / 2, back)
        hole_z(back, -w * 0.25, w * 0.15, top + h * 0.25, top + h * 0.6)  # a window
        hole_z(back, -w * 0.25, w * 0.15, top + h * 0.42, top + h * 0.43)  # its sash
        hole_x(-w / 2, back - d * 0.45, back - d * 0.15, top + h * 0.12, g)  # a door
        out.extend(_box_edges(w * 0.2, g - h * 0.2, back - d * 0.35, w * 0.3, h * 0.04, d * 0.22))  # a table top
        for sx in (-1, 1):
            for sz in (-1, 1):
                x, z = w * 0.2 + sx * w * 0.13, back - d * 0.35 + sz * d * 0.09
                out.append(((x, g - h * 0.18, z), (x, g, z)))
        out.extend(_box_edges(w * 0.33, g - h * 0.12, back - d * 0.12, w * 0.28, h * 0.24, d * 0.18))  # a bed / sofa
    elif kind == "classroom":
        grid(-w / 2, w / 2, -d / 2, back, 8, 10)
        wall_z(back, -w / 2, w / 2)
        wall_x(-w / 2, -d / 2, back)
        wall_x(w / 2, -d / 2, back)
        hole_z(back, -w * 0.3, w * 0.3, top + h * 0.25, top + h * 0.6)  # the blackboard
        out.append(((-w * 0.3, top + h * 0.62, back), (w * 0.3, top + h * 0.62, back)))  # its chalk tray
        for k in range(4):  # windows along the left
            z0 = -d / 2 + d * (0.1 + k * 0.22)
            hole_x(-w / 2, z0, z0 + d * 0.18, top + h * 0.2, top + h * 0.65)
        hole_x(w / 2, back - d * 0.25, back - d * 0.1, top + h * 0.15, g)  # the door
        for row in range(3):  # desks and chairs
            for col in range(4):
                x = -w * 0.3 + col * w * 0.2
                z = back - d * 0.4 - row * d * 0.18
                out.extend(_box_edges(x, g - h * 0.28, z, w * 0.11, h * 0.03, d * 0.07))
                for sx in (-1, 1):
                    out.append(((x + sx * w * 0.05, g - h * 0.27, z), (x + sx * w * 0.05, g, z)))
                out.extend(_box_edges(x, g - h * 0.17, z - d * 0.07, w * 0.08, h * 0.02, d * 0.05))
                out.append(((x, g - h * 0.17, z - d * 0.095), (x, g - h * 0.32, z - d * 0.095)))
        # the teacher's desk
        out.extend(_box_edges(0, g - h * 0.15, back - d * 0.12, w * 0.24, h * 0.3, d * 0.08))
    elif kind == "corridor":
        grid(-w / 2, w / 2, -d / 2, back, 3, 16)
        wall_x(-w / 2, -d / 2, back)
        wall_x(w / 2, -d / 2, back)
        wall_z(back, -w / 2, w / 2)
        out.append(((-w / 2, top, -d / 2), (w / 2, top, -d / 2)))
        for k in range(5):  # doors on the right, windows on the left
            z0 = -d / 2 + d * (0.05 + k * 0.19)
            hole_x(w / 2, z0, z0 + d * 0.07, top + h * 0.15, g)
            hole_x(-w / 2, z0, z0 + d * 0.13, top + h * 0.2, top + h * 0.6)
        for k in range(6):  # ceiling lights
            z = -d / 2 + d * (0.08 + k * 0.16)
            out.extend(_box_edges(0, top + 1, z, w * 0.2, 1, d * 0.03))
    elif kind == "street":
        road = w * 0.36
        grid(-road / 2, road / 2, -d / 2, back, 2, 12)
        for side in (-1, 1):  # sidewalks and their kerbs
            x_in, x_out = side * road / 2, side * w * 0.36
            out.append(((x_in, g, -d / 2), (x_in, g, back)))
            out.append(((x_in, g - 1.5, -d / 2), (x_in, g - 1.5, back)))
            out.append(((x_out, g, -d / 2), (x_out, g, back)))
        heights = [0.9, 0.55, 1.0, 0.7, 0.45, 0.8]
        for side in (-1, 1):  # buildings
            z = -d / 2
            for k, frac in enumerate(heights if side < 0 else heights[::-1]):
                depth = d / len(heights)
                bh = h * frac
                x = side * (w * 0.36 + w * 0.07)
                out.extend(_box_edges(x, g - bh / 2, z + depth / 2, w * 0.14, bh, depth * 0.9))
                for floor in range(1, int(bh // 18) + 1):  # floors, as lines on the street side
                    y = g - floor * 18
                    xs = x - side * w * 0.07
                    out.append(((xs, y, z + depth * 0.1), (xs, y, z + depth * 0.8)))
                z += depth
        for k in range(4):  # utility poles along the left
            z = -d / 2 + d * (0.1 + k * 0.25)
            x = -road / 2 - w * 0.02
            out.append(((x, g, z), (x, g - h * 0.75, z)))
            out.append(((x - 6, g - h * 0.68, z), (x + 6, g - h * 0.68, z)))
        out.append(((-road / 2 - w * 0.02, g - h * 0.7, -d / 2), (-road / 2 - w * 0.02, g - h * 0.7, back)))  # the wire
    return out


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
    elif kind == "scene":
        out = scene_parts(str(prim.get("scene") or "room"), (w, h, d))
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
    if prim.get("kind") in ("cylinder", "stairs", "floor", "scene"):
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
