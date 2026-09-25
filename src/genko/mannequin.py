"""A posable stick mannequin (2D, with a little depth), shared by the name and proof renders
and the pose guide for image tools.

The figure stands on `pos` (the pelvis, in page mm). `size[1]` is its height in mm; the body
is eight heads tall. Every joint has `yaw` (bend in the picture plane, radians, positive =
counter-clockwise on the page) and `pitch` (bend toward or away from the viewer, which only
shortens the segment by cos(pitch)). The whole figure turns with `rot`:
rot[2] leans it in the picture plane, rot[1] turns it about its vertical axis (the shoulders
and hips narrow by |cos| and the facing flips past 90°), rot[0] tips it toward the viewer
(the body shortens by cos).
"""

from __future__ import annotations

import math

JOINTS = ("hip", "spine", "neck", "head", "l_arm", "r_arm", "l_elbow", "r_elbow", "l_wrist", "r_wrist",
          "l_leg", "r_leg", "l_knee", "r_knee", "l_ankle", "r_ankle")

PROFILE = [0, math.pi / 2, 0]  # seen from the side, facing +x (the page's right)

PRESETS: dict[str, dict] = {
    "stand": {},
    "walk": {"rot": PROFILE, "l_leg": {"yaw": 0.4}, "l_knee": {"yaw": -0.1}, "r_leg": {"yaw": -0.35}, "r_knee": {"yaw": -0.5},
             "l_arm": {"yaw": -0.4}, "l_elbow": {"yaw": 0.2}, "r_arm": {"yaw": 0.35}, "r_elbow": {"yaw": 0.45}},
    "run": {"rot": PROFILE, "spine": {"yaw": -0.3}, "l_leg": {"yaw": 0.9}, "l_knee": {"yaw": -1.2}, "r_leg": {"yaw": -0.6},
            "r_knee": {"yaw": -1.1}, "l_arm": {"yaw": -0.8}, "l_elbow": {"yaw": 1.2}, "r_arm": {"yaw": 0.9}, "r_elbow": {"yaw": 1.3}},
    "sit": {"rot": PROFILE, "l_leg": {"yaw": 1.5}, "l_knee": {"yaw": -1.5}, "r_leg": {"yaw": 1.45}, "r_knee": {"yaw": -1.45},
            "l_arm": {"yaw": 0.3}, "l_elbow": {"yaw": 0.9}, "r_arm": {"yaw": 0.35}, "r_elbow": {"yaw": 0.9}},
    "point": {"r_arm": {"yaw": -1.55}, "r_elbow": {"yaw": 0.05}, "head": {"yaw": -0.15}},
    "look_back": {"rot": [0, 2.6, 0], "neck": {"yaw": 0.35}, "head": {"yaw": 0.3}},
    "arms_up": {"l_arm": {"yaw": 2.8}, "r_arm": {"yaw": -2.8}, "l_elbow": {"yaw": 0.2}, "r_elbow": {"yaw": -0.2}},
}


def default_joints() -> dict[str, dict[str, float]]:
    base = {name: {"yaw": 0.0, "pitch": 0.0} for name in JOINTS}
    base["l_arm"]["yaw"], base["r_arm"]["yaw"] = 0.25, -0.25
    base["l_leg"]["yaw"], base["r_leg"]["yaw"] = 0.08, -0.08
    return base


def apply_preset(prim: dict, name: str) -> None:
    if name not in PRESETS:
        raise ValueError(f"preset must be one of {', '.join(PRESETS)}")
    joints = default_joints()
    for key, values in PRESETS[name].items():
        if key == "rot":
            prim["rot"] = list(values)
        else:
            joints[key] = {**joints[key], **values}
    prim["joints"] = joints
    prim["preset"] = name


def _joint(joints: dict, name: str) -> tuple[float, float]:
    slot = joints.get(name) or {}
    return float(slot.get("yaw", 0.0)), float(slot.get("pitch", 0.0))


def skeleton(prim: dict) -> dict:
    """Points (page mm) of the figure: {"segments": [(a, b, part)], "head": (centre, radius), "facing": ±1,
    "bbox": (x, y, w, h)}. part is "body", "left" or "right" (the figure's own sides)."""
    x0, y0 = (float(v) for v in (prim.get("pos") or [100, 160, 0])[:2])
    size = prim.get("size") or [40, 80, 20]
    height = float(size[1] if isinstance(size, (list, tuple)) else size) or 80.0
    rot = list(prim.get("rot") or [0, 0, 0]) + [0, 0, 0]
    tip, turn, lean = float(rot[0]), float(rot[1]), float(rot[2])
    joints = {**default_joints(), **(prim.get("joints") or {})}
    unit = height / 8.0  # one head
    narrow = abs(math.cos(turn))  # widths as seen when the figure turns
    squash = max(0.2, abs(math.cos(tip)))
    facing = 1 if math.cos(turn) >= 0 else -1

    def step(point, angle, length, pitch=0.0):
        # angle 0 points down the page; positive angles turn counter-clockwise (toward +x at the bottom)
        length *= max(0.15, abs(math.cos(pitch)))
        a = angle + lean
        return (point[0] + math.sin(a) * length, point[1] + math.cos(a) * length * squash)

    segments = []
    named: dict[str, tuple[float, float]] = {}
    hip_yaw, _ = _joint(joints, "hip")
    spine_yaw, spine_pitch = _joint(joints, "spine")
    pelvis = (x0, y0)
    chest = step(pelvis, math.pi + spine_yaw + hip_yaw, unit * 3.0, spine_pitch)
    segments.append((pelvis, chest, "body"))
    neck_yaw, neck_pitch = _joint(joints, "neck")
    neck_top = step(chest, math.pi + spine_yaw + hip_yaw + neck_yaw, unit * 0.45, neck_pitch)
    segments.append((chest, neck_top, "body"))
    head_yaw, head_pitch = _joint(joints, "head")
    head_r = unit * 0.5
    head_c = step(neck_top, math.pi + spine_yaw + hip_yaw + neck_yaw + head_yaw, head_r, head_pitch)
    named.update(pelvis=pelvis, chest=chest, neck=neck_top, head=head_c)
    shoulder_half = unit * 0.9 * narrow
    hip_half = unit * 0.55 * narrow
    body_angle = spine_yaw + hip_yaw
    across = (math.cos(body_angle + lean), -math.sin(body_angle + lean) * squash)
    for side, sign in (("left", 1), ("right", -1)):
        prefix = "l" if side == "left" else "r"
        # the figure's left is on the viewer's right (+x) when it faces the viewer
        s = sign * facing
        shoulder = (chest[0] + across[0] * shoulder_half * s, chest[1] + across[1] * shoulder_half * s)
        segments.append((chest, shoulder, "body"))
        arm_yaw, arm_pitch = _joint(joints, f"{prefix}_arm")
        elbow_yaw, elbow_pitch = _joint(joints, f"{prefix}_elbow")
        wrist_yaw, _ = _joint(joints, f"{prefix}_wrist")
        elbow = step(shoulder, body_angle + arm_yaw * facing, unit * 1.45, arm_pitch)
        hand = step(elbow, body_angle + (arm_yaw + elbow_yaw) * facing, unit * 1.3, elbow_pitch)
        fingers = step(hand, body_angle + (arm_yaw + elbow_yaw + wrist_yaw) * facing, unit * 0.5)
        segments += [(shoulder, elbow, side), (elbow, hand, side), (hand, fingers, side)]
        named.update({f"{prefix}_shoulder": shoulder, f"{prefix}_elbow": elbow, f"{prefix}_hand": hand, f"{prefix}_fingers": fingers})
        hip = (pelvis[0] + across[0] * hip_half * s, pelvis[1] + across[1] * hip_half * s)
        segments.append((pelvis, hip, "body"))
        leg_yaw, leg_pitch = _joint(joints, f"{prefix}_leg")
        knee_yaw, knee_pitch = _joint(joints, f"{prefix}_knee")
        ankle_yaw, _ = _joint(joints, f"{prefix}_ankle")
        knee = step(hip, hip_yaw + leg_yaw * facing, unit * 2.0, leg_pitch)
        ankle = step(knee, hip_yaw + (leg_yaw + knee_yaw) * facing, unit * 2.0, knee_pitch)
        toe_angle = hip_yaw + (leg_yaw + knee_yaw) * facing - (math.pi / 2 - ankle_yaw) * facing
        toe = step(ankle, toe_angle, unit * 0.6)
        segments += [(hip, knee, side), (knee, ankle, side), (ankle, toe, side)]
        named.update({f"{prefix}_hip": hip, f"{prefix}_knee": knee, f"{prefix}_ankle": ankle, f"{prefix}_toe": toe})
    xs = [p[0] for seg in segments for p in seg[:2]] + [head_c[0] - head_r, head_c[0] + head_r]
    ys = [p[1] for seg in segments for p in seg[:2]] + [head_c[1] - head_r, head_c[1] + head_r]
    return {"segments": segments, "head": (head_c, head_r), "facing": facing, "points": named,
            "bbox": (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))}


# the points people drag, and (the point the segment starts from, the joint that turns it)
HANDLES = {
    "chest": ("pelvis", "spine"), "head": ("neck", "head"),
    **{f"{p}_elbow": (f"{p}_shoulder", f"{p}_arm") for p in "lr"},
    **{f"{p}_hand": (f"{p}_elbow", f"{p}_elbow") for p in "lr"},
    **{f"{p}_fingers": (f"{p}_hand", f"{p}_wrist") for p in "lr"},
    **{f"{p}_knee": (f"{p}_hip", f"{p}_leg") for p in "lr"},
    **{f"{p}_ankle": (f"{p}_knee", f"{p}_knee") for p in "lr"},
    **{f"{p}_toe": (f"{p}_ankle", f"{p}_ankle") for p in "lr"},
}


def pose_to(prim: dict, handle: str, target) -> dict:
    """The joint change ({joint: {"yaw": …}}) that points the dragged part at `target` (page mm);
    "pelvis" moves the whole figure ({"pos": …})."""
    if handle == "pelvis":
        pos = list(prim.get("pos") or [100, 160, 0]) + [0, 0, 0]
        return {"pos": [round(float(target[0]), 3), round(float(target[1]), 3), pos[2]]}
    if handle not in HANDLES:
        raise ValueError(f"handle must be pelvis or one of {', '.join(HANDLES)}")
    bone = skeleton(prim)
    base_name, joint = HANDLES[handle]
    base = bone["points"][base_name]
    rot = list(prim.get("rot") or [0, 0, 0]) + [0, 0, 0]
    lean = float(rot[2])
    squash = max(0.2, abs(math.cos(float(rot[0]))))
    facing = bone["facing"]
    dx, dy = float(target[0]) - base[0], float(target[1]) - base[1]
    if math.hypot(dx, dy) < 1e-6:
        return {}
    a = math.atan2(dx, dy / squash) - lean  # the drawn angle (0 = down the page)
    joints = {**default_joints(), **(prim.get("joints") or {})}
    y = {name: _joint(joints, name)[0] for name in JOINTS}
    body = y["spine"] + y["hip"]

    def wrap(v: float) -> float:
        return math.atan2(math.sin(v), math.cos(v))

    p = handle[0]
    if handle == "chest":
        value = wrap(a - math.pi - y["hip"])
    elif handle == "head":
        value = wrap(a - math.pi - body - y["neck"])
    elif handle.endswith("_elbow"):
        value = wrap((a - body) * facing)
    elif handle.endswith("_hand"):
        value = wrap((a - body) * facing - y[f"{p}_arm"])
    elif handle.endswith("_fingers"):
        value = wrap((a - body) * facing - y[f"{p}_arm"] - y[f"{p}_elbow"])
    elif handle.endswith("_knee"):
        value = wrap((a - y["hip"]) * facing)
    elif handle.endswith("_ankle"):
        value = wrap((a - y["hip"]) * facing - y[f"{p}_leg"])
    else:  # toe
        value = wrap((a - y["hip"]) * facing - y[f"{p}_leg"] - y[f"{p}_knee"] + math.pi / 2)
    return {"joints": {joint: {"yaw": round(value, 4)}}}


def in_rect(prim: dict, rect) -> bool:
    """Whether most of the figure is inside a page rect (x, y, w, h in mm)."""
    x, y, w, h = skeleton(prim)["bbox"]
    cx, cy = x + w / 2, y + h / 2
    rx, ry, rw, rh = rect
    return rx <= cx <= rx + rw and ry <= cy <= ry + rh
