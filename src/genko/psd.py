"""PSD export: one file per page with real layers (Pillow + struct, no other dependency).

Layers, bottom to top: paper, each placed image (the greyscale art before the
mono finish, so tones can be redone in the painting app), raster layers, the
ink strokes, tones, effects, panel borders, and one layer per balloon. Layer
names are Unicode (the `luni` block) with an ASCII fallback. The merged image
is the printed page. Each layer is stored cropped to its content.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
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


def _raw(channel: Image.Image) -> bytes:
    return struct.pack(">H", 0) + channel.tobytes()


def _packbits_rows(pixels) -> tuple:
    """PackBits (PSD compression 1) of each row of a 2-D uint8 array, done with numpy: (the rows' byte counts,
    the packed bytes). Runs of 3 or more become repeat packets, the rest literal packets, 128 bytes at most each."""
    import numpy as np

    height, width = pixels.shape
    if height > 1024:  # (a band of rows at a time keeps the index arrays small)
        parts = [_packbits_rows(pixels[top:top + 1024]) for top in range(0, height, 1024)]
        return np.concatenate([c for c, _ in parts]).astype(">u2"), b"".join(d for _, d in parts)
    flat = np.ascontiguousarray(pixels).reshape(-1)
    starts = np.ones(flat.size, dtype=bool)
    starts[1:] = flat[1:] != flat[:-1]
    starts[::width] = True  # (a run never crosses a row)
    run_start = np.flatnonzero(starts)
    run_len = np.diff(np.append(run_start, flat.size))
    long_run = run_len >= 3
    first = np.ones(run_start.size, dtype=bool)
    first[1:] = long_run[1:] | long_run[:-1] | (run_start[1:] % width == 0)
    seg_start = run_start[first]
    seg_len = np.add.reduceat(run_len, np.flatnonzero(first))
    seg_rep = long_run[first]
    chunks = (seg_len + 127) // 128
    pk_seg = np.repeat(np.arange(seg_start.size), chunks)
    within = np.arange(pk_seg.size) - np.repeat(np.cumsum(chunks) - chunks, chunks)
    pk_start = seg_start[pk_seg] + within * 128
    pk_len = np.minimum(128, seg_len[pk_seg] - within * 128)
    pk_rep = seg_rep[pk_seg] & (pk_len >= 2)
    size = np.where(pk_rep, 2, pk_len + 1)
    offset = np.cumsum(size) - size
    out = np.empty(int(size.sum()), dtype=np.uint8)
    out[offset] = np.where(pk_rep, (257 - pk_len) & 0xFF, pk_len - 1).astype(np.uint8)
    out[offset[pk_rep] + 1] = flat[pk_start[pk_rep]]
    lit = ~pk_rep
    lit_len = pk_len[lit]
    src = np.repeat(pk_start[lit] - np.cumsum(lit_len) + lit_len, lit_len) + np.arange(int(lit_len.sum()))
    dst = np.repeat(offset[lit] + 1 - pk_start[lit], lit_len) + src
    out[dst] = flat[src]
    counts = np.bincount(pk_start // width, weights=size, minlength=height).astype(">u2")
    return counts, out.tobytes()


def _rle(channel: Image.Image) -> bytes:
    """A layer's channel, PackBits-compressed (a plain area costs a few bytes a row, not its width)."""
    import numpy as np

    counts, packed = _packbits_rows(np.asarray(channel.convert("L")))
    return struct.pack(">H", 1) + counts.tobytes() + packed


def _layer_records(layers: list, size: tuple[int, int]) -> bytes:
    """Layers as (name, image) or (name, image, {opacity, visible, blend, clip, section}); section 1 starts
    (the top of) a folder and 3 ends it (its bottom), as Photoshop writes them."""
    records = b""
    data = b""
    count = 0
    for item in layers:
        name, image = item[0], item[1]
        meta = item[2] if len(item) > 2 else {}
        rgba = (image if image is not None else Image.new("RGBA", (1, 1))).convert("RGBA")
        box = rgba.getbbox() if image is not None else None
        if box is None:
            box = (0, 0, 1, 1)  # keep empty layers (e.g. a blank balloon) as 1-pixel layers
        left, top, right, bottom = box
        crop = rgba.crop(box)
        if image is None:
            crop = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
        r, g, b, a = crop.split()
        records += struct.pack(">iiii", top, left, bottom, right)
        channels = [(-1, _rle(a)), (0, _rle(r)), (1, _rle(g)), (2, _rle(b))]
        mask = meta.get("mask")  # (an L picture over the whole canvas: white shows)
        mask_data = struct.pack(">I", 0)
        if mask is not None:
            mask = mask.convert("L").resize(size)
            channels.append((-2, _rle(mask)))
            mask_data = struct.pack(">I", 20) + struct.pack(">iiii", 0, 0, size[1], size[0]) + bytes([255, 0, 0, 0])
        records += struct.pack(">H", len(channels))
        for cid, packed in channels:
            records += struct.pack(">hI", cid, len(packed))
        flags = 0 if meta.get("visible", True) else 2
        opacity = max(0, min(255, round(255 * float(meta.get("opacity", 1.0)))))
        blend = BLEND_BACK.get(meta.get("blend") or "normal", b"norm")
        if meta.get("section") == 1 and meta.get("blend", "normal") == "normal":
            blend = b"pass"
        records += b"8BIM" + blend + struct.pack(">BBBB", opacity, 1 if meta.get("clip") else 0, flags, 0)
        extra = mask_data + struct.pack(">I", 0) + _pascal(name) + _unicode_name(name)
        if meta.get("section"):
            extra += b"8BIM" + b"lsct" + struct.pack(">II", 4, int(meta["section"]))
        records += struct.pack(">I", len(extra)) + extra
        for _, packed in channels:
            data += packed
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
    import numpy as np

    packed = [_packbits_rows(np.asarray(channel)) for channel in rgb.split()]  # (the merged picture: RLE, all rows' counts first)
    body += struct.pack(">H", 1) + b"".join(c.tobytes() for c, _ in packed) + b"".join(d for _, d in packed)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)
    return path


# --- the page as layers --------------------------------------------------------------------------


def page_layers(page: Page, episode: Episode, dpi: int) -> list[tuple]:
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
            meta = {"opacity": layer.opacity, "blend": layer.blend, "clip": layer.clip}
            if layer.mask and layer.mask.get("png") and layer.mask.get("enabled", True):
                import io

                meta["mask"] = Image.open(io.BytesIO(layer.mask["png"])).convert("L").resize(size)
            out.append((layer.title or f"{layer.role.value} {layer.id[:6]}", raster.convert("RGBA").resize(size), meta))
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
    from genko import nombre

    if nombre.placements(episode, page):
        numero = blank()
        nombre.draw(numero, episode, page, dpi)
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


# --- reading a PSD (or PSB) with its layers ------------------------------------------------------------------------

BLEND_KEYS = {b"norm": "normal", b"mul ": "multiply", b"scrn": "screen", b"lddg": "add", b"over": "overlay",
              b"dark": "darken", b"lite": "lighten", b"idiv": "color_burn", b"div ": "color_dodge", b"lbrn": "linear_burn",
              b"sLit": "soft_light", b"hLit": "hard_light", b"diff": "difference", b"smud": "exclusion",
              b"fsub": "subtract", b"fdiv": "divide", b"hue ": "hue", b"sat ": "saturation", b"colr": "color",
              b"lum ": "luminosity", b"pass": "normal"}
BLEND_BACK = {v: k for k, v in reversed(list(BLEND_KEYS.items()))}


class PSDError(ValueError):
    pass


@dataclass
class PSDLayer:
    """One layer as it was in the file: its picture over the whole canvas (RGBA), and how it mixes."""

    name: str
    image: Image.Image | None  # None for folders
    opacity: float = 1.0
    visible: bool = True
    blend: str = "normal"
    clip: bool = False
    folder: bool = False
    parent: int | None = None  # the index (in `layers`) of the folder it is in
    mask: Image.Image | None = None  # L over the whole canvas: white shows
    kind: str = "pixels"  # pixels | folder | text | adjust (adjustment and fill layers carry no pixels we use)


@dataclass
class PSDFile:
    size: tuple[int, int]
    dpi: float
    layers: list[PSDLayer]  # bottom to top, folders after the layers in them (as Genko keeps them)
    merged: Image.Image | None
    mode: str  # rgb | gray | cmyk | …
    skipped: list[str]


class _Reader:
    def __init__(self, data: bytes, psb: bool = False):
        self.data, self.pos, self.psb = data, 0, psb

    def take(self, n: int) -> bytes:
        if n < 0 or self.pos + n > len(self.data):
            raise PSDError("the PSD file is cut short")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def i16(self) -> int:
        return struct.unpack(">h", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self.take(4))[0]

    def i32(self) -> int:
        return struct.unpack(">i", self.take(4))[0]

    def length(self) -> int:  # (PSB uses 8-byte lengths in some places)
        return struct.unpack(">Q", self.take(8))[0] if self.psb else self.u32()


def _unpackbits(data: bytes, size: int) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n and len(out) < size:
        c = data[i]
        i += 1
        if c < 128:
            out += data[i:i + c + 1]
            i += c + 1
        elif c > 128:
            if i < n:
                out += bytes([data[i]]) * (257 - c)
            i += 1
    if len(out) < size:
        out += b"\x00" * (size - len(out))
    return bytes(out[:size])


def _channel(r: _Reader, width: int, height: int, depth: int, compression: int | None = None, end: int | None = None) -> bytes:
    """One channel's samples (8-bit), from the reader's position."""
    import zlib

    if compression is None:
        compression = r.u16()
    bpp = max(1, depth // 8)
    rowbytes = (width * depth + 7) // 8 if depth == 1 else width * bpp
    size = rowbytes * height
    if width <= 0 or height <= 0:
        if end is not None:
            r.pos = end
        return b""
    if compression == 0:
        raw = r.take(size)
    elif compression == 1:
        counts = [(r.u32() if r.psb else r.u16()) for _ in range(height)]
        raw = b"".join(_unpackbits(r.take(c), rowbytes) for c in counts)
    elif compression in (2, 3):
        raw = zlib.decompress(r.take((end or len(r.data)) - r.pos))
        if compression == 3:  # (each row stored as differences from the sample before)
            import numpy as np

            dtype = {1: np.uint8, 2: ">u2", 4: ">u4"}[bpp]
            arr = np.frombuffer(raw[:size], dtype=dtype).reshape(height, width).astype(np.uint64)
            arr = np.cumsum(arr, axis=1) % (1 << (8 * bpp))
            raw = arr.astype(dtype).tobytes()
    else:
        raise PSDError(f"unknown PSD compression {compression}")
    if end is not None:
        r.pos = end
    return _depth8(raw[:size], width, height, depth)


def _depth8(raw: bytes, width: int, height: int, depth: int) -> bytes:
    """Samples of any depth as 8-bit."""
    if depth == 8:
        return raw
    if depth == 16:
        return raw[0::2]
    if depth == 32:
        import numpy as np

        f = np.frombuffer(raw, dtype=">f4")
        return (np.clip(f, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8).tobytes()
    if depth == 1:
        img = Image.frombytes("1", (width, height), raw)
        return Image.eval(img.convert("L"), lambda v: 255 - v).tobytes()
    raise PSDError(f"PSD depth {depth} is not supported")


def _to_rgba(mode: int, channels: dict[int, bytes], size: tuple[int, int]) -> Image.Image:
    def band(cid: int, default: int = 0) -> Image.Image:
        data = channels.get(cid)
        return Image.frombytes("L", size, data) if data else Image.new("L", size, default)

    if mode == 4:  # CMYK (stored inverted: 255 is no ink)
        from PIL import ImageOps

        cmyk = Image.merge("CMYK", [ImageOps.invert(band(i, 255)) for i in range(4)])
        rgb = cmyk_to_rgb(cmyk)
    elif mode in (1, 0, 2, 8):  # grayscale, bitmap, indexed (as grey), duotone
        g = band(0, 255)
        rgb = Image.merge("RGB", (g, g, g))
    else:
        rgb = Image.merge("RGB", (band(0), band(1), band(2)))
    alpha = channels.get(-1)
    rgba = rgb.convert("RGBA")
    rgba.putalpha(Image.frombytes("L", size, alpha) if alpha else Image.new("L", size, 255))
    return rgba


MODE_NAMES = {0: "bitmap", 1: "gray", 2: "indexed", 3: "rgb", 4: "cmyk", 7: "multichannel", 8: "duotone", 9: "lab"}


def read_psd(source) -> PSDFile:
    """A PSD or PSB (Photoshop, CLIP STUDIO PAINT, Krita, GIMP…) as its layers: pixels, names, opacity,
    visibility, blend mode, clipping, folders and layer masks; the merged picture too."""
    data = source if isinstance(source, (bytes, bytearray)) else Path(source).read_bytes()
    if data[:4] != b"8BPS":
        raise PSDError("not a PSD file")
    version = struct.unpack(">H", data[4:6])[0]
    if version not in (1, 2):
        raise PSDError("unknown PSD version")
    r = _Reader(bytes(data), psb=version == 2)
    r.take(12)
    nchan, height, width, depth, mode = r.u16(), r.u32(), r.u32(), r.u16(), r.u16()
    if mode == 9:
        raise PSDError("Lab PSD files are not supported: save it as RGB or CMYK")
    r.take(r.u32())  # colour mode data
    dpi = 72.0
    res_end = r.u32()
    res_end += r.pos
    while r.pos + 12 <= res_end:
        if r.take(4) != b"8BIM":
            break
        rid = r.u16()
        nlen = r.u8()
        r.take(nlen + (1 - nlen % 2))  # (the Pascal name, padded to even with its length byte)
        size = r.u32()
        block = r.take(size + size % 2)
        if rid == 1005 and size >= 4:
            dpi = struct.unpack(">I", block[:4])[0] / 65536 or 72.0
    r.pos = res_end
    lm_len = r.length()
    lm_end = r.pos + lm_len
    records: list[dict] = []
    skipped: list[str] = []
    if lm_len:
        li_len = r.length()
        li_end = r.pos + li_len
        if li_len:
            count = abs(r.i16())
            for _ in range(count):
                top, left, bottom, right = r.i32(), r.i32(), r.i32(), r.i32()
                chans = [(r.i16(), r.length()) for _ in range(r.u16())]
                if r.take(4) not in (b"8BIM", b"8B64"):
                    raise PSDError("broken PSD layer record")
                key = r.take(4)
                opacity, clipping, flags = r.u8(), r.u8(), r.u8()
                r.u8()
                extra_end = r.u32()
                extra_end += r.pos
                mask = None
                mlen = r.u32()
                if mlen:
                    mend = r.pos + mlen
                    mt, ml, mb, mr = r.i32(), r.i32(), r.i32(), r.i32()
                    default, mflags = r.u8(), r.u8()
                    mask = {"box": (ml, mt, mr, mb), "default": default, "disabled": bool(mflags & 2)}
                    r.pos = mend
                ranges = r.u32()  # blending ranges
                r.pos += ranges
                nlen = r.u8()
                name = r.take(nlen).decode("latin-1", "replace")
                r.pos += (4 - (nlen + 1) % 4) % 4
                section, kind = 0, "pixels"
                while r.pos + 12 <= extra_end:
                    sig = r.take(4)
                    if sig not in (b"8BIM", b"8B64"):
                        break
                    tag = r.take(4)
                    long_keys = {b"LMsk", b"Lr16", b"Lr32", b"Layr", b"Mt16", b"Mt32", b"Mtrn", b"Alph", b"FMsk",
                                 b"lnk2", b"FEid", b"FXid", b"PxSD"}
                    size = struct.unpack(">Q", r.take(8))[0] if r.psb and tag in long_keys else r.u32()
                    body = r.take(size)
                    if tag == b"luni" and len(body) >= 4:
                        n = struct.unpack(">I", body[:4])[0]
                        name = body[4:4 + 2 * n].decode("utf-16-be", "replace").rstrip("\x00")
                    elif tag in (b"lsct", b"lsdk") and len(body) >= 4:
                        section = struct.unpack(">I", body[:4])[0]
                    elif tag == b"TySh":
                        kind = "text"
                    elif tag in (b"SoCo", b"GdFl", b"PtFl", b"levl", b"curv", b"brit", b"hue2", b"blnc", b"nvrt",
                                 b"post", b"thrs", b"grdm", b"selc", b"mixr", b"phfl", b"expA", b"vibA", b"blwh", b"clrL"):
                        kind = "adjust"
                    if size % 2 and r.pos < extra_end and r.data[r.pos:r.pos + 4] not in (b"8BIM", b"8B64"):
                        r.pos += 1
                r.pos = extra_end
                records.append({"box": (left, top, right, bottom), "chans": chans, "blend": BLEND_KEYS.get(key, "normal"),
                                "opacity": opacity / 255, "clip": clipping == 1, "visible": not flags & 2, "name": name,
                                "section": section, "kind": kind, "mask": mask, "unknown_blend": key not in BLEND_KEYS})
            for rec in records:
                left, top, right, bottom = rec["box"]
                w, h = right - left, bottom - top
                chans = {}
                for cid, clen in rec["chans"]:
                    end = r.pos + clen
                    if clen < 2:
                        r.pos = end
                        continue
                    if cid == -2 and rec["mask"]:
                        ml, mt, mr, mb = rec["mask"]["box"]
                        chans[cid] = (_channel(r, mr - ml, mb - mt, depth, end=end), (mr - ml, mb - mt))
                    elif cid < -2:
                        r.pos = end
                    else:
                        chans[cid] = (_channel(r, w, h, depth, end=end), (w, h))
                rec["data"] = chans
        r.pos = li_end
    r.pos = lm_end
    merged = None
    try:
        compression = r.u16()
        rowbytes = (width + 7) // 8 if depth == 1 else width * max(1, depth // 8)
        if compression == 1:
            counts = [(r.u32() if r.psb else r.u16()) for _ in range(height * nchan)]
            planes = [_depth8(b"".join(_unpackbits(r.take(n), rowbytes) for n in counts[c * height:(c + 1) * height]),
                              width, height, depth) for c in range(nchan)]
        elif compression == 0:
            planes = [_depth8(r.take(rowbytes * height), width, height, depth) for _ in range(nchan)]
        else:
            planes = []
        colour = {4: 4, 3: 3}.get(mode, 1)
        if len(planes) >= colour:
            chans = {i: planes[i] for i in range(colour)}
            if len(planes) > colour and not records:
                chans[-1] = planes[colour]  # (a flat file's transparency)
            merged = _to_rgba(mode, chans, (width, height))
    except (PSDError, ValueError, struct.error):
        merged = None
    layers: list[PSDLayer] = []
    stack: list[int] = []  # (open folders, innermost last, while reading from the bottom)
    pending: list[list[int]] = []
    for rec in records:
        if rec["section"] == 3:  # the end of a folder (seen first, from the bottom)
            pending.append([])
            stack.append(-1)
            continue
        left, top, right, bottom = rec["box"]
        if rec["section"] in (1, 2):
            folder = PSDLayer(name=rec["name"] or "フォルダー", image=None, opacity=rec["opacity"], visible=rec["visible"],
                              blend=rec["blend"], folder=True, kind="folder")
            kids = pending.pop() if pending else []
            if stack:
                stack.pop()
            layers.append(folder)
            at = len(layers) - 1
            for k in kids:
                layers[k].parent = at
            if pending:
                pending[-1].append(at)
            continue
        canvas = None
        if rec["kind"] != "adjust" and right > left and bottom > top and any(c >= 0 for c in rec["data"]):
            part = _to_rgba(mode, {c: d for c, (d, _s) in rec["data"].items() if c >= -1}, (right - left, bottom - top))
            canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            canvas.paste(part, (left, top))
        if canvas is None:
            skipped.append(rec["name"])
            continue
        mask_img = None
        if rec["mask"] and -2 in rec["data"] and not rec["mask"]["disabled"]:
            ml, mt, mr, mb = rec["mask"]["box"]
            mask_img = Image.new("L", (width, height), rec["mask"]["default"])
            data, (mw, mh) = rec["data"][-2]
            if mw > 0 and mh > 0 and data:
                mask_img.paste(Image.frombytes("L", (mw, mh), data), (ml, mt))
        layers.append(PSDLayer(name=rec["name"] or "レイヤー", image=canvas, opacity=rec["opacity"], visible=rec["visible"],
                               blend=rec["blend"], clip=rec["clip"], mask=mask_img, kind=rec["kind"]))
        if pending:
            pending[-1].append(len(layers) - 1)
    return PSDFile(size=(width, height), dpi=dpi, layers=layers, merged=merged, mode=MODE_NAMES.get(mode, str(mode)),
                   skipped=skipped)


def cmyk_to_rgb(image: Image.Image, profile: str | None = None) -> Image.Image:
    """CMYK pixels as RGB (with an ICC profile when one is given; else the plain formula)."""
    if profile:
        from PIL import ImageCms

        return ImageCms.profileToProfile(image, profile, ImageCms.createProfile("sRGB"), outputMode="RGB")
    return image.convert("RGB")
