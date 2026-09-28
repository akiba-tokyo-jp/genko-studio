"""Photoshop brush files (.abr): the sampled tips inside, as image-tip brushes.

Two layouts exist. Old files (version 1 and 2) list the brushes one after another; newer ones (version 6
and later) keep the tips in an "8BIM samp" section. Only sampled (picture) tips are read; the settings
Photoshop keeps for its own engine are left out, as other programs do. Every number is big-endian.
"""

from __future__ import annotations

import base64
import io
import struct

from PIL import Image


class AbrError(ValueError):
    pass


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def take(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise AbrError("the file ends too early")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def i16(self) -> int:
        return struct.unpack(">h", self.take(2))[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def i32(self) -> int:
        return struct.unpack(">i", self.take(4))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self.take(4))[0]


def _unpack_bits(reader: _Reader, width: int, height: int) -> bytes:
    """PackBits rows: a count for each row first, then the rows."""
    lengths = [reader.u16() for _ in range(height)]
    out = bytearray()
    for length in lengths:
        row = reader.take(length)
        i, line = 0, bytearray()
        while i < len(row) and len(line) < width:
            n = row[i] if row[i] < 128 else row[i] - 256
            i += 1
            if n >= 0:
                line += row[i:i + n + 1]
                i += n + 1
            elif n > -128:
                line += bytes([row[i]]) * (1 - n)
                i += 1
        out += bytes(line[:width]).ljust(width, b"\0")
    return bytes(out)


def _image(reader: _Reader, width: int, height: int, depth: int, compressed: bool) -> Image.Image:
    if width <= 0 or height <= 0 or width > 16384 or height > 16384:
        raise AbrError("a tip has no size")
    if depth != 8:
        raise AbrError(f"only 8-bit tips are read (this one is {depth}-bit)")
    data = _unpack_bits(reader, width, height) if compressed else reader.take(width * height)
    return Image.frombytes("L", (width, height), data)


def _read_v12(reader: _Reader, version: int) -> list[dict]:
    count = reader.u16()
    out = []
    for n in range(count):
        kind = reader.i16()
        size = reader.i32()
        end = reader.pos + size
        if kind != 2:  # (computed round tips carry no picture)
            reader.pos = end
            continue
        reader.i32()  # misc
        spacing = reader.i16()
        name = ""
        if version == 2:
            length = reader.u32()
            name = reader.take(length * 2).decode("utf-16-be", "replace").rstrip("\0")
        reader.u8()  # antialiasing
        for _ in range(4):
            reader.i16()  # short bounds (an older copy)
        top, left, bottom, right = reader.i32(), reader.i32(), reader.i32(), reader.i32()
        depth = reader.i16()
        compressed = bool(reader.u8())
        image = _image(reader, right - left, bottom - top, depth, compressed)
        out.append({"name": name or f"ブラシ {n + 1}", "image": image, "spacing": spacing})
        reader.pos = end
    return out


def _read_v6(reader: _Reader, subversion: int) -> list[dict]:
    out = []
    while reader.pos + 12 <= len(reader.data):
        if reader.take(4) != b"8BIM":
            raise AbrError("a section does not start with 8BIM")
        key = reader.take(4)
        size = reader.u32()
        end = reader.pos + size
        if key != b"samp":
            reader.pos = end
            continue
        while reader.pos < end:
            length = reader.u32()
            padded = length + (-length % 4)
            brush_end = reader.pos + padded
            reader.take(47 if subversion == 1 else 301)  # the tip's id and Photoshop's own fields
            top, left, bottom, right = reader.i32(), reader.i32(), reader.i32(), reader.i32()
            depth = reader.i16()
            compressed = bool(reader.u8())
            try:
                image = _image(reader, right - left, bottom - top, depth, compressed)
            except AbrError:
                reader.pos = brush_end
                continue
            out.append({"name": f"ブラシ {len(out) + 1}", "image": image, "spacing": 25})
            reader.pos = brush_end
        reader.pos = end
    return out


def read(data: bytes) -> list[dict]:
    """The sampled tips in an .abr file: [{name, image (L, ink = white), spacing (%)}]."""
    reader = _Reader(data)
    version = reader.i16()
    if version in (1, 2):
        tips = _read_v12(reader, version)
    elif version in (6, 7, 10):
        tips = _read_v6(reader, reader.i16())
    else:
        raise AbrError(f"version {version} brush files are not read")
    if not tips:
        raise AbrError("the file has no picture tips")
    return tips


def tip_png(image: Image.Image, longest: int = 256) -> str:
    """A tip picture kept small, as the base64 PNG a brush carries."""
    image = image.convert("L")
    if max(image.size) > longest:
        image.thumbnail((longest, longest))
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def brushes_from(data: bytes, prefix: str = "") -> list[dict]:
    """Brush definitions (for define_brush / the library) from an .abr file's tips."""
    out = []
    for tip in read(data):
        out.append({"label": (prefix + tip["name"])[:40], "base": "gpen", "tip": "image", "tip_png": tip_png(tip["image"]),
                    "spacing": max(0.02, min(5.0, tip["spacing"] / 100)), "min_pressure": 0.3, "taper": False})
    return out


def tip_from_picture(path_or_image) -> str:
    """A tip from any picture (dark marks on light paper, or marks on transparency)."""
    from genko.brushes import _decode_tip

    image = path_or_image if isinstance(path_or_image, Image.Image) else Image.open(path_or_image)
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    ink = _decode_tip(base64.b64encode(buf.getvalue()).decode("ascii"))
    if ink is None:
        raise AbrError("the picture could not be read")
    return tip_png(ink)
