from __future__ import annotations

from PIL import Image, ImageFilter, ImageOps


def to_line_art(image: Image.Image, method: str = "adaptive") -> Image.Image:
    gray = image.convert("L")
    if method == "edges":
        return ImageOps.invert(gray.filter(ImageFilter.FIND_EDGES))
    if method == "sobel":
        return ImageOps.invert(gray.filter(ImageFilter.FIND_EDGES))
    raw = gray.tobytes()
    mean = sum(raw) / max(1, len(raw))
    threshold = max(8, mean - 12)
    return gray.point(lambda p: 0 if p < threshold else 255)


def runs_to_strokes(binary: Image.Image, width_mm: float, height_mm: float) -> list[list[tuple[float, float]]]:
    sx = width_mm / max(1, binary.width)
    sy = height_mm / max(1, binary.height)
    strokes: list[list[tuple[float, float]]] = []
    for y in range(binary.height):
        run: list[tuple[float, float]] = []
        for x in range(binary.width):
            if binary.getpixel((x, y)) < 80:
                run.append((x * sx, y * sy))
            elif len(run) >= 2:
                strokes.append(run)
                run = []
            else:
                run = []
        if len(run) >= 2:
            strokes.append(run)
    return strokes
