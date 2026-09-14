from __future__ import annotations

from PIL import Image, ImageChops, ImageDraw, ImageFont

from genko.models import Episode, LayerRole, Page, Rect

EXPORT_ROLES = (
    LayerRole.INK,
    LayerRole.BG,
    LayerRole.FINISH,
    LayerRole.FRAMES,
    LayerRole.TEXT,
)

NAME_COLOR = (58, 110, 165)
INK_COLOR = (20, 20, 20)


def export_plan(page: Page) -> list[LayerRole]:
    return list(EXPORT_ROLES)


def mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def rect_px(rect: Rect, dpi: int) -> tuple[int, int, int, int]:
    x = mm_to_px(rect.x, dpi)
    y = mm_to_px(rect.y, dpi)
    w = mm_to_px(rect.width, dpi)
    h = mm_to_px(rect.height, dpi)
    return x, y, x + w, y + h


def _stroke(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    dpi: int,
    color: tuple[int, int, int],
    width: int,
) -> None:
    if len(points) < 2:
        return
    xy = [(mm_to_px(x, dpi), mm_to_px(y, dpi)) for x, y in points]
    draw.line(xy, fill=color, width=width, joint="curve")


def _clip_mask(page: Page, size: tuple[int, int], dpi: int) -> Image.Image | None:
    leaves = [frame for frame in page.leaf_frames() if getattr(frame, "clip", True)]
    if not leaves:
        return None
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    for frame in leaves:
        draw.rectangle(rect_px(frame.rect, dpi), fill=255)
    return mask


def _and_alpha(layer: Image.Image, mask: Image.Image) -> Image.Image:
    alpha = layer.split()[3]
    return ImageChops.multiply(alpha, mask)


def render_page(
    page: Page,
    working_dpi: int,
    mode: str = "print",
    episode: Episode | None = None,
) -> Image.Image:
    width = mm_to_px(page.spec.width_mm, working_dpi)
    height = mm_to_px(page.spec.height_mm, working_dpi)
    size = (width, height)
    image = Image.new("RGB", size, (255, 255, 255))
    include_name = mode in ("name", "proof")

    fill_roles = (LayerRole.BG, LayerRole.INK, LayerRole.FINISH)
    if include_name:
        fill_roles = (LayerRole.BG, LayerRole.NAME, LayerRole.INK, LayerRole.FINISH)
    for role in fill_roles:
        fill = page.fills.get(role)
        if fill is None:
            continue
        if role in (LayerRole.NAME, LayerRole.DRAFT) and mode == "print":
            continue
        image.paste(Image.new("RGB", size, fill), (0, 0))

    ink_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    name_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    _stroke_draw = ImageDraw.Draw(ink_layer)
    _name_draw = ImageDraw.Draw(name_layer)
    for stroke in page.ink_strokes:
        _stroke(_stroke_draw, stroke, working_dpi, INK_COLOR, 3)
    if include_name:
        for stroke in page.name_strokes:
            _stroke(_name_draw, stroke, working_dpi, NAME_COLOR, 3)

    mask = _clip_mask(page, size, working_dpi)
    if mask is not None:
        ink_layer.putalpha(_and_alpha(ink_layer, mask))
        if include_name:
            name_layer.putalpha(_and_alpha(name_layer, mask))

    rgba = image.convert("RGBA")
    if include_name:
        rgba = Image.alpha_composite(rgba, name_layer)
    rgba = Image.alpha_composite(rgba, ink_layer)
    image = rgba.convert("RGB")

    draw = ImageDraw.Draw(image)
    for frame in page.leaf_frames():
        draw.rectangle(rect_px(frame.rect, working_dpi), outline=(20, 20, 20), width=2)

    try:
        font = ImageFont.load_default()
    except OSError:
        font = None
    x = mm_to_px(page.inner_rect_mm().x + 4, working_dpi)
    y = mm_to_px(page.inner_rect_mm().y + 4, working_dpi)
    lines = episode.story_for_page(page.index) if episode is not None else page.texts
    for line in lines:
        label = f"{line.speaker}: {line.text}" if line.speaker else line.text
        tx = mm_to_px(line.x_mm, working_dpi) if line.x_mm else x
        ty = mm_to_px(line.y_mm, working_dpi) if line.y_mm else y
        draw.text((tx, ty), label, fill=(10, 10, 10), font=font)
        y += 14
    return image
