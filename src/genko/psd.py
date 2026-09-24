"""PSD export: one file per page with real layers (Pillow + struct, no other dependency).

Layers, bottom to top: paper, each placed image (the greyscale art before the
mono finish, so tones can be redone in the painting app), raster layers, the
ink strokes, tones, effects, panel borders, and one layer per balloon. Layer
names are Unicode (the `luni` block) with an ASCII fallback. The merged image
is the printed page. Each layer is stored cropped to its content.
"""

from __future__ import annotations

import struct
from pathlib import Path

from PIL import Image, ImageDraw

from genko.models import Episode, LayerKind, LayerRole, Page


# --- file structure ------------------------------------------------------------------------


def _pascal(name: str) -> bytes:
    raw = name.encode("ascii", "replace")[:255]
    payload = bytes([len(raw)]) + raw
    pad = (4 - (len(payload) % 4)) % 4
    return payload + (b"\x00" * pad)


def _unicode_name(name: str) -> bytes:
    text = name.encode("utf-16-be")
    data = struct.pack(">I", len(text) // 2) + text
    if len(data) % 4:
        data += b"\x00" * (4 - len(data) % 4)
    return b"8BIM" + b"luni" + struct.pack(">I", len(data)) + data


def _resources(dpi: int) -> bytes:
    # ResolutionInfo (1005): horizontal and vertical resolution in pixels per inch, 16.16 fixed point
    res = struct.pack(">IHHIHH", int(dpi * 65536), 1, 1, int(dpi * 65536), 1, 1)
    block = b"8BIM" + struct.pack(">H", 1005) + b"\x00\x00" + struct.pack(">I", len(res)) + res
    return struct.pack(">I", len(block)) + block


def _layer_records(layers: list[tuple[str, Image.Image]], size: tuple[int, int]) -> bytes:
    records = b""
    data = b""
    count = 0
    for name, image in layers:
        rgba = image.convert("RGBA")
        box = rgba.getbbox()
        if box is None:
            box = (0, 0, 1, 1)  # keep empty layers (e.g. a blank balloon) as 1-pixel layers
        left, top, right, bottom = box
        crop = rgba.crop(box)
        r, g, b, a = crop.split()
        records += struct.pack(">iiii", top, left, bottom, right)
        records += struct.pack(">H", 4)
        channels = ((-1, a), (0, r), (1, g), (2, b))
        for cid, channel in channels:
            records += struct.pack(">hI", cid, 2 + channel.width * channel.height)
        records += b"8BIM" + b"norm" + struct.pack(">BBBB", 255, 0, 0, 0)  # opacity, clipping, flags (visible), filler
        extra = struct.pack(">I", 0) + struct.pack(">I", 0) + _pascal(name) + _unicode_name(name)
        records += struct.pack(">I", len(extra)) + extra
        for _, channel in channels:
            data += struct.pack(">H", 0) + channel.tobytes()  # raw
        count += 1
    info = struct.pack(">h", count) + records + data
    if len(info) % 2:
        info += b"\x00"
    layer_info = struct.pack(">I", len(info)) + info
    payload = layer_info + struct.pack(">I", 0)  # no global layer mask
    return struct.pack(">I", len(payload)) + payload


def write_psd(path: Path, merged: Image.Image, layers: list[tuple[str, Image.Image]], dpi: int = 72) -> Path:
    rgb = merged.convert("RGB")
    width, height = rgb.size
    header = b"8BPS" + struct.pack(">H", 1) + (b"\x00" * 6) + struct.pack(">HIIHH", 3, height, width, 8, 3)
    body = header + struct.pack(">I", 0) + _resources(dpi) + _layer_records(layers, rgb.size)
    body += struct.pack(">H", 0) + b"".join(channel.tobytes() for channel in rgb.split())
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)
    return path


# --- the page as layers --------------------------------------------------------------------------


def page_layers(page: Page, episode: Episode, dpi: int) -> list[tuple[str, Image.Image]]:
    from genko import render

    width = render.mm_to_px(page.spec.width_mm, dpi)
    height = render.mm_to_px(page.spec.height_mm, dpi)
    size = (width, height)

    def blank() -> Image.Image:
        return Image.new("RGBA", size, (0, 0, 0, 0))

    out: list[tuple[str, Image.Image]] = [("紙", Image.new("RGBA", size, (255, 255, 255, 255)))]
    for role in (LayerRole.BG, LayerRole.INK, LayerRole.FINISH):
        fill = page.fills.get(role)
        if fill is not None:
            out.append((f"塗り {role.value}", Image.new("RGBA", size, (*fill, 255))))
    for layer in page.layers:
        if not layer.visible or not layer.exportable or layer.role in (LayerRole.NAME, LayerRole.DRAFT):
            continue
        if layer.kind == LayerKind.FOLDER:
            continue
        if layer.kind == LayerKind.PLACED:
            art = render._placed_raster(layer, page, episode, size, dpi, mode="name")  # before the mono finish
            if art is not None:
                to = (layer.source or {}).get("to", "art")
                label = {"art": "絵", "bg": "背景", "ink": "線画", "draft": "下描き"}.get(to, to)
                out.append((f"{label} {layer.title or layer.id}".strip(), art))
            continue
        raster = render._open_raster(layer)
        if raster is not None:
            out.append((layer.title or f"{layer.role.value} {layer.id[:6]}", raster.resize(size)))
    ink_has_raster = any(layer.role == LayerRole.INK and layer.raster_png for layer in page.layers)
    if page.ink_strokes and not ink_has_raster:
        ink = blank()
        draw = ImageDraw.Draw(ink)
        for stroke in page.ink_strokes:
            render._stroke(draw, stroke, dpi, render.INK_COLOR, 3)
        mask = render._clip_mask(page, size, dpi)
        if mask is not None:
            ink.putalpha(render._and_alpha(ink, mask))
        out.append(("ペン入れ", ink))
    if any(layer.role == LayerRole.TONE and layer.visible for layer in page.layers):
        out.append(("トーン", render._draw_tone(blank(), page, dpi, "print")))
    if page.effects:
        out.append(("効果", render._draw_effects(blank(), page, dpi)))
    frames = blank()
    render._draw_frames(ImageDraw.Draw(frames), page, dpi)
    out.append(("コマ枠", frames))
    font_path = getattr(episode, "font_path", None)
    for line in episode.story_for_page(page.index):
        if not (line.x_mm or line.y_mm or line.balloon):
            continue
        balloon = blank()
        render._draw_balloon(ImageDraw.Draw(balloon), line, dpi, font_path, show_speaker=False)
        out.append((f"台詞 {line.text.replace(chr(10), '')[:24]}", balloon))
    if page.numero:
        numero = blank()
        draw = ImageDraw.Draw(numero)
        font = render._font(font_path)
        label = str(page.index)
        bbox = draw.textbbox((0, 0), label, font=font)
        draw.text(((width - (bbox[2] - bbox[0])) / 2, height - render.mm_to_px(12, dpi)), label, fill=(20, 20, 20), font=font)
        out.append(("ノンブル", numero))
    return out


def export_page_psd(episode: Episode, page: Page, dest: Path, dpi: int | None = None) -> Path:
    from genko.render import render_page

    dpi = int(dpi or episode.spec.dpi or 600)
    merged = render_page(page, dpi, mode="print", episode=episode)
    return write_psd(dest, merged, page_layers(page, episode, dpi), dpi)


def export_psd_pages(episode: Episode, dest: Path, dpi: int | None = None) -> list[Path]:
    """One PSD per page in the folder `dest`."""
    from genko.export import stem

    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    return [export_page_psd(episode, page, dest / f"{stem(episode)}_p{page.index:03d}.psd", dpi) for page in episode.pages]


def export_psd(episode: Episode, dest: Path, dpi: int = 150, page: int = 1) -> Path:
    """One page (default the first) as a layered PSD at `dest`."""
    target = next(p for p in episode.pages if p.index == page)
    return export_page_psd(episode, target, dest, dpi)
