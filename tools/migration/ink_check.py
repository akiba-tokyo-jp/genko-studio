#!/usr/bin/env python3
"""color-mode@1 (docs/cpp-migration/SPEC.md COLOR-02): what the reference Python's Pillow (ImageCms), psd-tools and a
plain TIFF tag reader make of colours and of the files the C++ build writes, for native/tests/contract/test_contract_ink.

    ink_check.py <request.json> <out.json>

The request: {"convert": {"icc": path, "rgb": [[r, g, b], …], "cmyk": [[c, m, y, k], …], "lab": [[L, a, b], …]}} — the
colours through Pillow (sRGB → CMYK and CMYK → sRGB with the profile, relative colorimetric with black point
compensation; Pillow's own LAB → RGB) — and/or {"read": {"files": [path, …], "points": [[x, y], …]}} — each file as
Pillow opens it (mode, size, its ICC profile's length, the pixels at the points, as RGBA too where Pillow converts it),
a PSD's colour mode, depth and layers by psd-tools, a TIFF's tags.
"""

import json
import struct
import sys

from PIL import Image, ImageCms

INTENT = ImageCms.Intent.RELATIVE_COLORIMETRIC
BPC = ImageCms.Flags.BLACKPOINTCOMPENSATION


def row(mode, values):
    image = Image.new(mode, (len(values), 1))
    image.putdata([tuple(v) for v in values])
    return image


def pixels(image):
    return [list(image.getpixel((i, 0))) for i in range(image.size[0])]


def convert(req):
    icc = ImageCms.getOpenProfile(req["icc"])
    srgb = ImageCms.createProfile("sRGB")
    out = {}
    if req.get("rgb"):
        made = ImageCms.profileToProfile(row("RGB", req["rgb"]), srgb, icc, renderingIntent=INTENT, outputMode="CMYK", flags=BPC)
        out["cmyk"] = pixels(made)
    if req.get("cmyk"):
        made = ImageCms.profileToProfile(row("CMYK", req["cmyk"]), icc, srgb, renderingIntent=INTENT, outputMode="RGB", flags=BPC)
        out["srgb"] = pixels(made)
    if req.get("lab"):
        out["lab_rgb"] = pixels(row("LAB", req["lab"]).convert("RGB"))
    return out


def tiff_tags(path):
    """The first IFD's tags (number → values) of a classic TIFF."""
    with open(path, "rb") as f:
        data = f.read()
    order = "<" if data[:2] == b"II" else ">"
    (ifd,) = struct.unpack(order + "I", data[4:8])
    (count,) = struct.unpack(order + "H", data[ifd:ifd + 2])
    sizes = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1}
    codes = {1: "B", 2: "c", 3: "H", 4: "I", 5: "II", 7: "B"}
    tags = {}
    for n in range(count):
        at = ifd + 2 + 12 * n
        tag, kind, length = struct.unpack(order + "HHI", data[at:at + 8])
        size = sizes.get(kind, 1) * length
        where = at + 8 if size <= 4 else struct.unpack(order + "I", data[at + 8:at + 12])[0]
        raw = data[where:where + size]
        if kind in (3, 4):
            tags[tag] = list(struct.unpack(order + codes[kind] * length, raw))
        else:
            tags[tag] = len(raw)
    return {"photometric": tags.get(262, [None])[0], "bits": tags.get(258), "samples": tags.get(277, [1])[0],
            "compression": tags.get(259, [None])[0], "inkset": tags.get(332, [None])[0], "icc": tags.get(34675, 0)}


def read(req):
    out = {}
    for path in req["files"]:
        info = {}
        try:
            image = Image.open(path)
            info["mode"] = image.mode
            info["size"] = list(image.size)
            info["icc"] = len(image.info.get("icc_profile") or b"")
            info["pixels"] = [list(image.getpixel(tuple(p))) for p in req.get("points", [])]
            if image.mode in ("LAB", "RGB", "RGBA"):
                rgba = image.convert("RGBA")
                info["rgba"] = [list(rgba.getpixel(tuple(p))) for p in req.get("points", [])]
        except Exception as error:  # (16-bit Lab TIFFs: Pillow does not read them)
            info["pillow_error"] = f"{type(error).__name__}: {error}"
        if path.endswith(".psd"):
            from psd_tools import PSDImage

            psd = PSDImage.open(path)
            info["psd"] = {"mode": int(psd.color_mode), "depth": psd.depth, "channels": psd.channels,
                           "layers": [layer.name for layer in psd]}
        if path.endswith(".tiff") or path.endswith(".tif"):
            info["tiff"] = tiff_tags(path)
        out[path] = info
    return out


def main():
    with open(sys.argv[1], encoding="utf-8") as f:
        req = json.load(f)
    out = {}
    if "convert" in req:
        out["convert"] = convert(req["convert"])
    if "read" in req:
        out["read"] = read(req["read"])
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False)


if __name__ == "__main__":
    main()
