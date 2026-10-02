#!/usr/bin/env python3
"""The Python reference for the C++ drawing tests (native/tests: render, brushes, PNG, random numbers): runs the
baseline (src/genko, Pillow 12.3.0, numpy) and writes what the C++ build must match. Development only.

    PYTHONPATH=src python3 tools/migration/render_harness.py <command> ...

Commands:
  unit-tables OUTDIR            the tables of the C++ unit tests (random.Random, genko.stroke, Image operations,
                                PNG files of every mode): OUTDIR/render_unit_tables.json and OUTDIR/png/*.png
  draw-cases OUT --seed N --count K
                                K random ImageDraw calls (line with width and joint="curve", polygon, ellipse, …) on
                                small images, with the pixels Pillow draws
  brush-cases OUT --seed N      brushes.draw for every built-in brush and custom brushes (J3 settings) with and
                                without pressure at 72, 150 and 600 dpi: the coverage and its origin
  make-books OUT --seed N --count K
                                K random books for drawing (layers, brushes, rasters, patches, masks, blend modes,
                                fills and gradients, corrections, panels): OUT/book-NN.genko
  render JOBS                   render_page for each job of a JSON list: {"book", "page", "dpi", "mode", "out",
                                "skip_unported": bool} → a PNG of the page; "story": false removes the lines
  layer-image JOBS              layer_image for each job: {"book", "page", "layer", "dpi", "out"}

With skip_unported the elements this C++ step does not draw yet (lines and balloons, tones, effect lines,
placed pictures, nombres, cover folds, animation, layer screens) are left out the way the C++
RenderOptions::skip_unported leaves them out.
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import math
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))


def bits(x: float) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", float(x)))[0]


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


# --- unit tables -------------------------------------------------------------------------------------------------------


def random_table() -> list:
    seeds = [["int", 0], ["int", 1], ["int", 42], ["int", -5], ["int", 2**32 + 7], ["int", 2**63 - 1],
             ["int", -(2**63)], ["str", ""], ["str", "genko"], ["str", "a1b2c3"], ["str", "日本語の線"],
             ["str", "frame"], ["str", "x" * 300], ["bytes", ""], ["bytes", b64(b"\x00\xff\x10")], ["bytes", b64(b"abc")]]
    out = []
    for kind, value in seeds:
        if kind == "bytes":
            rng = random.Random(base64.b64decode(value))
        else:
            rng = random.Random(value)
        calls = []
        for _ in range(12):
            calls.append(["random", bits(rng.random())])
        for a, b in ((-3.0, 7.0), (0.0, 1.0), (5.5, -2.25)):
            calls.append(["uniform", bits(a), bits(b), bits(rng.uniform(a, b))])
        for mu, sigma in ((0.0, 1.0), (0.0, 2.5), (10.0, 0.5), (0.0, 1.0), (-1.0, 3.0)):
            calls.append(["gauss", bits(mu), bits(sigma), bits(rng.gauss(mu, sigma))])
        for a, b in ((1, 6), (0, 0), (-10, 10), (0, 2**40)):
            calls.append(["randint", a, b, rng.randint(a, b)])
        for start, stop, step in ((0, 100, 7), (10, -10, -3), (5, 6, 1)):
            calls.append(["randrange3", start, stop, step, rng.randrange(start, stop, step)])
        for stop in (256, 1, 1000003):
            calls.append(["randrange", stop, rng.randrange(stop)])
        for k in (0, 1, 7, 32, 33, 64):
            calls.append(["getrandbits", k, str(rng.getrandbits(k))])
        items = list(range(10))
        calls.append(["choice", rng.choice(items)])
        rng.shuffle(items)
        calls.append(["shuffle", items])
        out.append({"seed": [kind, value], "calls": calls})
    return out


def pts_json(points) -> list:
    return [[bits(p[0]), bits(p[1])] + ([bits(p[2])] if len(p) > 2 else []) for p in points]


def stroke_geom_table() -> dict:
    from genko import brushes
    from genko.models import Stroke
    from genko.stroke import (_seg_cross, apply_pressure_curve, erase_to_crossing, fit_curve, pack_point,
                              split_by_eraser, stabilize_points, taper_points)

    rng = random.Random(20261002)

    def line(n, pressure, spread=30.0):
        x, y = rng.uniform(10, 150), rng.uniform(10, 200)
        out = []
        for _ in range(n):
            x += rng.uniform(-spread, spread) / 10 * rng.choice([0.2, 1.0, 3.0])
            y += rng.uniform(-spread, spread) / 10 * rng.choice([0.2, 1.0, 3.0])
            out.append((x, y, rng.random()) if pressure else (x, y))
        return out

    cases = []
    for i in range(48):
        n = rng.choice([1, 2, 3, 4, 5, 8, 13, 25])
        pressure = rng.random() < 0.6
        points = line(n, pressure)
        window = rng.choice([0, 2, 3, 5, 7, 9])
        by_speed = rng.random() < 0.5
        in_mm, out_mm = rng.choice([(None, None), (2.0, 0.0), (0.0, 3.5), (5.0, 5.0), (None, 1.5), (40.0, 40.0)])
        tol, step = rng.choice([(0.3, 0.8), (0.05, 0.6), (1.0, 1.5), (0.0, 0.5)])
        curve = rng.choice(["gpen", "linear", "", "soft"])
        strength = rng.choice([0, 1, 3, 6])
        cases.append({
            "points": pts_json(points), "window": window, "by_speed": by_speed,
            "stabilize": pts_json(stabilize_points([list(p) for p in points], window, by_speed=by_speed)),
            "in_mm": None if in_mm is None else bits(in_mm), "out_mm": None if out_mm is None else bits(out_mm),
            "taper": pts_json(taper_points(points, in_mm, out_mm)),
            "tolerance": bits(tol), "step": bits(step), "fit": pts_json(fit_curve(points, tol, step)),
            "curve": curve, "pressure_curve": pts_json(apply_pressure_curve(points, curve)),
            "strength": strength, "smoothed": pts_json(brushes.smoothed(points, strength)),
        })
    packs = []
    for _ in range(20):
        x, y = rng.uniform(-5, 300), rng.uniform(-5, 300)
        pressure = rng.choice([None, rng.random(), 1.5, -0.2, 0.03])
        tilt = rng.choice([0.0, 0.5, -1.0, 3.0])
        packs.append({"x": bits(x), "y": bits(y), "pressure": None if pressure is None else bits(pressure),
                      "tilt": bits(tilt), "out": pts_json([pack_point(x, y, pressure, tilt)])[0]})
    erasers = []
    for _ in range(30):
        points = line(rng.choice([2, 3, 6]), rng.random() < 0.5, 25.0)
        cx, cy = points[len(points) // 2][:2]
        eraser = [(cx + rng.uniform(-3, 3), cy + rng.uniform(-3, 3))]
        for _ in range(rng.choice([0, 1, 3])):
            eraser.append((eraser[-1][0] + rng.uniform(-6, 6), eraser[-1][1] + rng.uniform(-6, 6)))
        radius = rng.choice([0.5, 1.0, 2.5, 0.3])
        pieces = split_by_eraser(points, eraser, radius)
        erasers.append({"points": pts_json(points), "eraser": pts_json(eraser), "radius": bits(radius),
                        "pieces": [pts_json(p) for p in pieces]})
    crosses = []
    for _ in range(40):
        a, b, c, d = [(rng.uniform(0, 10), rng.uniform(0, 10)) for _ in range(4)]
        if rng.random() < 0.1:
            d = (c[0] + (b[0] - a[0]), c[1] + (b[1] - a[1]))  # parallel
        t = _seg_cross(a, b, c, d)
        crosses.append({"abcd": pts_json([a, b, c, d]), "t": None if t is None else bits(t)})
    crossings = []
    for _ in range(15):
        strokes = []
        for k in range(rng.choice([1, 2, 3, 5])):
            pts = line(rng.choice([2, 4, 8]), False, 80.0)
            strokes.append(Stroke(id=f"s{k}", points=pts, width_mm=rng.choice([0.35, 1.0]),
                                  kind=rng.choice(["gpen", "maru"]), rgb=rng.choice([None, (1, 2, 3)]),
                                  opacity=rng.choice([1.0, 0.5])))
        target = rng.choice(strokes).points
        mx, my = target[len(target) // 2]
        eraser = [(mx + rng.uniform(-1, 1), my + rng.uniform(-1, 1)),
                  (mx + rng.uniform(-4, 4), my + rng.uniform(-4, 4))]
        radius = rng.choice([0.5, 1.5])
        result = erase_to_crossing(strokes, eraser, radius)
        crossings.append({
            "strokes": [{"id": s.id, "points": pts_json(s.points), "width_mm": bits(s.width_mm), "kind": s.kind,
                         "rgb": list(s.rgb) if s.rgb else None, "opacity": bits(s.opacity)} for s in strokes],
            "eraser": pts_json(eraser), "radius": bits(radius),
            "result": [{"kept": s.id if any(s is o for o in strokes) else None, "points": pts_json(s.points),
                        "width_mm": bits(s.width_mm), "kind": s.kind, "rgb": list(s.rgb) if s.rgb else None,
                        "opacity": bits(s.opacity)} for s in result],
        })
    return {"cases": cases, "packs": packs, "erasers": erasers, "crosses": crosses, "crossings": crossings}


def pattern(mode: str, size: tuple[int, int]):
    """A picture both sides can make: band c of pixel (x, y) is (x * 31 + y * 17 + c * 59) % 251."""
    from PIL import Image

    w, h = size
    n = {"L": 1, "LA": 2, "RGB": 3, "RGBA": 4}[mode]
    data = bytes((x * 31 + y * 17 + c * 59) % 251 for y in range(h) for x in range(w) for c in range(n))
    return Image.frombytes(mode, size, data)


def image_table() -> list:
    """Small Image operations with the pixels Pillow gives (raw bytes in base64)."""
    from PIL import Image, ImageChops, ImageFilter, ImageOps

    rng = random.Random(7)

    def noise(mode, size):
        bands = len(Image.new(mode, (1, 1)).getbands()) if mode not in ("1",) else 1
        raw = bytes(rng.randrange(256) for _ in range(size[0] * size[1] * (4 if mode in ("RGBA", "RGB") else bands)))
        if mode == "RGB":
            return Image.frombytes("RGBA", size, raw).convert("RGB")
        if mode == "1":
            return Image.frombytes("L", size, raw).point(lambda v: 255 if v > 127 else 0, mode="1")
        if mode == "LA":
            return Image.frombytes("LA", size, raw[: size[0] * size[1] * 2])
        return Image.frombytes(mode, size, raw)

    def snap(im):
        return {"mode": im.mode, "size": list(im.size), "data": b64(im.tobytes())}

    cases = []
    stored: dict[int, str] = {}  # (each input picture is stored once, in the first case that uses it)

    def ref(im):
        key = id(im)
        if key not in stored:
            stored[key] = f"in{len(stored)}"
            return {"id": stored[key], **snap(im)}
        return {"ref": stored[key]}

    keep = []  # (the pictures stay alive, so their ids stay unique)

    def add(name, inputs, result):
        keep.extend(inputs)
        cases.append({"name": name, "inputs": [ref(i) for i in inputs], "result": snap(result) if hasattr(result, "mode") else result})

    l1, l2 = noise("L", (13, 9)), noise("L", (13, 9))
    rgba1, rgba2 = noise("RGBA", (11, 7)), noise("RGBA", (11, 7))
    rgb1 = noise("RGB", (11, 7))
    la1 = noise("LA", (6, 5))
    one = noise("1", (17, 5))
    flipped = one.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    keep.extend([l1, l2, rgba1, rgba2, rgb1, la1, one, flipped])
    add("chops.screen", [l1, l2], ImageChops.screen(l1, l2))
    add("chops.multiply", [l1, l2], ImageChops.multiply(l1, l2))
    add("chops.add", [l1, l2], ImageChops.add(l1, l2))
    add("chops.subtract", [l1, l2], ImageChops.subtract(l1, l2))
    add("chops.lighter", [l1, l2], ImageChops.lighter(l1, l2))
    add("chops.darker", [l1, l2], ImageChops.darker(l1, l2))
    add("chops.difference", [l1, l2], ImageChops.difference(l1, l2))
    add("chops.invert", [l1], ImageChops.invert(l1))
    add("chops.overlay", [rgb1, rgba2.convert("RGB")], ImageChops.overlay(rgb1, rgba2.convert("RGB")))
    add("chops.logical_and", [one, flipped], ImageChops.logical_and(one, flipped))
    add("alpha_composite", [rgba1, rgba2], Image.alpha_composite(rgba1, rgba2))
    dest = rgba1.copy()
    dest.alpha_composite(rgba2.crop((2, 1, 9, 6)), (5, 3))
    add("alpha_composite_in_place", [rgba1, rgba2], dest)
    for mode in ("L", "LA", "RGB", "RGBA", "1", "I", "F", "HSV", "RGBa", "La"):
        for src in (l1, rgba1, rgb1, la1, one):
            try:
                out = src.convert(mode)
            except ValueError:
                continue
            add(f"convert.{src.mode}.{mode}", [src], out)
    add("convert.L.1.none", [l1], l1.convert("1", dither=Image.Dither.NONE))
    for size, resample in (((20, 13), Image.Resampling.BICUBIC), ((5, 4), Image.Resampling.LANCZOS),
                           ((7, 30), Image.Resampling.BILINEAR), ((26, 18), Image.Resampling.NEAREST),
                           ((3, 3), Image.Resampling.BOX), ((9, 9), Image.Resampling.HAMMING)):
        add(f"resize.L.{size}.{int(resample)}", [l1], l1.resize(size, resample))
        add(f"resize.RGBA.{size}.{int(resample)}", [rgba1], rgba1.resize(size, resample))
    add("resize.default", [rgba1], rgba1.resize((16, 4)))
    add("resize.reducing_gap", [l1], l1.resize((3, 2), Image.Resampling.BICUBIC, reducing_gap=2.0))
    big = pattern("RGBA", (400, 300))  # (made the same way by the C++ test: not stored)
    thumb = big.copy()
    thumb.thumbnail((96, 96))
    cases.append({"name": "thumbnail", "inputs": [{"pattern": "RGBA", "size": [400, 300]}], "result": snap(thumb)})
    tall = pattern("L", (3, 400))
    cases.append({"name": "resize.tall", "inputs": [{"pattern": "L", "size": [3, 400]}],
                  "result": snap(tall.resize((2, 37), Image.Resampling.BICUBIC))})
    for angle in (0, 33.0, -112.5, 90, 180, 270, 359.9):
        add(f"rotate.L.{angle}", [l1], l1.rotate(angle, expand=True, resample=Image.Resampling.BICUBIC))
    add("rotate.noexpand", [l1], l1.rotate(17.0, resample=Image.Resampling.BILINEAR))
    add("transform.affine", [rgba1],
        rgba1.transform((14, 9), Image.Transform.AFFINE, (0.9, 0.1, 1.0, -0.1, 1.1, 0.5), Image.Resampling.BILINEAR))
    for name, f in (("gauss1.5", ImageFilter.GaussianBlur(1.5)), ("gauss0", ImageFilter.GaussianBlur(0)),
                    ("box2", ImageFilter.BoxBlur(2)), ("min3", ImageFilter.MinFilter(3)), ("max5", ImageFilter.MaxFilter(5)),
                    ("median3", ImageFilter.MedianFilter(3)), ("mode3", ImageFilter.ModeFilter(3)),
                    ("smooth", ImageFilter.SMOOTH), ("sharpen", ImageFilter.SHARPEN), ("blur", ImageFilter.BLUR),
                    ("unsharp", ImageFilter.UnsharpMask(2, 150, 3)), ("edge", ImageFilter.FIND_EDGES)):
        add(f"filter.L.{name}", [l1], l1.filter(f))
        if name not in ("mode3",):
            add(f"filter.RGBA.{name}", [rgba1], rgba1.filter(f))
    add("crop.outside", [l1], l1.crop((-3, -2, 20, 7)))
    add("crop.float", [l1], l1.crop((0.5, 1.5, 7.5, 6.49)))
    pasted = rgba1.copy()
    pasted.paste(l1.crop((0, 0, 5, 4)), (3, 2))
    add("paste.convert", [rgba1, l1], pasted)
    masked = rgb1.copy()
    masked.paste(rgba2, (0, 0), rgba2)
    add("paste.rgba_mask", [rgb1, rgba2], masked)
    filled = rgba1.copy()
    filled.paste((10, 200, 30, 128), (2, 2, 8, 6), l1.crop((0, 0, 6, 4)))
    add("paste.colour_mask", [rgba1, l1], filled)
    put = rgb1.copy()
    put.putalpha(l1.crop((0, 0, 11, 7)))
    add("putalpha.rgb", [rgb1, l1], put)
    put2 = l1.copy()
    put2.putalpha(77)
    add("putalpha.l_const", [l1], put2)
    add("point.l", [l1], l1.point(lambda v: v * 45 // 100))
    add("point.rgba", [rgba1], rgba1.point(lambda v: 255 - v))
    add("point.to1", [l1], l1.point(lambda p: 255 if p > 128 else 0, mode="1"))
    add("merge", [l1, l2], Image.merge("RGBA", (l1, l2, l1, l2)))
    add("split.alpha", [rgba1], rgba1.split()[3])
    add("getchannel.A", [rgba1], rgba1.getchannel("A"))
    add("composite", [rgba1, rgba2, l1], Image.composite(rgba1, rgba2, l1.crop((0, 0, 11, 7))))
    add("blend", [rgb1, rgba2], Image.blend(rgb1, rgba2.convert("RGB"), 0.3))
    add("ops.invert.rgb", [rgb1], ImageOps.invert(rgb1))
    add("ops.grayscale", [rgba1], ImageOps.grayscale(rgba1))
    add("offset", [l1], ImageChops.offset(l1, 3, -2))
    add("transpose", [rgba1], rgba1.transpose(Image.Transpose.ROTATE_90))
    add("hsv_roundtrip", [rgb1], rgb1.convert("HSV").convert("RGB"))
    sparse = l1.point(lambda v: 255 if v > 240 else 0)
    keep.append(sparse)
    cases.append({"name": "getbbox", "inputs": [ref(sparse)], "result": list(sparse.getbbox() or [])})
    cases.append({"name": "getextrema", "inputs": [ref(rgba1)], "result": [list(e) for e in rgba1.getextrema()]})
    return cases


def png_samples(outdir: Path) -> list:
    """PNG files of every mode Genko may meet, saved by Pillow, with the mode and pixels Pillow reads back."""
    from PIL import Image

    rng = random.Random(11)
    (outdir / "png").mkdir(parents=True, exist_ok=True)
    out = []

    def rnd(n):
        return bytes(rng.randrange(256) for _ in range(n))

    samples = []
    samples.append(("l8", Image.frombytes("L", (9, 7), rnd(63)), {}))
    samples.append(("la8", Image.frombytes("LA", (5, 4), rnd(40)), {}))
    samples.append(("rgb8", Image.frombytes("RGB", (6, 5), rnd(90)), {}))
    samples.append(("rgba8", Image.frombytes("RGBA", (7, 3), rnd(84)), {}))
    samples.append(("bit1", Image.frombytes("L", (19, 4), rnd(76)).point(lambda v: 255 if v > 99 else 0, mode="1"), {}))
    pal = Image.frombytes("L", (8, 6), bytes(rng.randrange(12) for _ in range(48))).convert("P")
    pal.putpalette([rng.randrange(256) for _ in range(36)])
    samples.append(("p8", pal, {}))
    samples.append(("p8_trns_index", pal, {"transparency": 3}))
    samples.append(("p8_trns_bytes", pal, {"transparency": bytes([255, 0, 128, 77, 255, 255, 3])}))
    samples.append(("p4", pal, {"bits": 4}))
    samples.append(("p2", Image.frombytes("L", (10, 3), bytes(rng.randrange(4) for _ in range(30))).convert("P"), {"bits": 2}))
    samples.append(("l_trns", Image.frombytes("L", (6, 6), rnd(36)), {"transparency": 17}))
    samples.append(("rgb_trns", Image.frombytes("RGB", (4, 4), rnd(48)), {"transparency": (10, 20, 30)}))
    i16 = Image.frombytes("I;16", (5, 3), rnd(30))
    samples.append(("i16", i16, {}))
    samples.append(("interlaced_rgba", Image.frombytes("RGBA", (13, 11), rnd(13 * 11 * 4)), {"interlace": True}))
    # (files Pillow does not write: made by hand, read by Pillow for the expected pixels)
    samples.append(("rgb16", None, {"raw": (5, 4, 16, 2, rnd(5 * 4 * 6))}))
    samples.append(("rgba16", None, {"raw": (4, 3, 16, 6, rnd(4 * 3 * 8))}))
    samples.append(("la16", None, {"raw": (6, 2, 16, 4, rnd(6 * 2 * 4))}))
    samples.append(("l2", None, {"raw": (11, 3, 2, 0, rnd(3 * 3))}))
    samples.append(("l4", None, {"raw": (7, 5, 4, 0, rnd(4 * 5))}))
    samples.append(("p1_trns", None, {"raw": (13, 2, 1, 3, rnd(2 * 2)), "plte": bytes([200, 10, 10, 10, 200, 10]),
                                      "trns": bytes([0])}))
    samples.append(("rgb16_trns", None, {"raw": (3, 3, 16, 2, rnd(3 * 3 * 6)), "trns": bytes([0, 7, 1, 2, 0, 9])}))
    for name, im, opts in samples:
        path = outdir / "png" / f"{name}.png"
        kwargs = dict(opts)
        interlace = kwargs.pop("interlace", False)
        if "raw" in kwargs:
            w, h, bits, colour, data = kwargs["raw"]
            path.write_bytes(raw_png(w, h, bits, colour, data, kwargs.get("plte"), kwargs.get("trns")))
        elif interlace:
            # Pillow does not write interlaced PNGs: make one with pypng-free code (zlib + Adam7 by hand)
            data = adam7_rgba(im)
            path.write_bytes(data)
        else:
            im.save(path, **kwargs)
        back = Image.open(path)
        back.load()
        trns = back.info.get("transparency")
        if isinstance(trns, bytes):
            trns = {"bytes": b64(trns)}
        elif isinstance(trns, tuple):
            trns = {"rgb": list(trns)}
        elif trns is not None:
            trns = {"index": trns}
        entry = {"name": name, "file": f"png/{name}.png", "mode": back.mode, "size": list(back.size),
                 "data": b64(back.tobytes()), "transparency": trns}
        if back.mode == "P":
            entry["rgba"] = b64(back.convert("RGBA").tobytes())
            entry["l"] = b64(back.convert("L").tobytes())
        else:
            entry["rgba"] = b64(back.convert("RGBA").tobytes())
            entry["l"] = b64(back.convert("L").tobytes())
        out.append(entry)
    return out


def raw_png(w: int, h: int, bits: int, colour: int, pixels: bytes, plte: bytes | None, trns: bytes | None) -> bytes:
    """A PNG written by hand: `pixels` are the rows' bytes (each row (w * channels * bits + 7) // 8 long)."""
    import zlib

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    stride = (w * channels * bits + 7) // 8
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(cid, data):
        return struct.pack(">I", len(data)) + cid + data + struct.pack(">I", zlib.crc32(cid + data) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bits, colour, 0, 0, 0))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    return out + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def adam7_rgba(im) -> bytes:
    import zlib

    w, h = im.size
    px = im.tobytes()
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        for y in range(y0, h, dy):
            row = bytearray()
            for x in range(x0, w, dx):
                i = (y * w + x) * 4
                row += px[i:i + 4]
            if row:
                raw += b"\x00" + row

    def chunk(cid, data):
        return struct.pack(">I", len(data)) + cid + data + struct.pack(">I", zlib.crc32(cid + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 1)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


# --- ImageDraw ----------------------------------------------------------------------------------------------------


def colour_json(c):
    return c if isinstance(c, int) else list(c)


def draw_cases(out: str, seed: int, count: int) -> None:
    """Random ImageDraw calls: wide lines with and without joint="curve" (the joints are Python code in Pillow),
    polygons (filled, outlined, wide outlines), ellipses, rectangles, points, arcs, pie slices and chords."""
    from PIL import Image, ImageDraw

    rng = random.Random(seed)
    cases = []

    def xy(w, h):
        if rng.random() < 0.3:
            return (rng.randint(-6, w + 6), rng.randint(-6, h + 6))
        return (rng.uniform(-12, w + 12), rng.uniform(-12, h + 12))

    def colour(mode):
        if mode == "L":
            return rng.randrange(256)
        if rng.random() < 0.2:
            return rng.randrange(1 << 32)  # an int on a colour image: 0xAABBGGRR
        c = tuple(rng.randrange(256) for _ in range(3))
        return c + (rng.randrange(256),) if rng.random() < 0.5 else c

    def box(w, h):
        (x0, y0), (x1, y1) = xy(w, h), xy(w, h)
        return [min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)]

    for _ in range(count):
        mode = rng.choice(["L", "RGB", "RGBA"])
        w, h = rng.choice([(48, 40), (64, 48), (33, 70)])
        background = colour(mode) if rng.random() < 0.5 else 0
        im = Image.new(mode, (w, h), background)
        blend = mode == "RGB" and rng.random() < 0.2
        draw = ImageDraw.Draw(im, "RGBA" if blend else None)
        ops = []
        for _ in range(rng.randrange(1, 4)):
            kind = rng.choice(["line", "line", "line", "polygon", "polygon", "ellipse", "rectangle", "point", "arc",
                               "pieslice", "chord"])
            fill = colour(mode)
            if kind == "line":
                pts = [xy(w, h) for _ in range(rng.choice([2, 2, 3, 4, 6]))]
                width = rng.choice([1, 2, 3, 4, 5, 6, 8, 9, 12, 17, 25, 40])
                joint = rng.choice([None, "curve", "curve"])
                draw.line(pts, fill=fill, width=width, joint=joint)
                ops.append({"op": "line", "xy": pts_json(pts), "fill": colour_json(fill), "width": width, "joint": joint})
            elif kind == "polygon":
                pts = [xy(w, h) for _ in range(rng.choice([3, 4, 5, 8]))]
                outline = colour(mode) if rng.random() < 0.6 else None
                fill_ = fill if rng.random() < 0.7 or outline is None else None
                width = rng.choice([1, 1, 2, 3, 6, 11])
                draw.polygon(pts, fill=fill_, outline=outline, width=width)
                ops.append({"op": "polygon", "xy": pts_json(pts), "fill": None if fill_ is None else colour_json(fill_),
                            "outline": None if outline is None else colour_json(outline), "width": width})
            elif kind in ("ellipse", "rectangle"):
                b = box(w, h)
                outline = colour(mode) if rng.random() < 0.5 else None
                fill_ = fill if rng.random() < 0.7 or outline is None else None
                width = rng.choice([1, 2, 4, 7])
                getattr(draw, kind)(b, fill=fill_, outline=outline, width=width)
                ops.append({"op": kind, "box": [bits(v) for v in b], "fill": None if fill_ is None else colour_json(fill_),
                            "outline": None if outline is None else colour_json(outline), "width": width})
            elif kind == "point":
                pts = [xy(w, h) for _ in range(rng.randrange(1, 9))]
                draw.point(pts, fill=fill)
                ops.append({"op": "point", "xy": pts_json(pts), "fill": colour_json(fill)})
            else:
                b = box(w, h)
                start, end = rng.uniform(-400, 400), rng.uniform(-400, 400)
                width = rng.choice([1, 2, 5])
                if kind == "arc":
                    draw.arc(b, start, end, fill=fill, width=width)
                    ops.append({"op": "arc", "box": [bits(v) for v in b], "start": bits(start), "end": bits(end),
                                "fill": colour_json(fill), "width": width})
                else:
                    outline = colour(mode) if rng.random() < 0.5 else None
                    getattr(draw, kind)(b, start, end, fill=fill, outline=outline, width=width)
                    ops.append({"op": kind, "box": [bits(v) for v in b], "start": bits(start), "end": bits(end),
                                "fill": colour_json(fill), "outline": None if outline is None else colour_json(outline),
                                "width": width})
        cases.append({"mode": mode, "size": [w, h], "background": colour_json(background), "blend": blend, "ops": ops,
                      "result": b64(im.tobytes())})
    # stroke.stamp_polyline: round dabs along a line (in pixels, or page mm)
    from genko.stroke import stamp_polyline

    for _ in range(20):
        im = Image.new("L", (60, 50), 0)
        draw = ImageDraw.Draw(im)
        coords = rng.choice(["px", "mm"])
        scale = 1.0 if coords == "px" else 25.4 / 96
        pts = [(rng.uniform(0, 60) * scale, rng.uniform(0, 50) * scale) + ((rng.random(),) if rng.random() < 0.5 else ())
               for _ in range(rng.choice([1, 2, 4]))]
        width = rng.choice([0.3, 1.0, 2.5])
        stamp_polyline(draw, pts, 96, width, 200, coords=coords)
        cases.append({"mode": "L", "size": [60, 50], "background": 0, "blend": False,
                      "ops": [{"op": "stamp", "xy": pts_json(pts), "width_mm": bits(width), "coords": coords}],
                      "result": b64(im.tobytes())})
    Path(out).write_text(dumps(cases), encoding="utf-8")


# --- brushes ------------------------------------------------------------------------------------------------------


def tip_pictures() -> dict:
    """Image tips: a soft round one with transparency, a grey mark on white (no alpha), and two Image.open refuses."""
    from PIL import Image, ImageDraw

    soft = Image.new("RGBA", (24, 18), (0, 0, 0, 0))
    ImageDraw.Draw(soft).ellipse((2, 3, 21, 15), fill=(10, 10, 10, 200))
    soft.putalpha(soft.getchannel("A").point(lambda v: v * 3 // 4))
    grey = Image.new("L", (16, 16), 250)
    ImageDraw.Draw(grey).polygon([(2, 14), (8, 1), (14, 14)], fill=30)
    out = {}
    for name, im in (("soft", soft), ("grey", grey)):
        buf = io.BytesIO()
        im.save(buf, "PNG")
        out[name] = b64(buf.getvalue())
    # two that Image.open refuses (a round tip is drawn instead): more pixels than Pillow opens, and no picture
    import zlib

    def chunk(cid, data):
        return struct.pack(">I", len(data)) + cid + data + struct.pack(">I", zlib.crc32(cid + data) & 0xFFFFFFFF)

    out["huge"] = b64(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 13400, 13355, 8, 0, 0, 0, 0))
                      + chunk(b"IDAT", bytes(16)) + chunk(b"IEND", b""))
    out["junk"] = b64(b"not a picture")
    return out


CUSTOM_BRUSHES = {
    "c_flat_follow": {"label": "平たい（向きに沿う）", "base": "calligraphy", "tip_follow": True, "tip_rotation": False},
    "c_flat_turn": {"label": "平たい（回転）", "tip": "flat", "tip_angle": 70, "tip_ratio": 0.4, "tip_rotation": True},
    "c_tip_soft": {"label": "画像先端", "tip": "image", "tip_png": "@soft", "spacing": 0.3, "width_mm": 2.0},
    "c_tip_grey": {"label": "画像先端（灰）", "tip": "image", "tip_png": "@grey", "spacing": 0.25, "turn_jitter": True},
    "c_tip_huge": {"label": "画像先端（大きすぎる）", "tip": "image", "tip_png": "@huge", "spacing": 0.3},
    "c_tip_junk": {"label": "画像先端（画像でない）", "tip": "image", "tip_png": "@junk"},
    "c_spaced": {"label": "間隔", "spacing": 0.5, "width_mm": 1.2},
    "c_spray": {"label": "散布", "pattern": "dots", "scatter": 1.2, "count": 4, "size_jitter": 0.8, "stamp_size": 0.3,
                "spacing": 0.2},
    "c_dash": {"label": "破線２", "pattern": "dash", "stamp_size": 2.0, "spacing": 3.0, "fixed_width": True},
    "c_lace": {"label": "レース２", "pattern": "lace", "spacing": 0.8, "width_mm": 4.0},
    "c_grass": {"label": "草２", "pattern": "grass", "scatter": 0.4, "spacing": 0.3},
    "c_hearts": {"label": "ハート２", "pattern": "hearts", "turn_jitter": True, "spacing": 1.0},
    "c_stars": {"label": "星２", "pattern": "stars", "spacing": 0.8, "size_jitter": 0.4},
    "c_leaves": {"label": "葉２", "pattern": "leaves", "count": 3, "scatter": 0.9, "spacing": 0.6},
    "c_speed": {"label": "速度", "base": "gpen", "speed": 0.8},
    "c_aa_none": {"label": "AA なし", "aa": "none", "width_mm": 0.8},
    "c_aa_weak": {"label": "AA 弱", "aa": "weak", "width_mm": 0.8},
    "c_aa_strong": {"label": "AA 強", "aa": "strong", "width_mm": 0.8},
    "c_water": {"label": "水彩２", "texture": "water", "width_mm": 3.0, "opacity": 0.7},
    "c_grain": {"label": "鉛筆２", "texture": "grain", "gamma": 2.5, "min_pressure": 0.2},
    "c_dry": {"label": "筆２", "texture": "dry", "min_pressure": 0.02, "width_mm": 2.0, "rgb": [200, 30, 30]},
    "c_soft": {"label": "  エアブラシ２  ", "texture": "soft", "width_mm": 5.0, "mix": 0.5, "stretch": 0.3, "post_smooth": 4},
    # (these do not make sense and are skipped by register)
    "bad_width": {"label": "x", "width_mm": 100},
    "bad_texture": {"label": "x", "texture": "bogus"},
    "bad_count": {"label": "x", "count": "many"},
    "bad_label": {"label": "   "},
    "bad_tip": {"label": "x", "tip": "image"},
    "gpen": {"label": "上書きしない", "width_mm": 9.0},
}


def brush_definitions() -> dict:
    tips = tip_pictures()
    out = {}
    for key, data in CUSTOM_BRUSHES.items():
        item = dict(data)
        if isinstance(item.get("tip_png"), str) and item["tip_png"].startswith("@"):
            item["tip_png"] = tips[item["tip_png"][1:]]
        out[key] = item
    return out


def brush_cases(outdir: str, seed: int) -> None:
    from genko import brushes

    root = Path(outdir)
    root.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    definitions = brush_definitions()
    brushes.CUSTOM.clear()
    brushes.register(definitions)
    keys = list(brushes.BRUSHES) + [k for k in definitions if k in brushes.CUSTOM]
    cases = []
    n = 0
    for key in keys:
        b = brushes.brush(key)
        for pressure in (True, False):
            for dpi in (72, 150, 600):
                page_mm = (182.0, 257.0)
                size = (max(1, round(page_mm[0] / 25.4 * dpi)), max(1, round(page_mm[1] / 25.4 * dpi)))
                count = rng.choice([1, 2, 3, 5, 9, 14])
                x, y = rng.choice([(20.0, 30.0), (1.5, 2.0), (175.0, 250.0), (90.0, 120.0)])
                pts = []
                for _ in range(count):
                    x += rng.uniform(-6, 6) * (0.3 if dpi == 600 else 1.0)
                    y += rng.uniform(-6, 6) * (0.3 if dpi == 600 else 1.0)
                    pts.append((x, y, rng.random()) if pressure else (x, y))
                width = b.width_mm * rng.choice([0.5, 1.0, 1.5]) * (0.5 if dpi == 600 else 1.0)
                stroke_seed = rng.choice(["", "a1b2c3d4e5f6", "000000000007", "線"])
                rotation = [rng.uniform(-180, 180) for _ in pts] if (b.tip_rotation or rng.random() < 0.15) else None
                po = rng.choice([0.0, 0.0, 0.5, 1.0, 1.7])
                drawn = brushes.draw(size, pts, dpi, width, key, seed=stroke_seed, rotation=rotation, pressure_opacity=po)
                case = {"kind": key, "points": pts_json(pts), "dpi": dpi, "width_mm": bits(width), "seed": stroke_seed,
                        "rotation": None if rotation is None else [bits(v) for v in rotation], "pressure_opacity": bits(po),
                        "size": list(size), "result": None}
                if drawn is not None:
                    mask, origin = drawn
                    name = f"brush-{n:04d}.png"
                    mask.save(root / name)
                    case["result"] = {"file": name, "origin": list(origin), "size": list(mask.size)}
                cases.append(case)
                n += 1
    dicts = {key: brushes.to_dict(brushes.brush(key)) for key in keys}
    (root / "brushes.json").write_text(dumps({"definitions": definitions, "registered": [k for k in brushes.CUSTOM],
                                              "to_dict": dicts, "cases": cases}), encoding="utf-8")


# --- random books for drawing -------------------------------------------------------------------------------------

BLEND_MODES = ["normal", "multiply", "screen", "add", "overlay", "darken", "lighten", "color_burn", "color_dodge",
               "linear_burn", "soft_light", "hard_light", "difference", "exclusion", "subtract", "divide", "hue",
               "saturation", "color", "luminosity", "no-such-mode"]
ADJUSTMENTS = [
    {"kind": "levels", "black": 30, "white": 220},
    {"kind": "levels", "black": 10.5, "white": 240, "gamma": 1.7, "out_black": 20, "out_white": 250, "channel": "g"},
    {"kind": "curve", "gamma": 2.2},
    {"kind": "curve", "points": [[0, 10], [64, 90], [128, 120], [200, 230], [255, 250]], "channel": "rgb"},
    {"kind": "curve", "points": [[0, 0], [100, 200], [100, 180], [255, 255]], "channel": "r"},
    {"kind": "hue", "shift": 75, "saturation": 1.4, "value": 0.9},
    {"kind": "hue", "shift": -30.5},
    {"kind": "invert"},
    {"kind": "posterize", "levels": 3},
    {"kind": "threshold", "threshold": 140},
    {"kind": "gradient_map", "colors": [[20, 10, 80], [250, 200, 40], [255, 255, 255]]},
    {"kind": "gradient_map", "stops": [[0.0, [0, 0, 0]], [0.3, [200, 20, 20]], [0.3, [20, 200, 20]], [1.0, [255, 255, 255]]]},
    {"kind": "bitonal", "threshold": 100},
    {"kind": "brightness_contrast", "brightness": 25, "contrast": -40},
    {"kind": "brightness_contrast", "brightness": -10, "contrast": 60, "channel": "b"},
    {"kind": "levels", "black": "x"},  # (a bad value: the layer leaves the picture as it is)
]


def png_bytes(im) -> bytes:
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return buf.getvalue()


def rand_colour(rng, alpha=True):
    c = tuple(rng.randrange(256) for _ in range(3))
    return c + (rng.choice([255, 255, 180, 90, 0]),) if alpha else c


def rand_picture(rng, size, mode: str) -> bytes:
    """A picture with a few shapes, saved as PNG in `mode` (RGBA, RGB, L, LA, P, P+tRNS, 1)."""
    from PIL import Image, ImageDraw

    w, h = size
    im = Image.new("RGBA", size, rand_colour(rng) if rng.random() < 0.4 else (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for _ in range(rng.randrange(2, 7)):
        x0, y0 = rng.uniform(-w * 0.2, w), rng.uniform(-h * 0.2, h)
        x1, y1 = x0 + rng.uniform(1, w), y0 + rng.uniform(1, h)
        shape = rng.choice(["ellipse", "rectangle", "line", "polygon"])
        if shape == "line":
            d.line([(x0, y0), (x1, y1)], fill=rand_colour(rng), width=rng.randrange(1, 9))
        elif shape == "polygon":
            d.polygon([(x0, y0), (x1, y0 + rng.uniform(0, h)), (x0 + rng.uniform(0, w), y1)], fill=rand_colour(rng))
        else:
            getattr(d, shape)((x0, y0, x1, y1), fill=rand_colour(rng))
    if mode == "RGB":
        return png_bytes(im.convert("RGB"))
    if mode == "L":
        return png_bytes(im.convert("L"))
    if mode == "LA":
        return png_bytes(im.convert("LA"))
    if mode == "1":
        return png_bytes(im.convert("L").point(lambda v: 255 if v > 100 else 0, mode="1"))
    if mode in ("P", "P+tRNS"):
        p = im.convert("RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=6)
        buf = io.BytesIO()
        p.save(buf, "PNG", **({"transparency": 2} if mode == "P+tRNS" else {}))
        return buf.getvalue()
    return png_bytes(im)


def rand_points(rng, page, n, pressure):
    w, h = float(page.spec.width_mm), float(page.spec.height_mm)
    x, y = rng.uniform(-3, w + 3), rng.uniform(-3, h + 3)
    step = rng.choice([0.4, 1.5, 4.0, 9.0])
    out = []
    for _ in range(n):
        x += rng.uniform(-step, step)
        y += rng.uniform(-step, step)
        out.append((x, y))
    pres = [rng.random() for _ in out] if pressure else []
    return out, pres


def rand_strokes(rng, page, models, kinds):
    out = []
    for _ in range(rng.randrange(0, 14)):
        n = rng.choice([1, 2, 3, 5, 8, 13, 21])
        points, pressure = rand_points(rng, page, n, rng.random() < 0.6)
        if pressure and rng.random() < 0.1:
            pressure = pressure[:-1] or [0.5, 0.5]  # (another length: not used)
        out.append(models.Stroke(
            id=models.new_id(), points=points, pressure=pressure,
            width_mm=rng.choice([0.3, 0.5, 0.8, 1.2, 2.5, 6.0, 0.0]), kind=rng.choice(kinds),
            rgb=rng.choice([None, None, (200, 30, 30), (20, 60, 180), (255, 255, 255)]),
            opacity=rng.choice([1.0, 1.0, 0.6, 0.25]),
            rotation=[rng.uniform(-180, 180) for _ in points] if rng.random() < 0.2 else [],
            pressure_opacity=rng.choice([0.0, 0.0, 0.0, 0.5, 1.0])))
    return out


def rand_patches(rng, models, page):
    out = []
    for _ in range(rng.randrange(1, 4)):
        w, h = rng.uniform(4, 30), rng.uniform(4, 30)
        box = [rng.uniform(-5, float(page.spec.width_mm)), rng.uniform(-5, float(page.spec.height_mm)), w, h]
        mode = rng.choice(["mask", "image", "mask"])
        size = rng.choice([(8, 6), (30, 22), (64, 64), (120, 90)])
        item = {"id": models.new_id(), "box": box, "mode": mode,
                "png": rand_picture(rng, size, rng.choice(["L", "RGBA", "RGB", "P", "LA"])),
                "opacity": rng.choice([1.0, 0.5, 0.8])}
        if mode == "mask" and rng.random() < 0.7:
            item["rgb"] = list(rand_colour(rng, alpha=False))
        out.append(item)
    return out


def rand_frames(rng, page, models):
    from genko import frames as framemod

    root = page.frames[0]
    for _ in range(rng.randrange(0, 5)):
        target = rng.choice(page.leaf_frames())
        try:
            page.split_frame(target.id, rng.choice(["horizontal", "vertical"]), rng.choice([0.5, 0.3, 0.62]),
                             rng.choice([3, 4.5, 0]))
        except ValueError:
            pass
    leaves = page.leaf_frames()
    for frame in leaves:
        r = rng.random()
        if r < 0.25:
            pts = framemod.corners(frame.rect)
            dx = rng.choice([3, 6.5])
            framemod.set_shape(frame, [(pts[0][0] + dx, pts[0][1]), pts[1], (pts[2][0], pts[2][1] - dx / 2),
                                       (pts[3][0] - dx, pts[3][1])])
        if rng.random() < 0.2:
            frame.curves = [rng.choice([0, 1.5, -1.0, 3]) for _ in framemod.shape(frame)]
        if rng.random() < 0.25:
            frame.corner_mm = rng.choice([1.5, 3.0])
        frame.bleed = rng.random() < 0.3
        frame.clip = rng.random() < 0.85
        frame.border_mm = rng.choice([0.8, 0.8, 0.5, 1.2, 0.0])
        if rng.random() < 0.3:
            kind = rng.choice(["solid", "double", "dashed", "dotted", "rough"])
            line = {"kind": kind}
            if rng.random() < 0.6:
                line["rgb"] = list(rand_colour(rng, alpha=False))
            if kind == "double" and rng.random() < 0.5:
                line["gap_mm"] = 1.0
            if kind == "dashed" and rng.random() < 0.5:
                line["dash_mm"] = 2.0
            if kind == "rough":
                line["wobble_mm"] = rng.choice([0.2, 0.6])
            frame.line = line
    return leaves


def make_render_book(rng, dest: Path, index: int) -> None:
    from genko import brushes, models
    from genko.io import save_episode
    from genko.models import Binding, Layer, LayerKind, LayerRole, PageSpec

    specs = [lambda: PageSpec.custom(70, 95, 60, 85, 3, 8, 8, 7, 6, expression=rng.choice(["mono", "color"])),
             lambda: PageSpec.custom(82.5, 100, 72, 90, 3.5, 9, 10, 8, 6.5, expression=rng.choice(["mono", "color"])),
             lambda: PageSpec.custom(60, 60, 54, 54, 2, 5, 5, 4, 4)]
    if index % 10 == 9:
        specs = [PageSpec.a5_doujin]
    spec = rng.choice(specs)()
    binding = rng.choice([Binding.RIGHT, Binding.LEFT])
    episode = models.new_episode("描画試験", 1, rng.randint(1, 3), spec, binding)
    episode.start_side = rng.choice([None, "left", "right"])
    custom = {k: dict(v) for k, v in brush_definitions().items() if k.startswith("c_") and rng.random() < 0.4}
    episode.brush_custom = custom
    brushes.CUSTOM.clear()
    brushes.register(custom)
    kinds = list(brushes.BRUSHES) + list(custom) + ["oil", "no-such-brush", ""]
    for page in episode.pages:
        page.numero = False  # (nombres are drawn in M4: most pages have none)
        rand_frames(rng, page, models)
        leaf_ids = [f.id for f in page.leaf_frames()]
        for layer in page.layers:
            if layer.kind == LayerKind.STROKES:
                layer.strokes = rand_strokes(rng, page, models, kinds)
                layer.panel_each = layer.panel_each or rng.random() < 0.3
                layer.panel_clip = rng.random() < 0.85
        size = (max(1, round(float(page.spec.width_mm) / 25.4 * 150)), max(1, round(float(page.spec.height_mm) / 25.4 * 150)))
        for n in range(rng.randrange(0, 7)):
            kind = rng.choice(["strokes", "raster", "raster", "fill", "adjust", "strokes"])
            layer = Layer(id=models.new_id(), role=rng.choice([LayerRole.USER, LayerRole.USER, LayerRole.INK, LayerRole.FINISH,
                                                               LayerRole.NAME, LayerRole.DRAFT, LayerRole.BG]),
                          kind={"strokes": LayerKind.STROKES, "raster": LayerKind.RASTER, "fill": LayerKind.FILL,
                                "adjust": LayerKind.ADJUST}[kind])
            layer.exportable = rng.random() < 0.9
            layer.visible = rng.random() < 0.9
            layer.blend = rng.choice(BLEND_MODES)
            layer.opacity = rng.choice([1.0, 1.0, 0.7, 0.4, 0.0])
            layer.clip = rng.random() < 0.25
            layer.panel_clip = rng.random() < 0.7
            layer.panel_each = rng.random() < 0.3
            layer.lock_alpha = rng.random() < 0.2
            if rng.random() < 0.2:
                layer.color = rand_colour(rng, alpha=False)
                layer.color_prints = rng.random() < 0.5
            if kind == "strokes":
                layer.strokes = rand_strokes(rng, page, models, kinds)
                if rng.random() < 0.3:
                    layer.patches = rand_patches(rng, models, page)
            elif kind == "raster":
                own = rng.choice([size, (size[0] // 2 + 3, size[1] // 3 + 7), (size[0] * 2, size[1] * 2), (17, 23)])
                layer.raster_png = rand_picture(rng, own, rng.choice(["RGBA", "RGBA", "RGB", "L", "LA", "P", "P+tRNS", "1"]))
                if rng.random() < 0.4:
                    layer.strokes = rand_strokes(rng, page, models, kinds)
                if rng.random() < 0.3:
                    layer.patches = rand_patches(rng, models, page)
            elif kind == "fill":
                layer.fill_rgb = rand_colour(rng, alpha=False)
                r = rng.random()
                if r < 0.4:
                    layer.fill = {"rgb": list(rand_colour(rng, alpha=False))}
                elif r < 0.9:
                    g = {"from": [rng.uniform(0, 50), rng.uniform(0, 60)], "to": [rng.uniform(0, 70), rng.uniform(0, 90)],
                         "shape": rng.choice(["linear", "radial", "ellipse", None]),
                         "repeat": rng.choice(["none", "repeat", "mirror", None])}
                    if rng.random() < 0.5:
                        g["stops"] = [[rng.random(), list(rand_colour(rng, alpha=False)), rng.choice([1.0, 0.5, None])]
                                      for _ in range(rng.randrange(1, 5))]
                    else:
                        g.update({"rgb_from": list(rand_colour(rng, alpha=False)), "rgb_to": list(rand_colour(rng, alpha=False)),
                                  "opacity_from": rng.choice([1.0, 0.3]), "opacity_to": rng.choice([1.0, 0.0])})
                    if g["shape"] == "ellipse":
                        g["ratio"] = rng.choice([0.3, 2.0])
                    layer.fill = {"gradient": g}
            else:
                layer.adjust = dict(rng.choice(ADJUSTMENTS))
            if kind in ("strokes", "raster") and rng.random() < 0.25:
                effect = {}
                if rng.random() < 0.6:
                    effect["border"] = {"width_mm": rng.choice([0.3, 0.8, 2.0]), "rgb": list(rand_colour(rng, alpha=False))}
                if rng.random() < 0.6:
                    effect["water_edge"] = {"width_mm": rng.choice([0.4, 1.0, 2.5]), "strength": rng.choice([0.6, 1.0])}
                layer.effect = effect
            if rng.random() < 0.2:
                layer.mask = {"png": rand_picture(rng, rng.choice([size, (40, 30)]), rng.choice(["L", "RGBA", "1"])),
                              "enabled": rng.random() < 0.8}
            page.layers.insert(rng.randrange(0, len(page.layers) + 1), layer)
        if rng.random() < 0.3:
            page.extra["paper_rgb"] = list(rand_colour(rng, alpha=False))
        if rng.random() < 0.4:
            page.fills = {role: rand_colour(rng, alpha=False) for role in rng.sample(
                [LayerRole.BG, LayerRole.NAME, LayerRole.INK, LayerRole.FINISH], rng.randrange(1, 4))}
        if rng.random() < 0.3:
            page.ruler = {"points": [[rng.uniform(0, 60), rng.uniform(0, 80)] for _ in range(rng.randrange(1, 3))]}
        if len(episode.pages) > 1 and rng.random() < 0.4:
            page.onion_from = rng.choice([p.index for p in episode.pages])
        # what M2-R1 does not draw yet (these pages are drawn with skip_unported on both sides)
        if rng.random() < 0.12:
            page.numero = True
        if rng.random() < 0.08:
            episode.add_line(page.index, "台詞", speaker="A", x_mm=10.0, y_mm=10.0)
        if rng.random() < 0.06:
            page.layers.append(Layer(id=models.new_id(), role=LayerRole.TONE, kind=LayerKind.TONE, lpi=60.0, density=0.3))
        if rng.random() < 0.05:
            page.effects = [{"id": models.new_id(), "kind": "speed", "frame_id": None, "params": {}}]
        if rng.random() < 0.05:
            page.prims = [{"id": models.new_id(), "kind": "cube"}]
        if rng.random() < 0.05:
            target = rng.choice([layer for layer in page.layers if layer.kind in (LayerKind.STROKES, LayerKind.RASTER)])
            target.screen = {"pattern": "dot", "lpi": 60.0, "angle": 45.0, "black": 0.1, "white": 0.95}
        _ = leaf_ids
    save_episode(episode, dest, actor="human:作者")
    return unported_of(episode)


def unported_of(episode) -> dict:
    """What each page carries that M2-R1 does not draw (page index → names, as render::NotYetPorted names them)."""
    out = {}
    for page in episode.pages:
        names = set()
        if page.numero:
            names.add("nombre")
        if episode.story_for_page(page.index):
            names.add("balloons")
        if any(layer.role.value == "tone" or layer.kind.value == "tone" for layer in page.layers):
            names.add("tones")
        if page.effects:
            names.add("effects")
        if any(layer.screen for layer in page.layers):
            names.add("screen")
        if page.onion_from:  # (the page underneath is drawn too)
            prev = next((p for p in episode.pages if p.index == page.onion_from), None)
            if prev is not None and prev is not page:
                names.add("onion")
        out[str(page.index)] = sorted(names)
    return out


def make_books(out: str, seed: int, count: int) -> None:
    import pyref_harness

    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for i in range(count):
        pyref_harness.fresh_process_state(True)
        name = f"book-{i:02d}.genko"
        manifest[name] = make_render_book(random.Random(seed * 1000 + i), root / name, i)
    (root / "MANIFEST.json").write_text(dumps(manifest), encoding="utf-8")


# --- rendering ----------------------------------------------------------------------------------------------------


def leave_out_unported() -> None:
    """What RenderOptions::skip_unported leaves out, left out here too (each drawing function does nothing)."""
    from genko import anim, balloons, covers, nombre, render, tones

    tones.draw_layer = lambda image, layer, page, dpi, print_mode: image
    tones.screened = lambda raster, spec, dpi: raster
    render._draw_effects = lambda image, page, dpi: image
    render._placed_raster = lambda *args, **kwargs: None
    render._finish_placed = lambda fitted, *args, **kwargs: fitted
    nombre.draw = lambda *args, **kwargs: None
    covers.draw_folds = lambda *args, **kwargs: None
    anim.at_frame = lambda page, frame: page
    balloons.draw_lines = lambda *args, **kwargs: None


def render_jobs(jobs_path: str) -> None:
    """Each job: {"book", "page", "dpi", "mode", "out", "skip_unported", "crop_marks", "then": [ops…]}."""
    from genko import render
    from genko.io import load_episode

    from genko import brushes

    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    if any(job.get("skip_unported") for job in jobs):
        leave_out_unported()
    books = {}
    for job in jobs:
        render._STROKE_CACHE.clear()
        render._FRAME_MASKS.clear()
        key = (job["book"], bool(job.get("skip_unported")))
        if key not in books or job.get("then"):  # (a job that changes the book gets its own copy)
            brushes.CUSTOM.clear()
            episode = load_episode(Path(job["book"]))
            if job.get("skip_unported"):
                episode.story = []
            if job.get("then"):
                books.pop(key, None)
            else:
                books[key] = episode
        else:
            episode = books[key]
        brushes.CUSTOM.clear()
        brushes.register(episode.brush_custom)
        page = next(p for p in episode.pages if p.index == job["page"])
        kind = job.get("kind", "page")
        if kind == "frame":
            render.render_frame(page, job["frame"], job["dpi"], mode=job["mode"], episode=episode).save(job["out"])
            continue
        if kind == "spread":
            render.render_spread(episode, job["first"], job["second"], dpi=job["dpi"], mode=job["mode"],
                                 to_trim=bool(job.get("to_trim"))).save(job["out"])
            continue
        if kind == "layer":
            layer = next(layer for layer in page.layers if layer.id == job["layer"])
            render.layer_image(page, layer, job["dpi"], episode).save(job["out"])
            continue
        image = render.render_page(page, job["dpi"], mode=job["mode"], episode=episode,
                                   crop_marks=bool(job.get("crop_marks")), rough=bool(job.get("rough")))
        if kind == "bitonal":
            render.to_bitonal(image, job["threshold"], job.get("screen")).save(job["out"])
            continue
        if job.get("then"):
            # the same page again after a line is added (the remembered layer picture is drawn on)
            from genko.models import Stroke

            layer = next(layer for layer in page.layers if layer.id == job["then"]["layer"])
            spec = job["then"]
            layer.strokes = list(layer.strokes) + [Stroke(id=spec["id"], points=[tuple(p) for p in spec["points"]],
                                                          pressure=list(spec.get("pressure") or []),
                                                          width_mm=spec["width_mm"], kind=spec["kind"])]
            image = render.render_page(page, job["dpi"], mode=job["mode"], episode=episode)
        image.save(job["out"])


def render_jobs_parallel(jobs_path: str, workers: int) -> None:
    """render_jobs split by book over `workers` processes (the parts are written next to the jobs file). A worker that
    dies (killed for memory, say) fails the run at once: multiprocessing.Pool would wait for it for ever."""
    import multiprocessing
    from concurrent.futures import ProcessPoolExecutor

    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    by_book: dict[str, list] = {}
    for job in jobs:
        by_book.setdefault(job["book"] + ("#skip" if job.get("skip_unported") else ""), []).append(job)
    parts = Path(jobs_path).with_suffix(".parts")
    parts.mkdir(parents=True, exist_ok=True)
    files = []
    for i, group in enumerate(by_book.values()):
        path = parts / f"jobs-{i:03d}.json"
        path.write_text(dumps(group), encoding="utf-8")
        files.append(str(path))
    with ProcessPoolExecutor(max(1, workers), mp_context=multiprocessing.get_context("spawn")) as pool:
        for _ in pool.map(render_jobs, files):
            pass


def unit_tables(outdir: str) -> None:
    root = Path(outdir)
    root.mkdir(parents=True, exist_ok=True)
    tables = {"random": random_table(), "stroke": stroke_geom_table(), "image": image_table(),
              "png": png_samples(root)}
    (root / "render_unit_tables.json").write_text(json.dumps(tables, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("unit-tables")
    p.add_argument("outdir")
    p = sub.add_parser("draw-cases")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=300)
    p = sub.add_parser("brush-cases")
    p.add_argument("outdir")
    p.add_argument("--seed", type=int, default=1)
    p = sub.add_parser("make-books")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=40)
    p = sub.add_parser("render")
    p.add_argument("jobs")
    p.add_argument("--workers", type=int, default=1)
    args = parser.parse_args(argv)
    if args.cmd == "unit-tables":
        unit_tables(args.outdir)
    elif args.cmd == "draw-cases":
        draw_cases(args.out, args.seed, args.count)
    elif args.cmd == "brush-cases":
        brush_cases(args.outdir, args.seed)
    elif args.cmd == "make-books":
        make_books(args.out, args.seed, args.count)
    elif args.cmd == "render":
        if args.workers > 1:
            render_jobs_parallel(args.jobs, args.workers)
        else:
            render_jobs(args.jobs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
