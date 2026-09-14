from __future__ import annotations

import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from genko.models import Episode
from genko.render import render_page


def _pack_image(image: Image.Image) -> bytes:
    rgb = image.convert("RGB")
    width, height = rgb.size
    channels = rgb.split()
    body = b"".join(channel.tobytes() for channel in channels)
    header = b"8BPS" + struct.pack(">H", 1) + (b"\x00" * 6)
    header += struct.pack(">HIIHH", 3, height, width, 8, 3)
    color_mode = struct.pack(">I", 0)
    resources = _image_resources(episode_names(image))
    layers = _text_layers(image)
    # merged image data, compression 0 (raw), planar RGB
    merged = struct.pack(">H", 0) + body
    return header + color_mode + resources + layers + merged


def episode_names(image: Image.Image) -> list[str]:
    return [str(image.info.get("title") or "page")]


def _pascal(name: str) -> bytes:
    raw = name.encode("ascii", "replace")[:255]
    payload = bytes([len(raw)]) + raw
    pad = (4 - (len(payload) % 4)) % 4
    return payload + (b"\x00" * pad)


def _image_resources(names: list[str]) -> bytes:
    blocks = b""
    for index, name in enumerate(names):
        data = name.encode("utf-8")[:255]
        pascal = bytes([len(data)]) + data
        if len(pascal) % 2:
            pascal += b"\x00"
        blocks += b"8BIM" + struct.pack(">H", 1000 + index) + pascal + struct.pack(">I", 0)
    return struct.pack(">I", len(blocks)) + blocks


def _text_layers(image: Image.Image) -> bytes:
    names: list[str] = list(image.info.get("text_layers") or [])
    if not names:
        return struct.pack(">I", 0)
    records = b""
    channel_data = b""
    width, height = image.size
    count = len(names)
    info = struct.pack(">h", count)
    empty = b"\x00" * (width * height)
    for name in names:
        # full-page empty layer, named after the line
        info += struct.pack(">IIII", 0, 0, height, width)
        info += struct.pack(">H", 3)
        for cid in (0, 1, 2):
            raw = struct.pack(">H", 0) + empty
            info += struct.pack(">hI", cid, len(raw))
            channel_data += raw
        info += b"8BIM" + b"norm" + struct.pack(">BBBB", 255, 0, 0, 0)
        extra = struct.pack(">I", 0) + struct.pack(">I", 0) + _pascal(name)
        info += struct.pack(">I", len(extra)) + extra
    layer_info = struct.pack(">I", len(info) + len(channel_data)) + info + channel_data
    global_mask = struct.pack(">I", 0)
    payload = layer_info + global_mask
    return struct.pack(">I", len(payload)) + payload


def write_psd(path: Path, image: Image.Image, text_layers: list[str] | None = None) -> None:
    tagged = image.copy()
    tagged.info["text_layers"] = text_layers or []
    path.write_bytes(_pack_image(tagged))


def export_psd(episode: Episode, dest: Path, dpi: int = 150) -> Path:
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    image = render_page(episode.pages[0], dpi, mode="print", episode=episode)
    names = [f"{line.speaker}:{line.text}" if line.speaker else line.text for line in episode.story]
    write_psd(dest, image, names)
    return dest
