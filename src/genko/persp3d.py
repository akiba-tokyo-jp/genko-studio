"""3D and the perspective ruler together (3D カメラとパース定規の連動): the vanishing points a 3D object (or the
page's camera) makes, as a perspective ruler; and the camera that puts the 3D where a perspective ruler says.

A direction u in a prim's own space is seen as r = view · turn · u (view: the page camera's turn, when there is
one). Lines along it meet on the page at c + focal · (r_x, r_y) / r_z, c being the camera's target (or, without a
camera, the prim's own centre). Axes seen straight across (r_z ≈ 0) have no vanishing point: they stay parallel.
"""

from __future__ import annotations

import math

import numpy as np

FAR_MM = 20000.0  # vanishing points further out than this count as none (the lines look parallel)
AXES = {"x": (1.0, 0.0, 0.0), "z": (0.0, 0.0, 1.0), "y": (0.0, 1.0, 0.0)}  # across, into the page, upright


def _parts(prim: dict, camera: dict | None):
    from genko.mesh3d import prim_rotation

    turn = prim_rotation(prim.get("rot") if prim.get("rot") is not None else ([0, 0, 0] if camera else [0.3, 0.6, 0]))
    if camera:
        view = prim_rotation([camera.get("tip", 0), camera.get("turn", 0), camera.get("roll", 0)])
        target = camera.get("target") or (prim.get("pos") or [100, 150])[:2]
        focal = float(camera.get("focal_mm") or prim.get("focal_mm") or 400)
        return view @ turn, (float(target[0]), float(target[1])), focal
    pos = (list(prim.get("pos") or [100, 150, 0]) + [0, 0, 0])[:3]
    return turn, (float(pos[0]), float(pos[1])), float(prim.get("focal_mm", 400) or 400)


def vanishing_points(prim: dict, camera: dict | None = None) -> dict[str, tuple[float, float]]:
    """{axis: (x, y) mm} for the prim's axes that meet on the page (x across, z into the page, y upright)."""
    rotation, (cx, cy), focal = _parts(prim, camera)
    out = {}
    for name, u in AXES.items():
        r = rotation @ np.array(u)
        if abs(r[2]) < 1e-4:
            continue
        x, y = cx + focal * r[0] / r[2], cy + focal * r[1] / r[2]
        if math.hypot(x - cx, y - cy) <= FAR_MM:
            out[name] = (round(float(x), 2), round(float(y), 2))
    return out


def ruler_from(prim: dict, camera: dict | None = None) -> dict | None:
    """A perspective ruler through the vanishing points (1 to 3), in the order x, z, y; None when every axis is
    seen straight on."""
    vps = vanishing_points(prim, camera)
    points = [list(vps[k]) for k in ("x", "z", "y") if k in vps]
    if not points:
        return None
    return {"kind": "perspective", "points": points[:3], "lock_horizon": len(points) >= 2}


def camera_for(ruler: dict, prim: dict, camera: dict | None = None) -> dict:
    """The page camera (tip, turn, roll, focal) whose vanishing points for this prim sit on the ruler's: one
    point is the depth axis, two are across and depth, three add the upright. The target stays."""
    goal = [tuple(map(float, p[:2])) for p in ruler.get("points") or []]
    if not goal:
        raise ValueError("the perspective ruler has no vanishing points")
    names = ["z"] if len(goal) == 1 else ["x", "z", "y"][: len(goal)]
    base = dict(camera or {})
    target = base.get("target") or (prim.get("pos") or [100, 150])[:2]
    flat = {**prim, "rot": [0, 0, 0]}

    def cost(v) -> float:
        tip, turn, roll, log_f = v
        cam = {**base, "tip": tip, "turn": turn, "roll": roll if len(goal) >= 3 else 0.0,
               "focal_mm": math.exp(log_f), "target": target}
        vps = vanishing_points(flat, cam)
        total = 0.0
        for name, (gx, gy) in zip(names, goal):
            if name not in vps:
                total += 1e6
                continue
            total += math.hypot(vps[name][0] - gx, vps[name][1] - gy)
        if len(goal) == 1:  # (one point: the across and upright axes stay square to the page)
            total += 50 * (abs(roll) + abs(tip) * 0.2)
        return total

    best = None
    for turn in (-1.2, -0.6, 0.0, 0.6, 1.2):
        for tip in (-0.3, 0.0, 0.3):
            start = np.array([tip, turn, 0.0, math.log(float(base.get("focal_mm") or 400))])
            found = _minimize(cost, start)
            if best is None or cost(found) < cost(best):
                best = found
    tip, turn, roll, log_f = best
    return {**base, "tip": round(float(tip), 4), "turn": round(float(turn), 4), "roll": round(float(roll) if len(goal) >= 3 else 0.0, 4),
            "focal_mm": round(float(min(5000.0, max(20.0, math.exp(log_f)))), 1), "target": [float(target[0]), float(target[1])]}


def _minimize(f, x0, steps: int = 400):
    """Nelder–Mead (small and enough for four numbers)."""
    n = len(x0)
    simplex = [np.array(x0, dtype=float)]
    for i in range(n):
        x = np.array(x0, dtype=float)
        x[i] += 0.25
        simplex.append(x)
    values = [f(x) for x in simplex]
    for _ in range(steps):
        order = np.argsort(values)
        simplex = [simplex[i] for i in order]
        values = [values[i] for i in order]
        if values[-1] - values[0] < 1e-6:
            break
        centre = sum(simplex[:-1]) / n
        worst = simplex[-1]
        reflected = centre + (centre - worst)
        fr = f(reflected)
        if fr < values[0]:
            expanded = centre + 2 * (centre - worst)
            fe = f(expanded)
            simplex[-1], values[-1] = (expanded, fe) if fe < fr else (reflected, fr)
        elif fr < values[-2]:
            simplex[-1], values[-1] = reflected, fr
        else:
            contracted = centre + 0.5 * (worst - centre)
            fc = f(contracted)
            if fc < values[-1]:
                simplex[-1], values[-1] = contracted, fc
            else:
                best = simplex[0]
                simplex = [best + 0.5 * (x - best) for x in simplex]
                values = [f(x) for x in simplex]
    return simplex[int(np.argmin(values))]
