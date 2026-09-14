from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from genko.models import Episode, LayerRole, Page, Rect


EXPORT_ROLES = (
    LayerRole.INK,
    LayerRole.BG,
    LayerRole.FINISH,
    LayerRole.FRAMES,
    LayerRole.TEXT,
)


def export_plan(page: Page) -> list[LayerRole]:
    return list(EXPORT_ROLES)


def _mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def _rect_px(rect: Rect, dpi: int) -> tuple[int, int, int, int]:
    x = _mm_to_px(rect.x, dpi)
    y = _mm_to_px(rect.y, dpi)
    w = _mm_to_px(rect.width, dpi)
    h = _mm_to_px(rect.height, dpi)
    return x, y, x + w, y + h


def render_page(page: Page, working_dpi: int) -> Image.Image:
    width = _mm_to_px(page.spec.width_mm, working_dpi)
    height = _mm_to_px(page.spec.height_mm, working_dpi)
    image = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(image)

    for role in (LayerRole.BG, LayerRole.INK, LayerRole.FINISH):
        fill = page.fills.get(role)
        if fill is None:
            continue
        image.paste(Image.new("RGB", image.size, fill), (0, 0))

    def _stroke(points: list[tuple[float, float]], color: tuple[int, int, int], width: int) -> None:
        if len(points) < 2:
            return
        xy = [
            (_mm_to_px(x, working_dpi), _mm_to_px(y, working_dpi))
            for x, y in points
        ]
        draw.line(xy, fill=color, width=width, joint="curve")

    for stroke in page.ink_strokes:
        _stroke(stroke, (20, 20, 20), 3)

    if LayerRole.FRAMES in EXPORT_ROLES:
        for frame in page.leaf_frames():
            draw.rectangle(_rect_px(frame.rect, working_dpi), outline=(20, 20, 20), width=2)

    try:
        font = ImageFont.load_default()
    except OSError:
        font = None
    y = _mm_to_px(page.inner_rect_mm().y + 4, working_dpi)
    x = _mm_to_px(page.inner_rect_mm().x + 4, working_dpi)
    return image, draw, font, x, y


def export_png_sequence(episode: Episode, dest: Path, working_dpi: int = 150) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for page in episode.pages:
        image, draw, font, x, y = render_page(page, working_dpi)
        for line in episode.story_for_page(page.index):
            label = f"{line.speaker}: {line.text}" if line.speaker else line.text
            draw.text((x, y), label, fill=(10, 10, 10), font=font)
            y += 14
        path = dest / f"{episode.title}_ep{episode.episode:02d}_p{page.index:03d}.png"
        image.save(path)
        written.append(path)
    return written
