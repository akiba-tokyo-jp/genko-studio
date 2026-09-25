"""J8 ops for 3D with surfaces: the posable figure (体型・関節・手), heads and hands, imported models (OBJ), the
page's camera and light, and 3D turned into pen lines and shaded surfaces on a layer (LT 変換・面のトーン)."""

from __future__ import annotations

import math
from typing import Any

import numpy as np

from genko import mesh3d
from genko.models import new_id
from genko.ops import ApplyError

OPS = ("add_figure", "pose_figure", "add_head", "add_hand", "import_model", "set_camera", "set_light", "render_prims")
BODY_LIMITS = {"heads": (4.0, 10.0), "shoulders": (0.6, 1.5), "hips": (0.6, 1.6), "build": (0.5, 1.8), "legs": (0.6, 1.5)}
AXES = ("x", "y", "z")
# the point people drag → (the joint that turns, the bone's start, its end)
HANDLES = {
    "neck": ("spine", "pelvis", "neck"), "head": ("neck", "neck", "head"),
    **{f"{p}_elbow": (f"{p}_arm", f"{p}_shoulder", f"{p}_elbow") for p in "lr"},
    **{f"{p}_wrist": (f"{p}_elbow", f"{p}_elbow", f"{p}_wrist") for p in "lr"},
    **{f"{p}_hand": (f"{p}_wrist", f"{p}_wrist", f"{p}_hand") for p in "lr"},
    **{f"{p}_knee": (f"{p}_leg", f"{p}_hip", f"{p}_knee") for p in "lr"},
    **{f"{p}_ankle": (f"{p}_knee", f"{p}_knee", f"{p}_ankle") for p in "lr"},
    **{f"{p}_toe": (f"{p}_ankle", f"{p}_ankle", f"{p}_toe") for p in "lr"},
}


def _vec(value, n: int = 3, what: str = "pos") -> list[float]:
    try:
        out = [float(v) for v in value][:n]
    except (TypeError, ValueError) as exc:
        raise ApplyError(f"{what} is [x, y, z]") from exc
    if len(out) < 2:
        raise ApplyError(f"{what} is [x, y, z]")
    return out + [0.0] * (n - len(out))


def _body(raw) -> dict:
    if not isinstance(raw, dict):
        raise ApplyError("body is {heads, shoulders, hips, build, legs}")
    out = {}
    for key, value in raw.items():
        if key not in BODY_LIMITS:
            raise ApplyError(f"unknown body key {key} (heads, shoulders, hips, build, legs)")
        lo, hi = BODY_LIMITS[key]
        value = float(value)
        if not lo <= value <= hi:
            raise ApplyError(f"body {key} must be between {lo:g} and {hi:g}")
        out[key] = value
    return out


def _hands(raw) -> dict:
    if not isinstance(raw, dict):
        raise ApplyError("hands is {l: pose, r: pose}")
    out = {}
    for side, pose in raw.items():
        if side not in ("l", "r") or pose not in mesh3d.HAND_POSES:
            raise ApplyError(f"hand poses are {', '.join(mesh3d.HAND_POSES)} (for l and r)")
        out[side] = str(pose)
    return out


def _joints(raw) -> dict:
    if not isinstance(raw, dict):
        raise ApplyError("joints is {name: {x, y, z}}")
    out = {}
    for name, values in raw.items():
        if name not in mesh3d.FIGURE_JOINTS:
            raise ApplyError(f"unknown joint {name}")
        if not isinstance(values, dict) or any(k not in AXES for k in values):
            raise ApplyError("each joint is {x, y, z} in radians")
        out[name] = {k: round(float(v), 4) for k, v in values.items()}
    return out


def _preset(prim: dict, name: str) -> None:
    if name not in mesh3d.FIGURE_PRESETS:
        raise ApplyError(f"preset must be one of {', '.join(mesh3d.FIGURE_PRESETS)}")
    data = mesh3d.FIGURE_PRESETS[name]
    prim["joints"] = {k: dict(v) for k, v in data.items() if k != "hands"}
    prim["hands"] = dict(data.get("hands") or {})
    prim["preset"] = name


def _find(page, prim_id, kinds=None) -> dict:
    prim = next((p for p in page.prims if p.get("id") == prim_id), None)
    if prim is None or (kinds and prim.get("kind") not in kinds):
        raise ApplyError(f"no {'/'.join(kinds) if kinds else '3D'} {prim_id}")
    return prim


def _frame(page, op: dict, prim: dict) -> None:
    if op.get("frame_id"):
        from genko.ops import _guide_frame

        frame_id = _guide_frame(page, op, prim["pos"])
        if frame_id:
            prim["frame_id"] = frame_id


def _page_of_joint(prim: dict, name: str, camera) -> np.ndarray:
    pts = mesh3d.figure_skeleton(prim)["points"]
    page, _depth = mesh3d.to_page(prim, np.array([pts[name]]), camera)
    return page[0]


def drag_joint(prim: dict, handle: str, target, camera=None) -> dict:
    """The joint change that points the dragged bone at `target` (page mm): the turn about whichever axis
    moves it most in the picture, found by a few Newton steps."""
    if handle == "pelvis":
        pos = list(prim.get("pos") or [100, 160, 0]) + [0, 0, 0]
        return {"pos": [round(float(target[0]), 3), round(float(target[1]), 3), pos[2]]}
    if handle not in HANDLES:
        raise ApplyError(f"handle must be pelvis or one of {', '.join(HANDLES)}")
    joint, start, end = HANDLES[handle]
    goal = math.atan2(float(target[1]) - _page_of_joint(prim, start, camera)[1], float(target[0]) - _page_of_joint(prim, start, camera)[0])

    def angle(p: dict) -> float:
        a, b = _page_of_joint(p, start, camera), _page_of_joint(p, end, camera)
        return math.atan2(b[1] - a[1], b[0] - a[0])

    def with_value(axis: str, value: float) -> dict:
        joints = {k: dict(v) for k, v in (prim.get("joints") or {}).items()}
        joints.setdefault(joint, {})[axis] = value
        return {**prim, "joints": joints}

    current = dict((prim.get("joints") or {}).get(joint) or {})
    best_axis, best_slope = "z", 0.0
    for axis in ("z", "x"):
        base = float(current.get(axis, 0.0))
        slope = math.remainder(angle(with_value(axis, base + 0.05)) - angle(with_value(axis, base)), math.tau) / 0.05
        if abs(slope) > abs(best_slope):
            best_axis, best_slope = axis, slope
    value = float(current.get(best_axis, 0.0))
    for _ in range(6):
        now = angle(with_value(best_axis, value))
        miss = math.remainder(goal - now, math.tau)
        if abs(miss) < 0.002:
            break
        slope = math.remainder(angle(with_value(best_axis, value + 0.02)) - now, math.tau) / 0.02
        if abs(slope) < 1e-4:
            break
        value += max(-0.8, min(0.8, miss / slope))
    return {"joints": {joint: {best_axis: round(math.remainder(value, math.tau), 4)}}}


def figure_handles(prim: dict, camera=None) -> list[tuple[str, tuple[float, float]]]:
    pts = mesh3d.figure_skeleton(prim)["points"]
    names = ["pelvis", *HANDLES]
    page, _depth = mesh3d.to_page(prim, np.array([pts[n] for n in names]), camera)
    return [(n, (float(p[0]), float(p[1]))) for n, p in zip(names, page)]


def apply(episode, op: dict[str, Any], name: str) -> None:
    from genko.ops import _require_page

    page = _require_page(episode, op)
    if name == "add_figure":
        height = float(op.get("height_mm") or 90)
        if not 10 <= height <= 400:
            raise ApplyError("height_mm must be between 10 and 400")
        prim = {"id": str(op.get("id") or new_id()), "kind": "figure", "pos": _vec(op.get("pos") or [100, 160, 0]),
                "size": [height / 2, height, height / 4], "rot": _vec(op.get("rot") or [0, 0, 0], what="rot"),
                "body": _body(op.get("body") or {}), "joints": {}, "hands": {}}
        if op.get("focal_mm"):
            prim["focal_mm"] = max(20.0, float(op["focal_mm"]))
        if op.get("preset"):
            _preset(prim, str(op["preset"]))
        if op.get("joints"):
            prim["joints"].update(_joints(op["joints"]))
        if op.get("hands"):
            prim["hands"].update(_hands(op["hands"]))
        if any(p.get("id") == prim["id"] for p in page.prims):
            raise ApplyError(f"3D {prim['id']} exists")
        _frame(page, op, prim)
        page.prims.append(prim)
        return
    if name == "pose_figure":
        prim = _find(page, op.get("id"), ("figure", "hand"))
        if prim["kind"] == "hand":
            if op.get("pose"):
                if op["pose"] not in mesh3d.HAND_POSES:
                    raise ApplyError(f"hand poses are {', '.join(mesh3d.HAND_POSES)} (for l and r)")
                prim["pose"] = str(op["pose"])
            for key in ("rot", "pos"):
                if op.get(key) is not None:
                    prim[key] = _vec(op[key], what=key)
            return
        if op.get("preset"):
            _preset(prim, str(op["preset"]))
        if "set_joints" in op:
            prim["joints"] = _joints(op["set_joints"] or {})
        if op.get("joints"):
            joints = prim.setdefault("joints", {})
            for joint, values in _joints(op["joints"]).items():
                joints.setdefault(joint, {}).update(values)
        if op.get("body"):
            prim["body"] = {**(prim.get("body") or {}), **_body(op["body"])}
        if op.get("hands"):
            prim["hands"] = {**(prim.get("hands") or {}), **_hands(op["hands"])}
        for key in ("rot", "pos"):
            if op.get(key) is not None:
                prim[key] = _vec(op[key], what=key)
        if op.get("height_mm"):
            height = float(op["height_mm"])
            prim["size"] = [height / 2, height, height / 4]
        if op.get("drag"):
            drag = dict(op["drag"])
            camera = (page.extra or {}).get("camera")
            change = drag_joint(prim, str(drag.get("handle")), _vec(drag.get("to") or [0, 0], 2, "to"), camera)
            if "pos" in change:
                prim["pos"] = change["pos"]
            for joint, values in change.get("joints", {}).items():
                prim.setdefault("joints", {}).setdefault(joint, {}).update(values)
            prim.pop("preset", None)
        return
    if name in ("add_head", "add_hand"):
        size = float(op.get("size_mm") or (30 if name == "add_head" else 25))
        prim = {"id": str(op.get("id") or new_id()), "kind": "head" if name == "add_head" else "hand",
                "pos": _vec(op.get("pos") or [100, 100, 0]), "size": [size, size, size], "rot": _vec(op.get("rot") or [0, 0, 0], what="rot")}
        if name == "add_hand":
            side = str(op.get("side") or "r")
            if side not in ("l", "r"):
                raise ApplyError("side is l or r")
            pose = str(op.get("pose") or "relaxed")
            if pose not in mesh3d.HAND_POSES:
                raise ApplyError(f"hand poses are {', '.join(mesh3d.HAND_POSES)} (for l and r)")
            prim.update(side=side, pose=pose)
        _frame(page, op, prim)
        page.prims.append(prim)
        return
    if name == "import_model":
        text = op.get("obj")
        if not isinstance(text, str) or not text.strip():
            raise ApplyError("obj is the model's OBJ text")
        try:
            mesh = mesh3d.read_obj(text)
        except (mesh3d.ObjError, ValueError, IndexError) as exc:
            raise ApplyError(str(exc)) from exc
        longest = float(op.get("size_mm") or 60)
        ratio = mesh.pop("ratio")
        prim = {"id": str(op.get("id") or new_id()), "kind": "mesh", "pos": _vec(op.get("pos") or [100, 150, 0]),
                "size": [longest, longest, longest], "rot": _vec(op.get("rot") or [0.2, 0.5, 0], what="rot"), "mesh": mesh,
                "title": str(op.get("name") or "モデル"), "ratio": ratio}
        _frame(page, op, prim)
        page.prims.append(prim)
        return
    if name == "set_camera":
        if op.get("off"):
            page.extra.pop("camera", None)
            return
        camera = dict(page.extra.get("camera") or {})
        for key in ("turn", "tip", "roll"):
            if op.get(key) is not None:
                camera[key] = round(float(op[key]), 4)
        if op.get("focal_mm") is not None:
            focal = float(op["focal_mm"])
            if not 20 <= focal <= 5000:
                raise ApplyError("focal_mm must be between 20 and 5000")
            camera["focal_mm"] = focal
        if "target" in op:
            camera["target"] = _vec(op["target"], 2, "target") if op["target"] else None
        camera.setdefault("target", [page.spec.width_mm / 2, page.spec.height_mm / 2])
        page.extra["camera"] = camera
        return
    if name == "set_light":
        light = dict(page.extra.get("light") or {})
        if op.get("dir") is not None:
            d = _vec(op["dir"], 3, "dir")
            if math.hypot(*d) < 1e-6:
                raise ApplyError("dir must point somewhere")
            light["dir"] = d
        if op.get("ambient") is not None:
            light["ambient"] = max(0.0, min(1.0, float(op["ambient"])))
        page.extra["light"] = light
        return
    if name == "render_prims":
        render_prims(episode, page, op)
        return


def render_prims(episode, page, op: dict) -> None:
    """3D into drawing: the pen lines on the layer (as pen lines), and the shaded surfaces (as greys, which a
    tone-ized layer prints as dots: 面のトーン化)."""
    from genko import prim3d
    from genko import raster as rasters
    from genko.models import coerce_stroke
    from genko.ops import _brush_kind, _paint_target

    target = _paint_target(page, op)
    ids = set(op.get("ids") or [])
    chosen = [mesh3d.with_camera(p, page) for p in page.prims if (not ids or p.get("id") in ids) and p.get("kind") != "mannequin"]
    if not chosen:
        raise ApplyError("no 3D figure or box to trace")
    if op.get("surfaces", True):
        light = dict(page.extra.get("light") or {})
        if op.get("light") is not None:
            light["dir"] = _vec(op["light"], 3, "light")
        ambient = float(op.get("ambient", light.get("ambient", 0.35)))
        dpi = rasters.WORKING_DPI
        size = (round(page.spec.width_mm / 25.4 * dpi), round(page.spec.height_mm / 25.4 * dpi))
        camera = (page.extra or {}).get("camera")
        shade, _z, alpha = mesh3d.raster([{k: v for k, v in p.items() if k != "camera"} for p in chosen], size, dpi, camera,
                                         light.get("dir"), ambient)
        if not alpha.any():
            raise ApplyError("the 3D has no surfaces to shade here")
        from PIL import Image

        grey = np.where(alpha, np.clip(shade * 255, 0, 255), 0).astype("uint8")
        rgba = np.dstack([grey, grey, grey, (alpha * 255).astype("uint8")])
        picture = Image.fromarray(rgba, "RGBA")
        base = rasters.ensure_raster(page, target) if target.raster_png else Image.new("RGBA", size, (0, 0, 0, 0))
        base = base.convert("RGBA").resize(size)
        base.alpha_composite(picture)
        rasters.save_raster(page, target, base)
        if op.get("tone"):
            from genko.ops import _screen_spec

            target.screen = _screen_spec({"pattern": "dot", **dict(op["tone"])})
    if op.get("lines", True):
        kind = _brush_kind(op.get("kind") or "mili", episode)
        for prim in chosen:
            for line in prim3d.trace(prim):
                stroke = coerce_stroke([(round(x, 3), round(y, 3)) for x, y in line])
                stroke.kind = kind
                stroke.width_mm = float(op.get("width_mm") or 0.3)
                if op.get("rgb"):
                    stroke.rgb = tuple(int(v) for v in op["rgb"])
                target.strokes.append(stroke)
