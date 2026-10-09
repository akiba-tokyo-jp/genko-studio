#!/usr/bin/env python3
"""BRUSH-01 紙質: an independent reference of the paper texture's arithmetic (native/src/render/paper.hpp).

The C++ build's paper is new (Python's Genko has none), so this is not the Python baseline: it is the algorithm of
render/paper.hpp written again from its description, with Python's floats (IEEE double, no fused multiply-add) and
integers, to fix the expected values of the small analysable fixture (ACCEPTANCE.md AC-BRUSH): from a pixel's mm
position to the paper's sample and the ink left, for fixed inputs, seeds and every setting. The test pictures (the
paper picture files and what Pillow makes of them: laid over white, made grey) come from here too.

  python tools/migration/paper_reference.py samples native/tests/data/paper/samples.json
  python tools/migration/paper_reference.py pictures native/tests/data/paper
  python tools/migration/paper_reference.py check native/tests/data/paper   (regenerates in memory and compares)
"""
import argparse
import base64
import io
import json
import math
import random
import sys
from pathlib import Path

MASK64 = (1 << 64) - 1
BASE_DPI = 300.0
DEG = 0.017453292519943295  # math.pi / 180 as a double


# --- the arithmetic (render/paper.hpp) -------------------------------------------------------------------------------

def splitmix(state):
    """SplitMix64: the next state and its output."""
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, z ^ (z >> 31)


def fnv1a(data: bytes):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & MASK64
    return h


def offsets(paper, line_seed, width, height):
    key = paper["seed"] & MASK64
    if paper["coords"] == "stroke":
        key ^= fnv1a(line_seed.encode("utf-8"))
    state, a = splitmix(key)
    state, b = splitmix(state)
    return a % width, b % height


def _poly(x):
    """sin and cos of x degrees, 0 <= x <= 45."""
    t = x * DEG
    t2 = t * t
    s = t * (1.0 + t2 * (-1.0 / 6.0 + t2 * (1.0 / 120.0 + t2 * (-1.0 / 5040.0 + t2 * (1.0 / 362880.0 + t2 * (
        -1.0 / 39916800.0 + t2 * (1.0 / 6227020800.0 + t2 * (-1.0 / 1307674368000.0))))))))
    c = 1.0 + t2 * (-1.0 / 2.0 + t2 * (1.0 / 24.0 + t2 * (-1.0 / 720.0 + t2 * (1.0 / 40320.0 + t2 * (
        -1.0 / 3628800.0 + t2 * (1.0 / 479001600.0 + t2 * (-1.0 / 87178291200.0 + t2 * (1.0 / 20922789888000.0))))))))
    return s, c


def sin_cos_degrees(degrees):
    r = math.fmod(degrees, 360.0)
    if r < 0.0:
        r += 360.0
    if r >= 360.0:
        r = 0.0
    q = 0 if r < 90.0 else 1 if r < 180.0 else 2 if r < 270.0 else 3
    x = r - 90.0 * q
    swap = x > 45.0
    if swap:
        x = 90.0 - x
    s, c = _poly(x)
    if swap:
        s, c = c, s
    if q == 1:
        s, c = c, -s
    elif q == 2:
        s, c = -s, -c
    elif q == 3:
        s, c = -c, s
    return s, c


def wrap(i, n, seam):
    if seam == "mirror":
        m = i % (2 * n)
        return 2 * n - 1 - m if m >= n else m
    return i % n


def sample(grain, paper, dpi, px, py, anchor, offset):
    width, height, values = grain["width"], grain["height"], grain["values"]
    x_mm = (px + 0.5) * 25.4 / dpi
    y_mm = (py + 0.5) * 25.4 / dpi
    if paper["coords"] == "stroke":
        dx = x_mm - anchor[0]
        dy = y_mm - anchor[1]
    else:
        dx = x_mm
        dy = y_mm
    s, c = sin_cos_degrees(paper["rotation"])
    texel = paper["scale"] * 25.4 / BASE_DPI
    u = (dx * c + dy * s) / texel
    v = (dy * c - dx * s) / texel
    if paper["flip_x"]:
        u = -u
    if paper["flip_y"]:
        v = -v
    U = math.floor((u - 0.5) * 256.0) + offset[0] * 256
    V = math.floor((v - 0.5) * 256.0) + offset[1] * 256
    ix, fx = U >> 8, U & 255
    iy, fy = V >> 8, V & 255
    seam = paper["seam"]
    x0, x1 = wrap(ix, width, seam), wrap(ix + 1, width, seam)
    y0, y1 = wrap(iy, height, seam), wrap(iy + 1, height, seam)
    g00, g10 = values[y0 * width + x0], values[y0 * width + x1]
    g01, g11 = values[y1 * width + x0], values[y1 * width + x1]
    value = (g00 * (256 - fx) * (256 - fy) + g10 * fx * (256 - fy) + g01 * (256 - fx) * fy + g11 * fx * fy + 32768) >> 16
    if paper["invert"]:
        value = 255 - value
    return {"x": px, "y": py, "x_mm": x_mm, "y_mm": y_mm, "u": u, "v": v, "U": U, "V": V,
            "texels": [[x0, y0], [x1, y0], [x0, y1], [x1, y1]], "weights": [fx, fy], "value": value}


def blend(cover, value, paper):
    d = math.floor(paper["density"] * 255.0 + 0.5)
    taken = ((255 - value) * d + 127) // 255
    if paper["blend"] == "subtract":
        return max(0, cover - taken)
    return (cover * (255 - taken) + 127) // 255


# --- the fixture -----------------------------------------------------------------------------------------------------

DEFAULTS = {"asset": "sha256:" + "0" * 64, "density": 0.5, "scale": 1.0, "rotation": 0.0, "flip_x": False,
            "flip_y": False, "invert": False, "blend": "multiply", "coords": "paper", "seam": "repeat", "seed": 0}

GRAIN = {"width": 5, "height": 3, "values": [255, 200, 31, 0, 128,
                                             90, 17, 240, 66, 3,
                                             180, 255, 1, 222, 140]}

CASES = [
    # (settings, dpi, line seed, anchor mm)
    ({}, 300, "", [0.0, 0.0]),
    ({"density": 0.7, "scale": 2.5, "rotation": 30.0, "flip_x": True, "seam": "mirror", "seed": 12345, "blend": "subtract"},
     600, "", [0.0, 0.0]),
    ({"density": 0.8, "scale": 0.1, "rotation": -135.5, "flip_y": True, "invert": True, "coords": "stroke", "seed": 7},
     72, "ab12cd34ef56", [12.345, 67.891]),
    ({"density": 1.0, "scale": 10.0, "rotation": 90.0, "flip_x": True, "flip_y": True, "seam": "mirror",
      "seed": 2147483647}, 1200, "", [0.0, 0.0]),
    ({"density": 0.0, "rotation": 45.0, "coords": "stroke", "blend": "subtract"}, 350, "line-2", [3.0, 4.0]),
    ({"density": 0.333, "scale": 1.7, "rotation": 359.75, "coords": "stroke", "seam": "mirror", "seed": 99},
     150, "線の id は UTF-8", [100.25, -2.5]),
    ({"density": 0.5, "scale": 0.37, "rotation": -360.0, "invert": True, "blend": "subtract", "seed": 1}, 220, "", [0.0, 0.0]),
]

PIXELS = [(0, 0), (1, 0), (0, 1), (17, 5), (123, 456), (4000, 3000), (7015, 9921), (31, 2)]
COVERS = [0, 1, 64, 128, 200, 254, 255]
ANGLES = [0.0, 1e-300, -1e-300, 15.0, 30.0, 44.99999999999999, 45.0, 45.00000000000001, 60.0, 89.9, 90.0, 135.5, 180.0,
          -135.5, 270.0, 300.0, 359.75, 360.0, -360.0, 720.5, -0.0]


def settings(extra):
    out = dict(DEFAULTS)
    out.update(extra)
    return out


def samples():
    out = {"about": "BRUSH-01 paper texture: a pixel's mm position to the paper's sample and the ink left "
                    "(render/paper.hpp), from tools/migration/paper_reference.py (an independent reference, not Python's Genko)",
           "grain": GRAIN,
           "sin_cos": [[a, *sin_cos_degrees(a)] for a in ANGLES],
           "cases": []}
    for extra, dpi, line_seed, anchor in CASES:
        paper = settings(extra)
        offset = list(offsets(paper, line_seed, GRAIN["width"], GRAIN["height"]))
        case = {"paper": paper, "dpi": dpi, "line_seed": line_seed, "anchor": anchor, "offset": offset, "pixels": []}
        for px, py in PIXELS:
            item = sample(GRAIN, paper, dpi, px, py, anchor, offset)
            item["ink"] = [[cover, blend(cover, item["value"], paper)] for cover in COVERS]
            case["pixels"].append(item)
        # a whole coverage picture through apply: its corner at (40, 25) on the page
        mask = [(i * 37 + 11) % 256 for i in range(6 * 4)]
        laid = []
        for n, cover in enumerate(mask):
            item = sample(GRAIN, paper, dpi, 40 + n % 6, 25 + n // 6, anchor, offset)
            laid.append(blend(cover, item["value"], paper))
        case["apply"] = {"origin": [40, 25], "size": [6, 4], "mask": mask, "result": laid}
        out["cases"].append(case)
    return out


# --- the pictures ----------------------------------------------------------------------------------------------------

def grain_rgba():
    """A 96 × 96 paper with fibres, blobs and a corner fading out to transparent (fixed seed)."""
    from PIL import Image, ImageDraw
    rng = random.Random(20261009)
    size = 96
    base = Image.new("L", (size, size), 236)
    draw = ImageDraw.Draw(base)
    for _ in range(140):  # fibres
        x, y = rng.uniform(0, size), rng.uniform(0, size)
        a = rng.uniform(0, math.tau)
        n = rng.uniform(4, 22)
        draw.line([(x, y), (x + n * math.cos(a), y + n * math.sin(a))], fill=rng.randint(70, 200), width=rng.choice([1, 1, 2]))
    for _ in range(60):  # pits
        x, y, r = rng.uniform(0, size), rng.uniform(0, size), rng.uniform(0.6, 2.4)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=rng.randint(20, 120))
    noise = Image.frombytes("L", (size, size), bytes(rng.randint(0, 40) for _ in range(size * size)))
    grey = Image.composite(Image.eval(base, lambda v: max(0, v - 40)), base, noise.point(lambda v: 255 if v > 34 else 0))
    rgb = Image.merge("RGB", [grey, Image.eval(grey, lambda v: max(0, v - 12)), Image.eval(grey, lambda v: min(255, v + 9))])
    alpha = Image.new("L", (size, size), 255)
    adraw = ImageDraw.Draw(alpha)
    for k in range(32):  # the top left corner fades out to transparent
        adraw.rectangle([0, 0, 31 - k, 31 - k], fill=max(0, 255 - 8 * (k + 1)))
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def grey_of(image):
    """What render/paper.hpp take_in makes of a picture: laid over white, made grey."""
    from PIL import Image
    rgba = image.convert("RGBA")
    white = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
    return Image.alpha_composite(white, rgba).convert("L")


def pictures():
    """The test's paper pictures and the grey Pillow makes of each: {name: bytes}."""
    from PIL import Image
    files = {}
    rgba = grain_rgba()
    buf = io.BytesIO()
    rgba.save(buf, "PNG")
    files["grain.png"] = buf.getvalue()
    buf = io.BytesIO()
    grey_of(rgba).convert("RGB").save(buf, "JPEG", quality=90)
    files["grain.jpg"] = buf.getvalue()
    buf = io.BytesIO()
    grey_of(rgba).convert("RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=16).save(buf, "GIF")
    files["grain.gif"] = buf.getvalue()
    buf = io.BytesIO()
    grey_of(rgba).save(buf, "BMP")
    files["grain.bmp"] = buf.getvalue()
    expected = {}
    for name in ("grain.png", "grain.jpg", "grain.gif", "grain.bmp"):
        image = Image.open(io.BytesIO(files[name]))
        image.load()
        grey = grey_of(image)
        expected[name] = {"mode": image.mode, "size": list(grey.size), "grey": base64.b64encode(grey.tobytes()).decode()}
    files["grain-grey.json"] = (json.dumps(expected, indent=1, sort_keys=True) + "\n").encode()
    return files


def dump(data):
    return (json.dumps(data, ensure_ascii=False, indent=1) + "\n").encode("utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("what", choices=["samples", "pictures", "check"])
    parser.add_argument("dest")
    args = parser.parse_args()
    if args.what == "samples":
        Path(args.dest).write_bytes(dump(samples()))
    elif args.what == "pictures":
        for name, data in pictures().items():
            (Path(args.dest) / name).write_bytes(data)
    else:
        folder = Path(args.dest)
        wanted = dict(pictures())
        wanted["samples.json"] = dump(samples())
        bad = [name for name, data in wanted.items() if (folder / name).read_bytes() != data]
        print("differ: " + ", ".join(bad) if bad else "same")
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
