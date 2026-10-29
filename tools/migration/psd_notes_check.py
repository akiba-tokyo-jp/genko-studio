"""PC5 J-c (SPEC.md PROD-01「注釈」): a PSD's notes as psd-tools reads them, for test_contract_annotations — the global
block Anno (each note's kind, open flag, icon and box rects, colour, author, name, date, and its text: UTF-16 after its
byte-order mark, lines ending in CR, given back with \\n) — and a PSD of notes psd-tools writes (a text note and a sound
note, whose bytes Genko keeps).

    <reference python> tools/migration/psd_notes_check.py view OUT.json FILE
    <reference python> tools/migration/psd_notes_check.py make OUT.psd
    <reference python> tools/migration/psd_notes_check.py make-far OUT.psd    (the text note's icon and box far past the canvas)
    <reference python> tools/migration/psd_notes_check.py make-odd OUT.psd    (its text a surrogate without its pair)
"""

import json
import sys

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import PixelLayer
from psd_tools.constants import ColorSpaceID, Tag
from psd_tools.psd.color import Color
from psd_tools.psd.layer_and_mask import TaggedBlocks
from psd_tools.psd.tagged_blocks import Annotation, Annotations, TaggedBlock


def text_of(data: bytes) -> str:
    if data[:2] == b"\xfe\xff":
        text = data[2:].decode("utf-16-be", errors="replace")
    else:
        text = data.decode("latin-1")
    return text.rstrip("\x00").replace("\r\n", "\n").replace("\r", "\n")


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    blocks = psd._record.layer_and_mask_information.tagged_blocks
    out = {"notes": None}
    if blocks is not None and Tag.ANNOTATIONS in blocks:
        notes = []
        for a in blocks.get_data(Tag.ANNOTATIONS):
            item = {
                "kind": a.kind.decode("latin-1"),
                "open": int(a.is_open),
                "icon": list(a.icon_location),
                "popup": list(a.popup_location),
                "color_space": int(getattr(a.color.id, "value", a.color.id)),
                "color": list(a.color.values),
                "author": a.author,
                "name": a.name,
                "modified": a.mod_date,
                "marker": a.marker.decode("latin-1"),
                "data": a.data.hex(),
            }
            if a.kind == b"txtA":
                item["text"] = text_of(a.data)
            notes.append(item)
        out["notes"] = notes
    return out


def make(path: str, variant: str = "") -> None:
    psd = PSDImage.new("RGB", (40, 30), color=(255, 255, 255), depth=8)
    psd.append(PixelLayer.frompil(Image.new("RGB", (10, 8), (200, 40, 40)), psd, "layer", top=4, left=6))
    text = "﻿音声の前\r2 行目".encode("utf-16-be")
    if variant == "odd":
        text = "﻿a".encode("utf-16-be") + b"\xd8\x00" + "b".encode("utf-16-be")
    icon = [4, 6, 24, 22] if variant != "far" else [50, 100000000, 70, 100000016]
    popup = [10, 30, 100, 200] if variant != "far" else [-2000000000, 100000000, 2000000000, 100000300]
    notes = Annotations(items=[
        Annotation(kind=b"txtA", is_open=1, flags=28, optional_blocks=1, icon_location=icon,
                   popup_location=popup, color=Color(ColorSpaceID.RGB, [65535, 0, 0, 0]), author="tester", name="",
                   mod_date="D:20240102030405Z", marker=b"txtC", data=text),
        Annotation(kind=b"sndM", is_open=0, flags=0, optional_blocks=1, icon_location=[20, 20, 40, 36],
                   popup_location=[0, 0, 0, 0], color=Color(ColorSpaceID.RGB, [0, 0, 65535, 0]), author="voice", name="",
                   mod_date="", marker=b"sndM", data=bytes(range(32))),
    ])
    lm = psd._record.layer_and_mask_information
    if lm.tagged_blocks is None:
        lm.tagged_blocks = TaggedBlocks()
    lm.tagged_blocks[Tag.ANNOTATIONS] = TaggedBlock(key=Tag.ANNOTATIONS, data=notes)
    psd.save(path)
    again = view(path)  # (a file psd-tools cannot read again is not one)
    assert again["notes"] is not None and len(again["notes"]) == 2, again


def main() -> int:
    mode, out_path = sys.argv[1], sys.argv[2]
    if mode.startswith("make"):
        make(out_path, mode[5:])
        return 0
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(view(sys.argv[3]), f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
