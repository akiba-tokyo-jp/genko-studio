"""The PSD files of psd-keep@1 and the layers at their depth (PC4 F-d, SPEC.md FILE-01「PSD・PSB」), made with psd-tools.

Run with the reference Python (psd-tools 1.10.9, Pillow 12.3.0, numpy 2.4.6):
    <reference python> native/tests/data/psd_keep/make_psd_keep.py native/tests/data/psd_keep
It writes, and lists with their sha256 in MANIFEST.json:
  keep.psd      RGB 8 bits, 24 × 16 px at 150 ppi, three layers — "効果" (layer effects lfx2: a drop shadow; a label colour
                lclr; locks lspf 5 = transparency and position; its id lyid 7; fill opacity iOpa; the effects' reference
                point fxrp; a layer name source lnsr), "Blend If" (blending ranges not at their default; lyid 8; lspf
                0x80000000, lock all), "ふつう" (lyid 9) —, the image resources a page keeps (global angle 1037 and
                altitude 1049, layer comps 1065, slices 1050 — a layer slice of lyid 7 —, a URL list 1054) beside the ones
                this build writes itself (resolution 1005, XMP 1060), and the global blocks a page keeps (annotations
                Anno: one text note; patterns Patt: one 2 × 2 pattern no fill uses).
  deep-rgb16.psd, deep-rgb32.psd, deep-cmyk16.psd, deep-lab16.psd, deep-gray16.psd
                8 × 6 px, two layers each ("下" opaque over all, "上" a 5 × 4 box half transparent), their samples at the
                file's depth with values no 8-bit sample holds (16 bits: odd values; 32 bits: past 1 too).
  deep-rgb16-lr16.psd
                deep-rgb16.psd's layers in the global block Lr16 (as Photoshop writes a 16-bit file's), the layer info
                section empty.
Every file is opened again with psd-tools before it is listed (a file it cannot read is not written).
"""

import hashlib
import io
import json
import struct
import sys
from pathlib import Path

from psd_tools import PSDImage
from psd_tools.constants import ChannelID, Compression, Tag
from psd_tools.psd.image_resources import ImageResource
from psd_tools.psd.layer_and_mask import ChannelData, ChannelDataList, ChannelInfo, LayerBlendingRanges, LayerRecord, TaggedBlocks
from psd_tools.psd.tagged_blocks import TYPES, TaggedBlock
from psd_tools.api.layers import PixelLayer


def u16(v):
    return struct.pack(">H", v)


def u32(v):
    return struct.pack(">I", v)


def i32(v):
    return struct.pack(">i", v)


# --- descriptors (the file format's ActionDescriptor) -------------------------------------------------------------------

def key(s):
    b = s.encode("ascii")
    return u32(0) + b if len(b) == 4 else u32(len(b)) + b


def ustr(s):
    u = (s + "\0").encode("utf-16-be")
    return u32(len(u) // 2) + u


def descriptor(class_id, items):
    out = ustr("") + key(class_id) + u32(len(items))
    for k, v in items:
        out += key(k) + v
    return out


def untf(unit, value):
    return b"UntF" + unit.encode("ascii") + struct.pack(">d", value)


def boolean(v):
    return b"bool" + bytes([1 if v else 0])


def enum(t, v):
    return b"enum" + key(t) + key(v)


def long(v):
    return b"long" + i32(v)


def text(s):
    return b"TEXT" + ustr(s)


def objc(class_id, items):
    return b"Objc" + descriptor(class_id, items)


def colour(r, g, b):
    return objc("RGBC", [("Rd  ", b"doub" + struct.pack(">d", r)), ("Grn ", b"doub" + struct.pack(">d", g)),
                         ("Bl  ", b"doub" + struct.pack(">d", b))])


def lfx2():
    shadow = objc("DrSh", [("enab", boolean(True)), ("present", boolean(True)), ("showInDialog", boolean(True)),
                           ("Md  ", enum("BlnM", "Mltp")), ("Clr ", colour(0.0, 0.0, 0.0)), ("Opct", untf("#Prc", 75.0)),
                           ("uglg", boolean(True)), ("lagl", untf("#Ang", 120.0)), ("Dstn", untf("#Pxl", 3.0)),
                           ("Ckmt", untf("#Pxl", 0.0)), ("blur", untf("#Pxl", 2.0)), ("Nose", untf("#Prc", 0.0)),
                           ("AntA", boolean(False)), ("layerConceals", boolean(True))])
    return u32(0) + u32(16) + descriptor("null", [("Scl ", untf("#Prc", 100.0)), ("masterFXSwitch", boolean(True)), ("DrSh", shadow)])


def comps():
    comp = objc("Comp", [("Nm  ", text("カンプ 1")), ("compID", long(1)), ("capturedInfo", long(1))])
    listed = b"VlLs" + u32(1) + comp
    return u32(16) + descriptor("null", [("list", listed)])


def slices():
    """Slices resource version 6: the slices' bounds, a group name, one layer slice of lyid 7."""
    out = u32(6) + i32(0) + i32(0) + i32(16) + i32(24) + ustr("スライス") + u32(1)
    out += u32(1) + u32(0) + u32(1) + u32(7)  # (id, group id, origin 1 = layer, its layer id)
    out += ustr("効果") + u32(1) + i32(2) + i32(3) + i32(14) + i32(11)  # (name, type 1 image, left top right bottom)
    out += ustr("") + ustr("") + ustr("") + ustr("") + b"\x00" + ustr("") + u32(0) + u32(0) + bytes([0, 0, 0, 0])
    return out


def annotations():
    note_text = "﻿メモ".encode("utf-16-be")
    one = b"txtA" + bytes([1, 0]) + u16(1) + i32(2) + i32(2) + i32(6) + i32(6) + i32(2) + i32(8) + i32(40) + i32(60)
    one += u16(0) + u16(65535) + u16(52428) + u16(0) + u16(0)  # (its colour: RGB)
    for pascal in (b"author", b"note", b""):
        body = bytes([len(pascal)]) + pascal
        one += body + b"\x00" * (len(body) % 2)
    one += u32(len(note_text) + 12) + b"txtC" + u32(len(note_text)) + note_text
    return u16(2) + u16(1) + u32(1) + u32(len(one) + 4) + one


def pattern():
    """One 2 × 2 RGB pattern (Patt: its length, the pattern, padded to 4 bytes)."""
    pid = b"0d9b8b1e-5a3f-4c2e-9e7a-6f1b2c3d4e5f"
    head = u32(1) + u32(3) + u16(2) + u16(2) + ustr("市松") + bytes([len(pid)]) + pid
    arrays = b""
    for plane in (b"\xff\x00\x00\xff", b"\x00\xff\xff\x00", b"\x80\x40\x40\x80"):
        body = u32(8) + i32(0) + i32(0) + i32(2) + i32(2) + u16(8) + b"\x00" + plane
        arrays += u32(1) + u32(len(body)) + body
    for _ in range(2):  # (the user and sheet masks: absent)
        arrays += u32(0)
    vma = u32(3) + u32(len(arrays) + 16 + 4) + i32(0) + i32(0) + i32(2) + i32(2) + u32(3) + arrays  # (its 3 colours, then 2 masks)
    one = head + vma
    return u32(len(one)) + one + b"\x00" * ((4 - len(one) % 4) % 4)


def xmp():
    return ('<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF '
            'xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:creator><rdf:Seq><rdf:li>作者の名前</rdf:li></rdf:Seq>'
            '</dc:creator></rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>').encode("utf-8")


# --- layers ---------------------------------------------------------------------------------------------------------

def layer(psd, name, box, planes, depth, alpha=None):
    """A pixel layer of `planes` (big-endian samples of the box, the file's colour channels in order) and `alpha`."""
    left, top, right, bottom = box
    w, h = right - left, bottom - top
    # (its name in the record in MacRoman, as "?" where it has none; in Unicode in luni)
    record = LayerRecord(top=top, left=left, bottom=bottom, right=right, name=name.encode("ascii", "replace").decode("ascii"))
    record.tagged_blocks = TaggedBlocks()
    record.tagged_blocks.set_data(Tag.UNICODE_LAYER_NAME, name)
    channels = ChannelDataList()
    infos = []
    full = {8: b"\xff", 16: b"\xff\xff", 32: struct.pack(">f", 1.0)}[depth]
    for cid, data in [(ChannelID.TRANSPARENCY_MASK, alpha if alpha is not None else full * (w * h))] + list(enumerate(planes)):
        c = ChannelData(Compression.RLE if depth == 8 else Compression.ZIP)
        c.set_data(data, w, h, depth)
        channels.append(c)
        infos.append(ChannelInfo(id=ChannelID(cid), length=len(c.data) + 2))
    record.channel_info = infos
    return PixelLayer(psd, record, channels, None)


def block(blocks, k, data):
    """A tagged block of these bytes, as psd-tools holds one of its key (parsed when it knows the key: it writes it so)."""
    tag = Tag(k.encode("ascii"))
    kls = TYPES.get(tag)
    blocks[tag] = TaggedBlock(key=tag, data=kls.frombytes(data) if kls is not None else data)


def keep_psd():
    psd = PSDImage.new("RGB", (24, 16), color=(255, 255, 255), depth=8)
    psd.image_resources[1005] = ImageResource(key=1005, data=struct.pack(">IHHIHH", 150 << 16, 1, 1, 150 << 16, 1, 1))
    for rid, data in [(1037, i32(120)), (1049, i32(30)), (1065, comps()), (1050, slices()), (1054, u32(0)), (1060, xmp())]:
        psd.image_resources[rid] = ImageResource(key=rid, data=data)
    w, h = 10, 8
    red = layer(psd, "効果", (2, 3, 2 + w, 3 + h), [b"\xd0" * (w * h), b"\x20" * (w * h), b"\x30" * (w * h)], 8)
    b = red._record.tagged_blocks
    block(b, "lfx2", lfx2())
    block(b, "lclr", struct.pack(">H6x", 1))
    block(b, "lspf", u32(5))
    block(b, "lyid", u32(7))
    block(b, "iOpa", bytes([192, 0, 0, 0]))
    block(b, "fxrp", struct.pack(">dd", 7.0, 7.0))
    block(b, "lnsr", b"rend")
    green = layer(psd, "Blend If", (8, 2, 20, 12), [b"\x10" * 120, b"\xc0" * 120, b"\x40" * 120], 8)
    green._record.blending_ranges = LayerBlendingRanges(composite_ranges=[(0, 0xC8FF), (0, 65535)],
                                                        channel_ranges=[[(0, 65535), (0x2000, 65535)], [(0, 65535), (0, 65535)],
                                                                        [(0, 65535), (0, 65535)], [(0, 65535), (0, 65535)]])
    block(green._record.tagged_blocks, "lyid", u32(8))
    block(green._record.tagged_blocks, "lspf", u32(0x80000000))
    blue = layer(psd, "ふつう", (14, 6, 22, 14), [b"\x20" * 64, b"\x40" * 64, b"\xe0" * 64], 8)
    block(blue._record.tagged_blocks, "lyid", u32(9))
    for one in (red, green, blue):
        psd.append(one)
    globals_ = psd._record.layer_and_mask_information.tagged_blocks
    if globals_ is None:
        globals_ = TaggedBlocks()
        psd._record.layer_and_mask_information.tagged_blocks = globals_
    block(globals_, "Anno", annotations())
    block(globals_, "Patt", pattern())
    return psd


def deep_planes(mode, depth, w, h, seed):
    """Samples no 8-bit sample holds: 16 bits odd values, 32 bits values past 1 too."""
    colours = {"RGB": 3, "CMYK": 4, "LAB": 3, "L": 1}[mode]
    planes = []
    for c in range(colours):
        out = b""
        for i in range(w * h):
            if depth == 16:
                v = (seed * 7919 + c * 4099 + i * 1237) % 65536 | 1
                out += u16(v)
            else:
                v = ((seed * 31 + c * 17 + i * 13) % 1000) / 700.0  # (0 to 1.43)
                out += struct.pack(">f", v)
        planes.append(out)
    return planes


def deep_psd(mode, depth):
    w, h = 8, 6
    psd = PSDImage.new(mode, (w, h), color=0, depth=depth)
    psd.image_resources[1005] = ImageResource(key=1005, data=struct.pack(">IHHIHH", 300 << 16, 1, 1, 300 << 16, 1, 1))
    below = layer(psd, "下", (0, 0, w, h), deep_planes(mode, depth, w, h, 1), depth)
    half = {16: u16(32768) * 20, 32: struct.pack(">f", 0.5) * 20}[depth]
    above = layer(psd, "上", (2, 1, 7, 5), deep_planes(mode, depth, 5, 4, 2), depth, alpha=half)
    psd.append(below)
    psd.append(above)
    return psd


def checked(psd, path, composite=True):
    """The file psd-tools writes (`composite`: its merged picture the layers psd-tools composites — at 8 bits only —; else
    the one PSDImage.new made, of its own depth), opened again with psd-tools before it is kept."""
    data = io.BytesIO()
    if composite:
        psd.save(data)
    else:
        psd._update_record()
        psd._record.write(data)
    raw = data.getvalue()
    again = PSDImage.open(io.BytesIO(raw))  # (psd-tools reads it: every block it knows parsed)
    assert len(list(again.descendants())) >= 2, path
    path.write_bytes(raw)
    return raw


def lr16(raw):
    """The file psd-tools wrote with its layer info moved into the global block Lr16 — the layer info section's length 0,
    as Photoshop writes a 16-bit file —, read again by psd-tools (which takes the layers from Lr16)."""
    at = 26
    at += 4 + struct.unpack(">I", raw[at:at + 4])[0]  # (colour mode data)
    at += 4 + struct.unpack(">I", raw[at:at + 4])[0]  # (image resources)
    section = struct.unpack(">I", raw[at:at + 4])[0]
    start, end = at + 4, at + 4 + section
    info_length = struct.unpack(">I", raw[start:start + 4])[0]
    info = raw[start + 4:start + 4 + info_length]
    rest = raw[start + 4 + info_length:end]  # (the global layer mask info, the global blocks)
    mask = 4 + struct.unpack(">I", rest[0:4])[0]
    lr = b"8BIMLr16" + u32(len(info)) + info + b"\x00" * ((4 - len(info) % 4) % 4)
    body = u32(0) + rest[:mask] + lr + rest[mask:]
    out = raw[:at] + u32(len(body)) + body + raw[end:]
    again = PSDImage.open(io.BytesIO(out))
    assert len(list(again.descendants())) == 2
    return out


def main():
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    files = {}
    files["keep.psd"] = checked(keep_psd(), folder / "keep.psd")
    for name, mode, depth in [("deep-rgb16.psd", "RGB", 16), ("deep-rgb32.psd", "RGB", 32), ("deep-cmyk16.psd", "CMYK", 16),
                              ("deep-lab16.psd", "LAB", 16), ("deep-gray16.psd", "L", 16)]:
        files[name] = checked(deep_psd(mode, depth), folder / name, composite=False)
    moved = lr16(files["deep-rgb16.psd"])
    (folder / "deep-rgb16-lr16.psd").write_bytes(moved)
    files["deep-rgb16-lr16.psd"] = moved
    import PIL
    import psd_tools
    manifest = {
        "about": "PC4 F-d test files (test_contract_psd: psd-keep@1, the layers at their depth, PSB). Made with psd-tools "
                 + psd_tools.__version__ + " (Pillow " + PIL.__version__ + "); see make_psd_keep.py for what each holds.",
        "generators": {"*.psd": "<reference python> native/tests/data/psd_keep/make_psd_keep.py native/tests/data/psd_keep"},
        "files": {name: hashlib.sha256(raw).hexdigest() for name, raw in sorted(files.items())},
    }
    (folder / "MANIFEST.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
