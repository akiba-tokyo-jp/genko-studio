"""3D with surfaces (J8): meshes for the posable figure (体型と関節), a head for faces from any angle, hands
with finger poses, imported models (OBJ), boxes and scenes; seen through the page's camera, lit by one light,
drawn as shaded surfaces (面) and as the lines a pen would draw (seen edges and outlines, the hidden ones left
out).

Page space is mm: x to the right, y down the page, z away from the viewer. A prim's own shape is centred on
its `pos` and turned by `rot` [tip, turn, lean] as prim3d does. The page camera (`page.extra["camera"]`:
{turn, tip, roll (radians), focal_mm, target [x, y]}) turns the whole 3D world about the target before it is
seen; without one each prim is seen as before (prim3d's projection about its own centre).

Figure ("figure"): `size` [.., height, ..], `body` {heads (等身, 7.5), shoulders, hips, build (thickness),
legs (leg length)}, `joints` {name: {x, y, z}} (radians: x swings toward the viewer, y twists, z turns in
the picture, counter-clockwise), `hands` {"l": pose, "r": pose} (open, relaxed, fist, point, peace, grip).
Head ("head"): an egg with its centre line, eye line, nose and jaw marked. Hand ("hand"): one hand alone,
`side` l | r and `pose`. Mesh ("mesh"): `mesh` {"v": [x, y, z, …] (unit box), "f": [[i, j, k, …], …]}.
"""

from __future__ import annotations

import math
from functools import lru_cache

import numpy as np

FIGURE_JOINTS = ("hip", "spine", "chest", "neck", "head", "l_arm", "r_arm", "l_elbow", "r_elbow", "l_wrist", "r_wrist",
                 "l_leg", "r_leg", "l_knee", "r_knee", "l_ankle", "r_ankle")
HAND_POSES = ("open", "relaxed", "fist", "point", "peace", "grip")
MESH_KINDS = ("figure", "head", "hand", "mesh")
MAX_FACES = 30000
SMOOTH = 1000  # (added to a face's part when it belongs to a rounded piece)

# finger curl (0 straight .. 1 closed) per pose: thumb, index, middle, ring, little
_CURLS = {"open": (0.0, 0.0, 0.0, 0.0, 0.0), "relaxed": (0.2, 0.25, 0.3, 0.35, 0.4), "fist": (0.7, 1.0, 1.0, 1.0, 1.0),
          "point": (0.6, 0.0, 1.0, 1.0, 1.0), "peace": (0.7, 0.0, 0.0, 1.0, 1.0), "grip": (0.5, 0.6, 0.6, 0.6, 0.6)}

FIGURE_PRESETS: dict[str, dict] = {
    "stand": {},
    "walk": {"l_leg": {"x": 0.45}, "l_knee": {"x": -0.15}, "r_leg": {"x": -0.35}, "r_knee": {"x": -0.45},
             "l_arm": {"x": -0.35, "z": 0.1}, "l_elbow": {"x": 0.25}, "r_arm": {"x": 0.35, "z": -0.1}, "r_elbow": {"x": 0.4}},
    "run": {"spine": {"x": 0.3}, "l_leg": {"x": 1.0}, "l_knee": {"x": -1.3}, "r_leg": {"x": -0.6}, "r_knee": {"x": -1.1},
            "l_arm": {"x": -0.8, "z": 0.15}, "l_elbow": {"x": 1.3}, "r_arm": {"x": 0.9, "z": -0.15}, "r_elbow": {"x": 1.4},
            "hands": {"l": "fist", "r": "fist"}},
    "sit": {"l_leg": {"x": 1.5}, "l_knee": {"x": -1.5}, "r_leg": {"x": 1.45}, "r_knee": {"x": -1.45},
            "l_arm": {"x": 0.3, "z": 0.1}, "l_elbow": {"x": 1.0}, "r_arm": {"x": 0.35, "z": -0.1}, "r_elbow": {"x": 1.0}},
    "point": {"r_arm": {"z": -1.5, "x": 0.2}, "r_elbow": {"z": -0.05}, "head": {"y": -0.3}, "hands": {"r": "point"}},
    "arms_up": {"l_arm": {"z": 2.8}, "r_arm": {"z": -2.8}, "l_elbow": {"z": 0.2}, "r_elbow": {"z": -0.2}, "hands": {"l": "open", "r": "open"}},
    "think": {"r_arm": {"x": 0.6, "z": -0.2}, "r_elbow": {"x": 2.3}, "head": {"z": 0.15, "x": -0.1}, "l_arm": {"x": 0.3, "z": 0.3},
              "l_elbow": {"x": 1.4, "z": -0.8}, "hands": {"r": "relaxed"}},
    "kneel": {"l_leg": {"x": 1.4}, "l_knee": {"x": -1.4}, "r_leg": {"x": 0.1}, "r_knee": {"x": -1.55}, "r_ankle": {"x": 0.6}},
    "peace": {"r_arm": {"z": -0.4, "x": 0.9}, "r_elbow": {"x": 1.6, "z": 0.4}, "hands": {"r": "peace"}, "head": {"z": -0.1}},
}


# --- small matrix helpers -----------------------------------------------------------------------------------


def rx(t: float) -> np.ndarray:  # swings +y toward −z (the viewer) for t > 0
    c, s = math.cos(t), math.sin(t)
    return np.array([[1, 0, 0], [0, c, s], [0, -s, c]], dtype=float)


def ry(t: float) -> np.ndarray:
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]], dtype=float)


def rz(t: float) -> np.ndarray:  # counter-clockwise on the page (y points down)
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, s, 0], [-s, c, 0], [0, 0, 1]], dtype=float)


def prim_rotation(rot) -> np.ndarray:
    """The same turn as prim3d._rotate: turn about the upright axis, then tip, then lean, as one matrix."""
    tip, turn, lean = (list(rot or [0, 0, 0]) + [0, 0, 0])[:3]
    m_turn = np.array([[math.cos(turn), 0, -math.sin(turn)], [0, 1, 0], [math.sin(turn), 0, math.cos(turn)]])
    m_tip = np.array([[1, 0, 0], [0, math.cos(tip), -math.sin(tip)], [0, math.sin(tip), math.cos(tip)]])
    m_lean = np.array([[math.cos(lean), -math.sin(lean), 0], [math.sin(lean), math.cos(lean), 0], [0, 0, 1]])
    return m_lean @ m_tip @ m_turn


def _joint_matrix(joint: dict | None) -> np.ndarray:
    joint = joint or {}
    return rz(float(joint.get("z", 0))) @ rx(float(joint.get("x", 0))) @ ry(float(joint.get("y", 0)))


# --- mesh pieces ----------------------------------------------------------------------------------------------


class Builder:
    def __init__(self) -> None:
        self.v: list[tuple[float, float, float]] = []
        self.f: list[tuple[int, ...]] = []
        self.parts: list[int] = []  # (which part each face belongs to: for the lines)

    def add(self, verts, faces, part: int = 0, smooth: bool = False) -> None:
        """smooth: a rounded piece (no lines where its facets meet, only its outline)."""
        base = len(self.v)
        self.v.extend(tuple(float(c) for c in p) for p in verts)
        for face in faces:
            self.f.append(tuple(base + i for i in face))
            self.parts.append(part + (SMOOTH if smooth else 0))

    def ellipsoid(self, centre, radii, rot: np.ndarray | None = None, nu: int = 14, nv: int = 9, part: int = 0) -> None:
        verts, faces = [], []
        for j in range(nv + 1):
            phi = math.pi * j / nv
            for i in range(nu):
                th = 2 * math.pi * i / nu
                p = np.array([radii[0] * math.sin(phi) * math.cos(th), -radii[1] * math.cos(phi), radii[2] * math.sin(phi) * math.sin(th)])
                if rot is not None:
                    p = rot @ p
                verts.append(tuple(np.asarray(centre) + p))
        for j in range(nv):
            for i in range(nu):
                a, b = j * nu + i, j * nu + (i + 1) % nu
                faces.append((a, b, b + nu, a + nu))
        self.add(verts, faces, part, smooth=True)

    def capsule(self, a, b, r: float, n: int = 10, part: int = 0) -> None:
        """A limb from a to b: a tube with rounded ends."""
        a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
        axis = b - a
        length = float(np.linalg.norm(axis))
        if length < 1e-6:
            self.ellipsoid(a, (r, r, r), part=part)
            return
        w = axis / length
        u = np.cross(w, [0.0, 0.0, 1.0])
        if np.linalg.norm(u) < 1e-6:
            u = np.cross(w, [1.0, 0.0, 0.0])
        u /= np.linalg.norm(u)
        v = np.cross(w, u)
        rings = []
        caps = 3
        for k in range(caps + 1):  # the start cap
            t = math.pi / 2 * (1 - k / caps)
            rings.append((a - w * r * math.sin(t), r * math.cos(t)))
        for k in range(caps + 1):  # the end cap
            t = math.pi / 2 * k / caps
            rings.append((b + w * r * math.sin(t), r * math.cos(t)))
        verts, faces = [], []
        for centre, rad in rings:
            for i in range(n):
                th = 2 * math.pi * i / n
                verts.append(tuple(centre + (u * math.cos(th) + v * math.sin(th)) * rad))
        for j in range(len(rings) - 1):
            for i in range(n):
                p, q = j * n + i, j * n + (i + 1) % n
                faces.append((p, q, q + n, p + n))
        self.add(verts, faces, part, smooth=True)

    def box(self, centre, size, rot: np.ndarray | None = None, part: int = 0) -> None:
        w, h, d = size
        corners = []
        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    p = np.array([sx * w / 2, sy * h / 2, sz * d / 2])
                    corners.append(tuple(np.asarray(centre) + (rot @ p if rot is not None else p)))
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        self.add(corners, faces, part)

    def arrays(self) -> tuple[np.ndarray, list[tuple[int, ...]], list[int]]:
        return np.array(self.v, dtype=float).reshape(-1, 3), self.f, self.parts


def _hand(b: Builder, wrist: np.ndarray, frame: np.ndarray, length: float, pose: str, side: int, part: int) -> None:
    """A hand at the wrist: the palm and five fingers of two bones, curled by the pose. `frame` is the
    forearm's turn (its y points along the hand)."""
    curls = _CURLS.get(pose, _CURLS["relaxed"])
    palm_len, palm_w, thick = length * 0.5, length * 0.45, length * 0.14
    down, across, front = frame[:, 1], frame[:, 0], frame[:, 2]
    palm_c = wrist + down * palm_len / 2
    b.box(palm_c, (palm_w, palm_len, thick), frame, part)
    for k, curl in enumerate(curls):
        if k == 0:  # the thumb: from the palm's side, toward the front
            base = wrist + down * palm_len * 0.3 + across * (palm_w / 2) * side
            direction = rz(-0.7 * side) @ rx(0.4)
        else:
            base = wrist + down * palm_len + across * (palm_w * (0.36 - 0.24 * (k - 1))) * side
            direction = np.eye(3)
        seg = length * (0.22 if k in (1, 2, 3) else 0.18)
        turn = frame @ direction
        p = base
        for bone in range(2):
            bend = rx(-curl * 1.3)  # curling toward the palm (its front)
            turn = turn @ bend
            q = p + turn[:, 1] * seg
            b.capsule(p, q, thick * 0.45, 6, part)
            p = q
    del front


@lru_cache(maxsize=64)
def _figure_cached(key: str):
    import json

    return _figure(json.loads(key))


def figure_skeleton(prim: dict) -> dict:
    """The figure's joints in its own space (mm, before its turn): {name: point}, and each bone's turn."""
    size = prim.get("size") or [40, 80, 20]
    height = float(size[1] if isinstance(size, (list, tuple)) else size) or 80.0
    body = {"heads": 7.5, "shoulders": 1.0, "hips": 1.0, "build": 1.0, "legs": 1.0, **(prim.get("body") or {})}
    u = height / max(4.0, min(10.0, float(body["heads"])))
    joints = prim.get("joints") or {}
    rest_arm = {"l_arm": {"z": 0.12}, "r_arm": {"z": -0.12}}
    legs = max(0.6, min(1.5, float(body["legs"])))

    def j(name):
        base = dict(rest_arm.get(name, {}))
        for axis, value in (joints.get(name) or {}).items():
            base[axis] = base.get(axis, 0.0) + float(value)
        return _joint_matrix(base)

    pts: dict[str, np.ndarray] = {}
    turns: dict[str, np.ndarray] = {}
    hip_r = j("hip")
    pelvis = np.zeros(3)
    pts["pelvis"] = pelvis
    spine_r = hip_r @ j("spine")
    waist = pelvis + spine_r @ np.array([0, -1.3 * u, 0])
    chest_r = spine_r @ j("chest")
    neck_base = waist + chest_r @ np.array([0, -1.45 * u, 0])
    neck_r = chest_r @ j("neck")
    head_base = neck_base + neck_r @ np.array([0, -0.45 * u, 0])
    head_r = neck_r @ j("head")
    head_c = head_base + head_r @ np.array([0, -0.55 * u, 0])
    pts.update(waist=waist, neck=neck_base, head_base=head_base, head=head_c)
    turns.update(hip=hip_r, spine=spine_r, chest=chest_r, neck=neck_r, head=head_r)
    shoulder_w = 0.95 * u * max(0.6, min(1.5, float(body["shoulders"])))
    hip_w = 0.5 * u * max(0.6, min(1.6, float(body["hips"])))
    for p, s in (("l", 1), ("r", -1)):
        shoulder = neck_base + chest_r @ np.array([s * shoulder_w, 0.2 * u, 0])
        arm_r = chest_r @ j(f"{p}_arm")
        elbow = shoulder + arm_r @ np.array([0, 1.5 * u, 0])
        fore_r = arm_r @ j(f"{p}_elbow")
        wrist = elbow + fore_r @ np.array([0, 1.3 * u, 0])
        hand_r = fore_r @ j(f"{p}_wrist")
        hip = pelvis + hip_r @ np.array([s * hip_w, 0.15 * u, 0])
        thigh_r = hip_r @ j(f"{p}_leg")
        knee = hip + thigh_r @ np.array([0, 2.0 * u * legs, 0])
        shin_r = thigh_r @ j(f"{p}_knee")
        ankle = knee + shin_r @ np.array([0, 1.95 * u * legs, 0])
        foot_r = shin_r @ j(f"{p}_ankle")
        toe = ankle + foot_r @ np.array([0, 0.25 * u, -0.85 * u])
        pts.update({f"{p}_shoulder": shoulder, f"{p}_elbow": elbow, f"{p}_wrist": wrist, f"{p}_hip": hip, f"{p}_knee": knee,
                    f"{p}_ankle": ankle, f"{p}_toe": toe, f"{p}_hand": wrist + hand_r @ np.array([0, 0.45 * u, 0])})
        turns.update({f"{p}_arm": arm_r, f"{p}_elbow": fore_r, f"{p}_wrist": hand_r, f"{p}_leg": thigh_r, f"{p}_knee": shin_r,
                      f"{p}_ankle": foot_r})
    return {"points": pts, "turns": turns, "unit": u, "body": body}


def _figure(prim: dict):
    sk = figure_skeleton(prim)
    pts, turns, u = sk["points"], sk["turns"], sk["unit"]
    build = max(0.5, min(1.8, float(sk["body"]["build"])))
    b = Builder()
    t = u * 0.2 * build  # limb radius
    # the torso: chest, belly and pelvis as eggs; the neck; the head
    b.ellipsoid((pts["waist"] + pts["neck"]) / 2 + turns["chest"] @ np.array([0, 0.1 * u, 0]),
                (0.95 * u * max(0.6, float(sk["body"]["shoulders"])) * 0.85 * build ** 0.3, 0.85 * u, 0.5 * u * build), turns["chest"], part=1)
    b.ellipsoid((pts["pelvis"] + pts["waist"]) / 2, (0.62 * u * build ** 0.3 * max(0.6, float(sk["body"]["hips"])), 0.75 * u, 0.42 * u * build),
                turns["spine"], part=1)
    b.capsule(pts["neck"], pts["head_base"], t * 0.8, part=1)
    _head_mesh(b, pts["head"], turns["head"], u, part=2)
    hands = prim.get("hands") or {}
    for p, s in (("l", 1), ("r", -1)):
        b.capsule(pts[f"{p}_shoulder"], pts[f"{p}_elbow"], t * 1.05, part=3)
        b.capsule(pts[f"{p}_elbow"], pts[f"{p}_wrist"], t * 0.85, part=3)
        _hand(b, pts[f"{p}_wrist"], turns[f"{p}_wrist"], 0.8 * u, str(hands.get(p) or "relaxed"), s, part=4)
        b.capsule(pts[f"{p}_hip"], pts[f"{p}_knee"], t * 1.45, part=5)
        b.capsule(pts[f"{p}_knee"], pts[f"{p}_ankle"], t * 1.1, part=5)
        foot_mid = (pts[f"{p}_ankle"] + pts[f"{p}_toe"]) / 2
        b.box(foot_mid, (0.4 * u, 0.3 * u, 1.0 * u), turns[f"{p}_ankle"] @ rx(-1.35), part=6)
    return b.arrays(), _head_marks(pts["head"], turns["head"], u)


def _head_mesh(b: Builder, centre, turn, u: float, part: int) -> None:
    b.ellipsoid(centre, (0.42 * u, 0.55 * u, 0.5 * u), turn, 16, 11, part)
    # the jaw and chin, a little forward and down
    b.ellipsoid(centre + turn @ np.array([0, 0.28 * u, -0.12 * u]), (0.3 * u, 0.28 * u, 0.3 * u), turn, 12, 8, part)


def _head_marks(centre, turn, u: float) -> list[list[np.ndarray]]:
    """The guide lines on a face: the centre line (front, top to chin), the eye line, the nose."""
    rx_, ry_, rz_ = 0.42 * u, 0.55 * u, 0.5 * u

    def on(theta, phi, lift=1.01):  # a point on the egg: theta round (0 = facing the viewer), phi down from the top
        p = np.array([rx_ * math.sin(phi) * math.sin(theta), -ry_ * math.cos(phi), -rz_ * math.sin(phi) * math.cos(theta)]) * lift
        return centre + turn @ p

    centre_line = [on(0.0, math.pi * k / 20) for k in range(3, 21)]
    eye_line = [on(math.pi * (k / 20 - 0.5) * 0.9, math.pi * 0.55) for k in range(21)]
    nose = [on(0, math.pi * 0.62, 1.01), on(0, math.pi * 0.66, 1.12), on(0, math.pi * 0.7, 1.02)]
    return [centre_line, eye_line, nose]


def _mesh_of(prim: dict):
    """(V, F, parts) in the prim's own space (before its turn and place), and extra guide polylines."""
    kind = prim.get("kind")
    size = prim.get("size") or [40, 40, 40]
    if kind == "figure":
        import json

        key = json.dumps({k: prim.get(k) for k in ("size", "body", "joints", "hands")}, sort_keys=True)
        return _figure_cached(key)
    if kind == "head":
        height = float(size[1] if isinstance(size, (list, tuple)) else size) or 30.0
        u = height / 1.1
        b = Builder()
        _head_mesh(b, np.zeros(3), np.eye(3), u, 2)
        return b.arrays(), _head_marks(np.zeros(3), np.eye(3), u)
    if kind == "hand":
        length = float(size[1] if isinstance(size, (list, tuple)) else size) or 20.0
        b = Builder()
        side = 1 if prim.get("side", "r") == "l" else -1
        _hand(b, np.array([0, -length / 2, 0]), np.eye(3), length, str(prim.get("pose") or "relaxed"), side, 4)
        return b.arrays(), []
    if kind in ("box", "cylinder", "stairs", "floor", "scene"):
        return _solid(prim), []
    if kind == "mesh":
        data = prim.get("mesh") or {}
        flat = np.array(data.get("v") or [], dtype=float).reshape(-1, 3)
        w, h, d = (list(size) + [size[-1]] * 3)[:3] if isinstance(size, (list, tuple)) else (size, size, size)
        verts = flat * np.array([float(w), float(h), float(d)])
        faces = [tuple(int(i) for i in f) for f in data.get("f") or []]
        return (verts, faces, [0] * len(faces)), []
    return (np.zeros((0, 3)), [], []), []


def _solid(prim: dict):
    """The surfaces of the older 3D guides (boxes, cylinders, stairs, floors, the walls and floor of scenes)."""
    from genko import prim3d

    w, h, d = prim3d._size(prim)
    kind = prim.get("kind")
    b = Builder()
    if kind == "box":
        b.box((0, 0, 0), (w, h, d))
    elif kind == "cylinder":
        n = 24
        verts = [(w / 2 * math.cos(2 * math.pi * k / n), y, d / 2 * math.sin(2 * math.pi * k / n)) for y in (-h / 2, h / 2) for k in range(n)]
        faces = [(k, (k + 1) % n, n + (k + 1) % n, n + k) for k in range(n)] + [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
        b.add(verts, faces, 0, smooth=True)
    elif kind == "stairs":
        steps = max(2, min(30, int(prim.get("steps") or 6)))
        rise, run = h / steps, d / steps
        for k in range(steps):  # each step a box from the ground up
            top = h / 2 - rise * (k + 1)
            b.box((0, (top + h / 2) / 2, -d / 2 + run * (k + 0.5)), (w, h / 2 - top, run))
    elif kind == "floor":
        b.add([(-w / 2, 0, -d / 2), (w / 2, 0, -d / 2), (w / 2, 0, d / 2), (-w / 2, 0, d / 2)], [(0, 1, 2, 3)])
    elif kind == "scene":
        g, top, back = h / 2, -h / 2, d / 2
        b.add([(-w / 2, g, -d / 2), (w / 2, g, -d / 2), (w / 2, g, back), (-w / 2, g, back)], [(0, 1, 2, 3)])  # the floor
        if prim.get("scene") != "street":
            b.add([(-w / 2, top, back), (w / 2, top, back), (w / 2, g, back), (-w / 2, g, back)], [(0, 1, 2, 3)])  # the back wall
            for x in (-w / 2, w / 2):
                b.add([(x, top, -d / 2), (x, top, back), (x, g, back), (x, g, -d / 2)], [(0, 1, 2, 3)])
    return b.arrays()


def with_camera(prim: dict, page) -> dict:
    """The prim carrying its page's camera (when the page has one), for drawing and tracing."""
    camera = (getattr(page, "extra", None) or {}).get("camera") if page is not None else None
    return {**prim, "camera": camera} if camera else prim


@lru_cache(maxsize=128)
def _lines_cached(key: str, dpi: float):
    import json

    data = json.loads(key)
    return lines([data["prim"]], data.get("camera"), dpi)


def prim_lines(prim: dict, dpi: float = 40.0) -> list[list[tuple[float, float]]]:
    """A mesh prim's pen lines (kept while the prim is unchanged)."""
    import json

    key = json.dumps({"prim": {k: v for k, v in prim.items() if k != "camera"}, "camera": prim.get("camera")}, sort_keys=True,
                     default=float)
    return _lines_cached(key, dpi)


# --- the camera ----------------------------------------------------------------------------------------------


def to_page(prim: dict, local: np.ndarray, camera: dict | None = None) -> tuple[np.ndarray, np.ndarray]:
    """Points of a prim's own space on the page (N×2 mm) and their depth (N, larger = further)."""
    cx, cy, cz = (list(prim.get("pos") or [100, 150, 0]) + [0, 0, 0])[:3]
    turned = local @ prim_rotation(prim.get("rot") or [0, 0, 0]).T
    if camera:
        world = turned + np.array([float(cx), float(cy), float(cz)])
        target = camera.get("target") or [cx, cy]
        tx, ty = float(target[0]), float(target[1])
        view = prim_rotation([camera.get("tip", 0), camera.get("turn", 0), camera.get("roll", 0)])
        rel = (world - np.array([tx, ty, 0.0])) @ view.T
        focal = float(camera.get("focal_mm") or prim.get("focal_mm") or 400)
        scale = focal / np.maximum(focal * 0.2, focal + rel[:, 2])
        return np.stack([tx + rel[:, 0] * scale, ty + rel[:, 1] * scale], axis=1), rel[:, 2]
    focal = float(prim.get("focal_mm", 400) or 400)
    scale = focal / np.maximum(focal * 0.2, focal + float(cz) + turned[:, 2])
    return np.stack([float(cx) + turned[:, 0] * scale, float(cy) + turned[:, 1] * scale], axis=1), turned[:, 2] + float(cz)


def seen(prim: dict, camera: dict | None = None):
    """The prim's mesh on the page: (points N×2, depth N, faces, parts, guide polylines on the page)."""
    (verts, faces, parts), marks = _mesh_of(prim)
    if not len(verts):
        return np.zeros((0, 2)), np.zeros(0), [], [], []
    pts, depth = to_page(prim, verts, camera)
    guides = []
    for line in marks:
        if len(line):
            p, dz = to_page(prim, np.array(line), camera)
            guides.append((p, dz))
    return pts, depth, faces, parts, guides


# --- drawing: surfaces and lines --------------------------------------------------------------------------------


def _turned(prim: dict, verts: np.ndarray, camera: dict | None) -> np.ndarray:
    """The prim's points turned as they are seen (its own turn, then the camera's)."""
    out = verts @ prim_rotation(prim.get("rot") or [0, 0, 0]).T
    if camera:
        out = out @ prim_rotation([camera.get("tip", 0), camera.get("turn", 0), camera.get("roll", 0)]).T
    return out


def _triangles(faces):
    for face in faces:
        for k in range(1, len(face) - 1):
            yield face[0], face[k], face[k + 1]


def _normals(verts3: np.ndarray, faces) -> np.ndarray:
    out = []
    for face in faces:
        a, b, c = verts3[face[0]], verts3[face[1]], verts3[face[2 if len(face) > 2 else 1]]
        n = np.cross(b - a, c - a)
        norm = np.linalg.norm(n)
        out.append(n / norm if norm > 1e-9 else np.zeros(3))
    return np.array(out).reshape(-1, 3)


def raster(prims: list[dict], size: tuple[int, int], dpi: float, camera: dict | None = None, light=None,
           ambient: float = 0.35, box: tuple[float, float] = (0.0, 0.0)):
    """Shaded surfaces (L, 255 = lit white, darker in shadow; alpha where there is a surface) and the depth
    buffer, for the prims with meshes, drawn at dpi over a picture of `size` px whose top-left is `box` (mm)."""
    w, h = size
    k = dpi / 25.4
    zbuf = np.full((h, w), np.inf, dtype=np.float32)
    shade = np.zeros((h, w), dtype=np.float32)
    lit = np.asarray(light if light is not None else (-0.5, -0.7, -0.6), dtype=float)
    lit = lit / (np.linalg.norm(lit) or 1.0)
    for prim in prims:
        (verts, faces, _parts), _marks = _mesh_of(prim)
        if not faces:
            continue
        pts, depth = to_page(prim, verts, camera)
        normals = _normals(_turned(prim, verts, camera), faces)
        px = (pts - np.array(box)) * k
        for fi, face in enumerate(faces):
            n = normals[fi]
            # (a face turned away still shows its inside from the other side: light it by the side seen)
            view_side = n[2]
            if view_side > 0:
                n = -n
            value = ambient + (1 - ambient) * max(0.0, float(n @ lit))
            for a, b_, c in _triangles([face]):
                _fill_triangle(px[[a, b_, c]], depth[[a, b_, c]], value, zbuf, shade)
    alpha = np.isfinite(zbuf)
    return shade, zbuf, alpha


def _fill_triangle(p: np.ndarray, z: np.ndarray, value: float, zbuf: np.ndarray, shade: np.ndarray) -> None:
    h, w = zbuf.shape
    x0, y0 = max(0, int(math.floor(p[:, 0].min()))), max(0, int(math.floor(p[:, 1].min())))
    x1, y1 = min(w - 1, int(math.ceil(p[:, 0].max()))), min(h - 1, int(math.ceil(p[:, 1].max())))
    if x1 < x0 or y1 < y0:
        return
    (ax, ay), (bx, by), (cx, cy) = p
    den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(den) < 1e-9:
        return
    gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / den
    l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / den
    l3 = 1 - l1 - l2
    inside = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
    if not inside.any():
        return
    depth = l1 * z[0] + l2 * z[1] + l3 * z[2]
    region = zbuf[y0:y1 + 1, x0:x1 + 1]
    nearer = inside & (depth < region)
    region[nearer] = depth[nearer]
    shade[y0:y1 + 1, x0:x1 + 1][nearer] = value


def lines(prims: list[dict], camera: dict | None = None, dpi: float = 40.0, crease_deg: float = 40.0) -> list[list[tuple[float, float]]]:
    """The lines a pen would draw (page mm): outlines and creases of the meshes, the face guides, hidden
    parts left out (a depth test against the surfaces)."""
    meshy = [p for p in prims if p.get("kind") in MESH_KINDS]
    if not meshy:
        return []
    segs = []
    for prim in meshy:
        pts, depth, faces, parts, guides = seen(prim, camera)
        if not faces:
            continue
        (verts, _f, _p), _m = _mesh_of(prim)
        normals = _normals(_turned(prim, verts, camera), faces)
        facing = normals[:, 2] < 0  # toward the viewer
        edge_faces: dict[tuple[int, int], list[int]] = {}
        for fi, face in enumerate(faces):
            for a, b in zip(face, face[1:] + face[:1]):
                edge_faces.setdefault((min(a, b), max(a, b)), []).append(fi)
        cos_crease = math.cos(math.radians(crease_deg))
        for (a, b), fs in edge_faces.items():
            if len(fs) == 1:
                keep = parts[fs[0]] < SMOOTH  # (an open edge; a rounded piece's poles are not edges)
            else:
                f0, f1 = fs[0], fs[1]
                rounded = parts[f0] >= SMOOTH and parts[f1] >= SMOOTH
                keep = facing[f0] != facing[f1] or (not rounded and float(normals[f0] @ normals[f1]) < cos_crease)
            if keep:
                segs.append((pts[a], pts[b], depth[a], depth[b]))
        for p, dz in guides:
            for i in range(len(p) - 1):
                segs.append((p[i], p[i + 1], dz[i] - 0.3, dz[i + 1] - 0.3))
    if not segs:
        return []
    all_pts = np.array([s[0] for s in segs] + [s[1] for s in segs])
    x0, y0 = all_pts.min(axis=0) - 2
    x1, y1 = all_pts.max(axis=0) + 2
    k = dpi / 25.4
    size = (max(1, int((x1 - x0) * k) + 2), max(1, int((y1 - y0) * k) + 2))
    _shade, zbuf, _alpha = raster(meshy, size, dpi, camera, box=(float(x0), float(y0)))
    tolerance = max(0.8, 2.5 * 25.4 / dpi)  # (a pixel's worth of depth, and the rim of rounded parts)
    out: list[list[tuple[float, float]]] = []
    for a, b, za, zb in segs:
        length = float(np.hypot(*(b - a)))
        n = max(2, int(length * k * 1.5))
        run: list[tuple[float, float]] = []
        for i in range(n + 1):
            t = i / n
            p = a + (b - a) * t
            z = za + (zb - za) * t
            px, py = int((p[0] - x0) * k), int((p[1] - y0) * k)
            near = zbuf[max(0, py - 1):py + 2, max(0, px - 1):px + 2]
            # seen when some pixel beside it is empty or no nearer than it (the rim of a rounded part)
            visible = near.size == 0 or bool(np.any(~np.isfinite(near) | (z <= near + tolerance)))
            if visible:
                run.append((float(p[0]), float(p[1])))
            elif len(run) >= 2:
                out.append(run)
                run = []
            else:
                run = []
        if len(run) >= 2:
            out.append(run)
    return _join(out)


def _join(pieces: list[list[tuple[float, float]]]) -> list[list[tuple[float, float]]]:
    """Short pieces that meet end to end joined into longer lines (fewer, smoother pen lines)."""
    out: list[list[tuple[float, float]]] = []
    for piece in pieces:
        if out and math.dist(out[-1][-1], piece[0]) < 0.05:
            out[-1].extend(piece[1:])
        else:
            out.append(list(piece))
    return out


# --- OBJ -----------------------------------------------------------------------------------------------------


class ObjError(ValueError):
    pass


def read_obj(text: str) -> dict:
    """A mesh from OBJ text ("v" and "f" lines; normals and textures are ignored), fitted into a unit box
    centred on 0 with y down (OBJ's y is up)."""
    verts: list[tuple[float, float, float]] = []
    faces: list[list[int]] = []
    for raw in text.splitlines():
        parts = raw.strip().split()
        if not parts:
            continue
        if parts[0] == "v" and len(parts) >= 4:
            verts.append((float(parts[1]), -float(parts[2]), -float(parts[3])))
        elif parts[0] == "f" and len(parts) >= 4:
            idx = []
            for token in parts[1:]:
                i = int(token.split("/")[0])
                idx.append(i - 1 if i > 0 else len(verts) + i)
            faces.append(idx)
    if not verts or not faces:
        raise ObjError("the OBJ file has no faces")
    if len(faces) > MAX_FACES:
        raise ObjError(f"the model has too many faces (at most {MAX_FACES})")
    if any(not 0 <= i < len(verts) for face in faces for i in face):
        raise ObjError("the OBJ file points at corners it does not have")
    v = np.array(verts)
    lo, hi = v.min(axis=0), v.max(axis=0)
    span = float((hi - lo).max()) or 1.0
    v = (v - (lo + hi) / 2) / span
    ratio = (hi - lo) / span
    return {"v": [round(float(c), 5) for c in v.flatten()], "f": faces, "ratio": [round(float(r), 4) for r in ratio]}


# --- glTF / GLB (VRM is a GLB) -----------------------------------------------------------------------------------

_COMPONENTS = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_gltf(data: bytes) -> dict:
    """A mesh from a .glb / .vrm (binary glTF) or a .gltf with its buffer inside (data: URI): every mesh's
    triangles in their rest pose, the nodes' places applied, fitted into a unit box like read_obj."""
    import base64
    import json
    import struct

    if data[:4] == b"glTF":
        if len(data) < 20:
            raise ObjError("the model file is cut short")
        _magic, _version, total = struct.unpack_from("<III", data, 0)
        at, doc, blob = 12, None, b""
        while at + 8 <= min(total, len(data)):
            length, kind = struct.unpack_from("<II", data, at)
            chunk = data[at + 8:at + 8 + length]
            if kind == 0x4E4F534A:
                doc = json.loads(chunk.decode("utf-8"))
            elif kind == 0x004E4942:
                blob = chunk
            at += 8 + length
        if doc is None:
            raise ObjError("the model file has no scene")
        buffers = [blob]
    else:
        try:
            doc = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            raise ObjError("the model file is not glTF") from exc
        buffers = []
        for buffer in doc.get("buffers") or []:
            uri = str(buffer.get("uri") or "")
            if not uri.startswith("data:"):
                raise ObjError("a .gltf must carry its data inside (or use .glb)")
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))

    def accessor(index: int) -> np.ndarray:
        acc = doc["accessors"][index]
        view = doc["bufferViews"][acc["bufferView"]]
        fmt, size = _COMPONENTS[acc["componentType"]]
        width = _WIDTH[acc["type"]]
        start = int(view.get("byteOffset", 0)) + int(acc.get("byteOffset", 0))
        stride = int(view.get("byteStride", 0)) or size * width
        raw = buffers[view.get("buffer", 0)]
        count = int(acc["count"])
        if stride == size * width:
            values = np.frombuffer(raw, dtype=np.dtype("<" + fmt), count=count * width, offset=start)
        else:
            values = np.array([struct.unpack_from("<" + fmt * width, raw, start + i * stride) for i in range(count)]).flatten()
        return values.reshape(count, width).astype(float)

    def node_matrix(node: dict) -> np.ndarray:
        if node.get("matrix"):
            return np.array(node["matrix"], dtype=float).reshape(4, 4).T
        m = np.eye(4)
        t = node.get("translation") or [0, 0, 0]
        x, y, z, w = node.get("rotation") or [0, 0, 0, 1]
        rot = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        s = node.get("scale") or [1, 1, 1]
        m[:3, :3] = rot * np.array(s)
        m[:3, 3] = t
        return m

    verts: list[np.ndarray] = []
    faces: list[list[int]] = []

    def visit(index: int, parent: np.ndarray) -> None:
        node = doc["nodes"][index]
        m = parent @ node_matrix(node)
        if node.get("mesh") is not None:
            for primitive in doc["meshes"][node["mesh"]].get("primitives") or []:
                if primitive.get("mode", 4) != 4 or "POSITION" not in (primitive.get("attributes") or {}):
                    continue
                pos = accessor(primitive["attributes"]["POSITION"])
                world = (np.hstack([pos, np.ones((len(pos), 1))]) @ m.T)[:, :3]
                base = sum(len(v) for v in verts)
                idx = accessor(primitive["indices"]).astype(int).flatten() if primitive.get("indices") is not None else np.arange(len(pos))
                verts.append(world)
                faces.extend([[base + int(idx[k]), base + int(idx[k + 1]), base + int(idx[k + 2])] for k in range(0, len(idx) - 2, 3)])
                if len(faces) > MAX_FACES:
                    raise ObjError(f"the model has too many faces (at most {MAX_FACES})")
        for child in node.get("children") or []:
            visit(child, m)

    scene = (doc.get("scenes") or [{"nodes": list(range(len(doc.get("nodes") or [])))}])[int(doc.get("scene", 0))]
    for root in scene.get("nodes") or []:
        visit(root, np.eye(4))
    if not verts or not faces:
        raise ObjError("the OBJ file has no faces")
    v = np.vstack(verts) * np.array([1.0, -1.0, -1.0])  # (glTF: y up, z toward the viewer)
    lo, hi = v.min(axis=0), v.max(axis=0)
    span = float((hi - lo).max()) or 1.0
    v = (v - (lo + hi) / 2) / span
    return {"v": [round(float(c), 5) for c in v.flatten()], "f": faces, "ratio": [round(float(r), 4) for r in (hi - lo) / span]}
