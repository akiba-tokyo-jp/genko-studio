#!/usr/bin/env python3
"""The Python reference for the C++ contract tests of tones, effect lines and rulers (M3-B; native/tests/contract/
test_contract_tone_ops.cpp, test_contract_tone_render.cpp, test_contract_rulers.cpp): random books that carry them,
ops applied step by step with what Python made at each step, the pages drawn, and the rulers' snapping. Development
only; nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/tone_harness.py <command> ...

Commands:
  make-books OUT --seed N --count K [--render]
        K random v3 books (OUT/book-NN.genko) with tone layers (every pattern, gradients, dot shapes, offsets, patches,
        regions, scrapes), effect lines (every kind, bundles, tapers, avoid / within), rulers (every kind) and layer
        screens. --render: small pages to draw at 350 dpi, without what the C++ build does not draw yet (lines and
        balloons, nombres, 3D guides, placed pictures).
  apply JOBS
        each job {"book", "ops", "agent", "dry_run", "out", "ids_from"}: apply_ops with counting ids from ids_from; to
        out: the reply (without job_id) or {"ok": false, "error", "raised"}, and after a batch that changed the book its
        payload ("payload": project.json as save_episode writes it, each patch picture as its pixels' digest).
  sequences OUT --books DIR --seed N --count K [--ops a,b,…] [--steps S]
        K random op sequences over the books in DIR (tone, effect and ruler ops mixed with the basic ops named in --ops),
        each op made from the book as it is at that step and applied alone: OUT (JSON) holds, per sequence, the book,
        and per step the ops, agent, dry_run, ids_from, the reply and the payload.
  snaps OUT --seed N --count K
        rulers.snap, symmetry_copies, outline, perspective_grid, shape_outline, smooth_curve, horizon, directions and
        snap_to_grid for random rulers and strokes (floats as their bits, so the C++ results are compared exactly).
  fixture OUT
        the books of the fixed cases (OUT/fixture.genko, OUT/strict.genko), with ids to name in the cases.
  render JOBS
        each job {"book", "page", "dpi", "mode", "out", "kind": page | layer | bitonal, "layer", "threshold", "screen",
        "screen_dots"}: the page (or a layer alone, or black and white) as the baseline draws it, nothing left out;
        out + ".error" instead when Python cannot draw it.

Ids: genko.models.new_id gives 000000000001, 000000000002, … counting from `ids_from` (a different start for each
step), as genko::core::ScopedIdSource(counting_ids(ids_from)) does in C++.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import io
import json
import math
import random
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / "src", Path(__file__).resolve().parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import pyref_harness  # noqa: E402


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


def bits(x: float) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", float(x)))[0]


def ids_from(start: int) -> None:
    """new_id counts from `start` (000000000001 for 1)."""
    import genko.models as models

    counter = pyref_harness._Counter()
    counter.next = start
    models.uuid4 = counter


def png_b64(im) -> str:
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def picture_digest(data: bytes | None) -> str:
    """A patch picture as its pixels: "px:" + sha256 of mode|WxH|raw pixels (or "bytes:" when it is not a picture)."""
    from PIL import Image

    if data is None:
        return "missing"
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
    except Exception:
        return "bytes:" + hashlib.sha256(data).hexdigest()
    head = f"{im.mode}|{im.width}x{im.height}|".encode()
    return "px:" + hashlib.sha256(head + im.tobytes()).hexdigest()


def normalized_payload(episode) -> dict:
    """project.json as save_episode writes it (version 3, no revision), each patch picture as its pixels' digest."""
    from genko import io as gio
    from genko.assets import AssetStore

    with tempfile.TemporaryDirectory() as tmp:
        store = AssetStore(Path(tmp))
        payload = gio._payload(copy.deepcopy(episode), store)
        for page in payload["pages"]:
            for layer in page["layers"]:
                for patch in layer.get("patches") or []:
                    if "asset" in patch:
                        patch["asset"] = picture_digest(store.get_bytes(patch["asset"], ".png"))
    payload.pop("revision", None)
    return payload


def reply_of(episode, ops, agent: str, dry_run: bool) -> tuple[dict, bool]:
    """(the reply without job_id, or the error; whether the book changed). A saved batch's reply also has
    "snapshot_full": snapshot(episode, full=True) after it."""
    from genko.headless import apply_ops, snapshot
    from genko.ops import ApplyError

    try:
        reply = apply_ops(episode, ops, dry_run=dry_run, agent=agent)
    except ApplyError as exc:
        return {"ok": False, "error": str(exc), "raised": "ApplyError"}, False
    except Exception as exc:  # (Python lets these out of apply_ops: a traceback in its command line)
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "raised": type(exc).__name__}, False
    reply = dict(reply)
    reply.pop("job_id", None)
    changed = not dry_run and not (len(ops) == 1 and isinstance(ops[0], dict) and ops[0].get("op") == "undo")
    if changed:
        # (on a copy: Page.ink_strokes / name_strokes make the layer when the page has none)
        reply["snapshot_full"] = snapshot(copy.deepcopy(episode), full=True)
    return reply, changed


# --- random books --------------------------------------------------------------------------------------------------

TONE_PATTERNS = ["dot", "line", "cross", "noise", "flat", "check", "brick", "wave", "grid", "hatch", "star", "sand", "image"]
EFFECT_KINDS = ["focus", "speed", "uni_flash", "beta_flash", "white"]
RULER_KINDS = ["line", "curve", "parallel", "concentric", "radial", "perspective", "symmetry", "guide", "parallel_curve",
               "multi_curve", "radial_curve", "rect", "ellipse", "polygon"]


def tile_picture(rng: random.Random) -> str:
    from PIL import Image, ImageDraw

    size = rng.choice([(8, 8), (12, 6), (5, 9)])
    im = Image.new("RGBA", size, (255, 255, 255, 0) if rng.random() < 0.5 else (255, 255, 255, 255))
    d = ImageDraw.Draw(im)
    d.ellipse((1, 1, size[0] - 2, size[1] - 2), fill=(rng.randrange(80), rng.randrange(80), rng.randrange(80), 255))
    if rng.random() < 0.5:
        d.line((0, 0, size[0], size[1]), fill=(0, 0, 0, 200), width=1)
    return png_b64(im if rng.random() < 0.7 else im.convert("L"))


def mask_picture(rng: random.Random) -> str:
    from PIL import Image, ImageDraw

    size = rng.choice([(20, 14), (33, 40), (9, 9)])
    im = Image.new("L", size, 0)
    ImageDraw.Draw(im).ellipse((1, 2, size[0] - 3, size[1] - 1), fill=255)
    return png_b64(im)


def rand_xy(rng: random.Random, page, margin: float = 5.0) -> list:
    w, h = float(page.spec.width_mm), float(page.spec.height_mm)
    return [round(rng.uniform(-margin, w + margin), rng.choice([0, 1, 2, 3, 6])),
            round(rng.uniform(-margin, h + margin), rng.choice([0, 1, 2, 3, 6]))]


def rand_tone(rng: random.Random) -> dict:
    tone = {"pattern": rng.choice(TONE_PATTERNS)}
    if tone["pattern"] == "image":
        tone["tile_png"] = tile_picture(rng)
    if rng.random() < 0.4:
        tone["scale_mm"] = rng.choice([0.5, 1.5, 2.0, 3.0, 4.25])
    if rng.random() < 0.35:
        tone["gradient"] = {"shape": rng.choice(["linear", "radial", "linear"]), "start": rng.choice([0.0, 0.1, 0.6]),
                            "end": rng.choice([0.0, 0.5, 0.9])}
        if rng.random() < 0.6:
            tone["gradient"]["angle"] = rng.choice([0, 30, 90, 135.5, -45])
    if rng.random() < 0.3:
        tone["dot_shape"] = rng.choice(["square", "diamond", "ellipse", "round"])
    if rng.random() < 0.3:
        tone["offset_mm"] = [rng.choice([0.25, -0.5, 1.0, 0.0]), rng.choice([0.0, 0.75, -1.25])]
    if rng.random() < 0.15:
        tone["seed"] = rng.choice([3, 11, 0, 2**40 + 5])
    return tone


def rand_params(rng: random.Random, kind: str, page, frame) -> dict:
    """Settings for an effect of this kind (most of what each kind takes, sometimes)."""
    p: dict = {}
    r = frame.rect if frame is not None else page.bleed_rect_mm()
    cx, cy = float(r.x) + float(r.width) / 2, float(r.y) + float(r.height) / 2

    def maybe(key, value, share=0.4):
        if rng.random() < share:
            p[key] = value

    maybe("count", rng.choice([5, 12, 30, 60]), 0.7)
    maybe("seed", rng.choice(["s1", 7, "効果"]), 0.3)
    maybe("jitter", rng.choice([0, 0.1, 0.5, 1]))
    maybe("width_mm", rng.choice([0.3, 0.8, 1.5]))
    maybe("rgb", rng.choice([[200, 30, 30], [255, 255, 255], [15, 15, 15]]), 0.2)
    if kind in ("focus", "uni_flash", "beta_flash"):
        maybe("center", [round(cx + rng.uniform(-10, 10), 2), round(cy + rng.uniform(-10, 10), 2)])
        maybe("inner", [rng.choice([3, 8.5, 15]), rng.choice([4, 10])])
        maybe("clear", rng.choice([0.2, 0.6]), 0.2)
    if kind == "focus":
        maybe("taper", rng.choice([True, False, "in", "out", "both", "none", "True"]))
        maybe("twist", rng.choice([0, 15, -40]), 0.25)
        maybe("length_mm", rng.choice([5, 12.5]), 0.25)
        maybe("bundle", rng.choice([1, 3, 6]), 0.3)
        maybe("bundle_gap", rng.choice([0.2, 0.7]), 0.3)
        maybe("jitter_length", rng.choice([0, 0.4]), 0.25)
        maybe("jitter_position", rng.choice([0, 0.6]), 0.25)
        maybe("jitter_width", rng.choice([0, 1]), 0.25)
        if rng.random() < 0.2:
            p["inner_path"] = [[round(cx - 8, 1), round(cy - 6, 1)], [round(cx + 8, 1), round(cy - 6, 1)],
                               [round(cx + 6, 1), round(cy + 9, 1)]]
    if kind == "speed":
        maybe("angle", rng.choice([0, 30, 90, -15.5]))
        maybe("length", rng.choice([0.3, 0.7, 1]))
        maybe("curve", rng.choice([0, 4, -6]), 0.3)
        maybe("taper", rng.choice([True, False, "in", "out", "both"]))
        maybe("spacing_mm", rng.choice([2, 4.5]), 0.25)
        maybe("bundle", rng.choice([2, 4]), 0.2)
        maybe("jitter_position", rng.choice([0, 0.5]), 0.2)
        maybe("jitter_width", rng.choice([0, 0.8]), 0.2)
        if rng.random() < 0.25:
            p["path"] = [rand_xy(rng, page, -5) for _ in range(rng.choice([2, 3, 4]))]
            maybe("spread_mm", rng.choice([5, 15]))
    if kind == "uni_flash":
        maybe("length_mm", rng.choice([6, 12]))
    if kind == "beta_flash":
        maybe("spikes", rng.choice([12, 40, 70]))
        maybe("depth", rng.choice([0.2, 0.45, 0.9]))
    if rng.random() < 0.2:
        p["avoid"] = [{"ellipse": [round(cx + rng.uniform(-15, 15), 1), round(cy + rng.uniform(-15, 15), 1), rng.choice([5, 12]),
                                   rng.choice([4, 9])]}]
        if rng.random() < 0.5:
            p["avoid"].append({"path": [[cx - 20, cy + 5], [cx - 5, cy + 5], [cx - 12, cy + 20]]})
    if rng.random() < 0.15:
        p["within"] = [[cx - 30, cy - 30], [cx + 25, cy - 28], [cx + 30, cy + 30], [cx - 28, cy + 25]]
    return p


def rand_ruler(rng: random.Random, page, ruler_id: str) -> dict:
    kind = rng.choice(RULER_KINDS)
    w, h = float(page.spec.width_mm), float(page.spec.height_mm)
    need = {"line": 2, "curve": 4, "parallel": 0, "concentric": 2, "radial": 1, "perspective": rng.choice([1, 2, 3]),
            "symmetry": 2, "guide": 0, "parallel_curve": 3, "multi_curve": 3, "radial_curve": 3, "rect": 2, "ellipse": 2,
            "polygon": rng.choice([3, 4, 5])}[kind]
    points = [[round(rng.uniform(5, w - 5), 3), round(rng.uniform(5, h - 5), 3)] for _ in range(need)]
    if kind in ("rect", "ellipse"):
        points[1] = [points[0][0] + rng.choice([12.5, -20, 33]), points[0][1] + rng.choice([9, -15.25, 30])]
    ruler = {"id": ruler_id, "kind": kind, "points": points, "active": rng.random() < 0.85, "visible": True}
    if kind in ("parallel", "concentric", "rect", "ellipse"):
        ruler["angle"] = rng.choice([0.0, 30.0, 112.5, -45.0])
    if kind == "concentric":
        ruler["ratio"] = rng.choice([1.0, 0.5, 2.25])
    if kind == "guide":
        ruler["axis"] = rng.choice(["h", "v"])
        ruler["at"] = round(rng.uniform(10, min(w, h) - 10), 3)
    if kind == "symmetry":
        ruler["copies"] = rng.choice([2, 2, 3, 6])
        ruler["mirror"] = rng.random() < 0.4
    if kind == "multi_curve":
        ruler["points2"] = [[p[0] + 10, p[1] + 25] for p in points]
    if kind == "radial_curve":
        ruler["center"] = [round(w / 2, 3), round(-40.0, 3)]
    if kind == "perspective" and rng.random() < 0.5:
        ruler["grid"] = rng.choice([4, 8])
        ruler["lock_horizon"] = rng.random() < 0.5
    if rng.random() < 0.2:
        ruler["reach_mm"] = rng.choice([3.0, 25.0])
    if rng.random() < 0.15:
        leaves = page.leaf_frames()
        if leaves:
            ruler["frame_id"] = rng.choice(leaves).id
    return ruler


def make_book(rng: random.Random, dest: Path, render: bool) -> dict:
    """A random book with tones, effects, rulers and screens; returns what its pages carry (for the tests)."""
    from genko import fill as fills
    from genko import models
    from genko.io import save_episode
    from genko.models import Binding, Layer, LayerKind, LayerRole, PageSpec

    if render:
        spec = rng.choice([lambda: PageSpec.custom(70, 95, 60, 85, 3, 8, 8, 7, 6),
                           lambda: PageSpec.custom(60, 60, 54, 54, 2, 5, 5, 4, 4),
                           lambda: PageSpec.custom(82.5, 100, 72, 90, 3.5, 9, 10, 8, 6.5, expression="color")])()
        pages = rng.randint(1, 2)
    else:
        spec = rng.choice([lambda: PageSpec.custom(70, 95, 60, 85, 3, 8, 8, 7, 6), PageSpec.b5_doujin,
                           lambda: PageSpec.custom(90, 120, 80, 110, 3, 6, 6, 5, 5)])()
        pages = rng.randint(1, 3)
    episode = models.new_episode("トーン試験", 1, pages, spec, rng.choice([Binding.RIGHT, Binding.LEFT]))
    episode.strict_gates = not render and rng.random() < 0.25
    if rng.random() < 0.3:
        episode.brush_custom = {"my-pen": {"base": "gpen", "width_mm": 0.4, "label": "私のペン"}}
    for page in episode.pages:
        page.numero = False  # (nombres are drawn in M4)
        page.name_ok = rng.random() < 0.5
        for _ in range(rng.randrange(0, 3)):
            target = rng.choice(page.leaf_frames())
            try:
                page.split_frame(target.id, rng.choice(["horizontal", "vertical"]), rng.choice([0.4, 0.5, 0.62]), rng.choice([3, 4.5]))
            except ValueError:
                pass
        leaves = page.leaf_frames()
        for frame in leaves:
            if rng.random() < 0.2:
                from genko import frames as framemod

                pts = framemod.corners(frame.rect)
                framemod.set_shape(frame, [(pts[0][0] + 4, pts[0][1]), pts[1], pts[2], (pts[3][0] - 4, pts[3][1])])
            frame.clip = rng.random() < 0.85
            frame.bleed = rng.random() < 0.2
        ink = next(layer for layer in page.layers if layer.role == LayerRole.INK)
        for _ in range(rng.randrange(0, 5)):
            a, b = rand_xy(rng, page), rand_xy(rng, page)
            ink.strokes.append(models.Stroke(id=models.new_id(), points=[tuple(a), ((a[0] + b[0]) / 2, a[1]), tuple(b)],
                                             pressure=[0.4, 0.9, 0.6] if rng.random() < 0.5 else [],
                                             width_mm=rng.choice([0.4, 1.0, 2.5]), kind=rng.choice(["gpen", "mili", "maru"])))
        if rng.random() < 0.3:
            ink.locked = rng.random() < 0.3
        # tone layers
        for n in range(rng.randrange(1 if render else 0, 4)):
            layer = Layer(id=models.new_id(), role=LayerRole.TONE, kind=LayerKind.TONE, lpi=rng.choice([60.0, 30, 85.5, 15]),
                          density=rng.choice([0.3, 0.1, 0.55, 0.9, 1.5]), angle=rng.choice([45.0, 0.0, 30.0, 112.5, -20.0]),
                          tone=rand_tone(rng), title=f"トーン{n}")
            layer.opacity = rng.choice([1.0, 1.0, 0.6])
            layer.panel_clip = rng.random() < 0.8
            layer.visible = rng.random() < 0.9
            r = rng.random()
            if r < 0.3 and leaves:
                frame = rng.choice(leaves)
                from genko import frames as geo

                patch = fills.polygon_patch([list(p) for p in geo.outline(frame)], (0, 0, 0))
                if patch:
                    layer.patches.append(patch)
            elif r < 0.5:
                layer.region = [tuple(rand_xy(rng, page, -3)) for _ in range(rng.choice([3, 4, 5]))]
            elif r < 0.65:
                patch = fills.polygon_patch([rand_xy(rng, page, -3) for _ in range(4)], (0, 0, 0))
                if patch:
                    layer.patches.append(patch)
            for _ in range(rng.randrange(0, 3)):
                a, b = rand_xy(rng, page), rand_xy(rng, page)
                layer.strokes.append(models.Stroke(id=models.new_id(), points=[tuple(a), tuple(b)], width_mm=rng.choice([3.0, 6.0]),
                                                   kind=rng.choice(["scrape", "scrape_soft", "mili", "gpen"])))
            page.layers.insert(rng.randrange(1, len(page.layers) + 1), layer)
        # a layer's greys as dots
        if rng.random() < (0.5 if render else 0.25):
            target = rng.choice([layer for layer in page.layers if layer.kind == LayerKind.STROKES])
            target.screen = {"pattern": rng.choice(["dot", "line", "cross", "noise", "wave"]), "lpi": rng.choice([40, 60.0]),
                             "angle": rng.choice([45.0, 15]), "black": 0.1, "white": rng.choice([0.95, 0.8])}
            if rng.random() < 0.4:
                target.screen["shape"] = rng.choice(["square", "diamond", "ellipse"])
            if rng.random() < 0.3:
                target.screen["offset_mm"] = [0.5, -0.25]
            if not target.strokes:
                a = rand_xy(rng, page)
                target.strokes.append(models.Stroke(id=models.new_id(), points=[tuple(a), (a[0] + 20, a[1] + 15)], width_mm=4.0,
                                                    kind="airbrush", rgb=(120, 120, 120)))
        # effects
        for n in range(rng.randrange(0, 4 if render else 3)):
            kind = rng.choice(EFFECT_KINDS)
            frame = rng.choice(leaves + [None])
            effect = {"id": models.new_id(), "kind": kind, "frame_id": frame.id if frame is not None else None,
                      "params": rand_params(rng, kind, page, frame)}
            if rng.random() < 0.15:
                effect["visible"] = False
            page.effects.append(effect)
        # rulers
        page.rulers = [rand_ruler(rng, page, models.new_id()) for _ in range(rng.randrange(0, 4))]
        if rng.random() < 0.3:
            page.ruler = {"kind": "perspective", "points": [rand_xy(rng, page) for _ in range(rng.choice([1, 2]))]}
    if not render and rng.random() < 0.3:
        episode.page_locks = {episode.pages[-1].id: rng.choice(["ai:other", "human:誰か"])}
    save_episode(episode, dest, actor="human:作者")
    return {}


def make_books(out: str, seed: int, count: int, render: bool) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        pyref_harness.fresh_process_state(True)
        make_book(random.Random(seed * 1000 + i), root / f"book-{i:02d}.genko", render)


def make_fixture(out: str) -> None:
    """The books of the fixed cases, with ids to name in them: fixture.genko (two pages; panels f-top / f-bottom on
    page 1; layers bg, name, ink, finish, a locked pen layer "locked", tones t-dot, t-grad, t-img; effects e-focus,
    e-speed, e-beta; rulers r-line, r-persp, r-guide, r-conc, r-sym, r-curve, r-rect, r-fixed, r-par; page 2: layers
    bg2, name2, ink2, finish2, effect e2, ruler r2) and strict.genko (the same with strict_gates, page 2 locked by
    ai:other)."""
    from genko import fill as fills
    from genko import frames as geo
    from genko import models
    from genko.io import save_episode
    from genko.models import Binding, Layer, LayerKind, LayerRole, PageSpec, Stroke

    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    pyref_harness.fresh_process_state(True)
    rng = random.Random(20261002)
    ep = models.new_episode("定規とトーン", 1, 2, PageSpec.custom(90, 120, 80, 110, 3, 6, 6, 5, 5), Binding.RIGHT)
    p1, p2 = ep.pages
    for page in ep.pages:
        page.numero = False  # (nombres are drawn in M4)
    p2.name_ok = True
    top, bottom = p1.split_frame(p1.frames[0].id, "horizontal", 0.5, 4)
    top.id, bottom.id = "f-top", "f-bottom"
    for page, names in ((p1, ("bg", "name", "ink", "finish")), (p2, ("bg2", "name2", "ink2", "finish2"))):
        for layer, name in zip(page.layers, names):
            layer.id = name
    ink = p1.layers[2]
    r = bottom.rect
    x0, y0, x1, y1 = float(r.x) + 10, float(r.y) + 10, float(r.x) + 40, float(r.y) + 30
    for (ax, ay), (bx, by) in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        ink.strokes.append(Stroke(id=models.new_id(), points=[(ax, ay), (bx, by)], width_mm=0.6, kind="mili"))
    p1.layers.append(Layer(id="locked", role=LayerRole.USER, kind=LayerKind.STROKES, locked=True, title="鍵"))
    t_dot = Layer(id="t-dot", role=LayerRole.TONE, kind=LayerKind.TONE, lpi=60.0, density=0.3, angle=45.0, tone={"pattern": "dot"}, title="網")
    t_dot.patches.append(fills.polygon_patch([list(p) for p in geo.outline(top)], (0, 0, 0)))
    t_grad = Layer(id="t-grad", role=LayerRole.TONE, kind=LayerKind.TONE, lpi=40.0, density=0.5, angle=30.0,
                   tone={"pattern": "line", "gradient": {"shape": "linear", "angle": 30, "start": 0.1, "end": 0.7}})
    t_grad.region = [(10.0, 70.0), (60.0, 70.0), (60.0, 100.0), (10.0, 100.0)]
    t_img = Layer(id="t-img", role=LayerRole.TONE, kind=LayerKind.TONE, lpi=60.0, density=0.3, angle=0.0,
                  tone={"pattern": "image", "tile_png": tile_picture(rng), "scale_mm": 2.0})
    p1.layers += [t_dot, t_grad, t_img]
    p1.effects = [{"id": "e-focus", "kind": "focus", "frame_id": "f-top", "params": {"count": 30}},
                  {"id": "e-speed", "kind": "speed", "frame_id": "f-bottom", "params": {"count": 12, "angle": 30}},
                  {"id": "e-beta", "kind": "beta_flash", "frame_id": None, "params": {"spikes": 20}}]

    def ruler(rid, kind, points, **more):
        return {"id": rid, "kind": kind, "points": [[float(x), float(y)] for x, y in points], "active": True, "visible": True, **more}

    p1.rulers = [ruler("r-line", "line", [(10, 20), (80, 30)]),
                 ruler("r-persp", "perspective", [(-50, 40), (140, 40)], lock_horizon=True),
                 ruler("r-guide", "guide", [], axis="h", at=50.0),
                 ruler("r-conc", "concentric", [(45, 60), (60, 60)], ratio=0.5, angle=15.0),
                 ruler("r-sym", "symmetry", [(45, 0), (45, 120)], copies=2),
                 ruler("r-curve", "curve", [(10, 80), (40, 70), (80, 90)]),
                 ruler("r-rect", "rect", [(20, 20), (60, 45)], angle=10.0),
                 ruler("r-fixed", "perspective", [(45, 10)], fixed=True),
                 ruler("r-par", "parallel", [], angle=30.0)]
    p1.ruler = {"kind": "perspective", "points": [[45, 20]]}
    p2.effects = [{"id": "e2", "kind": "uni_flash", "frame_id": None, "params": {}}]
    p2.rulers = [ruler("r2", "line", [(1, 2), (30, 2)])]
    save_episode(ep, root / "fixture.genko", actor="human:作者")
    ep.strict_gates = True
    ep.page_locks = {p2.id: "ai:other"}
    save_episode(ep, root / "strict.genko", actor="human:作者")


def render_jobs(jobs_path: str) -> None:
    """Each job {"book", "page", "dpi", "mode", "out", "kind": page | layer | bitonal, "layer", "threshold", "screen",
    "screen_dots"}: the whole page drawn by the baseline (nothing left out), saved as PNG."""
    from genko import brushes, render
    from genko.io import load_episode

    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    books: dict = {}
    for job in jobs:
        render._STROKE_CACHE.clear()
        render._FRAME_MASKS.clear()
        if job["book"] not in books:
            books[job["book"]] = load_episode(Path(job["book"]))
        episode = books[job["book"]]
        brushes.CUSTOM.clear()
        brushes.register(episode.brush_custom)
        render.SCREEN_DOTS = bool(job.get("screen_dots"))
        page = next(p for p in episode.pages if p.index == job["page"])
        kind = job.get("kind", "page")
        try:
            if kind == "layer":
                layer = next(layer for layer in page.layers if layer.id == job["layer"])
                image = render.layer_image(page, layer, job["dpi"], episode)
            else:
                image = render.render_page(page, job["dpi"], mode=job["mode"], episode=episode)
                if kind == "bitonal":
                    image = render.to_bitonal(image, job["threshold"], job.get("screen"))
        except Exception as exc:  # (a page Python cannot draw: said, not drawn)
            Path(job["out"] + ".error").write_text(f"{type(exc).__name__}: {exc}", encoding="utf-8")
            continue
        finally:
            render.SCREEN_DOTS = False
        image.save(job["out"])


# --- applying ops ----------------------------------------------------------------------------------------------------


def run_apply(jobs_path: str) -> None:
    from genko.io import load_episode

    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    for job in jobs:
        pyref_harness.fresh_process_state(False)
        episode = load_episode(Path(job["book"]))
        ids_from(int(job.get("ids_from", 1)))
        reply, changed = reply_of(episode, job["ops"], job.get("agent", "genko"), bool(job.get("dry_run")))
        if reply.get("ok") and changed:
            reply["payload"] = normalized_payload(episode)
        Path(job["out"]).write_text(dumps(reply), encoding="utf-8")


# --- random op sequences ----------------------------------------------------------------------------------------------

BASIC_OPS = ("set_note", "set_meta", "name_ok", "lock_page", "unlock_page", "set_autosave", "add_page", "delete_page",
             "add_layer", "set_layer", "add_stroke", "split_frame", "select_frame")


class Gen:
    """Random ops made from the book as it is."""

    def __init__(self, rng: random.Random, episode, basic: set[str]) -> None:
        self.rng = rng
        self.ep = episode
        self.basic = basic

    def page(self):
        return self.rng.choice(self.ep.pages)

    def page_no(self, page=None):
        r = self.rng.random()
        if r < 0.04:
            return self.rng.choice([99, "x", None, 1.0])
        return (page or self.page()).index

    def layer_id(self, page, tone: bool | None = None):
        layers = [layer for layer in page.layers if tone is None or (tone == (layer.role.value == "tone" or layer.kind.value == "tone"))]
        if not layers or self.rng.random() < 0.08:
            return self.rng.choice(["nope", "", None])
        return self.rng.choice(layers).id

    def frame_id(self, page):
        leaves = page.leaf_frames()
        if not leaves or self.rng.random() < 0.08:
            return self.rng.choice(["no-panel", False])
        return self.rng.choice(leaves).id

    def xy(self, page, margin=5.0):
        return rand_xy(self.rng, page, margin)

    def num(self, values, bad=()):
        if bad and self.rng.random() < 0.06:
            return self.rng.choice(list(bad))
        return self.rng.choice(values)

    def effect_id(self, page):
        if not page.effects or self.rng.random() < 0.1:
            return self.rng.choice(["no-effect", None])
        e = self.rng.choice(page.effects)
        return e.get("id") if isinstance(e, dict) else None

    def ruler_id(self, page):
        if not page.rulers or self.rng.random() < 0.1:
            return self.rng.choice(["no-ruler", None])
        r = self.rng.choice(page.rulers)
        return r.get("id") if isinstance(r, dict) else None

    # --- tone, effect and ruler ops ---------------------------------------------------------------------------------

    def tone_fields(self, op: dict, page) -> None:
        rng = self.rng
        if rng.random() < 0.5:
            op["pattern"] = self.num(TONE_PATTERNS, ["stripes", 5])
            if op["pattern"] == "image" and rng.random() < 0.85:
                op["tile_png"] = tile_picture(rng) if rng.random() < 0.85 else "bm90IGEgcGljdHVyZQ=="
        for key, values, bad in (("density", [0.1, 0.3, 0.75, 1, 0], [1.5, -0.1, "x"]), ("lpi", [30, 60, 85.5, 5, 300], [3, 400, "abc"]),
                                 ("angle", [0, 15, 45.0, 90, -30], ["a"]), ("scale_mm", [1, 2.5, 50, 0.3], [0.1, 60, "s"])):
            if rng.random() < 0.3:
                op[key] = self.num(values, bad)
        if rng.random() < 0.2:
            op["gradient"] = rng.choice([{"shape": "linear", "angle": 30, "start": 0.2, "end": 0.8}, {"shape": "radial", "start": 0, "end": 0.5},
                                         {"start": 0.6, "end": 0.0}, {"shape": "conic"}, {"start": 2}, None, {}])
        if rng.random() < 0.2:
            op["dot_shape"] = rng.choice(["square", "diamond", "ellipse", "round", None, "", "star", 3])
        if rng.random() < 0.2:
            op["offset_mm"] = rng.choice([[0.5, 0], [1, -1.25], [0, 0], None, [1], "ab", [1, 2, 3], 5])
        if rng.random() < 0.15:
            op["move_by_mm"] = rng.choice([[0.25, 0.25], [-1, 0], [1, "x"]])
        if rng.random() < 0.2:
            op["name"] = rng.choice(["網", "", "トーンA"])

    def area(self, page):
        rng = self.rng
        r = rng.random()
        if r < 0.45:
            return {"poly": [self.xy(page, -2) for _ in range(rng.choice([3, 4, 5, 2]))]}
        if r < 0.6:
            x, y = self.xy(page, -10)
            return {"rect": [x, y, rng.choice([10, 25.5, 0.1]), rng.choice([8, 30])]}
        if r < 0.7:
            x, y = self.xy(page, -10)
            return {"ellipse": [x, y, rng.choice([12, 30]), rng.choice([9, 22.5])]}
        if r < 0.85:
            x, y = self.xy(page, -10)
            return {"mask": {"box": [x, y, rng.choice([5, 12.5]), rng.choice([6, 9])], "png": mask_picture(rng)}}
        return rng.choice([{"poly": []}, {"mask": {"box": [1, 2, 3, 4]}}, {}, {"poly": [[1, 2], [3, 4]]}])

    def add_tone(self):
        page = self.page()
        op = {"op": "add_tone", "page": self.page_no(page)}
        if self.rng.random() < 0.6:
            op["id"] = self.rng.choice(["t-" + str(self.rng.randrange(1000)), self.layer_id(page)])
        self.tone_fields(op, page)
        r = self.rng.random()
        if r < 0.35:
            op["area"] = self.area(page)
        elif r < 0.65:
            op["frame_id"] = self.frame_id(page)
        elif r < 0.75:
            x, y = self.xy(page, -2)
            op["at"] = {"x_mm": x, "y_mm": y, "reference": self.rng.choice(["layer", "reference"]),
                        "gap_mm": self.rng.choice([0, 0.3, 1])}
        if self.rng.random() < 0.2:
            op["after"] = self.layer_id(page)
        return op

    def set_tone(self):
        page = self.page()
        op = {"op": "set_tone", "page": self.page_no(page), "id": self.layer_id(page, tone=self.rng.random() < 0.85)}
        self.tone_fields(op, page)
        if self.rng.random() < 0.15:
            op[self.rng.choice(["scale_mm", "tile_png"])] = None
        return op

    def delete_tone(self):
        # (a tone layer, or an id no layer has: the C++ build refuses to delete a layer that is not a tone, which Python
        # deletes; docs/cpp-migration/SPEC.md COMP-01a, checked by the unit tests)
        page = self.page()
        page_no = self.page_no(page)
        self.rng.random()  # (the draw that chose a layer of either kind)
        return {"op": "delete_tone", "page": page_no, "id": self.layer_id(page, tone=True)}

    def add_effect(self):
        page = self.page()
        kind = self.num(EFFECT_KINDS, ["sparkle", ""])
        leaves = page.leaf_frames()
        frame = self.rng.choice(leaves + [None]) if leaves else None
        op = {"op": "add_effect", "page": self.page_no(page), "kind": kind,
              "params": rand_params(self.rng, kind if kind in EFFECT_KINDS else "focus", page, frame)}
        if frame is not None:
            op["frame_id"] = frame.id
        elif self.rng.random() < 0.1:
            op["frame_id"] = "no-panel"
        if self.rng.random() < 0.5:
            op["id"] = "e-" + str(self.rng.randrange(1000))
        if self.rng.random() < 0.12:  # settings it refuses
            op["params"][self.rng.choice(["jitter", "taper", "bundle", "count", "avoid", "within", "depth"])] = self.rng.choice(
                [3, "sideways", 0, 5000, [{"circle": 1}], [[1, 2]], -1])
        return op

    def edit_effect(self):
        page = self.page()
        op = {"op": "edit_effect", "page": self.page_no(page), "id": self.effect_id(page)}
        if self.rng.random() < 0.7:
            kind = self.rng.choice(EFFECT_KINDS)
            params = rand_params(self.rng, kind, page, None)
            for key in list(params)[: self.rng.randrange(0, 3)]:
                params[key] = None
            op["params"] = params
        if self.rng.random() < 0.3:
            op["kind"] = self.num(EFFECT_KINDS, ["sparkle"])
        if self.rng.random() < 0.2:
            op["frame_id"] = self.rng.choice([self.frame_id(page), None, ""])
        if self.rng.random() < 0.2:
            op["visible"] = self.rng.choice([True, False, 0])
        return op

    def delete_effect(self):
        page = self.page()
        return {"op": "delete_effect", "page": self.page_no(page), "id": self.effect_id(page)}

    def effect_to_layer(self):
        page = self.page()
        op = {"op": "effect_to_layer", "page": self.page_no(page), "id": self.effect_id(page)}
        r = self.rng.random()
        if r < 0.5:
            op["layer_id"] = self.layer_id(page)
        elif r < 0.8:
            op["layer"] = self.rng.choice(["ink", "name", "draft", "finish", "user", "bogus"])
        if self.rng.random() < 0.3:
            op["keep"] = True
        return op

    def ruler_fields(self, op: dict, page) -> None:
        rng = self.rng
        if rng.random() < 0.6:
            op["points"] = [self.xy(page) for _ in range(rng.choice([0, 1, 2, 3, 4]))]
            if rng.random() < 0.05:
                op["points"] = rng.choice([[[1]], [["a", 2], [3, 4]], "ab", [[1, 2, 3], [4, 5, 6]]])
        for key, values, bad in (("angle", [0, 30, 90.5, -60], ["x", "inf"]), ("ratio", [1, 0.5, 2, 0, -1], ["r"]),
                                 ("reach_mm", [3, 10, 0, 25.5], []), ("at", [10, 50.25, 0], ["here"])):
            if rng.random() < 0.15:
                op[key] = self.num(values, bad)
        if rng.random() < 0.15:
            op["axis"] = rng.choice(["h", "v", "z"])
        if rng.random() < 0.15:
            op["copies"] = rng.choice([2, 3, 6, 1, 33, 4.7, "2"])
        if rng.random() < 0.15:
            op["grid"] = rng.choice([0, 5, 60, 61, None, 12.9])
        for key in ("mirror", "active", "visible", "lock_horizon", "fixed"):
            if rng.random() < 0.08:
                op[key] = rng.choice([True, False, 0, 1, "yes"])
        if rng.random() < 0.15:
            op["points2"] = [self.xy(page) for _ in range(rng.choice([1, 2, 3]))]
        if rng.random() < 0.15:
            op["center"] = rng.choice([self.xy(page), [1], None])
        if rng.random() < 0.12:
            op["layer_id"] = rng.choice([self.layer_id(page), None, ""])
        if rng.random() < 0.12:
            op["frame_id"] = rng.choice([self.frame_id(page), None])

    def add_ruler(self):
        page = self.page()
        op = {"op": "add_ruler", "page": self.page_no(page), "kind": self.num(RULER_KINDS, ["spiral", ""])}
        if self.rng.random() < 0.6:
            op["id"] = self.rng.choice(["r-" + str(self.rng.randrange(1000)), self.ruler_id(page)])
        self.ruler_fields(op, page)
        return op

    def edit_ruler(self):
        page = self.page()
        op = {"op": "edit_ruler", "page": self.page_no(page), "id": self.ruler_id(page)}
        self.ruler_fields(op, page)
        if self.rng.random() < 0.15:
            op["horizon_y"] = self.rng.choice([50, 120.5, "up"])
        return op

    def delete_ruler(self):
        page = self.page()
        op = {"op": "delete_ruler", "page": self.page_no(page)}
        if self.rng.random() < 0.8:
            op["id"] = self.ruler_id(page)
        return op

    def set_ruler(self):
        page = self.page()
        op = {"op": "set_ruler", "page": self.page_no(page)}
        if self.rng.random() < 0.8:
            op["kind"] = self.rng.choice(["perspective", "line", "", 5])
        if self.rng.random() < 0.8:
            op["points"] = [self.xy(page) for _ in range(self.rng.choice([0, 1, 2]))]
            if self.rng.random() < 0.1:
                op["points"] = self.rng.choice(["ab", [[1, 2], "cd"], [{"x": 1}]])
        return op

    def ruler_to_layer(self):
        page = self.page()
        op = {"op": "ruler_to_layer", "page": self.page_no(page), "id": self.ruler_id(page)}
        r = self.rng.random()
        if r < 0.5:
            op["layer_id"] = self.layer_id(page)
        elif r < 0.75:
            op["layer"] = self.rng.choice(["ink", "name", "finish", "draft", "bogus"])
        if self.rng.random() < 0.4:
            op["width_mm"] = self.rng.choice([0.3, 1, 0, "w"])
        if self.rng.random() < 0.4:
            op["kind"] = self.rng.choice(["gpen", "mili", "fx", "oil", "my-pen", "bogus", ""])
        if self.rng.random() < 0.3:
            op["rgb"] = self.rng.choice([[200, 10, 10], [1, 2, 3, 4], [], ["a"]])
        return op

    # --- the basic ops (those the C++ build has) ---------------------------------------------------------------------

    def set_note(self):
        return {"op": "set_note", "page": self.page_no(), "note": self.rng.choice(["メモ", "", "line\nline"])}

    def set_meta(self):
        rng = self.rng
        op = {"op": "set_meta"}
        if rng.random() < 0.6:
            op["strict_gates"] = rng.random() < 0.5
        if rng.random() < 0.3:
            op["title"] = rng.choice(["新しい題", "t"])
        if rng.random() < 0.1:
            op["binding"] = rng.choice(["left", "right"])
        return op

    def name_ok(self):
        op = {"op": "name_ok"}
        if self.rng.random() < 0.8:
            op["page"] = self.page_no()
        return op

    def lock_page(self):
        op = {"op": "lock_page", "page": self.page_no()}
        if self.rng.random() < 0.6:
            op["agent"] = self.rng.choice(["ai:other", "human:作者", "genko"])
        return op

    def unlock_page(self):
        return {"op": "unlock_page", "page": self.page_no()}

    def set_autosave(self):
        return {"op": "set_autosave", "enabled": self.rng.random() < 0.5}

    def add_page(self):
        op = {"op": "add_page"}
        if self.rng.random() < 0.4:
            op["after"] = self.rng.choice([0, 1, len(self.ep.pages)])
        return op

    def delete_page(self):
        return {"op": "delete_page", "page": self.page_no()}

    def add_layer(self):
        return {"op": "add_layer", "page": self.page_no(), "kind": self.rng.choice(["pen", "paint", "tone", "fill"]),
                "id": "l-" + str(self.rng.randrange(1000))}

    def set_layer(self):
        page = self.page()
        op = {"op": "set_layer", "page": self.page_no(page), "id": self.layer_id(page)}
        if self.rng.random() < 0.5:
            op["screen"] = self.rng.choice([{"lpi": 40}, {"pattern": "line", "lpi": 50}, None])
        if self.rng.random() < 0.5:
            op["visible"] = self.rng.random() < 0.5
        return op

    def add_stroke(self):
        page = self.page()
        return {"op": "add_stroke", "page": self.page_no(page), "layer_id": self.layer_id(page),
                "points": [self.xy(page) for _ in range(3)], "width_mm": self.rng.choice([0.5, 2])}

    def split_frame(self):
        page = self.page()
        return {"op": "split_frame", "page": self.page_no(page), "frame_id": self.frame_id(page),
                "axis": self.rng.choice(["horizontal", "vertical"]), "ratio": 0.5, "gutter_mm": 3}

    def select_frame(self):
        page = self.page()
        return {"op": "select_frame", "page": self.page_no(page), "frame_id": self.frame_id(page)}

    def one(self) -> dict:
        mine = ["add_tone", "set_tone", "delete_tone", "add_effect", "edit_effect", "delete_effect", "effect_to_layer",
                "add_ruler", "edit_ruler", "delete_ruler", "set_ruler", "ruler_to_layer"]
        names = mine * 3 + [name for name in BASIC_OPS if name in self.basic]
        return getattr(self, self.rng.choice(names))()


def run_sequences(out: str, books: str, seed: int, count: int, basic: set[str], steps: int) -> None:
    from genko.io import load_episode

    paths = sorted(str(p) for p in Path(books).glob("book-*.genko"))
    result = []
    for k in range(count):
        rng = random.Random(seed * 100003 + k)
        book = paths[k % len(paths)]
        pyref_harness.fresh_process_state(False)
        episode = load_episode(Path(book))
        sequence = {"book": book, "steps": []}
        for s in range(rng.randrange(max(2, steps // 2), steps + 1)):
            gen = Gen(rng, episode, basic)
            ops = [gen.one() for _ in range(1 if rng.random() < 0.8 else rng.choice([2, 3]))]
            agent = rng.choice(["genko"] * 6 + ["ai:hermes", "human:作者", "ai:other"])
            dry_run = rng.random() < 0.06
            start = 1 + 1000 * (k * 100 + s)
            ids_from(start)
            reply, changed = reply_of(episode, copy.deepcopy(ops), agent, dry_run)
            step = {"ops": ops, "agent": agent, "dry_run": dry_run, "ids_from": start, "reply": reply}
            if reply.get("ok") and changed:
                step["payload"] = normalized_payload(episode)
            sequence["steps"].append(step)
        result.append(sequence)
    Path(out).write_text(dumps(result), encoding="utf-8")


# --- rulers ---------------------------------------------------------------------------------------------------------


def pts_bits(points) -> list:
    return [[bits(p[0]), bits(p[1])] + ([bits(p[2])] if len(p) > 2 else []) for p in points]


def snaps(out: str, seed: int, count: int) -> None:
    from genko import rulers

    rng = random.Random(seed)

    class FakePage:
        class spec:
            width_mm = 182.0
            height_mm = 257.0

        @staticmethod
        def leaf_frames():
            return []

        @staticmethod
        def bleed_rect_mm():
            raise AssertionError

    cases = []
    for i in range(count):
        page = FakePage
        rulers_list = [rand_ruler(rng, page, f"r{j}") for j in range(rng.choice([1, 1, 2, 3]))]
        for r in rulers_list:
            r.pop("frame_id", None)
            if rng.random() < 0.2:
                r["layer_id"] = rng.choice(["L1", "L2"])
        x, y = rng.uniform(0, 182), rng.uniform(0, 257)
        n = rng.choice([2, 3, 5, 9, 17])
        pressure = rng.random() < 0.6
        stroke = []
        for _ in range(n):
            x += rng.uniform(-8, 8) * rng.choice([0.3, 1, 3])
            y += rng.uniform(-8, 8) * rng.choice([0.3, 1, 3])
            stroke.append((x, y, rng.random()) if pressure else (x, y))
        if pressure and rng.random() < 0.1:
            stroke = [p[:2] if j % 2 else p for j, p in enumerate(stroke)]  # (some points without a pressure)
        if rng.random() < 0.3:  # start near a ruler: pick a ruler's point
            for r in rulers_list:
                if r.get("points"):
                    px, py = r["points"][0]
                    shift = (px + rng.uniform(-4, 4) - stroke[0][0], py + rng.uniform(-4, 4) - stroke[0][1])
                    stroke = [(p[0] + shift[0], p[1] + shift[1], *p[2:]) for p in stroke]
                    break
        layer_id = rng.choice([None, None, "L1", "L2"])
        only = rng.choice([None] * 5 + [rulers_list[0]["id"]])
        case = {"rulers": rulers_list, "points": pts_bits(stroke), "layer_id": layer_id, "only": only}
        try:
            case["snap"] = pts_bits(rulers.snap([list(p) for p in stroke], rulers_list, None, only, layer_id))
        except Exception as exc:  # noqa: BLE001
            case["snap"] = {"raised": type(exc).__name__}
        try:
            case["symmetry"] = [pts_bits(c) for c in rulers.symmetry_copies([list(p) for p in stroke], rulers_list, None, layer_id)]
        except Exception as exc:  # noqa: BLE001
            case["symmetry"] = {"raised": type(exc).__name__}
        page_size = rng.choice([(182, 257), (210.5, 297.0), (60, 60)])
        case["page_size"] = list(page_size)
        per = []
        for r in rulers_list:
            item = {"outline": [pts_bits(line) for line in rulers.outline(r, page_size)]}
            if r["kind"] == "perspective":
                item["grid"] = [pts_bits(seg) for seg in rulers.perspective_grid(r, page_size)]
                item["grid5"] = [pts_bits(seg) for seg in rulers.perspective_grid(r, page_size, 5)]
                eye = rulers.horizon(r)
                item["horizon"] = None if eye is None else pts_bits([eye[0], eye[1]])
            if r["kind"] in ("rect", "ellipse", "polygon"):
                item["shape"] = pts_bits(rulers.shape_outline(r))
            if r["kind"] in ("parallel", "radial", "perspective"):
                item["directions"] = pts_bits(rulers.directions(r, stroke[0][:2]))
            if r["kind"] in ("curve", "parallel_curve", "multi_curve", "radial_curve"):
                per_mm = rng.choice([1.0, 0.5, 2.0])
                item["smooth"] = pts_bits(rulers.smooth_curve(r["points"], per_mm))
                item["per_mm"] = bits(per_mm)
            per.append(item)
        case["per_ruler"] = per
        spacing = rng.choice([0, 2.5, 5, 7.75])
        origin = rng.choice([(0.0, 0.0), (1.5, -2.0)])
        case["grid_snap"] = {"spacing": bits(spacing), "origin": pts_bits([origin])[0],
                             "result": pts_bits([rulers.snap_to_grid(stroke[-1][:2], spacing, origin)])[0]}
        cases.append(case)
    Path(out).write_text(dumps(cases), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make-books")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=15)
    p.add_argument("--render", action="store_true")
    p = sub.add_parser("apply")
    p.add_argument("jobs")
    p = sub.add_parser("sequences")
    p.add_argument("out")
    p.add_argument("--books", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=150)
    p.add_argument("--steps", type=int, default=12)
    p.add_argument("--ops", default="")
    p = sub.add_parser("snaps")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=500)
    p = sub.add_parser("fixture")
    p.add_argument("out")
    p = sub.add_parser("render")
    p.add_argument("jobs")
    args = parser.parse_args(argv)
    if args.cmd == "make-books":
        make_books(args.out, args.seed, args.count, args.render)
    elif args.cmd == "fixture":
        make_fixture(args.out)
    elif args.cmd == "render":
        render_jobs(args.jobs)
    elif args.cmd == "apply":
        run_apply(args.jobs)
    elif args.cmd == "sequences":
        run_sequences(args.out, args.books, args.seed, args.count, {name for name in args.ops.split(",") if name}, args.steps)
    elif args.cmd == "snaps":
        snaps(args.out, args.seed, args.count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
