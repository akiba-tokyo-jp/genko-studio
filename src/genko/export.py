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
    "render_page",
]


def export_png_sequence(
    episode: Episode,
    dest: Path,
    working_dpi: int = 150,
    mode: str = "print",
) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for page in episode.pages:
        image = render_page(page, working_dpi, mode=mode, episode=episode)
        path = dest / f"{episode.title}_ep{episode.episode:02d}_p{page.index:03d}.png"
        image.save(path)
        written.append(path)
    return written


def export_print(
    episode: Episode,
    dest: Path,
    fmt: str = "png",
    dpi: int = 600,
    threshold: int = 180,
    crop_marks: bool = True,
) -> list[Path]:
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    images = [
        render_page(page, dpi, mode="print", episode=episode, crop_marks=crop_marks)
        for page in episode.pages
    ]
    fmt = fmt.lower()
    written: list[Path] = []
    if fmt == "pdf":
        path = dest / f"{episode.title}_ep{episode.episode:02d}.pdf"
        rgb = [image.convert("RGB") for image in images]
        rgb[0].save(path, format="PDF", save_all=True, append_images=rgb[1:], resolution=dpi)
        return [path]
    for page, image in zip(episode.pages, images):
        stem = f"{episode.title}_ep{episode.episode:02d}_p{page.index:03d}"
        if fmt == "tiff":
            path = dest / f"{stem}.tiff"
            to_bitonal(image, threshold=threshold).save(path, format="TIFF", compression="group4")
        elif fmt == "png1":
            path = dest / f"{stem}.png"
            to_bitonal(image, threshold=threshold).save(path)
        else:
            path = dest / f"{stem}.png"
            image.save(path)
        written.append(path)
    return written


def export_strip(episode: Episode, dest: Path, dpi: int = 150) -> Path:
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    pages = [render_page(page, dpi, mode="print", episode=episode) for page in episode.pages]
    width = max(image.width for image in pages)
    height = sum(image.height for image in pages)
    strip = Image.new("RGB", (width, height), (255, 255, 255))
    y = 0
    for image in pages:
        strip.paste(image, (0, y))
        y += image.height
    strip.save(dest)
    return dest


def export_epub(episode: Episode, dest: Path, dpi: int = 150) -> Path:
    from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    images = []
    tmp_dir = dest.parent / f".{dest.stem}-epub"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    for page in episode.pages:
        image = render_page(page, dpi, mode="print", episode=episode)
        path = tmp_dir / f"p{page.index:03d}.png"
        image.save(path)
        images.append(path)
    manifest = []
    spines = []
    xhtml_files = []
    for image in images:
        index = image.stem
        item_id = f"img_{index}"
        page_id = f"page_{index}"
        manifest.append(f'<item id="{item_id}" href="images/{image.name}" media-type="image/png"/>')
        manifest.append(f'<item id="{page_id}" href="{index}.xhtml" media-type="application/xhtml+xml"/>')
        spines.append(f'<itemref idref="{page_id}"/>')
        xhtml_files.append(
            (
                f"{index}.xhtml",
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>"
                f"{episode.title}</title></head><body><img src=\"images/{image.name}\" alt=\"{index}\"/></body></html>",
            )
        )
    opf = (
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"2.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        f"<dc:title>{episode.title}</dc:title><dc:language>ja</dc:language>"
        "<dc:identifier id=\"bookid\">genko</dc:identifier></metadata>"
        f"<manifest>{''.join(manifest)}</manifest><spine>{''.join(spines)}</spine></package>"
    )
    container = (
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"
    )
    with ZipFile(dest, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        zf.writestr("META-INF/container.xml", container, compress_type=ZIP_DEFLATED)
        zf.writestr("OEBPS/content.opf", opf, compress_type=ZIP_DEFLATED)
        for name, body in xhtml_files:
            zf.writestr(f"OEBPS/{name}", body, compress_type=ZIP_DEFLATED)
        for image in images:
            zf.write(image, f"OEBPS/images/{image.name}")
    return dest

