"""Screen outputs: webtoon strips and SNS images (colour or greyscale, never screened).

Pages are trimmed (the bleed is cut off), rendered without the monochrome print
finish (no dots: tones are flat grey, placed art keeps its greys or colours) and
tagged sRGB.

- webtoon: every page at `width_px`, stacked top to bottom (`gap_px` apart) and cut
  into slices no taller than `max_height` px: 001.png, 002.png … The strip is never
  held in memory as one image.
- sns: one image per page with the long edge at `long_edge` px; `spreads` adds the
  pages that form a spread as one image, laid out as the book opens.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

from genko.models import Episode, Page
from genko.render import mm_to_px, render_page, render_spread

PROFILES = ("webtoon", "sns")


def _srgb() -> bytes | None:
    try:
        from PIL import ImageCms

        return ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
    except Exception:  # Pillow built without littlecms
        return None


def _save(image: Image.Image, path: Path, fmt: str, quality: int) -> Path:
    icc = _srgb()
    extra = {"icc_profile": icc} if icc else {}
    if fmt == "jpeg":
        path = path.with_suffix(".jpg")
        image.convert("RGB").save(path, format="JPEG", quality=quality, optimize=True, **extra)
    else:
        path = path.with_suffix(".png")
        image.convert("RGB").save(path, format="PNG", optimize=True, **extra)
    return path


def trimmed(page: Page, episode: Episode, dpi: int) -> Image.Image:
    """The page as read on screen: no bleed, no dots, no crop marks."""
    image = render_page(page, dpi, mode="print", episode=episode, finish=False)
    trim = page.trim_rect_mm()
    x0, y0 = mm_to_px(trim.x, dpi), mm_to_px(trim.y, dpi)
    return image.crop((x0, y0, x0 + mm_to_px(trim.width, dpi), y0 + mm_to_px(trim.height, dpi)))


def _dpi_for_width(page: Page, width_px: int) -> int:
    trim_w = page.spec.trim_size()[0]
    return max(36, round(width_px / (trim_w / 25.4)))


def export_webtoon(episode: Episode, dest: Path, width_px: int = 800, max_height: int = 1280, gap_px: int = 0,
                   fmt: str = "png", quality: int = 92) -> list[Path]:
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    # heights first (render each page once, at its own scale), then cut the strip into slices
    heights = []
    for page in episode.pages:
        trim_w, trim_h = page.spec.trim_size()
        heights.append(round(width_px * trim_h / trim_w))
    tops, y = [], 0
    for h in heights:
        tops.append(y)
        y += h + gap_px
    total = y - gap_px if heights else 0
    written: list[Path] = []
    cache: dict[int, Image.Image] = {}
    start = 0
    while start < total:
        end = min(total, start + max_height)
        slice_image = Image.new("RGB", (width_px, end - start), (255, 255, 255))
        for i, (top, h) in enumerate(zip(tops, heights)):
            if top >= end or top + h <= start:
                continue
            if i not in cache:
                page = episode.pages[i]
                cache[i] = trimmed(page, episode, _dpi_for_width(page, width_px)).resize((width_px, h), Image.Resampling.LANCZOS)
            slice_image.paste(cache[i], (0, top - start))
        for i in [k for k in cache if tops[k] + heights[k] <= end]:
            del cache[i]  # pages fully above the next slice are no longer needed
        written.append(_save(slice_image, dest / f"{len(written) + 1:03d}", fmt, quality))
        start = end
    return written


def export_sns(episode: Episode, dest: Path, long_edge: int = 2048, fmt: str = "jpeg", quality: int = 92,
               spreads: bool = False) -> list[Path]:
    from genko.export import stem

    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for page in episode.pages:
        trim_w, trim_h = page.spec.trim_size()
        dpi = max(36, round(long_edge / (max(trim_w, trim_h) / 25.4)))
        image = trimmed(page, episode, dpi)
        image.thumbnail((long_edge, long_edge), Image.Resampling.LANCZOS)
        written.append(_save(image, dest / f"{stem(episode)}_p{page.index:03d}", fmt, quality))
    if spreads:
        done: set[int] = set()
        for page in episode.pages:
            partner = page.spread_with
            if not partner or page.index in done:
                continue
            done |= {page.index, partner}
            first, second = sorted((page.index, partner))
            dpi = max(36, round(long_edge / (2 * page.spec.trim_size()[0] / 25.4)))
            image = render_spread(episode, first, second, dpi=dpi, mode="print", finish=False, to_trim=True)
            image.thumbnail((long_edge, long_edge), Image.Resampling.LANCZOS)
            written.append(_save(image, dest / f"{stem(episode)}_spread_{first:03d}-{second:03d}", fmt, quality))
    return written


def probe(path: Path) -> dict:
    """Size and colour profile of a written file (for tests and the CLI's JSON)."""
    with Image.open(path) as img:
        return {"size": img.size, "icc": bool(img.info.get("icc_profile")), "format": img.format}

