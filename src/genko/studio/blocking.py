"""Where characters sit in a panel, derived from the panel brief.

A rough heuristic (M0): enough to keep balloons off faces and to point tails.
"""

from __future__ import annotations

from dataclasses import dataclass

Box = tuple[float, float, float, float]  # x, y, w, h in mm

# Head height as a fraction of panel height, and head centre height, per shot.
_HEAD = {
    "ECU": (0.80, 0.50),
    "CU": (0.55, 0.45),
    "MCU": (0.38, 0.36),
    "MS": (0.26, 0.30),
    "FS": (0.14, 0.22),
    "LS": (0.10, 0.35),
    "ELS": (0.05, 0.45),
}
_POS = {"left": 0.22, "left_third": 0.35, "center": 0.5, "right_third": 0.65, "right": 0.78}


@dataclass(frozen=True)
class Figure:
    char_id: str
    head: Box
    body: Box


def figures(panel: dict, rect: Box) -> list[Figure]:
    x, y, w, h = rect
    size_frac, centre_frac = _HEAD.get(panel.get("shot", "MS"), (0.0, 0.0))
    if not size_frac:
        return []
    out: list[Figure] = []
    for char in panel.get("characters", []):
        scale = min(1.5, max(0.5, 0.5 + float(char.get("scale", 0.8)) * 0.625))
        hh = min(h * 0.95, h * size_frac * scale)
        hw = hh * 0.85
        cx = x + w * _POS.get(char.get("pos", "center"), 0.5)
        cy = y + h * centre_frac
        hx = min(max(x, cx - hw / 2), x + w - hw)
        hy = min(max(y, cy - hh / 2), y + h - hh)
        bw = min(w, hw * 2.4)
        bx = min(max(x, cx - bw / 2), x + w - bw)
        out.append(Figure(char.get("id", ""), (hx, hy, hw, hh), (bx, hy, bw, y + h - hy)))
    return out
