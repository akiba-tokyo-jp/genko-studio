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
  adjust-cases OUT              filters.apply_filter for correction-layer settings (good, bad and odd ones): the
                                tables Image.point gets, or the exception
  colour-cases OUT ICC           colour.to_cmyk / from_cmyk / proof / ink_coverage on a fixed picture, with and without
                                the CMYK profile ICC (each rendering intent), and the profile checks: their bytes or
                                the exception
  abr-cases OUT FILE...           abr.brushes_from for each .abr FILE (prefix: its stem and a space) and
                                abr.tip_from_picture for each other FILE: the definitions with each tip's pixels, or
                                the exception
  brush-library OUT CONFIG      brushes.save_to_library / load_library in the config folder CONFIG (a few brushes
                                kept, one forgotten): the file's text and what it reads back
  material-library STEPS OUT CONFIG
                                materials.add_material / import_image / update_material / delete_material /
                                add_folder / folders / get_material / import_pack / export_pack, one step of the JSON
                                list STEPS after another in the config folder CONFIG: each step's result (or error)
                                and the library's files after it (new ids as $0, $1, … in the order made)
  make-books OUT --seed N --count K
                                K random books for drawing (layers, brushes, rasters, patches, masks, blend modes,
                                fills and gradients, corrections, panels): OUT/book-NN.genko
  render JOBS                   render_page for each job of a JSON list: {"book", "page", "dpi", "mode", "out",
                                "skip_unported": bool} → a PNG of the page
  layer-image JOBS              layer_image for each job: {"book", "page", "layer", "dpi", "out"}
  text-cases OUT --seed N --count K
                                the letters of chosen lines and K random ones (every style key that changes them,
                                every balloon kind, vertical and across, at a few dpi) as balloons._paint_text draws
                                them, with text_layout's picture, em and corner; tategaki's pure functions; each cell's
                                glyph: OUT (JSON, pictures as PNG)
  balloon-cases OUT --seed N --count K
                                balloons.draw_lines for chosen groups of lines (every shape, every tail kind, bent
                                and curved tails, joined balloons, turned ones, picture balloons, cuts, hand-drawn
                                outlines, every style key of the balloon) and K random ones, on RGB and RGBA pictures
                                at a few dpi; and the shapes' geometry (_edge_point, _tail_polygon, _polyline_tail,
                                _uneven, _electric, _outline, _wobbly, _smooth_closed, _turned, _thought_trail): OUT

With skip_unported the elements this C++ step does not draw yet (placed pictures, cover folds, animation) are left
out the way the C++ RenderOptions::skip_unported leaves them out (tones, effect lines and layer screens are drawn on
both sides since M3-B, the 3D guides since M3-C, nombres since the M2 material work, lines and their balloons since
M4).

The reference lays text out as the measured one did: Pillow without raqm (BASIC layout, FreeType 2.14.3). A Pillow that
has raqm (and finds fribidi) is held to BASIC here, so the nombres compare with the C++ drawing of them.
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
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

try:  # (the measured reference's text layout: BASIC, without raqm)
    from PIL import ImageFont as _ImageFont
    _ImageFont.core.HAVE_RAQM = False
except ImportError:
    pass


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


def make_render_book(rng, dest: Path, index: int, story: bool = False) -> None:
    """A random book for drawing. `story`: with lines of dialogue in balloons, and a jacket in every fourth book (whose
    folds this build does not draw yet), drawn from numbers of their own (the pages are the same either way)."""
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
    if story:
        srng = random.Random(rng.getrandbits(64))
        for page in episode.pages:
            if srng.random() < 0.55:
                rand_story(srng, episode, page)
        if index % 4 == 3:  # (what is still not drawn: a jacket's folds, in name and proof)
            jacket = episode.pages[-1]
            jacket.extra["cover"] = {"kind": "jacket", "spine_mm": srng.choice([0, 8, 15]), "flap_mm": srng.choice([0, 20])}
            for page in episode.pages:  # (a jacket is wider than the pages: no onion skin between them)
                if page is jacket or page.onion_from == jacket.index:
                    page.onion_from = None
    save_episode(episode, dest, actor="human:作者")
    return unported_of(episode)


def rand_story(rng, episode, page) -> None:
    """A few lines on the page: balloons of every shape with tails, joined, turned, cut, drawn by hand, set under a
    layer; some not placed (drawn as labels)."""
    from genko.migrate import _line

    w_page, h_page = float(page.spec.width_mm), float(page.spec.height_mm)
    panels = {f.id: (f.rect.x, f.rect.y, f.rect.width, f.rect.height) for f in page.leaf_frames()}
    layer_ids = [layer.id for layer in page.layers]
    group = None
    for n in range(rng.randrange(1, 5)):
        if rng.random() < 0.12:  # not placed: a label at the inner frame's corner
            data = {"id": "%012x" % rng.getrandbits(48), "page_index": page.index, "text": _rand_text(rng, 6), "x_mm": 0, "y_mm": 0,
                    "balloon": ""}
            if rng.random() < 0.5:
                data["speaker"] = rng.choice(["主人公", "A", ""])
        else:
            data = rand_balloon_line(rng, "%012x" % rng.getrandbits(48), w_page, h_page, list(panels), small_text=True)
            data["page_index"] = page.index
            if group is not None or rng.random() < 0.15:
                group = group or rng.choice(["g", "会話", 1])
                data["style"]["group"] = group
            if rng.random() < 0.08 and layer_ids:
                data["style"]["below_layer"] = rng.choice(layer_ids + ["no-such-layer"])
        line = _line(data)
        episode.story.append(line)
        page.texts.append(line)


def unported_of(episode) -> dict:
    """What each page carries that M2-R1 does not draw (page index → names, as render::NotYetPorted names them)."""
    from genko import covers

    out = {}
    for page in episode.pages:
        names = set()
        if covers.folds(page):  # (drawn in name and proof)
            names.add("covers")
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
        manifest[name] = make_render_book(random.Random(seed * 1000 + i), root / name, i, story=True)
    (root / "MANIFEST.json").write_text(dumps(manifest), encoding="utf-8")


# --- rendering ----------------------------------------------------------------------------------------------------


def leave_out_unported() -> None:
    """What RenderOptions::skip_unported leaves out, left out here too (each drawing function does nothing)."""
    from genko import anim, covers, render

    render._placed_raster = lambda *args, **kwargs: None
    render._finish_placed = lambda fitted, *args, **kwargs: fitted
    covers.draw_folds = lambda *args, **kwargs: None
    anim.at_frame = lambda page, frame: page


@contextmanager
def _unported_scope(enabled: bool):
    """Keep reference-only omissions local even when groups run in the parent."""
    if not enabled:
        yield
        return
    from genko import anim, balloons, covers, render, tones
    names = [(tones, 'draw_layer'), (tones, 'screened'), (render, '_draw_effects'),
             (render, '_draw_prims'), (render, '_placed_raster'), (render, '_finish_placed'),
             (covers, 'draw_folds'), (anim, 'at_frame'), (balloons, 'draw_lines')]  # (balloons: drawn, kept as it is)
    originals = [(module, name, getattr(module, name)) for module, name in names]
    try:
        leave_out_unported()
        yield
    finally:
        for module, name, function in originals:
            setattr(module, name, function)
        render._STROKE_CACHE.clear()
        render._FRAME_MASKS.clear()


def render_jobs(jobs_path: str) -> None:
    """Render one homogeneous book/mode batch without leaking skip mode to its caller."""
    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    with _unported_scope(any(job.get('skip_unported') for job in jobs)):
        _render_jobs(jobs)


def _render_jobs(jobs: list) -> None:
    from genko import render
    from genko.io import load_episode
    from genko import brushes

    books = {}
    for job in jobs:
        render._STROKE_CACHE.clear()
        render._FRAME_MASKS.clear()
        key = (job["book"], bool(job.get("skip_unported")))
        if key not in books or job.get("then"):  # (a job that changes the book gets its own copy)
            brushes.CUSTOM.clear()
            episode = load_episode(Path(job["book"]))
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


def effective_render_workers(requested: int) -> int:
    """Cap reference processes by the actual CPU quota and a conservative image-memory budget.

    A high-DPI renderer can peak above 1 GiB with stroke pictures, masks,
    RGBA working images and numpy buffers (1.11 GiB measured in the M3-A1
    contract). Reserve half the container for native/other CTest work, then
    budget 1536 MiB per Python renderer.
    This is admission control for this test workload, not a per-image hard limit.
    """
    import os

    cpus = max(1, os.cpu_count() or 1)
    if hasattr(os, 'sched_getaffinity'):
        try:
            cpus = min(cpus, max(1, len(os.sched_getaffinity(0))))
        except OSError:
            pass
    cgroup = Path('/sys/fs/cgroup')
    try:
        quota, period = (cgroup / 'cpu.max').read_text().split()
        if quota != 'max' and int(period) > 0:
            cpus = min(cpus, max(1, int(quota) // int(period)))
    except (OSError, ValueError):
        pass
    # Unknown/unlimited memory: choose the safe serial fallback, not all host CPUs.
    memory_workers = 1
    try:
        limit = int((cgroup / 'memory.max').read_text())
        current = int((cgroup / 'memory.current').read_text())
        if limit > 0 and current >= 0:
            available = max(0, limit - max(current, limit // 2))
            memory_workers = max(1, available // (1536 * 1024**2))
    except (OSError, ValueError):
        pass
    return min(max(1, requested), cpus, memory_workers)


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
    effective = min(effective_render_workers(workers), max(1, len(files)))
    print(dumps({'render_resources': {'requested_workers': workers, 'effective_workers': effective,
                                    'groups': len(files)}}), file=sys.stderr, flush=True)
    if effective == 1:
        for path in files:
            render_jobs(path)
        return
    with ProcessPoolExecutor(effective, mp_context=multiprocessing.get_context("spawn")) as pool:
        for _ in pool.map(render_jobs, files):
            pass


# --- correction layers: filters.apply_filter's tables -------------------------------------------------------------------

# (NaN and infinities as text, "nan" and "inf": the C++ build reads the JSON literals NaN and Infinity as null)
ADJUST_CASES = [
    # levels: ints (any size), then the table form
    ("levels", {}), ("levels", {"black": 30, "white": 220}), ("levels", {"black": 2.7, "white": "200"}),
    ("levels", {"black": 300, "white": 100}), ("levels", {"black": -50, "white": 5000}),
    ("levels", {"black": 100, "white": 100}), ("levels", {"black": 254, "white": 255}), ("levels", {"black": True, "white": 200}),
    ("levels", {"black": "x"}), ("levels", {"black": None}), ("levels", {"black": [1]}), ("levels", {"white": "nan"}),
    ("levels", {"black": 1e300}), ("levels", {"white": -1e300}), ("levels", {"black": -1e30}), ("levels", {"black": 1e19, "white": 2e19}),
    ("levels", {"black": "99999999999999999999"}), ("levels", {"white": "-99999999999999999999"}),
    ("levels", {"gamma": 0.5}),
    ("levels", {"black": 10.5, "white": 240, "gamma": 1.7, "out_black": 20, "out_white": 250, "channel": "g"}),
    ("levels", {"gamma": None, "out_black": 10}), ("levels", {"black": 1e17, "white": 0, "gamma": 1}),
    ("levels", {"black": "inf", "gamma": 1}), ("levels", {"black": "nan", "gamma": 1}), ("levels", {"out_black": "nan"}),
    ("levels", {"out_white": "inf"}), ("levels", {"out_white": 1e308, "out_black": -1e308}), ("levels", {"out_white": 1e300}),
    ("levels", {"gamma": 1, "channel": "q"}), ("levels", {"gamma": 1, "channel": "b"}), ("levels", {"gamma": 1, "channel": 5}),
    ("levels", {"gamma": 1, "channel": ""}), ("levels", {"channel": "r"}), ("levels", {"gamma": "x"}), ("levels", {"gamma": 100}),
    ("levels", {"gamma": -1}), ("levels", {"gamma": 1, "white": 5, "black": 10}),
    ("levels", {"out_black": 300, "out_white": -40, "gamma": 2}), ("levels", {"gamma": 1, "out_black": "x", "out_white": None}),
    # curve: a gamma, or a curve through points
    ("curve", {}), ("curve", {"gamma": 2.2}), ("curve", {"gamma": "abc"}), ("curve", {"gamma": "nan"}), ("curve", {"gamma": 0.01}),
    ("curve", {"points": [[0, 10], [64, 90], [128, 120], [200, 230], [255, 250]], "channel": "rgb"}),
    ("curve", {"points": [[0, 0], [100, 200], [100, 180], [255, 255]], "channel": "r"}), ("curve", {"points": [[0, 0]]}),
    ("curve", {"points": [[0, 0], [0.4, 9]]}), ("curve", {"points": [[0, 0, 0]]}), ("curve", {"points": [5, 6]}),
    ("curve", {"points": [["1", "2"], ["200", 100]]}), ("curve", {"points": [["nan", 1], [3, 4]]}),
    ("curve", {"points": [["inf", 1], [3, 4]]}), ("curve", {"points": [[0, "nan"], [255, 255]]}), ("curve", {"points": "ab"}),
    ("curve", {"points": {"a": 1}}), ("curve", {"points": [[0, 0], [10, 300], [20, -50], [255, 255]], "channel": "g"}),
    ("curve", {"points": [[-100, 0], [400, 255]]}), ("curve", {"points": [[0, 0], [128, 128], [129, 255], [255, 255]]}),
    ("curve", {"points": [[0, 255], [255, 0]]}), ("curve", {"points": [[1e300, 0], [0, 1]]}),
    ("curve", {"points": [[2.5, 0], [3.5, 255]]}), ("curve", {"points": [[0, 0], [255, 255]], "gamma": 3}),
    ("curve", {"points": [], "gamma": 3}), ("curve", {"points": 0}), ("curve", {"points": [[None, 1], [2, 3]]}),
    ("curve", {"points": [[1, None], [2, 3]]}), ("curve", {"points": [[1, 2], 7]}), ("curve", {"points": [[1, 2], [3]]}),
    # hue
    ("hue", {}), ("hue", {"shift": 75, "saturation": 1.4, "value": 0.9}), ("hue", {"shift": -30.5}), ("hue", {"shift": 1e20}),
    ("hue", {"shift": "nan"}), ("hue", {"shift": "inf"}), ("hue", {"saturation": "inf"}), ("hue", {"value": 1e308}),
    ("hue", {"saturation": -5}), ("hue", {"saturation": None}), ("hue", {"shift": [1]}), ("hue", {"shift": "12"}),
    ("hue", {"shift": 359.9}), ("hue", {"shift": -720}), ("hue", {"value": 0.5, "saturation": 0}),
    ("hue", {"shift": 180, "saturation": 2.5, "value": 1.5}), ("hue", {"shift": True}),
    # invert, posterize, threshold, bitonal
    ("invert", {}), ("invert", {"anything": 1}),
    ("posterize", {}), ("posterize", {"levels": 3}), ("posterize", {"levels": 1}), ("posterize", {"levels": 1000}),
    ("posterize", {"levels": "4"}), ("posterize", {"levels": 2.9}), ("posterize", {"levels": "2.9"}),
    ("posterize", {"levels": "nan"}), ("posterize", {"levels": 1e30}), ("posterize", {"levels": "inf"}),
    ("posterize", {"levels": -1e300}), ("posterize", {"levels": None}), ("posterize", {"levels": 7}), ("posterize", {"levels": 64}),
    ("posterize", {"levels": -3}),
    ("threshold", {}), ("threshold", {"threshold": 140}), ("threshold", {"threshold": "x"}), ("threshold", {"threshold": 1e30}),
    ("threshold", {"threshold": -1e30}), ("threshold", {"threshold": 127.9}), ("threshold", {"threshold": "0"}),
    ("threshold", {"threshold": 256}),
    ("bitonal", {}), ("bitonal", {"threshold": 100}), ("bitonal", {"threshold": None}), ("bitonal", {"threshold": 254.5}),
    ("bitonal", {"threshold": -1}),
    # gradient_map: colours spread evenly, or stops; table values Pillow keeps in 0..255 its own way
    ("gradient_map", {}), ("gradient_map", {"colors": [[20, 10, 80], [250, 200, 40], [255, 255, 255]]}),
    ("gradient_map", {"stops": [[0.0, [0, 0, 0]], [0.3, [200, 20, 20]], [0.3, [20, 200, 20]], [1.0, [255, 255, 255]]]}),
    ("gradient_map", {"colors": [[0, 0, 0]]}), ("gradient_map", {"stops": [[0.5, [1, 2, 3]]]}),
    ("gradient_map", {"colors": [[0, 0], [255, 255, 255]]}), ("gradient_map", {"stops": [[0, [0, 0]], [1, [255, 255, 255]]]}),
    ("gradient_map", {"colors": [[300, -20, 1000], [-500, 128, 70000]]}),
    ("gradient_map", {"colors": [[0, 0, 0], [10000000000, -10000000000, 4294967296]]}),
    ("gradient_map", {"colors": [[0, 0, 0], [1e19, 0, 0]]}), ("gradient_map", {"colors": [[0, 0, 0], [1e19, 0, 0], [1, 2]]}),
    ("gradient_map", {"stops": [[0, [0, 0, 0]], [1, [1e19, 0, 0]]]}),
    ("gradient_map", {"stops": [[0, [0, 0, 0]], ["1", ["255", 0, 0]]]}),
    ("gradient_map", {"stops": [[0.5, [10, 20, 30]], [0.2, [200, 100, 50]]]}),
    ("gradient_map", {"stops": [[0.5, [10, 20, 30]], [0.5, [5, 5, 5]], [1, [0, 0, 0]]]}),
    ("gradient_map", {"stops": [[-1, [0, 0, 0]], [2, [255, 255, 255]]]}),
    ("gradient_map", {"stops": [["nan", [0, 0, 0]], [1, [255, 255, 255]]]}),
    ("gradient_map", {"stops": [[0, [0, 0, 0]], [1, {"a": 1}]]}), ("gradient_map", {"stops": [[0, 5], [1, [1, 2, 3]]]}),
    ("gradient_map", {"stops": [{"a": 1}, [1, [1, 2, 3]]]}), ("gradient_map", {"stops": [[0], [1, [1, 2, 3]]]}),
    ("gradient_map", {"stops": "ab"}), ("gradient_map", {"stops": [[0, "123"], [1, "456"]]}), ("gradient_map", {"colors": "abc"}),
    ("gradient_map", {"colors": [[0, 0, 0, 99], [255, 255, 255, "x"]]}),
    ("gradient_map", {"stops": [[0, [0, 0, 0, "x"]], [1, [1, 1, 1]]]}),
    # brightness_contrast
    ("brightness_contrast", {}), ("brightness_contrast", {"brightness": 25, "contrast": -40}),
    ("brightness_contrast", {"brightness": -10, "contrast": 60, "channel": "b"}), ("brightness_contrast", {"brightness": "x"}),
    ("brightness_contrast", {"contrast": None}), ("brightness_contrast", {"brightness": "nan"}),
    ("brightness_contrast", {"contrast": "-inf"}), ("brightness_contrast", {"brightness": 1e300, "contrast": 1e300}),
    ("brightness_contrast", {"contrast": 100}), ("brightness_contrast", {"contrast": -100}),
    ("brightness_contrast", {"brightness": 100, "channel": "rgb"}), ("brightness_contrast", {"channel": "r"}),
    # not an adjustment at all
    ("nope", {}),
]


def adjust_cases(out: str) -> None:
    """filters.apply_filter for each setting of ADJUST_CASES on a small picture (as ops._adjust_spec tries one): the
    tables Pillow makes of what Image.point is given (each band's 256 values, read back from a picture of every value),
    or the exception (its type and message)."""
    from PIL import Image

    from genko import filters

    original = Image.Image.point
    records: list = []

    def point(self, lut, mode=None):
        result = original(self, lut, mode)
        bands = len(self.getbands())
        probe = Image.new(self.mode, (256, 1))
        probe.putdata([(i,) * bands if bands > 1 else i for i in range(256)])
        records.append([list(band.tobytes()) for band in original(probe, lut, mode).split()])
        return result

    cases = []
    Image.Image.point = point
    try:
        for kind, params in ADJUST_CASES:
            records.clear()
            entry = {"kind": kind, "params": params}
            try:
                filters.apply_filter(Image.new("RGBA", (4, 4), (120, 80, 40, 255)), kind, dict(params))
                entry["tables"] = list(records)
            except Exception as exc:  # (the ops refuse ValueError, TypeError and KeyError; the renderer passes ValueError)
                entry["error"] = [type(exc).__name__, str(exc)]
            cases.append(entry)
    finally:
        Image.Image.point = original
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(dumps({"cases": cases}) + "\n", encoding="utf-8")


def unit_tables(outdir: str) -> None:
    root = Path(outdir)
    root.mkdir(parents=True, exist_ok=True)
    tables = {"random": random_table(), "stroke": stroke_geom_table(), "image": image_table(),
              "png": png_samples(root)}
    (root / "render_unit_tables.json").write_text(json.dumps(tables, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")


def colour_cases(out: str, icc: str) -> None:
    """colour.py on a fixed RGBA picture (every grey, saturated and dark colours, half-clear pixels): each result's
    mode and bytes (base64), or the exception; the sRGB profile's bytes (its creation time left out)."""
    from PIL import Image

    from genko import colour

    w, h = 64, 40
    picture = Image.new("RGBA", (w, h))
    picture.putdata([((x * 4) % 256, (y * 6 + x) % 256, (x * y) % 256, 255 if y < 30 else 100 + x) for y in range(h) for x in range(w)])

    def shot(make):
        try:
            image = make()
            return {"mode": image.mode, "size": list(image.size), "bytes": base64.b64encode(image.tobytes()).decode("ascii")}
        except Exception as exc:  # (the reference's own exception: its type and message)
            return {"error": [type(exc).__name__, str(exc)]}

    cases = {"picture": base64.b64encode(picture.tobytes()).decode("ascii"), "size": [w, h]}
    cases["to_cmyk"] = shot(lambda: colour.to_cmyk(picture))
    cases["to_cmyk_200"] = shot(lambda: colour.to_cmyk(picture, ink_limit=200))
    cases["from_cmyk"] = shot(lambda: colour.from_cmyk(colour.to_cmyk(picture)))
    cases["proof"] = shot(lambda: colour.proof(picture))
    cases["ink"] = colour.ink_coverage(colour.to_cmyk(picture))
    for intent in colour.INTENTS:
        cases["to_cmyk_icc_" + intent] = shot(lambda: colour.to_cmyk(picture, icc, intent=intent))
        cases["from_cmyk_icc_" + intent] = shot(lambda: colour.from_cmyk(colour.to_cmyk(picture, icc), icc, intent=intent))
    cases["proof_icc"] = shot(lambda: colour.proof(picture, icc))
    cases["ink_icc"] = colour.ink_coverage(colour.to_cmyk(picture, icc))
    cases["is_cmyk"] = colour.is_cmyk_profile(icc)
    cases["name"] = colour.profile_name(icc)
    srgb = bytearray(colour.srgb_icc())
    srgb[24:36] = bytes(12)  # (the date and time it was made)
    cases["srgb_icc"] = base64.b64encode(bytes(srgb)).decode("ascii")
    srgb_file = Path(out).with_suffix(".srgb.icc")
    srgb_file.write_bytes(colour.srgb_icc())
    cases["srgb_is_cmyk"] = colour.is_cmyk_profile(srgb_file)
    cases["to_cmyk_srgb_profile"] = shot(lambda: colour.to_cmyk(picture, srgb_file))
    cases["to_cmyk_bad_intent"] = shot(lambda: colour.to_cmyk(picture, icc, intent="vivid"))
    cases["missing_profile"] = shot(lambda: colour.to_cmyk(picture, Path(out).with_suffix(".none.icc")))
    Path(out).write_text(json.dumps(cases) + "\n", encoding="utf-8")


def _tip_pixels(tip_png: str) -> dict:
    from PIL import Image

    image = Image.open(io.BytesIO(base64.b64decode(tip_png)))
    return {"mode": image.mode, "size": list(image.size), "bytes": base64.b64encode(image.tobytes()).decode("ascii")}


def abr_cases(out: str, files: list[str]) -> None:
    """abr.py on the given files: each brush definition with its tip as pixels (the PNG's bytes are the encoder's)."""
    from genko import abr

    cases = {}
    for name in files:
        path = Path(name)
        try:
            if path.suffix.lower() == ".abr":
                found = []
                for definition in abr.brushes_from(path.read_bytes(), prefix=f"{path.stem} "):
                    found.append({**{k: v for k, v in definition.items() if k != "tip_png"}, "tip": _tip_pixels(definition["tip_png"]),
                                  "tip_kind": definition["tip"]})
                cases[path.name] = {"brushes": found}
            else:
                cases[path.name] = {"tip": _tip_pixels(abr.tip_from_picture(str(path)))}
        except Exception as exc:  # (the reference's own exception: its type and message)
            cases[path.name] = {"error": [type(exc).__name__, str(exc)]}
    Path(out).write_text(json.dumps(cases, ensure_ascii=False) + "\n", encoding="utf-8")


def brush_library(out: str, config: str) -> None:
    """brushes.save_to_library and load_library in a config folder of the test's own."""
    import os

    os.environ["GENKO_CONFIG_DIR"] = config
    from genko import brushes

    brushes.save_to_library("my_a", {"label": "細い線", "base": "gpen", "width_mm": 0.35, "opacity": 0.8, "taper": True})
    brushes.save_to_library("my_b", {"label": "B", "base": "maru", "width_mm": 1, "rgb": [255, 255, 255], "spacing": 0.25})
    brushes.save_to_library("my_c", {"label": "\"引用\"\n改行", "base": "gpen", "min_pressure": 1e-05})
    brushes.save_to_library("my_b", None)
    text = brushes.library_path().read_text(encoding="utf-8")
    Path(out).write_text(json.dumps({"text": text, "loaded": brushes.load_library()}, ensure_ascii=False) + "\n", encoding="utf-8")


def material_library(steps_path: str, out: str, config: str) -> None:
    """The person's material library changed step by step, as test_contract_materials changes it in C++."""
    import hashlib
    import os
    import zipfile

    from PIL import Image

    os.environ["GENKO_CONFIG_DIR"] = config
    from genko import materials

    steps = json.loads(Path(steps_path).read_text(encoding="utf-8"))
    created: list[str] = []

    def ref(value):
        return created[int(value[1:])] if isinstance(value, str) and value.startswith("$") else value

    def hide(text: str) -> str:
        for n, made in sorted(enumerate(created), key=lambda e: -len(e[1])):
            text = text.replace(made, f"${n}")
        return text

    def note(made: str) -> None:
        if made.startswith("u-") and made not in created:
            created.append(made)

    def pixels(data: bytes) -> dict:
        with Image.open(io.BytesIO(data)) as image:
            image.load()
            return {"mode": image.mode, "size": list(image.size), "sha256": hashlib.sha256(image.tobytes()).hexdigest()}

    results = []
    for step in steps:
        do = step["do"]
        result: dict = {}
        if do == "picture":  # (inputs: made here, read by both sides)
            Path(step["path"]).parent.mkdir(parents=True, exist_ok=True)
            w, h = step["size"]
            mode = step["mode"]
            image = Image.new("RGBA", (w, h))
            image.putdata([((x * 37 + y * 11) % 256, (x * 7 + y * 53) % 256, (x * x + y) % 256, (x * 13 + y * 29) % 256)
                           for y in range(h) for x in range(w)])
            if mode == "P":
                image = Image.new("P", (w, h))
                image.putpalette([v for k in range(16) for v in ((k * 16) % 256, (k * 85) % 256, (255 - k * 16) % 256)])
                image.putdata([(x + y) % 16 for y in range(h) for x in range(w)])
                image.save(step["path"], step["format"], transparency=3)
            else:
                image.convert(mode).save(step["path"], step["format"])
            continue
        if do == "file":
            Path(step["path"]).parent.mkdir(parents=True, exist_ok=True)
            Path(step["path"]).write_bytes(step["text"].encode("utf-8"))
            continue
        if do == "zip":
            with zipfile.ZipFile(step["path"], "w", zipfile.ZIP_DEFLATED) as archive:
                for name, source, text in step["members"]:
                    archive.writestr(name, Path(source).read_bytes() if source else text.encode("utf-8"))
            continue
        if do == "write_library":
            library = materials.library_dir()
            library.mkdir(parents=True, exist_ok=True)
            (library / step["name"]).write_text(step["text"], encoding="utf-8")
        try:
            if do == "write_library":
                value = None
            elif do == "add_material":
                value = materials.add_material(step["name"], step["kind"], step.get("folder", "マイ素材"), **step.get("data", {}))
            elif do == "import_image":
                value = materials.import_image(Path(step["path"]), step.get("name"), step.get("folder", "画像"), step.get("width_mm"))
            elif do == "update_material":
                value = materials.update_material(ref(step["id"]), **step["change"])
            elif do == "delete_material":
                value = materials.delete_material(ref(step["id"]))
            elif do == "add_folder":
                value = materials.add_folder(step["name"])
            elif do == "folders":
                value = materials.folders()
            elif do == "get_material":
                value = materials.get_material(ref(step["id"]))
            elif do == "user_materials":
                value = materials.user_materials()
            elif do == "import_pack":
                value = materials.import_pack(step["path"], step.get("folder"))
            elif do == "export_pack":
                target = materials.export_pack([ref(i) for i in step["ids"]], step["path"])
                with zipfile.ZipFile(target) as archive:
                    value = [[name, archive.read(name).decode("utf-8") if name.endswith(".json") else pixels(archive.read(name))]
                             for name in archive.namelist()]
            else:
                raise SystemExit(f"unknown step {do}")
            for item in value if isinstance(value, list) else [value]:
                if isinstance(item, dict) and isinstance(item.get("id"), str):
                    note(item["id"])
            result["ok"] = value
        except Exception as exc:  # (as the C++ side reports it: the type and the message)
            result["error"] = [type(exc).__name__, str(exc)]
        library = materials.library_dir()
        try:
            for item in json.loads((library / "library.json").read_text(encoding="utf-8")):
                if isinstance(item, dict) and isinstance(item.get("id"), str):
                    note(item["id"])
        except (OSError, ValueError, TypeError):
            pass
        files = {}
        for file in sorted(library.iterdir()) if library.is_dir() else []:
            if file.suffix == ".png":
                files[file.name] = pixels(file.read_bytes())
            elif file.name in ("library.json", "folders.json"):
                files[file.name] = file.read_text(encoding="utf-8")
        result["files"] = files
        results.append(json.loads(hide(json.dumps(result, ensure_ascii=False))))
    Path(out).write_text(json.dumps(results, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")


# --- lettering (M4: the text of a line, as tategaki.compose and balloons.text_layout set it) ------------------------

TEXT_FONTS = ("antique", "gothic", "mincho", "maru", "hand", "sfx", "sfx_pop")
TEXT_KINDS = ("speech", "rounded", "box", "cloud", "thought", "shout", "electric", "flash", "whisper", "narration", "sfx",
              "none", "picture", "dotted_box", "tone_box", "fancy_box")
_HIRA = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんがぎぐげござじずぜぞだでどばびぶべぼぱぴぷぺぽ"
_SMALL = "ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ"
_KATA = "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンガギグゲゴヴー"
_KANJI = "日本語漫画原稿台詞今日天気世界東京大阪先生学校時間言葉心夢空海山川花鳥風月雨雪光影力戦勝負愛葛辻々〆"
_PUNCT = "、。，．！？!?…‥―～〜「」『』（）()【】［］〈〉《》・：；ー−‐∼"
_LATIN = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
_TOKENS = ["この", "その", "ちょっと", "ありがとう", "ございます", "やっぱり", "Hello", "OK", "Mr. Smith", "E-mail", "NASA",
           "12", "100", "2024", "!?", "!!", "！？", "？！？", "!!!!", "１２", "１２３", "１２３４", "ふう〜", "えっ", "ドドドド",
           "ゴゴゴ", "ズバッ", "まんが作りの", "モヤモヤを", "ズバッと解決する", "このコーナー！", "「そうか」", "（笑）",
           "葛\U000e0100", "辻\ufe00", "A\ufe0f", "\ufe0e", "…", "。", "。」", "．", "\n", " ", "\u3000", "a b", "x-y"]


def _text_png(im) -> str:
    return b64(png_bytes(im))


def _rand_text(rng, longest: int = 14) -> str:
    out = []
    for _ in range(rng.randrange(1, longest)):
        pool = rng.choice(["hira", "hira", "kanji", "kata", "punct", "small", "latin", "digit", "token", "token"])
        if pool == "token":
            out.append(rng.choice(_TOKENS))
        elif pool == "latin":
            out.append("".join(rng.choice(_LATIN) for _ in range(rng.randrange(1, 7))))
        elif pool == "digit":
            out.append(str(rng.randrange(0, 10 ** rng.randrange(1, 5))))
        else:
            chars = {"hira": _HIRA, "kanji": _KANJI, "kata": _KATA, "punct": _PUNCT, "small": _SMALL}[pool]
            out.append("".join(rng.choice(chars) for _ in range(rng.randrange(1, 5))))
    return "".join(out)


def _substring(rng, text: str) -> str:
    if not text:
        return ""
    a = rng.randrange(len(text))
    return text[a:a + rng.randrange(1, 5)]


def _text_picture() -> str:
    """A small picture for style.fill_png (letters painted with it)."""
    from PIL import Image, ImageDraw

    im = Image.new("RGB", (24, 16), (240, 200, 40))
    draw = ImageDraw.Draw(im)
    draw.rectangle((0, 8, 23, 15), fill=(30, 60, 200))
    draw.ellipse((4, 2, 14, 12), fill=(220, 20, 60))
    return _text_png(im)


def text_units(rng) -> dict:
    """The pure functions of tategaki and fonts on random and chosen texts."""
    from genko import fonts, tategaki

    texts = ["", "\n", "あ", "12", "１２", "１２３４", "!?", "！？！", "!!!!", "Hello World", "Mr. Smith", "E-mail me",
             "葛\U000e0100城", "\ufe0e先", "あ\n\ufe0fい", "\x01ab\x02c", "\x01", "x\x01yz", "こんにちは。", "「はい。」",
             "まんが作りのモヤモヤをズバッと解決するこのコーナー！", "そうか。。。わかった．", "えっ。\nほんと。」", "。", "。」\u3000",
             "ありがとうございます", "この本はとても面白い", "ちょっと待って（笑）", "東京へ行く、大阪へ行く。"]
    texts += [_rand_text(rng) for _ in range(120)]
    out: dict = {"cells": [], "columns": [], "phrases": [], "phrase_columns": [], "without_periods": [], "mark_tcy": [],
                 "mono_runs": [], "char_styles": [], "weight_level": [], "bold_px": [], "normalize": [], "has_glyph": [],
                 "face_font": [], "ruby_spans": [], "emphasis_cells": []}
    for text in texts:
        for tcy, latin in ((True, False), (False, False), (True, True), (False, True)):
            out["cells"].append([text, tcy, latin, tategaki.cells(text, tcy, latin)])
        tcy, latin, per = rng.random() < 0.7, rng.random() < 0.5, rng.choice([1, 2, 3, 4, 5, 7, 9])
        cols = tategaki.columns_of(text, per, tcy, latin)
        out["columns"].append([text, per, tcy, latin, cols])
        out["phrases"].append([text, tategaki.phrases(text)])
        out["phrase_columns"].append([text, per, tcy, latin, tategaki.phrase_columns(
            text, per, lambda part: len(tategaki.cells(part, tcy, latin)))])
        out["without_periods"].append([text, tategaki.without_periods(text)])
        runs = [[_substring(rng, text), rng.choice([{"tcy": True}, {"scale": 1.4}, {"tcy": 1, "bold": True}, {}])]
                for _ in range(rng.randrange(0, 4))]
        out["mark_tcy"].append([text, runs, tategaki.mark_tcy(text, runs)])
        base = rng.choice([None, {"bold": 1}, {"bold": 2, "scale": 0.8}])
        out["char_styles"].append([text, runs, base, tategaki.char_styles(text, runs, base)])
        ruby = [[_substring(rng, text), rng.choice(["あ", "あい", "かんじ", "とうきょう", "x", ""])] for _ in range(rng.randrange(0, 4))]
        ruby += [["", "a"], ["a"]] if rng.random() < 0.2 else []
        out["mono_runs"].append([ruby, tategaki.mono_runs(ruby)])
        out["ruby_spans"].append([text, per, tcy, latin, ruby, [list(s) for s in tategaki._ruby_spans(cols, ruby)]])
        marks = [_substring(rng, text) for _ in range(rng.randrange(0, 4))]
        out["emphasis_cells"].append([text, per, tcy, latin, marks, sorted(list(c) for c in tategaki.emphasis_cells(cols, marks))])
    for value in (None, True, False, 0, 1, 2, 3, -1, 1.7, 2.9, -0.5, "bold", "heavy", "normal", "x", "", [1], {"a": 1}):
        out["weight_level"].append([value, tategaki.weight_level(value)])
    for em in (4, 8, 16, 22, 23, 45, 46, 67, 68, 100, 135):
        for level in (0, 1, 2, True, "heavy", None):
            out["bold_px"].append([em, level, tategaki.bold_px(em, level)])
    look = "―～〜−‐∼—－-あ漢A"
    for key in TEXT_FONTS:
        face = fonts.face(key)
        out["normalize"].append([key, look, face.normalize(look)])
        for char in "あ漢A―～〜−‐∼—－-ー…‥。、「」﹁﹂︵︶ゕゖヵヶ \u3000\ufe0f\U000e0100\uffff😀ß":
            out["face_font"].append([key, char, Path(face.font(20, char).path).name])
    names = {str(fonts.GOTHIC): "gothic", str(fonts.MINCHO): "mincho", str(fonts.MARU): "maru", str(fonts.HAND): "hand",
             str(fonts.SFX): "sfx", str(fonts.SFX_POP): "sfx_pop"}
    for path, name in names.items():
        for char in "あ漢Aろ―～〜−‐∼—－-﹁﹂︵︶ \u3000\ufe0f\uffff😀":
            out["has_glyph"].append([name, char, fonts.has_glyph(path, char)])
    return out


def text_glyphs(rng) -> list:
    """tategaki.glyph, tcy_glyph, latin_glyph and draw_mark: each cell's picture."""
    from PIL import Image

    from genko import fonts, tategaki

    out = []
    chars = list("あ漢A。、，．ぁっゃゕヵ「」『』（）()【】［］〈〉《》ー―〜～…‥：；=−‐\n ") + ["葛\U000e0100", "A\ufe0f"]
    for key in TEXT_FONTS:
        face = fonts.face(key)
        for char in chars:
            em, bold = rng.choice([9, 12, 23, 40]), rng.choice([0, 0, 1, 3])
            fill = rng.choice([(10, 10, 10), (200, 30, 30)])
            image = tategaki.glyph(char, face.font(em, char), em, fill, bold)
            out.append({"kind": "glyph", "font": key, "text": char, "em": em, "fill": list(fill), "bold": bold, "png": _text_png(image)})
        for cell in ("12", "100", "!?", "OK", "!!!"):
            em, bold = rng.choice([9, 16, 31]), rng.choice([0, 2])
            image = tategaki.tcy_glyph(cell, face.font(em, "0"), em, (10, 10, 10), bold)
            out.append({"kind": "tcy", "font": key, "text": cell, "em": em, "fill": [10, 10, 10], "bold": bold, "png": _text_png(image)})
        for word in ("Hello", "Mr. Smith", "NASA", "iiii"):
            em, bold = rng.choice([9, 16, 31]), rng.choice([0, 2])
            image = tategaki.latin_glyph(word, face.font(em, "A"), em, (10, 10, 10), bold)
            out.append({"kind": "latin", "font": key, "text": word, "em": em, "fill": [10, 10, 10], "bold": bold, "png": _text_png(image)})
    for _ in range(24):
        image = Image.new("RGBA", (40, 40), (0, 0, 0, 0))
        cx, cy, size = rng.uniform(5, 35), rng.uniform(5, 35), rng.uniform(1, 20)
        kind, vertical = rng.choice(["sesame", "dot"]), rng.random() < 0.5
        tategaki.draw_mark(image, (cx, cy), size, kind, (30, 80, 200), vertical)
        out.append({"kind": "mark", "centre": [bits(cx), bits(cy)], "size": bits(size), "mark": kind, "vertical": vertical,
                    "png": _text_png(image)})
    return out


def _chosen_lines() -> list:
    """Lines chosen to reach every style key that changes the letters, and every balloon kind."""
    root = ROOT / "src" / "genko" / "fonts"
    long = "まんが作りのモヤモヤをズバッと解決するこのコーナー！今日はいい天気ですね。でも明日は雨かもしれない。"
    lines = []

    def add(text, style=None, **kw):
        line = {"text": text, "style": dict(style or {})}
        line.update(kw)
        lines.append(line)

    for wrap in ("horizontal", "vertical"):
        for key in TEXT_FONTS:
            add("こんにちは、世界！漢字とカナ。ABC 123", {"font": key}, wrap=wrap)
        add(long, wrap=wrap)
        add(long, {"size_mm": 3.5}, wrap=wrap)
        add(long, {"size_mm": 6.2, "tracking": 0.15, "leading": 0.8}, wrap=wrap)
        add(long, {"tracking": -0.1, "leading": 0.0}, wrap=wrap)
        for align in ("top", "center", "bottom", "justify", "left", "right"):
            add("一行目\n二行目は長いです\n三", {"align": align}, wrap=wrap)
        add("2024年12月の!?と！？と!!!!", {"tcy": True}, wrap=wrap)
        add("2024年12月の!?と！？と!!!!", {"tcy": False}, wrap=wrap)
        add("Hello World と Mr. Smith の E-mail", {"latin": "rotate"}, wrap=wrap)
        add("Hello World と Mr. Smith の E-mail", {"latin": "upright"}, wrap=wrap)
        add("東京と大阪の先生", wrap=wrap, ruby_runs=[["東京", "とうきょう"], ["大阪", "おおさか"], ["先生", "せんせい"]])
        add("東京と大阪の先生", {"mono_ruby": True, "ruby_scale": 0.35}, wrap=wrap,
            ruby_runs=[["東京", "とうきょう"], ["大阪", "おおさか"], ["先生", "せんせい"]])
        add("東京と大阪", {"ruby_scale": 0.8}, wrap=wrap, ruby_runs=[["東京", "とう\nきょう"], ["ない", "x"], ["", "a"], ["大阪", ""]])
        add("ここが大事なところ", wrap=wrap, emphasis_runs=["大事", "ところ"])
        add("ここが大事なところ", {"emphasis_mark": "dot"}, wrap=wrap, emphasis_runs=["大事", "ところ"])
        add("大事なところと東京", {"emphasis_mark": "dot"}, wrap=wrap, emphasis_runs=["大事"], ruby_runs=[["大事", "だいじ"]])
        add("小さく大きく太く赤く縦中横12", wrap=wrap,
            style_runs=[["小さく", {"scale": 0.7}], ["大きく", {"scale": 1.8}], ["太く", {"bold": 2}], ["赤く", {"rgb": [210, 30, 30]}],
                        ["12", {"tcy": True}], ["縦中", {"weight": "heavy", "bold": True}]])
        add("太字の台詞", {"bold": True}, wrap=wrap)
        add("極太の台詞", {"weight": "heavy"}, wrap=wrap)
        add("標準の台詞", {"weight": "normal", "bold": True}, wrap=wrap)
        add("斜体の台詞", {"italic": True}, wrap=wrap)
        add("フチ付き", {"outline_mm": 0.5}, wrap=wrap)
        add("赤いフチ", {"outline_mm": 0.8, "outline_rgb": [255, 0, 0], "rgb": [255, 255, 255]}, wrap=wrap)
        add("色の台詞", {"rgb": [30, 80, 200]}, wrap=wrap)
        add("長体の台詞です", {"scale_x": 0.6}, wrap=wrap)
        add("平体の台詞です", {"scale_x": 1.6}, wrap=wrap)
        add("傾いた台詞", {"skew_deg": 15}, wrap=wrap)
        add("逆に傾く", {"skew_deg": -25}, wrap=wrap)
        add("弓なりの台詞です", {"arc": 0.6}, wrap=wrap)
        add("逆の弓なり", {"arc": -1.0}, wrap=wrap)
        add("グラデーション", {"gradient": {"rgb_from": [20, 20, 200], "rgb_to": [230, 40, 40]}}, wrap=wrap)
        add("斜めのグラデ", {"gradient": {"rgb_from": [0, 0, 0], "rgb_to": [250, 250, 0], "angle": 30}}, wrap=wrap)
        add("横のグラデ", {"gradient": {"angle": 0}}, wrap=wrap)
        add("ゆがみ", {"warp": [[0.1, 0.0], [0.9, 0.1], [1.0, 1.0], [0.0, 0.85]]}, wrap=wrap)
        add("遠近の文字", {"warp": [[0.0, 0.0], [1.0, 0.2], [1.0, 0.8], [0.0, 1.0]]}, wrap=wrap)
        add("絵の文字", {"fill_png": _text_picture(), "size_mm": 8}, wrap=wrap)
        add("読めない絵", {"fill_png": "@@not base64@@"}, wrap=wrap)
        add("こんにちは。元気ですか。", wrap=wrap)
        add("こんにちは。元気ですか。", {"periods": True}, wrap=wrap)
        add("「そうか。」そうだ．。", wrap=wrap)
        add("ぁぃぅぇぉっゃゅょゎ、。！？ーーー「（」）", wrap=wrap)
        add("葛\U000e0100城と辻\ufe00堂とA\ufe0fと\ufe0e", wrap=wrap)
        add("半角123と全角１２３と12345", wrap=wrap)
        add("!?!!？？！!?", wrap=wrap)
        add("約物「（括弧）」、『二重』。", {"yakumono": False}, wrap=wrap)
        add("約物「（括弧）」、『二重』。", wrap=wrap)
        add("", wrap=wrap)
        add(" ", wrap=wrap)
        add("\n", wrap=wrap)
        add("。", wrap=wrap)
        add("ー", wrap=wrap)
        add("あ" * 60, wrap=wrap)
        add(long * 2, {"size_mm": 2.5}, wrap=wrap, w_mm=60, h_mm=50)
        add("話す人", wrap=wrap, speaker="主人公")
        add("ずらした文字", {"text_dx_mm": 3.0, "text_dy_mm": -2.0}, wrap=wrap, speaker="誰か")
        add("OpenType", {"features": ["jp78"]}, wrap=wrap)
        add("ファイルの字", {"font": str(root / "ZenMaruGothic-Bold.ttf")}, wrap=wrap)
        add("無い字体", {"font": str(root / "missing.ttf")}, wrap=wrap)
        add("手描きでない", {"hand": False}, wrap=wrap)
        add("揺れる線", {"wobble": 0.5}, wrap=wrap)
        for kind in TEXT_KINDS:
            add("フキダシの中の台詞です！", wrap=wrap, balloon=kind)
        add("ドドドド", {"spike_depth": 0.5}, wrap=wrap, balloon="shout")
        add("ドドドドド", wrap=wrap, balloon="sfx")
        add("ドン\nガン", {"font": "sfx_pop"}, wrap=wrap, balloon="sfx")
        add("文字だけ\n二行", wrap=wrap, balloon="none")
        add("知らない形", wrap=wrap, balloon="mystery")
    for path in ([[0, 10], [40, 10]], [[0, 18], [15, 2], [30, 18], [45, 2]], [[5, 5]], [[0, 0], [0, 0], [30, 20]]):
        for align in ("center", "left", "right"):
            add("パスに沿う文字 OK!", {"text_path": path, "align": align}, w_mm=45, h_mm=20)
    add("太いパスの字", {"text_path": [[0, 15], [40, 5]], "weight": "bold", "scale_x": 0.8, "outline_mm": 0.4,
                         "gradient": {"rgb_to": [0, 120, 255]}, "tracking": 0.1, "size_mm": 6}, w_mm=45, h_mm=20)
    add("効果音のパス", {"text_path": [[0, 15], [40, 5]]}, balloon="sfx", w_mm=45, h_mm=20)
    return lines


def _rand_line(rng) -> dict:
    text = _rand_text(rng, rng.choice([4, 8, 14, 22]))
    style: dict = {}

    def maybe(p, key, value):
        if rng.random() < p:
            style[key] = value

    maybe(0.5, "font", rng.choice(TEXT_FONTS + (None,)))
    maybe(0.3, "size_mm", round(rng.uniform(2.5, 8.5), 2))
    maybe(0.2, "tracking", rng.choice([0.0, 0.05, -0.05, 0.12, 0.25]))
    maybe(0.2, "leading", rng.choice([0.0, 0.2, 0.4, 0.9]))
    maybe(0.3, "align", rng.choice(["top", "center", "bottom", "justify", "left", "right"]))
    maybe(0.2, "tcy", rng.random() < 0.5)
    maybe(0.2, "latin", rng.choice(["rotate", "upright"]))
    maybe(0.15, "ruby_scale", rng.choice([0.3, 0.5, 0.7, 0.9, 0.2]))
    maybe(0.15, "mono_ruby", True)
    maybe(0.2, "emphasis_mark", rng.choice(["sesame", "dot", "star"]))
    maybe(0.15, "bold", rng.choice([True, False, 2]))
    maybe(0.15, "weight", rng.choice([None, "normal", "bold", "heavy"]))
    maybe(0.1, "italic", True)
    maybe(0.15, "outline_mm", rng.choice([0.3, 0.6, 1.2]))
    maybe(0.1, "outline_rgb", rng.choice([[255, 255, 255], [0, 0, 0], [255, 0, 0]]))
    maybe(0.15, "rgb", rng.choice([[200, 30, 30], [0, 0, 0], [30, 80, 200], [255, 255, 255]]))
    maybe(0.1, "scale_x", rng.choice([0.5, 0.7, 1.3, 2.0]))
    maybe(0.1, "skew_deg", rng.choice([8, -12, 30, 70]))
    maybe(0.1, "arc", rng.choice([0.3, -0.5, 1.0]))
    maybe(0.08, "gradient", {"rgb_from": [rng.randrange(256) for _ in range(3)], "rgb_to": [rng.randrange(256) for _ in range(3)],
                             "angle": rng.choice([90, 0, 45, 135, -30, 200])})
    maybe(0.06, "warp", [[round(rng.uniform(-0.15, 0.25), 3), round(rng.uniform(-0.15, 0.25), 3)],
                         [round(rng.uniform(0.75, 1.15), 3), round(rng.uniform(-0.15, 0.25), 3)],
                         [round(rng.uniform(0.75, 1.15), 3), round(rng.uniform(0.75, 1.15), 3)],
                         [round(rng.uniform(-0.15, 0.25), 3), round(rng.uniform(0.75, 1.15), 3)]])
    maybe(0.15, "periods", True)
    maybe(0.1, "yakumono", False)
    maybe(0.1, "hand", False)
    maybe(0.05, "wobble", 0.4)
    maybe(0.1, "spike_depth", round(rng.uniform(0.05, 0.6), 2))
    maybe(0.05, "text_dx_mm", round(rng.uniform(-3, 3), 2))
    maybe(0.05, "text_dy_mm", round(rng.uniform(-3, 3), 2))
    maybe(0.03, "features", ["jp90"])
    maybe(0.04, "fill_png", _text_picture())
    w, h = round(rng.uniform(14, 60), 1), round(rng.uniform(10, 50), 1)
    if rng.random() < 0.06:
        style["text_path"] = [[round(rng.uniform(0, w), 1), round(rng.uniform(0, h), 1)] for _ in range(rng.randrange(1, 5))]
    line = {"text": text, "style": style, "w_mm": w, "h_mm": h, "x_mm": round(rng.uniform(2, 9), 1), "y_mm": round(rng.uniform(4, 9), 1),
            "wrap": rng.choice(["horizontal", "vertical"]),
            "balloon": rng.choice(["speech"] * 6 + list(TEXT_KINDS) + [""])}
    if rng.random() < 0.3:
        line["ruby_runs"] = [[_substring(rng, text), "".join(rng.choice(_HIRA) for _ in range(rng.randrange(1, 6)))]
                             for _ in range(rng.randrange(1, 4))]
    if rng.random() < 0.25:
        line["emphasis_runs"] = [_substring(rng, text) for _ in range(rng.randrange(1, 3))]
    if rng.random() < 0.25:
        line["style_runs"] = [[_substring(rng, text), rng.choice([{"scale": rng.choice([0.6, 1.4, 2.2])}, {"bold": rng.choice([True, 2])},
                                                                   {"rgb": [rng.randrange(256) for _ in range(3)]}, {"tcy": True},
                                                                   {"weight": "heavy"}])]
                              for _ in range(rng.randrange(1, 3))]
    if rng.random() < 0.15:
        line["speaker"] = rng.choice(["主人公", "A", "名無し"])
    return line


def text_cases(out: str, seed: int, count: int) -> None:
    """The letters of lines as balloons._paint_text draws them (text_layout's picture, em and corner on the way), the
    pure functions of tategaki, and each cell's glyph: what the C++ text module must match byte for byte."""
    from PIL import Image

    from genko import balloons
    from genko.migrate import _line

    rng = random.Random(seed)
    units = text_units(rng)
    glyphs = text_glyphs(rng)
    root = ROOT / "src" / "genko" / "fonts"
    jobs = []
    for n, line in enumerate(_chosen_lines()):
        for dpi in ((150, 350) if n % 3 else (72, 200, 600)):
            jobs.append((f"chosen-{n}-{dpi}", line, dpi, None, True))
    for n in range(count):
        line = _rand_line(rng)
        font_path = str(root / rng.choice(["ZenOldMincho-SemiBold.ttf", "DelaGothicOne-Regular.ttf"])) if rng.random() < 0.08 else None
        jobs.append((f"random-{n}", line, rng.choice([72, 96, 150, 150, 200, 300, 300, 350, 400, 600]), font_path, rng.random() < 0.7))
    cases = []
    for name, data, dpi, font_path, show_speaker in jobs:
        data = {"id": name, "page_index": 0, "x_mm": 6.0, "y_mm": 8.0, **data}
        line = _line(data)
        size = (balloons.px((line.x_mm or 0) + (line.w_mm or 40) + 14, dpi), balloons.px((line.y_mm or 0) + (line.h_mm or 20) + 14, dpi))
        captured = []
        layout = balloons.text_layout

        def spy(*args, **kwargs):
            result = layout(*args, **kwargs)
            captured.append(result)
            return result

        case = {"id": name, "line": data, "dpi": dpi, "font_path": font_path, "show_speaker": show_speaker, "canvas": list(size)}
        balloons.text_layout = spy
        try:
            canvas = Image.new("RGB", size, (255, 255, 255))
            balloons._paint_text(canvas, line, dpi, show_speaker, font_path)
            case["painted"] = _text_png(canvas)
        except Exception as e:  # (Python's own error: the C++ build raises the same)
            case["error"] = [type(e).__name__, str(e.args[0]) if e.args else str(e)]
        finally:
            balloons.text_layout = layout
        if captured:
            image, em, corner = captured[0]
            case["layout"] = {"mode": image.mode, "size": list(image.size), "png": _text_png(image), "em": int(em),
                              "corner": [bits(float(corner[0])), bits(float(corner[1]))]}
        cases.append(case)
    Path(out).write_text(dumps({"units": units, "glyphs": glyphs, "cases": cases}), encoding="utf-8")


# --- balloons (M4: the shapes, tails and joined balloons of balloons.py, as draw_lines draws them) ----------------------

TAIL_KINDS = ("wedge", "straight", "zigzag", "fade", "bubbles")


def _balloon_picture() -> str:
    """A small picture with see-through parts, for a picture balloon (style.picture)."""
    from PIL import Image, ImageDraw

    im = Image.new("RGBA", (36, 24), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im)
    draw.ellipse((1, 1, 34, 22), fill=(250, 245, 210, 255), outline=(40, 30, 20, 255), width=2)
    draw.polygon([(6, 18), (2, 23), (12, 20)], fill=(40, 30, 20, 200))
    return _text_png(im)


def _rand_point(rng, x, y, w, h, spread=18.0) -> list:
    return [round(rng.uniform(x - spread, x + w + spread), 1), round(rng.uniform(y - spread, y + h + spread), 1)]


def _rand_tail(rng, x, y, w, h) -> dict:
    tail = {"to": _rand_point(rng, x, y, w, h)}
    r = rng.random()
    if r < 0.25:
        tail["via"] = _rand_point(rng, x, y, w, h, 8.0)
    elif r < 0.4:
        tail["vias"] = [_rand_point(rng, x, y, w, h, 10.0) for _ in range(rng.randrange(1, 4))]
    if rng.random() < 0.7:
        tail["kind"] = rng.choice(TAIL_KINDS + ("", "unknown"))
    if rng.random() < 0.25:
        tail["width_mm"] = rng.choice([0.8, 2.5, 4.0])
    return tail


def rand_balloon_line(rng, ident: str, area_w: float, area_h: float, frame_ids=(), small_text=False) -> dict:
    """A line in a balloon (as project.json keeps it): every shape, tails of every kind, the balloon's style keys."""
    w, h = round(rng.uniform(9, min(42, area_w * 0.6)), 1), round(rng.uniform(7, min(32, area_h * 0.5)), 1)
    x, y = round(rng.uniform(-4, area_w - w + 4), 1), round(rng.uniform(-4, area_h - h + 4), 1)
    kind = rng.choice(["speech"] * 4 + list(TEXT_KINDS) + ["", "mystery"])
    style: dict = {}

    def maybe(p, key, value):
        if rng.random() < p:
            style[key] = value

    maybe(0.25, "border_mm", rng.choice([0.1, 0.2, 0.5, 0.8, 1.4, None]))
    maybe(0.1, "fill", "none")
    maybe(0.12, "fill_rgb", [rng.randrange(256) for _ in range(3)])
    maybe(0.1, "fill_opacity", rng.choice([0.0, 0.35, 0.8, 1.0, -0.5]))
    maybe(0.12, "line_rgb", [rng.randrange(256) for _ in range(3)])
    maybe(0.1, "double", True)
    maybe(0.15, "hand", False)
    maybe(0.1, "wobble", rng.choice([0.3, 0.8, 1.5]))
    maybe(0.15, "spikes", rng.choice([5, 9, 16, 31]))
    maybe(0.15, "spike_depth", rng.choice([0.03, 0.12, 0.3, 0.55, 0.7]))
    maybe(0.12, "spike_jitter", rng.choice([0.2, 0.6, 1.0, 1.5]))
    maybe(0.12, "bumps", rng.choice([4, 9, 15, 0.5]))
    maybe(0.08, "rotate_deg", rng.choice([12.5, -30, 90, 180, 0.005, -0.5]))
    maybe(0.08, "cuts", [{"points": [[round(rng.uniform(-2, w + 2), 1), round(rng.uniform(-2, h + 2), 1)]
                                     for _ in range(rng.randrange(1, 4))], "width_mm": rng.choice([0.6, 2.0, 3.5])}
                         for _ in range(rng.randrange(1, 3))])
    maybe(0.06, "text_dx_mm", round(rng.uniform(-2, 2), 1))
    maybe(0.3 if small_text else 0.1, "size_mm", rng.choice([2.0, 2.5, 3.2]))
    if kind == "picture" and rng.random() < 0.8:
        style["picture"] = _balloon_picture() if rng.random() < 0.85 else "@@not a picture@@"
    text = _rand_text(rng, 5 if small_text else rng.choice([4, 8, 12]))
    line = {"id": ident, "page_index": 1, "text": text, "x_mm": x, "y_mm": y, "w_mm": w, "h_mm": h, "balloon": kind,
            "style": style, "wrap": rng.choice(["horizontal", "vertical"])}
    r = rng.random()
    if r < 0.55:
        line["tails"] = [_rand_tail(rng, x, y, w, h) for _ in range(rng.choice([1, 1, 1, 2, 3]))]
    elif r < 0.65:
        line["tail"] = _rand_point(rng, x, y, w, h)
    if rng.random() < 0.1:
        n = rng.randrange(3, 9)
        cx, cy = x + w / 2, y + h / 2
        line["path"] = [[round(cx + w / 2 * math.cos(math.tau * k / n) * rng.uniform(0.8, 1.15), 2),
                         round(cy + h / 2 * math.sin(math.tau * k / n) * rng.uniform(0.8, 1.15), 2)] for k in range(n)]
        style["path_curve"] = rng.random() < 0.5
    if rng.random() < 0.2:
        line["speaker"] = rng.choice(["主人公", "A", "名無し"])
    if frame_ids and rng.random() < 0.6:
        line["frame_id"] = rng.choice(list(frame_ids) + ["no-such-frame"])
    return line


def _chosen_balloons() -> list:
    """Groups of lines chosen to reach every shape, tail kind and balloon style key: (name, lines, canvas mm, panels)."""
    picture = _balloon_picture()
    cases = []
    counter = [0]

    def ln(text, x, y, w, h, balloon="speech", style=None, **kw) -> dict:
        counter[0] += 1
        data = {"id": "%012x" % (0xb0b0000000 + counter[0] * 7919), "page_index": 1, "text": text, "x_mm": x, "y_mm": y,
                "w_mm": w, "h_mm": h, "balloon": balloon, "style": dict(style or {})}
        data.update(kw)
        return data

    def add(name, lines, canvas=(80.0, 70.0), panels=None):
        cases.append((name, lines, canvas, panels))

    kinds = list(TEXT_KINDS) + ["mystery", ""]
    for kind in kinds:  # every shape, with a tail where it has one
        add(f"shape-{kind or 'empty'}", [ln("フキダシの形", 15, 12, 40, 26, kind, tails=[{"to": [62, 58]}])])
        add(f"shape-{kind or 'empty'}-plain", [ln("形だけ", 10, 10, 36, 22, kind, {"hand": False, "border_mm": 0.6})])
        add(f"shape-{kind or 'empty'}-wobble", [ln("揺れる", 12, 14, 38, 24, kind, {"wobble": 0.9}, tails=[{"to": [8, 60]}])])
    for tail_kind in TAIL_KINDS + ("unknown",):
        for kind in ("speech", "box", "rounded", "shout", "electric", "cloud", "whisper", "narration"):
            add(f"tail-{tail_kind}-{kind}", [ln("しっぽ", 20, 10, 34, 22, kind, tails=[
                {"to": [70, 60], "kind": tail_kind},
                {"to": [5, 52], "kind": tail_kind, "via": [12, 42]},
                {"to": [44, 66], "kind": tail_kind, "width_mm": 3.0}])])
    add("tail-vias", [ln("折れ線", 25, 8, 30, 20, "speech", tails=[{"to": [70, 62], "vias": [[50, 40], [40, 55]]},
                                                                  {"to": [4, 60], "vias": [[10, 30]], "kind": "fade"}])])
    add("tail-vias-box", [ln("折れ線", 25, 8, 30, 20, "box", tails=[{"to": [70, 62], "vias": [[60, 20], [64, 50], [50, 60]]}])])
    add("tail-old", [ln("昔のしっぽ", 20, 10, 36, 22, "speech", tail=[60, 60])])
    add("tail-old-and-new", [ln("両方", 20, 10, 36, 22, "speech", tail=[60, 60], tails=[{"to": [5, 60]}])])
    add("tail-empty-to", [ln("先なし", 20, 10, 36, 22, "speech", tails=[{"to": []}, {"to": [10, 60], "kind": "zigzag"}])])
    add("tail-outside", [ln("外へ", 30, 20, 30, 20, "speech", tails=[{"to": [120, -30]}, {"to": [-40, 95]}])])
    add("tail-inside", [ln("中へ", 20, 20, 40, 30, "speech", tails=[{"to": [40, 35]}])])
    add("tail-fade-many", [ln("消える", 20, 15, 34, 20, "speech", tails=[{"to": [70, 65], "kind": "fade"}, {"to": [5, 65], "kind": "fade"}])])
    # a thought's bubbles: toward its speaker, or down and away inside its panel
    panels = {"left": (0.0, 0.0, 40.0, 70.0), "right": (40.0, 0.0, 40.0, 70.0), "tiny": (30.0, 30.0, 5.0, 5.0)}
    add("thought-no-panel", [ln("考え", 25, 10, 30, 20, "thought")])
    for frame, x, y in (("left", 8, 8), ("left", 8, 45), ("right", 46, 8), ("right", 47, 50), ("tiny", 20, 20), ("nowhere", 10, 10)):
        add(f"thought-panel-{frame}-{x}-{y}", [ln("考え中", x, y, 24, 14, "thought", frame_id=frame)], panels=panels)
    add("thought-tail", [ln("考え", 25, 10, 30, 20, "thought", tails=[{"to": [70, 65]}])], panels=panels)
    add("bubbles-tail", [ln("泡", 25, 10, 30, 20, "speech", tails=[{"to": [70, 65], "kind": "bubbles"}])])
    # joined balloons: one outline
    add("group-speech", [ln("一つ目", 8, 8, 34, 22, "speech", {"group": "a"}, tails=[{"to": [10, 62]}]),
                         ln("二つ目", 30, 22, 36, 24, "speech", {"group": "a"})])
    add("group-mixed", [ln("雲と", 8, 8, 34, 22, "cloud", {"group": "b", "border_mm": 0.8}),
                        ln("箱", 34, 26, 30, 22, "box", {"group": "b"}, tails=[{"to": [70, 66]}]),
                        ln("考え", 10, 40, 24, 16, "thought", {"group": "b"})])
    add("group-keys", [ln("1", 5, 5, 24, 16, "speech", {"group": 1}), ln("1.0", 20, 15, 24, 16, "speech", {"group": 1.0}),
                       ln("True", 35, 25, 24, 16, "speech", {"group": True}), ln("'1'", 45, 45, 24, 16, "speech", {"group": "1"})])
    add("group-first-sfx", [ln("ドン", 8, 8, 30, 20, "sfx", {"group": "s"}), ln("台詞", 30, 30, 30, 20, "speech", {"group": "s"})])
    add("group-first-none", [ln("文字", 8, 8, 30, 20, "none", {"group": "n"}), ln("台詞", 30, 30, 30, 20, "speech", {"group": "n"})])
    add("group-picture-second", [ln("台詞", 8, 8, 30, 20, "speech", {"group": "p"}),
                                 ln("絵", 34, 30, 34, 24, "picture", {"group": "p", "picture": picture})])
    add("group-double", [ln("二重", 10, 10, 30, 20, "speech", {"group": "d", "double": True, "border_mm": 0.5}),
                         ln("線", 30, 28, 34, 22, "rounded", {"group": "d"})])
    # turned balloons
    for deg in (15, -40, 90, 180, 0.005):
        add(f"turned-{deg}", [ln("回る台詞", 20, 15, 36, 22, "speech", {"rotate_deg": deg}, speaker="主人公",
                                 tails=[{"to": [60, 60], "via": [55, 45]}, {"to": [10, 60], "vias": [[12, 50]]}])])
    add("turned-group", [ln("回る", 10, 10, 30, 20, "box", {"rotate_deg": 20, "group": "t"}),
                         ln("組", 32, 28, 30, 20, "speech", {"rotate_deg": -5, "group": "t"}, tails=[{"to": [70, 66]}])])
    add("turned-path", [ln("手描き", 20, 20, 30, 20, "speech", {"rotate_deg": 30},
                           path=[[20, 20], [52, 18], [55, 42], [18, 40]])])
    add("turned-picture", [ln("絵", 20, 20, 36, 24, "picture", {"rotate_deg": 25, "picture": picture})])
    add("turned-thought", [ln("考え", 20, 20, 30, 20, "thought", {"rotate_deg": 10}, frame_id="left")], panels=panels)
    # picture balloons
    add("picture", [ln("絵の台詞", 15, 15, 40, 26, "picture", {"picture": picture}, speaker="A")])
    add("picture-bad", [ln("読めない絵", 15, 15, 40, 26, "picture", {"picture": "@@not a picture@@"})])
    add("picture-none", [ln("絵なし", 15, 15, 40, 26, "picture")])
    add("picture-on-speech", [ln("絵の鍵", 15, 15, 40, 26, "speech", {"picture": picture})])
    # the balloon eraser (cuts) and hand-drawn outlines
    add("cuts", [ln("消しゴム", 15, 15, 40, 26, "speech", {"cuts": [{"points": [[0, 0], [20, 5], [40, 0]], "width_mm": 3.0},
                                                                      {"points": [[38, 24]]}, {"points": [[5, 20], [10, 26]], "width_mm": 0.4}]},
                    tails=[{"to": [70, 65]}])])
    add("cuts-group", [ln("一", 8, 8, 30, 20, "box", {"group": "c"}), ln("二", 30, 25, 30, 20, "box", {"group": "c", "cuts": [{"points": [[0, 0], [30, 20]]}]})])
    add("cuts-empty", [ln("空", 15, 15, 40, 26, "speech", {"cuts": [{"points": []}]})])
    add("path", [ln("手描き", 20, 20, 30, 20, "speech", path=[[20, 20], [52, 18], [55, 42], [30, 50], [18, 40]], tails=[{"to": [70, 66]}])])
    add("path-curve", [ln("曲線", 20, 20, 30, 20, "speech", {"path_curve": True}, path=[[20, 20], [52, 18], [55, 42], [30, 50], [18, 40]])])
    add("path-curve-two", [ln("二点", 20, 20, 30, 20, "speech", {"path_curve": True}, path=[[20, 20], [52, 48]])])
    # the balloon's own style keys
    for key, values in (("border_mm", [0.05, 0.2, 0.6, 1.2, None]), ("fill", ["none", "white", None]),
                        ("fill_rgb", [[255, 240, 200], [30, 30, 30]]), ("fill_opacity", [0.0, 0.4, 1.0, -1, 2]),
                        ("line_rgb", [[200, 20, 20], [0, 0, 255]]), ("double", [True]), ("hand", [False, True]),
                        ("spikes", [6, 24, 2.7]), ("spike_depth", [0.01, 0.4, 0.9]), ("spike_jitter", [0.5, 1.0, 3.0]),
                        ("bumps", [3, 10, 25, 0.4]), ("wobble", [0.2, 2.0, -1])):
        for value in values:
            for kind in ("speech", "shout", "cloud", "electric", "box"):
                add(f"style-{key}-{value}-{kind}", [ln("様式", 14, 12, 40, 26, kind, {key: value}, tails=[{"to": [66, 62]}])])
    for kind in ("dotted_box", "tone_box", "fancy_box", "narration"):
        for border in (0.1, 0.35, 0.9):
            add(f"frame-{kind}-{border}", [ln("飾り枠の文", 10, 10, 50, 30, kind, {"border_mm": border})])
        add(f"frame-{kind}-group", [ln("一", 5, 5, 30, 24, kind, {"group": "f"}), ln("二", 30, 25, 40, 30, kind, {"group": "f"})])
    add("tone-box-colours", [ln("トーン", 10, 10, 50, 30, "tone_box", {"line_rgb": [30, 60, 200], "fill_rgb": [250, 250, 200]})])
    # at the picture's edges, partly outside it, tiny and large
    add("edge-left-top", [ln("端", -10, -8, 30, 20, "speech", tails=[{"to": [-5, 30]}])])
    add("edge-right-bottom", [ln("端", 60, 55, 30, 20, "box", tails=[{"to": [90, 80]}])])
    add("tiny", [ln("小", 30, 30, 2, 1.5, "speech", tails=[{"to": [40, 40]}]), ln("小", 10, 10, 3, 2, "cloud")])
    add("tiny-shout", [ln("小", 30, 30, 4, 3, "shout"), ln("小", 10, 10, 4, 3, "electric")])
    add("large", [ln("大きな台詞です。とても大きい。", 2, 2, 76, 64, "rounded", tails=[{"to": [40, 69]}])])
    add("speakers", [ln("話す", 15, 15, 30, 20, "speech", speaker="主人公"), ln("無し", 40, 40, 30, 20, "none", speaker="名無し")])
    add("negative-size", [ln("負", 40, 40, -20, -10, "speech")])
    return cases


def balloon_units(rng) -> dict:
    """The shapes' geometry on chosen and random boxes, tips and seeds (floats as their bits)."""
    from genko import balloons as b

    out: dict = {"edge_point": [], "tail_polygon": [], "polyline_tail": [], "uneven": [], "electric": [], "outline": [],
                 "wobbly": [], "smooth_closed": [], "turned": [], "thought_trail": [], "tails_of": []}
    kinds = list(TEXT_KINDS) + ["mystery"]
    for n in range(400):
        x0, y0 = rng.randrange(0, 300), rng.randrange(0, 300)
        box = (x0, y0, x0 + rng.randrange(1, 400), y0 + rng.randrange(1, 300))
        tip = (rng.randrange(-100, 800), rng.randrange(-100, 700))
        kind = rng.choice(kinds)
        spread = rng.choice([4.0, 7.5, 12, 30.0, 0.5, 9.333333333333334, 500])
        out["edge_point"].append([kind, list(box), list(tip), bits(spread), pts_json(b._edge_point(kind, box, tip, spread))])
        via = rng.choice([None, (rng.randrange(-50, 700), rng.randrange(-50, 600))])
        style = rng.choice(TAIL_KINDS + ("unknown",))
        out["tail_polygon"].append([kind, list(box), list(tip), list(via) if via else None, bits(spread), style,
                                    pts_json(b._tail_polygon(kind, box, tip, via, spread, style))])
        vias = [(rng.randrange(-50, 700), rng.randrange(-50, 600)) for _ in range(rng.randrange(1, 4))]
        if n % 7 == 0:
            vias.append(vias[-1])  # (a bend repeated: a piece of no length)
        out["polyline_tail"].append([kind, list(box), list(tip), [list(v) for v in vias], bits(spread),
                                     pts_json(b._polyline_tail(kind, box, tip, vias, spread))])
        seed = rng.choice(["", "genko", "%012x" % rng.getrandbits(48)])
        fbox = tuple(v * rng.choice([1, 0.5]) for v in box)
        out["uneven"].append([[bits(v) for v in fbox], seed, pts_json(b._uneven(fbox, seed))])
        st = {k: v for k, v in (("spikes", rng.choice([None, 0, 7, 20, 3.9])), ("spike_depth", rng.choice([None, 0.01, 0.2, 0.7])))
              if v is not None or rng.random() < 0.3}
        out["electric"].append([list(box), st, pts_json(b._electric(box, st))])
        points = b._outline(kind, box)
        out["outline"].append([kind, list(box), None if points is None else pts_json(points)])
        if points:
            amount, size = rng.choice([0.2, 0.8, 1.5]), rng.choice([10, 33.5, 200])
            out["wobbly"].append([pts_json(points), bits(amount), bits(size), seed, pts_json(b._wobbly(points, amount, size, seed))])
        ring = [(rng.randrange(0, 500), rng.randrange(0, 500)) for _ in range(rng.randrange(1, 9))]
        out["smooth_closed"].append([[list(p) for p in ring], pts_json(b._smooth_closed(ring))])
        point = (rng.uniform(-50, 250), rng.uniform(-50, 250))
        centre = (rng.uniform(0, 200), rng.uniform(0, 200))
        degrees = rng.choice([0.0, 15.0, -30.5, 90.0, 180.0, rng.uniform(-360, 360)])
        out["turned"].append([pts_json([point])[0], pts_json([centre])[0], bits(degrees), pts_json([b._turned(point, centre, degrees)])[0]])
    from genko.migrate import _line

    frames = {"a": (0.0, 0.0, 60.0, 80.0), "b": (60.0, 0.0, 50.0, 40.0), "c": (60.0, 40.0, 5.0, 6.0), "d": (0, 0, 1000, 1000)}
    for n in range(120):
        data = {"id": "t%d" % n, "page_index": 1, "text": "", "x_mm": round(rng.uniform(-10, 120), 2), "y_mm": round(rng.uniform(-10, 90), 2),
                "w_mm": rng.choice([0, 10, 25.5, 40]), "h_mm": rng.choice([0, 8, 20, 33.3])}
        if rng.random() < 0.85:
            data["frame_id"] = rng.choice(list(frames) + ["zz"])
        line = _line(data)
        panels = frames if rng.random() < 0.85 else None
        out["thought_trail"].append([data, panels, pts_json([b._thought_trail(line, panels)])[0]])
    for n in range(40):
        data = {"id": "u%d" % n, "page_index": 1, "text": ""}
        if rng.random() < 0.6:
            data["tails"] = [rng.choice([{"to": [1, 2]}, {"to": []}, {"to": None}, {}, {"to": [3.5, 4], "kind": "fade"}, {"via": [1, 1]}])
                             for _ in range(rng.randrange(0, 4))]
        if rng.random() < 0.5:
            data["tail"] = [rng.randrange(0, 50), rng.uniform(0, 50)]
        out["tails_of"].append([data, b.tails_of(_line(data))])
    return out


def balloon_cases(out: str, seed: int, count: int) -> None:
    """balloons.draw_lines on chosen and random groups of lines (their pictures, or Python's error) and the shapes'
    geometry: what the C++ balloons must match byte for byte."""
    from PIL import Image

    from genko import balloons

    rng = random.Random(seed)
    units = balloon_units(rng)
    root = ROOT / "src" / "genko" / "fonts"
    jobs = []
    for n, (name, lines, canvas, panels) in enumerate(_chosen_balloons()):
        for dpi in ((150, 350) if n % 3 else (72, 200, 300)):
            mode = "RGBA" if n % 5 == 0 else "RGB"
            jobs.append((f"chosen-{name}-{dpi}", lines, canvas, panels, dpi, None, n % 2 == 0, mode))
    for n in range(count):
        canvas = (rng.choice([60.0, 80.0, 100.0]), rng.choice([50.0, 70.0, 90.0]))
        panels = None
        if rng.random() < 0.6:
            half = canvas[0] / 2
            panels = {"p1": (0.0, 0.0, half, canvas[1]), "p2": (half, 0.0, half, canvas[1] / 2), "p3": (half, canvas[1] / 2, half, canvas[1] / 2)}
        group_key = rng.choice([None, None, "g", 1, 1.0])
        lines = []
        for k in range(rng.choice([1, 1, 1, 2, 2, 3, 4])):
            data = rand_balloon_line(rng, "%012x" % rng.getrandbits(48), canvas[0], canvas[1], list(panels or {}))
            if group_key is not None and (k > 0 or rng.random() < 0.8):
                data["style"]["group"] = group_key
            lines.append(data)
        font_path = str(root / "ZenOldMincho-SemiBold.ttf") if rng.random() < 0.05 else None
        dpi = rng.choice([72, 96, 150, 150, 200, 299, 300, 350, 400])
        jobs.append((f"random-{n}", lines, canvas, panels, dpi, font_path, rng.random() < 0.6, rng.choice(["RGB", "RGB", "RGBA"])))
    from genko.migrate import _line

    cases = []
    for name, lines, canvas, panels, dpi, font_path, show_speaker, mode in jobs:
        size = (balloons.px(canvas[0], dpi), balloons.px(canvas[1], dpi))
        background = (255, 255, 255) if mode == "RGB" else (230, 240, 255, 255) if len(cases) % 2 else (0, 0, 0, 0)
        case = {"id": name, "lines": lines, "canvas": list(size), "mode": mode, "background": list(background), "dpi": dpi,
                "font_path": font_path, "show_speaker": show_speaker,
                "panels": None if panels is None else {k: list(v) for k, v in panels.items()}}
        try:
            image = Image.new(mode, size, background)
            balloons.draw_lines(image, [_line(dict(d)) for d in lines], dpi, font_path, show_speaker, panels)
            case["png"] = _text_png(image)
        except Exception as e:  # (Python's own error: the C++ build raises the same)
            case["error"] = [type(e).__name__, str(e.args[0]) if e.args else str(e)]
        cases.append(case)
    Path(out).write_text(dumps({"units": units, "cases": cases}), encoding="utf-8")


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
    p = sub.add_parser("adjust-cases")
    p.add_argument("out")
    p = sub.add_parser("colour-cases")
    p.add_argument("out")
    p.add_argument("icc")
    p = sub.add_parser("abr-cases")
    p.add_argument("out")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("brush-library")
    p.add_argument("out")
    p.add_argument("config")
    p = sub.add_parser("material-library")
    p.add_argument("steps")
    p.add_argument("out")
    p.add_argument("config")
    p = sub.add_parser("make-books")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=40)
    p = sub.add_parser("text-cases")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=400)
    p = sub.add_parser("balloon-cases")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=300)
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
    elif args.cmd == "adjust-cases":
        adjust_cases(args.out)
    elif args.cmd == "colour-cases":
        colour_cases(args.out, args.icc)
    elif args.cmd == "abr-cases":
        abr_cases(args.out, args.files)
    elif args.cmd == "brush-library":
        brush_library(args.out, args.config)
    elif args.cmd == "material-library":
        material_library(args.steps, args.out, args.config)
    elif args.cmd == "text-cases":
        text_cases(args.out, args.seed, args.count)
    elif args.cmd == "balloon-cases":
        balloon_cases(args.out, args.seed, args.count)
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
