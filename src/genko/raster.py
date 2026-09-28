from __future__ import annotations

import io

from PIL import Image, ImageDraw, ImageChops

from genko.models import Layer, LayerKind, Page
from genko.render import mm_to_px
from genko.stroke import stamp_polyline

WORKING_DPI = 200


def ensure_raster(page: Page, layer: Layer, dpi: int = WORKING_DPI) -> Image.Image:
    if layer.raster_png:
        return Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
    width = mm_to_px(page.spec.width_mm, dpi)
    height = mm_to_px(page.spec.height_mm, dpi)
    return Image.new("RGBA", (width, height), (0, 0, 0, 0))


def save_raster(page: Page, layer: Layer, image: Image.Image) -> None:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    layer.raster_png = buf.getvalue()
    layer.raster_relpath = f"pages/{page.index:03d}/{layer.role.value}.png"


def _clip(page: Page, image: Image.Image, dpi: int) -> Image.Image:
    leaves = [frame for frame in page.leaf_frames() if getattr(frame, "clip", True)]
    if not leaves:
        return image
    mask = Image.new("L", image.size, 0)
    draw = ImageDraw.Draw(mask)
    from genko.render import fill_frame

    for frame in leaves:
        fill_frame(draw, frame, dpi)
    alpha = image.split()[3]
    image.putalpha(ImageChops.multiply(alpha, mask))
    return image


def bake_stroke(
    page: Page,
    layer: Layer,
    points: list,
    dpi: int = WORKING_DPI,
    rgb: tuple[int, int, int] = (20, 20, 20),
    kind: str = "gpen",
    width_mm: float = 0.35,
) -> None:
    image = ensure_raster(page, layer, dpi)
    old_alpha = image.split()[3]
    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    alpha = 140 if kind == "oil" else 255
    stamp_polyline(draw, points, dpi, width_mm, fill=(*rgb, alpha), coords="mm")
    image = Image.alpha_composite(image, overlay)
    if getattr(layer, "lock_alpha", False):
        r, g, b, a = image.split()
        image.putalpha(ImageChops.multiply(a, old_alpha))
    image = _clip(page, image, dpi)
    save_raster(page, layer, image)
    if layer.kind == LayerKind.STROKES:
        layer.kind = LayerKind.RASTER


def erase_raster(page: Page, layer: Layer, points: list, width_mm: float = 2.0, dpi: int = WORKING_DPI,
                 texture: str = "") -> None:
    """The eraser on paint: hard (a clean edge), soft (軟らかめ: the edge fades out) or rough (粗め: a grain
    left behind, as a dry eraser leaves)."""
    if not layer.raster_png:
        from genko.models import stroke_points

        for stroke in layer.strokes:
            bake_stroke(page, layer, stroke_points(stroke), dpi)
    image = ensure_raster(page, layer, dpi)
    if texture not in ("soft", "rough"):
        draw = ImageDraw.Draw(image)
        stamp_polyline(draw, points, dpi, width_mm, fill=(0, 0, 0, 0), coords="mm")
        save_raster(page, layer, image)
        return
    import random

    from PIL import ImageFilter

    mask = Image.new("L", image.size, 0)
    width = width_mm * (0.7 if texture == "soft" else 1.0)
    stamp_polyline(ImageDraw.Draw(mask), points, dpi, width, fill=255, coords="mm")
    if texture == "soft":
        mask = mask.filter(ImageFilter.GaussianBlur(max(1.0, width_mm / 25.4 * dpi / 4)))
    else:
        rng = random.Random(f"{layer.id}{points[0] if points else ''}")
        grain = max(1, round(dpi / 100))
        small = Image.new("L", (max(1, mask.width // grain), max(1, mask.height // grain)))
        small.putdata([255 if rng.random() < 0.7 else 0 for _ in range(small.width * small.height)])
        mask = ImageChops.multiply(mask, small.resize(mask.size, Image.Resampling.NEAREST))
    alpha = image.split()[3]
    image.putalpha(ImageChops.subtract(alpha, mask))
    save_raster(page, layer, image)
