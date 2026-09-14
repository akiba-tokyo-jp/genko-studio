from __future__ import annotations

from PIL import ImageDraw

def _mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def pack_point(x_mm: float, y_mm: float, pressure: float | None = None) -> list[float]:
    value = 0.7 if pressure is None else max(0.05, min(1.0, float(pressure)))
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
