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


COLORS = ("auto", "rgb", "cmyk", "gray", "bitonal")


def page_color(page: Page, color: str) -> str:
    """The colour a page is written in. auto: a monochrome book's pages in grey (its tones stay crisp: no JPEG, no
    colour), its covers and a colour book's pages in RGB."""
    if color != "auto":
        return color
    from genko.covers import is_cover

    return "rgb" if page.spec.expression == "color" or is_cover(page) else "gray"


def _pdf_boxes(page: Page, area: str) -> dict[str, tuple[float, float, float, float]]:
    """The page's MediaBox (the written area), BleedBox and TrimBox in points, from the bottom left."""
    paper = (0.0, 0.0, page.spec.width_mm, page.spec.height_mm)
    rects = {"paper": paper}
    for key, rect in (("bleed", page.bleed_rect_mm()), ("trim", page.trim_rect_mm())):
        rects[key] = (rect.x, rect.y, rect.width, rect.height)
    mx, my, mw, mh = rects[area]
    pt = 72 / 25.4

    def box(rect):
        x, y, w, h = rect
        x0, x1 = max(x, mx) - mx, min(x + w, mx + mw) - mx
        top, bottom = max(y, my) - my, min(y + h, my + mh) - my
        return (round(x0 * pt, 3), round((mh - bottom) * pt, 3), round(x1 * pt, 3), round((mh - top) * pt, 3))

    return {"MediaBox": box(rects[area]), "BleedBox": box(rects["bleed"]), "TrimBox": box(rects["trim"])}


def write_pdf(path: Path, pictures: list, boxes: list[dict], profiles: list[bytes | None] | None = None) -> Path:
    """A print PDF: each page one picture, stored losslessly (Flate), in its own colour: 1-bit and 8-bit grey,
    RGB or CMYK (with an ICC profile when given). Every page carries its MediaBox, BleedBox and TrimBox."""
    import zlib

    objects: list[bytes] = []

    def add(body: bytes) -> int:
        objects.append(body)
        return len(objects)

    def stream(head: str, data: bytes) -> bytes:
        return f"<< {head} /Length {len(data)} >>\nstream\n".encode() + data + b"\nendstream"

    catalog = add(b"")  # (filled in last)
    pages_obj = add(b"")
    kids: list[int] = []
    profile_ids: dict[bytes, int] = {}
    for n, (picture, box) in enumerate(zip(pictures, boxes)):
        mode = picture.mode
        space, bits, comps = {"1": ("/DeviceGray", 1, 1), "L": ("/DeviceGray", 8, 1), "RGB": ("/DeviceRGB", 8, 3),
                              "CMYK": ("/DeviceCMYK", 8, 4)}[mode]
        profile = (profiles or [None] * len(pictures))[n]
        if profile and mode in ("RGB", "CMYK"):
            if profile not in profile_ids:
                profile_ids[profile] = add(stream(f"/N {comps} /Filter /FlateDecode", zlib.compress(profile, 6)))
            space = f"[/ICCBased {profile_ids[profile]} 0 R]"
        data = zlib.compress(picture.tobytes(), 6)
        image = add(stream(f"/Type /XObject /Subtype /Image /Width {picture.width} /Height {picture.height} "
                           f"/ColorSpace {space} /BitsPerComponent {bits} /Filter /FlateDecode", data))
        x0, y0, x1, y1 = box["MediaBox"]
        draw = f"q {x1 - x0:.3f} 0 0 {y1 - y0:.3f} 0 0 cm /Im0 Do Q".encode()
        content = add(stream("", draw))
        boxes_text = " ".join(f"/{key} [{' '.join(f'{v:.3f}' for v in value)}]" for key, value in box.items())
        kids.append(add(f"<< /Type /Page /Parent {pages_obj} 0 R {boxes_text} /Resources << /XObject << /Im0 {image} 0 R >> >> "
                        f"/Contents {content} 0 R >>".encode()))
    objects[catalog - 1] = f"<< /Type /Catalog /Pages {pages_obj} 0 R >>".encode()
    objects[pages_obj - 1] = f"<< /Type /Pages /Kids [{' '.join(f'{k} 0 R' for k in kids)}] /Count {len(kids)} >>".encode()
    out = bytearray(b"%PDF-1.6\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    out += b"".join(f"{o:010d} 00000 n \n".encode() for o in offsets)
    out += f"trailer\n<< /Size {len(objects) + 1} /Root {catalog} 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    path = Path(path)
    path.write_bytes(bytes(out))
    return path


def export_print(
    episode: Episode,
    dest: Path,
    fmt: str = "png",
    dpi: int | None = None,
    threshold: int = 180,
    crop_marks: bool = True,
    area: str = "paper",
    color: str = "auto",
    icc: str | None = None,
) -> list[Path]:
    """Print pages. `area`: "paper" (the whole sheet, with crop marks), "bleed" (the finished size and its
    bleed: what most printers take) or "trim" (the finished size only). `color`: "auto" (a monochrome book in
    grey, a colour book in RGB), "rgb" (sRGB, its profile embedded), "cmyk" (TIFF or PDF, through the printer's
    profile `icc` when given), "gray", or "bitonal" (1-bit black and white, 二階調)."""
    if area not in AREAS:
        raise ValueError(f"area must be one of {', '.join(AREAS)}")
    if color not in COLORS:
        raise ValueError("color must be auto, rgb, cmyk, gray or bitonal")
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

    def coloured(image, how: str):
        if how == "cmyk":
            return colour.to_cmyk(image, icc)
        if how == "bitonal":
            return to_bitonal(image, threshold=threshold)
        return image.convert("L" if how == "gray" else "RGB")

    def profile_for(how: str) -> bytes | None:
        return Path(icc).read_bytes() if icc and how == "cmyk" else colour.srgb_icc() if how == "rgb" else None

    if fmt == "pdf":
        path = dest / f"{stem(episode)}.pdf"
        hows = [page_color(page, color) for page in episode.pages]
        pictures = [coloured(image, how) for image, how in zip(images, hows)]
        return [write_pdf(path, pictures, [_pdf_boxes(page, area) for page in episode.pages], [profile_for(h) for h in hows])]
    from genko.covers import file_stem

    for page, image in zip(episode.pages, images):
        name = f"{stem(episode)}_{file_stem(page)}"
        how = page_color(page, color)
        profile = profile_for(how)
        if fmt == "cmyk" or (fmt == "tiff" and how not in ("rgb", "bitonal") and color != "auto"):  # (colour and grey TIFF)
            path = dest / f"{name}.tiff"
            coloured(image, how).save(path, format="TIFF", compression="tiff_lzw", dpi=(dpi, dpi),
                                      **({"icc_profile": profile} if profile else {}))
        elif fmt == "tiff" and how == "rgb" and color == "auto":  # (a colour page, as a colour TIFF)
            path = dest / f"{name}.tiff"
            coloured(image, how).save(path, format="TIFF", compression="tiff_lzw", dpi=(dpi, dpi), icc_profile=profile)
        elif fmt == "tiff":
            path = dest / f"{name}.tiff"
            to_bitonal(image, threshold=threshold).save(path, format="TIFF", compression="group4", dpi=(dpi, dpi))
        elif fmt == "png1":
            path = dest / f"{name}.png"
            to_bitonal(image, threshold=threshold).save(path, dpi=(dpi, dpi))
        else:
            path = dest / f"{name}.png"
            picture = coloured(image, how)
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
        for n, (name, image, *meta) in enumerate(page_layers(page, episode, dpi), start=1):
            if image is None:
                continue
            mask = (meta[0] if meta else {}).get("mask")
            if mask is not None:  # (the layer's mask applies: a PNG has no mask of its own)
                from PIL import ImageChops

                image = image.convert("RGBA")
                image.putalpha(ImageChops.multiply(image.getchannel("A"), mask.convert("L").resize(image.size)))
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


def export_kindle(episode: Episode, dest: Path, long_edge: int = KINDLE_LONG_EDGE, gray: bool | None = None,
                  dots: bool = False) -> Path:
    """A fixed-layout book for Kindle (KDP takes it as it is; Kindle Previewer opens it): every page the same
    size, JPEG, the metadata Kindle reads (comic, right-to-left for manga, the original resolution, no
    margins or gutter). A monochrome book is written in grey."""
    if gray is None:
        gray = getattr(episode.spec, "expression", "mono") == "mono"
    return export_epub(episode, dest, kindle=True, long_edge=long_edge, gray=gray, jpeg=True, dots=dots)


def export_epub(episode: Episode, dest: Path, dpi: int = 150, *, kindle: bool = False, long_edge: int | None = None,
                gray: bool = False, jpeg: bool = False, dots: bool = False) -> Path:
    """EPUB 3, fixed layout, one page image per spine item. Right-bound books read right to left. `long_edge`
    scales every page to that many pixels on its long side (the same size for all); `kindle` adds what the
    Kindle devices read. Pages are cut to the finished size, and the tones drawn as flat greys (a reader scales
    the page, and scaled dots beat into moiré); `dots` keeps the print's dots."""
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
    for page, part in covers.reading_order(episode):  # (the front cover first, the back cover last)
        image = render_page(page, dpi, mode="print", episode=episode, dots=dots)
        if (covers.cover_of(page) or {}).get("kind") == "jacket":
            image = covers.front_of(page, image, dpi, episode.binding.value, "裏表紙" if part == "back" else "表紙")
        else:  # (a reader shows the finished page: no bleed, no marks, no paper around it)
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
        stem_name = f"cover_{part}" if part != "page" else covers.file_stem(page)
        pages.append((stem_name, image.size, buf.getvalue(), page))
    manifest = ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>']
    spine = []
    xhtml = []
    for i, (stem, (w, h), _, page) in enumerate(pages):
        cover = ' properties="cover-image"' if i == 0 else ""
        manifest.append(f'<item id="img_{stem}" href="images/{stem}.{ext}" media-type="{media}"{cover}/>')
        manifest.append(f'<item id="page_{stem}" href="{stem}.xhtml" media-type="application/xhtml+xml"/>')
        if covers.is_cover(page):  # (a cover stands alone, in the middle of the screen)
            spine.append(f'<itemref idref="page_{stem}" properties="rendition:page-spread-center"/>')
        else:
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
