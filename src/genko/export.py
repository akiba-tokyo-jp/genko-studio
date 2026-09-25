from __future__ import annotations

from pathlib import Path

from PIL import Image

from genko.models import Episode, LayerRole, Page
from genko.psd import export_psd
from genko.render import EXPORT_ROLES, export_plan, render_page, to_bitonal

__all__ = [
    "EXPORT_ROLES",
    "export_plan",
    "export_png_sequence",
    "export_print",
    "export_strip",
    "export_psd",
    "export_epub",
    "export_kindle",
    "export_layers",
    "render_page",
]


_BAD = set('<>:"/\\|?*')
_RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}


def safe_name(text: str, fallback: str = "genko") -> str:
    """A file name that works on Windows, macOS and Linux (Japanese is kept)."""
    out = "".join("_" if (ch in _BAD or ord(ch) < 32) else ch for ch in str(text or ""))
    out = out.strip().rstrip(". ")
    if not out:
        out = fallback
    if out.split(".")[0].upper() in _RESERVED:
        out = "_" + out
    return out[:80]


def stem(episode: Episode) -> str:
    return f"{safe_name(episode.title)}_ep{episode.episode:02d}"


def export_png_sequence(
    episode: Episode,
    dest: Path,
    working_dpi: int = 150,
    mode: str = "print",
) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    from genko.covers import file_stem

    for page in episode.pages:
        image = render_page(page, working_dpi, mode=mode, episode=episode)
        path = dest / f"{stem(episode)}_{file_stem(page)}.png"
        image.save(path)
        written.append(path)
    return written


AREAS = ("paper", "bleed", "trim")
AREA_LABELS = {"paper": "用紙全体（トンボ付き）", "bleed": "裁ち落としまで（入稿の標準）", "trim": "仕上がりまで"}


def crop_to(image, page, area: str, dpi: int):
    """A page image cut to the paper, the bleed or the finished size."""
    if area == "paper":
        return image
    from genko.render import mm_to_px

    r = page.bleed_rect_mm() if area == "bleed" else page.trim_rect_mm()
    x0, y0 = mm_to_px(r.x, dpi), mm_to_px(r.y, dpi)
    return image.crop((x0, y0, x0 + mm_to_px(r.width, dpi), y0 + mm_to_px(r.height, dpi)))


def export_print(
    episode: Episode,
    dest: Path,
    fmt: str = "png",
    dpi: int | None = None,
    threshold: int = 180,
    crop_marks: bool = True,
    area: str = "paper",
    color: str = "rgb",
    icc: str | None = None,
) -> list[Path]:
    """Print pages. `area`: "paper" (the whole sheet, with crop marks), "bleed" (the finished size and its
    bleed: what most printers take) or "trim" (the finished size only). `color`: "rgb" (sRGB, its profile
    embedded), "cmyk" (TIFF or PDF, through the printer's profile `icc` when given) or "gray"."""
    if area not in AREAS:
        raise ValueError(f"area must be one of {', '.join(AREAS)}")
    if color not in ("rgb", "cmyk", "gray"):
        raise ValueError("color must be rgb, cmyk or gray")
    fmt = fmt.lower()
    if color == "cmyk" and fmt not in ("tiff", "pdf", "cmyk"):
        raise ValueError("CMYK is written as TIFF or PDF")
    from genko import colour

    if icc and color == "cmyk" and not colour.is_cmyk_profile(icc):
        raise ValueError("the profile is not a CMYK printing profile")
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    dpi = int(dpi or episode.spec.dpi or 600)  # print resolution comes from the page spec (B4 comic: 600)
    images = [
        crop_to(render_page(page, dpi, mode="print", episode=episode, crop_marks=crop_marks and area == "paper"), page, area, dpi)
        for page in episode.pages
    ]
    written: list[Path] = []

    def coloured(image):
        if color == "cmyk":
            return colour.to_cmyk(image, icc)
        return image.convert("L" if color == "gray" else "RGB")

    profile = (Path(icc).read_bytes() if icc and color == "cmyk" else colour.srgb_icc() if color == "rgb" else None)
    if fmt == "pdf":
        path = dest / f"{stem(episode)}.pdf"
        pictures = [coloured(image) for image in images]
        extra = {"quality": 95} if color == "cmyk" else {}
        pictures[0].save(path, format="PDF", save_all=True, append_images=pictures[1:], resolution=dpi, **extra)
        return [path]
    from genko.covers import file_stem

    for page, image in zip(episode.pages, images):
        name = f"{stem(episode)}_{file_stem(page)}"
        if fmt == "cmyk" or (fmt == "tiff" and color != "rgb"):  # (colour and grey TIFF for print)
            path = dest / f"{name}.tiff"
            coloured(image).save(path, format="TIFF", compression="tiff_lzw", dpi=(dpi, dpi),
                                 **({"icc_profile": profile} if profile else {}))
        elif fmt == "tiff":
            path = dest / f"{name}.tiff"
            to_bitonal(image, threshold=threshold).save(path, format="TIFF", compression="group4")
        elif fmt == "png1":
            path = dest / f"{name}.png"
            to_bitonal(image, threshold=threshold).save(path)
        else:
            path = dest / f"{name}.png"
            picture = coloured(image)
            picture.save(path, dpi=(dpi, dpi), **({"icc_profile": profile} if profile else {}))
        written.append(path)
    return written


def export_layers(episode: Episode, dest: Path, dpi: int = 350, area: str = "paper") -> list[Path]:
    """Every page's layers as separate transparent PNGs (レイヤーごとの書き出し): a folder per page, the files
    numbered from the bottom layer up, as the layered PSD holds them."""
    from genko import covers
    from genko.psd import page_layers

    dest = Path(dest)
    written: list[Path] = []
    for page in episode.pages:
        folder = dest / f"{stem(episode)}_{covers.file_stem(page)}"
        folder.mkdir(parents=True, exist_ok=True)
        for n, (name, image, *_meta) in enumerate(page_layers(page, episode, dpi), start=1):
            if image is None:
                continue
            path = folder / f"{n:02d}_{safe_name(name, 'layer')}.png"
            crop_to(image, page, area, dpi).save(path, dpi=(dpi, dpi))
            written.append(path)
    return written


def export_strip(episode: Episode, dest: Path, dpi: int = 150) -> Path:
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    from genko.covers import is_cover

    pages = [render_page(page, dpi, mode="print", episode=episode) for page in episode.pages if not is_cover(page)]
    width = max(image.width for image in pages)
    height = sum(image.height for image in pages)
    strip = Image.new("RGB", (width, height), (255, 255, 255))
    y = 0
    for image in pages:
        strip.paste(image, (0, y))
        y += image.height
    strip.save(dest)
    return dest


KINDLE_LONG_EDGE = 2560  # px: what Amazon asks of comic pages (Kindle Publishing Guidelines, fixed layout)


def export_kindle(episode: Episode, dest: Path, long_edge: int = KINDLE_LONG_EDGE, gray: bool | None = None) -> Path:
    """A fixed-layout book for Kindle (KDP takes it as it is; Kindle Previewer opens it): every page the same
    size, JPEG, the metadata Kindle reads (comic, right-to-left for manga, the original resolution, no
    margins or gutter). A monochrome book is written in grey."""
    if gray is None:
        gray = getattr(episode.spec, "expression", "mono") == "mono"
    return export_epub(episode, dest, kindle=True, long_edge=long_edge, gray=gray, jpeg=True)


def export_epub(episode: Episode, dest: Path, dpi: int = 150, *, kindle: bool = False, long_edge: int | None = None,
                gray: bool = False, jpeg: bool = False) -> Path:
    """EPUB 3, fixed layout, one page image per spine item. Right-bound books read right to left. `long_edge`
    scales every page to that many pixels on its long side (the same size for all); `kindle` adds what the
    Kindle devices read."""
    import hashlib
    import io
    import time
    from xml.sax.saxutils import escape
    from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

    from genko.models import Binding

    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    title = escape(episode.title)
    rtl = episode.binding == Binding.RIGHT
    ident = "urn:genko:" + hashlib.sha256(f"{episode.title}/{episode.episode}".encode("utf-8")).hexdigest()[:24]
    modified = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    from genko import covers

    pages = []
    ext, media = ("jpg", "image/jpeg") if jpeg else ("png", "image/png")
    if long_edge:  # (render at about the resolution the long edge needs, so small type stays sharp)
        longest = max(max(p.trim_rect_mm().width, p.trim_rect_mm().height) for p in episode.pages)
        dpi = max(dpi, int(long_edge / (longest / 25.4)) + 1)
    size = None
    for page in covers.pages_in_order(episode):  # (the front cover first, the back cover last)
        image = render_page(page, dpi, mode="print", episode=episode)
        if (covers.cover_of(page) or {}).get("kind") == "jacket":
            image = covers.front_of(page, image, dpi, episode.binding.value)
        elif kindle:  # (a reader shows the finished page: no bleed, no marks)
            image = crop_to(image, page, "trim", dpi)
        if long_edge:
            if size is None:
                scale = long_edge / max(image.size)
                size = (round(image.width * scale), round(image.height * scale))
            scale = min(size[0] / image.width, size[1] / image.height)  # (the same size for all; never stretched)
            fitted = image.convert("RGB").resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
            image = Image.new("RGB", size, (255, 255, 255))
            image.paste(fitted, ((size[0] - fitted.width) // 2, (size[1] - fitted.height) // 2))
        image = image.convert("L" if gray else "RGB")
        buf = io.BytesIO()
        image.save(buf, format="JPEG" if jpeg else "PNG", **({"quality": 90} if jpeg else {}))
        pages.append((covers.file_stem(page), image.size, buf.getvalue(), page))
    manifest = ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>']
    spine = []
    xhtml = []
    for i, (stem, (w, h), _, page) in enumerate(pages):
        cover = ' properties="cover-image"' if i == 0 else ""
        manifest.append(f'<item id="img_{stem}" href="images/{stem}.{ext}" media-type="{media}"{cover}/>')
        manifest.append(f'<item id="page_{stem}" href="{stem}.xhtml" media-type="application/xhtml+xml"/>')
        side = page.side(episode.start_side)
        spine.append(f'<itemref idref="page_{stem}" properties="page-spread-{side}"/>')
        xhtml.append((f"{stem}.xhtml",
                      '<?xml version="1.0" encoding="utf-8"?><!DOCTYPE html>'
                      '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head>'
                      f'<title>{title}</title><meta name="viewport" content="width={w}, height={h}"/>'
                      '<style>html,body{margin:0;padding:0}img{display:block;width:100%;height:100%}</style></head>'
                      f'<body><img src="images/{stem}.{ext}" alt="{page.index}"/></body></html>'))
    nav = ('<?xml version="1.0" encoding="utf-8"?><!DOCTYPE html>'
           '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja">'
           f'<head><title>{title}</title></head><body><nav epub:type="toc"><ol>'
           f'<li><a href="{pages[0][0]}.xhtml">{title}</a></li></ol></nav></body></html>') if pages else ""
    kindle_meta = ""
    if kindle and pages:
        w, h = pages[0][1]
        kindle_meta = (f'<meta name="cover" content="img_{pages[0][0]}"/>'
                       '<meta name="fixed-layout" content="true"/>'
                       f'<meta name="original-resolution" content="{w}x{h}"/>'
                       '<meta name="book-type" content="comic"/>'
                       f'<meta name="primary-writing-mode" content="{"horizontal-rl" if rtl else "horizontal-lr"}"/>'
                       '<meta name="zero-gutter" content="true"/><meta name="zero-margin" content="true"/>'
                       '<meta name="orientation-lock" content="none"/><meta name="region-mag" content="false"/>')
    opf = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja"'
        ' prefix="rendition: http://www.idpf.org/vocab/rendition/#">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        f'<dc:title>{title}</dc:title><dc:language>ja</dc:language><dc:identifier id="bookid">{ident}</dc:identifier>'
        f'<meta property="dcterms:modified">{modified}</meta>'
        '<meta property="rendition:layout">pre-paginated</meta>'
        '<meta property="rendition:spread">landscape</meta>'
        '<meta property="rendition:orientation">auto</meta>'
        f'{kindle_meta}'
        '</metadata>'
        f'<manifest>{"".join(manifest)}</manifest>'
        f'<spine page-progression-direction="{"rtl" if rtl else "ltr"}">{"".join(spine)}</spine></package>'
    )
    container = (
        '<?xml version="1.0"?>'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
    )
    with ZipFile(dest, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        zf.writestr("META-INF/container.xml", container, compress_type=ZIP_DEFLATED)
        zf.writestr("OEBPS/content.opf", opf, compress_type=ZIP_DEFLATED)
        zf.writestr("OEBPS/nav.xhtml", nav, compress_type=ZIP_DEFLATED)
        for name, body in xhtml:
            zf.writestr(f"OEBPS/{name}", body, compress_type=ZIP_DEFLATED)
        for stem, _, data, _ in pages:
            zf.writestr(f"OEBPS/images/{stem}.{ext}", data, compress_type=ZIP_STORED)
    return dest
