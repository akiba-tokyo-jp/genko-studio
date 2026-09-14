from __future__ import annotations

from PIL import ImageDraw


def _mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def stabilize_points(points: list, window: int = 5) -> list:
    if window < 3 or len(points) < 3:
        return points
    half = max(1, int(window) // 2)
    out: list = []
    for i, point in enumerate(points):
        sl = points[max(0, i - half) : min(len(points), i + half + 1)]
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
