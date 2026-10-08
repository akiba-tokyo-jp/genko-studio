#!/usr/bin/env python3
"""The Python reference for the C++ contract tests (native/tests/contract): runs the baseline (src/genko) and writes
what the C++ build must match. Development only; nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/pyref_harness.py <command> ...

Commands (JSON is UTF-8 without escapes):
  snapshot BOOK [--full] [--ids]       genko.headless.snapshot(load_episode(BOOK), full) on stdout
  resave BOOK DEST [--ids]             load_episode(BOOK), then save_episode(…, DEST): the book as Python's v3 writer
                                       writes it
  batch JOBS                           many jobs (a JSON list) in one process:
                                       {"op": "snapshot", "book", "full", "ids", "out"} | {"op": "resave", "book",
                                       "dest", "ids"} | {"op": "apply", "book", "ops", "agent", "dry_run", "ids",
                                       "out", "dest"?} (apply_ops on the book read; the reply, or {"ok": false,
                                       "error"}, to out; saved with save_episode into the new folder dest) |
                                       {"op": "steps", "book", "steps": [{"ops", "agent"?, "dry_run"?}], "ids",
                                       "first_id"?, "store", "out", "digest"?, "reread"?, "dump"?} (batches one after
                                       another on the book in memory, as a session: see steps_job; "dump", a folder:
                                       the layers' PNGs after each step written there, see _png_pixels) |
                                       {"op": "add_adjust_layers", "book", "dest", "layers"} (correction layers of
                                       any filter put on pages by hand: add_adjust_layers_job)
  restore BOOK --actor A [--redo] [--force]
                                       journal.restore under ProjectLock, as `genko undo`/`redo` does: the reply (or
                                       {"ok": false, "error"}) on stdout
  lock-hold BOOK --agent A             hold genko.lock.ProjectLock until stdin closes: prints "locked" (or "busy:
                                       <message>" and exits 3), then "released"
  lock-try BOOK --agent A              take and give back ProjectLock: {"ok": true} or {"ok": false, "error"}
  upgrade BOOK                         load_episode then save_episode into the same folder (a v1/v2 book becomes v3
                                       with project.v2.bak.json and its first journal line, as the baseline does)
  make-random OUT --seed N --count K   K random v3 books made with new_episode and direct field assignment
  make-opsbook DEST                    the v3 book of the op contract tests (native/tests/contract/ops_cases.json
                                       names its ids: they are counted, so it is the same book every time)
  make-drawbook DEST                   the v3 book test_contract_drawn_by_ops draws lines on (nothing the C++ build
                                       does not draw yet; ids counted)
  make-sequences OUT --books DIR --seed N --count K
                                       K random op sequences (1 to 12 ops, in batches by various actors, some dry
                                       runs, for_pages, strict_gates, page locks) over the make-random books in DIR
  show-cases CASES BOOK [--only TEXT]  each case of a contract case file run on BOOK: its last reply, flagged where
                                       it does not do what the case says (for writing cases)
  make-rasterbook DEST                 the v3 book of the raster op contract tests (native/tests/contract/
                                       raster_cases.json names its layers; ids counted)
  make-raster-books OUT --seed N --count K
                                       K random books for drawing (render_harness.py make-books) without what the C++
                                       build does not draw yet
  make-raster-sequences OUT --books DIR --seed N --count K
                                       K random sequences of the ops of M3-A1 (with basic ops of M2) over those books
  filter-edges OUT                     pictures on the edges of the filters' float32 decisions (despeckle's luminance,
                                       lineart's ratio) and filters.apply_filter of each
  numbers OUT --seed N --count K       cases for repr(float), round(), sum(), math.hypot/dist and format(x, "g")
  json-dumps OUT --seed N --count K    random JSON documents and json.dumps of each (indent 2, default, canonical,
                                       ensure_ascii)
  json-errors OUT                      malformed JSON texts and the message json.loads gives for each
  unit-tables                          Python's results for the C++ unit tests (json.dumps cases, round(), sum(),
                                       the paper presets, covers.spec_for) on stdout; stored as
                                       native/tests/data/pyref/unit_tables.json (its sha256 in MANIFEST.json there)

--ids makes genko.models.new_id return 000000000001, 000000000002, … (from 1 again for each book), the same ids as
genko::core::ScopedIdSource(counting_ids()) in C++, so books whose ids are made while reading (v1: layers, pages,
strokes) compare exactly. Each job also starts with an empty blob cache and only the built-in brushes, as a new
process would.
"""
from __future__ import annotations

import argparse
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


# --- deterministic ids ------------------------------------------------------------------------------------------


class _Uuid:
    __slots__ = ("hex",)

    def __init__(self, text: str) -> None:
        self.hex = text


class _Counter:
    def __init__(self, first: int = 1) -> None:
        self.next = first

    def __call__(self) -> _Uuid:
        n = self.next
        self.next += 1
        return _Uuid(f"{n:012x}" + "0" * 20)


def counting_ids(first: int = 1) -> None:
    import genko.models as models

    models.uuid4 = _Counter(first)


def fresh_process_state(ids: bool, first: int = 1) -> None:
    from genko import blobcache, brushes

    blobcache.clear()
    brushes.CUSTOM.clear()  # (a new process knows only the brushes of the book it reads)
    if ids:
        counting_ids(first)


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


# --- snapshot / resave ----------------------------------------------------------------------------------------------


def snapshot_of(book: str, full: bool, ids: bool) -> dict:
    from genko.headless import snapshot
    from genko.io import load_episode

    fresh_process_state(ids)
    return snapshot(load_episode(Path(book)), full=full)


def resave(book: str, dest: str, ids: bool) -> None:
    from genko.io import load_episode, save_episode

    fresh_process_state(ids)
    episode = load_episode(Path(book))
    save_episode(episode, Path(dest), actor="genko")


def apply_job(job: dict) -> None:
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode
    from genko.ops import ApplyError

    fresh_process_state(bool(job.get("ids")))
    episode = load_episode(Path(job["book"]))
    try:
        reply = apply_ops(episode, job["ops"], dry_run=bool(job.get("dry_run")), agent=job.get("agent", "genko"))
    except ApplyError as exc:
        reply = {"ok": False, "error": str(exc)}
    Path(job["out"]).write_text(dumps(reply), encoding="utf-8")
    if job.get("dest") and reply.get("ok") and not job.get("dry_run"):
        save_episode(episode, Path(job["dest"]), actor=job.get("agent", "genko"))


def _digest(value) -> str:
    import hashlib

    return hashlib.sha256(dumps(value).encode("utf-8")).hexdigest()


def _pixels_digest(data: bytes | None) -> str:
    """What a PNG holds: "png:" and the sha256 of "<mode>|<w>x<h>|" and its pixels as RGBA ("unreadable:" and the sha256
    of the bytes when Pillow cannot read them; "missing" for no bytes). The C++ tests make the same of their PNGs."""
    import hashlib

    from PIL import Image

    if data is None:
        return "missing"
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
        rgba = im.convert("RGBA")
    except Exception:
        return "unreadable:" + hashlib.sha256(data).hexdigest()
    h = hashlib.sha256(f"{im.mode}|{im.size[0]}x{im.size[1]}|".encode("utf-8"))
    h.update(rgba.tobytes())
    return "png:" + h.hexdigest()


def _png_pixels(payload: dict, store, dump: Path | None = None, prefix: str = "") -> dict:
    """The payload with each PNG it refers to as the pixels it holds (_pixels_digest): a layer's pixels, its mask and its
    patches, and the mask of an area kept on a page (saved_areas). The C++ build writes other PNG bytes for the same
    pixels; the tests compare the pixels (and the JSON assets, strokes, byte for byte). With `dump` (a folder), the
    layers' PNGs are also written there as "<prefix>p<page>-l<layer>-<asset|mask|patchN>.png" (the C++ tests write theirs
    under the same names, to show where pictures differ)."""
    import base64

    def digest(data, name):
        if dump is not None and data is not None:
            dump.mkdir(parents=True, exist_ok=True)
            (dump / f"{prefix}{name}.png").write_bytes(data)
        return _pixels_digest(data)

    for p, page in enumerate(payload.get("pages") or []):
        if not isinstance(page, dict):
            continue
        for k, layer in enumerate(page.get("layers") or []):
            if not isinstance(layer, dict) or layer.get("kind") == "placed":
                continue
            if isinstance(layer.get("asset"), str):
                layer["asset"] = digest(store.get_bytes(layer["asset"], ".png"), f"p{p}-l{k}-asset")
            if isinstance(layer.get("mask"), dict) and isinstance(layer["mask"].get("asset"), str):
                layer["mask"]["asset"] = digest(store.get_bytes(layer["mask"]["asset"], ".png"), f"p{p}-l{k}-mask")
            for n, patch in enumerate(layer.get("patches") or []):
                if isinstance(patch, dict) and isinstance(patch.get("asset"), str):
                    patch["asset"] = digest(store.get_bytes(patch["asset"], ".png"), f"p{p}-l{k}-patch{n}")
        areas = page.get("saved_areas")
        if isinstance(areas, dict):
            for area in areas.values():
                mask = area.get("mask") if isinstance(area, dict) else None
                if isinstance(mask, dict) and isinstance(mask.get("png"), str):
                    try:
                        mask["png"] = _pixels_digest(base64.b64decode(mask["png"]))
                    except ValueError:
                        pass
    return payload


def _full_snapshot(episode) -> dict:
    """snapshot(full=True) without its side effect. It reads page.name_strokes and page.ink_strokes, and Page._layer
    adds a NAME or INK layer (with a new id) to a page that has none. `genko inspect --full` and the server's
    /v1/inspect take it of a book just read and never save it, so that layer is never seen (the "layers" of the same
    snapshot are listed before it is added); here the session goes on, so the pages get their layers back and the
    ids the snapshot used are given back."""
    import genko.models as models
    from genko.headless import snapshot

    counter = models.uuid4
    mark = getattr(counter, "next", None)
    kept = [(page, list(page.layers)) for page in episode.pages]
    full = snapshot(episode, full=True)
    for page, layers in kept:
        page.layers[:] = layers
    if mark is not None:
        counter.next = mark
    return full


def steps_job(job: dict) -> None:
    """Batches applied one after another to one book in memory, as a session does: after each, the reply (or the
    error), the full snapshot (see _full_snapshot) and the project.json payload (Python's v3 writer, without
    "revision"; its PNGs as the pixels they hold: _png_pixels). With "digest", the snapshot and payload (and the
    reply's snapshot) are replaced by the sha256 of their json.dumps. With "reread" (a new folder), the book is then
    saved there with save_episode and read back as a new process would (ids counted from 1 again with "ids"): one
    more record, {"reread": true, "full", "payload"}."""
    import copy

    from genko.assets import AssetStore
    from genko.headless import apply_ops
    from genko.io import _payload, load_episode, save_episode
    from genko.ops import ApplyError

    fresh_process_state(bool(job.get("ids")))
    episode = load_episode(Path(job["book"]))
    if job.get("ids"):  # (reading makes ids too, for the default layers each Page starts with: the ops' count anew)
        counting_ids(int(job.get("first_id", 1)))
    store = AssetStore(Path(job["store"]))
    digest = bool(job.get("digest"))
    dump = Path(job["dump"]) if job.get("dump") else None
    records = []
    for s, step in enumerate(job["steps"]):
        try:
            reply = apply_ops(episode, copy.deepcopy(step["ops"]), dry_run=bool(step.get("dry_run")),
                              agent=step.get("agent", "genko"))
        except ApplyError as exc:
            reply = {"ok": False, "error": str(exc)}
        except Exception as exc:  # (apply_ops lets these through: Python's command line stops with a traceback)
            reply = {"ok": False, "error": str(exc), "uncaught": type(exc).__name__}
        raw_payload = _payload(episode, store)
        if dump is not None:
            dump.mkdir(parents=True, exist_ok=True)
            (dump / f"{s}-project.json").write_text(dumps(raw_payload), encoding="utf-8")
        payload = _png_pixels(raw_payload, store, dump, f"{s}-")
        payload.pop("revision", None)
        full = _full_snapshot(episode)
        if digest:
            if "snapshot" in reply:
                reply["snapshot"] = _digest(reply["snapshot"])
            full, payload = _digest(full), _digest(payload)
        records.append({"reply": reply, "full": full, "payload": payload})
    if job.get("reread"):
        save_episode(episode, Path(job["reread"]), actor="genko")
        fresh_process_state(bool(job.get("ids")))
        again = load_episode(Path(job["reread"]))
        payload = _png_pixels(_payload(again, store), store, dump, "reread-")
        payload.pop("revision", None)
        full = _full_snapshot(again)
        if digest:
            full, payload = _digest(full), _digest(payload)
        records.append({"reread": True, "full": full, "payload": payload})
    Path(job["out"]).write_text(dumps(records), encoding="utf-8")


def restore_book(book: str, actor: str, redo: bool, force: bool) -> dict:
    from genko.journal import restore
    from genko.lock import ProjectLock
    from genko.ops import ApplyError

    try:
        with ProjectLock(Path(book), agent=actor):
            return restore(Path(book), actor=actor, redo=redo, force=force)
    except ApplyError as exc:
        return {"ok": False, "error": str(exc)}


def lock_hold(book: str, agent: str) -> int:
    from genko.lock import ProjectLock
    from genko.ops import ApplyError

    lock = ProjectLock(Path(book), agent=agent)
    try:
        lock.acquire()
    except ApplyError as exc:
        print(f"busy: {exc}", flush=True)
        return 3
    print("locked", flush=True)
    sys.stdin.read()
    lock.release()
    print("released", flush=True)
    return 0


def lock_try(book: str, agent: str) -> dict:
    from genko.lock import ProjectLock
    from genko.ops import ApplyError

    try:
        with ProjectLock(Path(book), agent=agent):
            return {"ok": True}
    except ApplyError as exc:
        return {"ok": False, "error": str(exc)}


def upgrade(book: str) -> None:
    from genko.io import load_episode, save_episode

    fresh_process_state(False)
    save_episode(load_episode(Path(book)), Path(book), actor="genko")


def add_adjust_layers_job(job: dict) -> None:
    """Correction layers of the kinds add_layer does not make (blur, mosaic, wave, …: filters.py draws them when a
    book has them) put on top of pages by hand, as a book made elsewhere has them: {"book", "dest", "ids"?, "layers":
    [{"page", "id", "adjust"}]}, saved into the new folder dest."""
    from genko.io import load_episode, save_episode
    from genko.models import Layer, LayerKind, LayerRole

    fresh_process_state(bool(job.get("ids")))
    episode = load_episode(Path(job["book"]))
    for spec in job["layers"]:
        page = next(p for p in episode.pages if p.index == spec["page"])
        page.layers.append(Layer(id=spec["id"], role=LayerRole.USER, kind=LayerKind.ADJUST, adjust=dict(spec["adjust"])))
    save_episode(episode, Path(job["dest"]), actor="genko")


def run_batch(jobs_path: str) -> None:
    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    for job in jobs:
        if job["op"] == "add_adjust_layers":
            add_adjust_layers_job(job)
        elif job["op"] == "snapshot":
            data = snapshot_of(job["book"], bool(job.get("full")), bool(job.get("ids")))
            Path(job["out"]).write_text(dumps(data), encoding="utf-8")
        elif job["op"] == "resave":
            resave(job["book"], job["dest"], bool(job.get("ids")))
        elif job["op"] == "apply":
            apply_job(job)
        elif job["op"] == "steps":
            steps_job(job)
        else:
            raise SystemExit(f"unknown job {job['op']!r}")


# --- random books -----------------------------------------------------------------------------------------------------

SPECIAL_FLOATS = [0.1 + 0.2, 1e-7, 1e16, 1e22, -0.0, 0.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
                  123456789.123, 1e-05, 0.0001, 1 / 3, 2.675, 0.5, 1.5, -2.5, 9007199254740993.0, 1e15]
SPECIAL_INTS = [0, 1, -1, 2**31, 2**53 + 1, -(2**62), 2**63 - 1, -(2**63), 2**64 - 1, 12345678901234]
TEXTS = ["", "東京と東京", "始めよう🙂", "a\tb\nc", "quote \" and back\\slash", "/slash/", "\u0001\u001f\u007f",
         "\u2028\u2029", "e\u0301 combining", "𠮷野家", "ｶﾀｶﾅ", "　全角スペース", "<tag>&amp;", "zero\u0000byte"]
KNOWN_TOP = {"version", "revision", "title", "episode", "binding", "start_side", "strict_gates", "autosave", "font_path",
             "page_locks", "brush", "nombre", "spec", "bible", "tickets", "studio", "pages", "story", "min_reader",
             "writer", "book_id", "features", "timelapse"}
KNOWN_PAGE = {"id", "art_ok", "plan", "index", "note", "name_ok", "stage", "spread_with", "numero", "onion_from",
              "lt_threshold", "effects", "ruler", "rulers", "prims", "frames", "layers", "texts", "fills",
              "name_strokes", "ink_strokes", "cover", "anim", "assignee"}


def rand_float(rng: random.Random, geometric: bool = False) -> float:
    if geometric:
        return rng.choice([round(rng.uniform(-50, 400), rng.randrange(0, 6)), rng.uniform(0, 300), 0.1 + 0.2,
                           -0.0, 1 / 3, 2.675])
    if rng.random() < 0.4:
        return rng.choice(SPECIAL_FLOATS)
    return rng.choice([rng.uniform(-1e3, 1e3), rng.uniform(0, 1), round(rng.uniform(-1e6, 1e6), 3),
                       rng.uniform(-1, 1) * 10.0 ** rng.randint(-300, 300)])


def rand_num(rng: random.Random, geometric: bool = True):
    """An int or a float, as Python keeps either."""
    if rng.random() < 0.4:
        return rng.randint(-20, 400) if geometric else rng.choice(SPECIAL_INTS[:8])
    return rand_float(rng, geometric)


def rand_text(rng: random.Random) -> str:
    if rng.random() < 0.5:
        return rng.choice(TEXTS)
    alphabet = "abcXYZ019 あいう漢字🙂\"\\/\t\n\u00e9\u3000"
    return "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 12)))


def rand_key(rng: random.Random, avoid: set[str]) -> str:
    while True:
        key = rng.choice(["future_", "x_", "未来_", "k"]) + rand_text(rng)[:6].replace("\u0000", "")
        if key not in avoid:
            return key


def rand_json(rng: random.Random, depth: int = 0):
    kind = rng.randrange(10 if depth < 3 else 6)
    if kind == 0:
        return None
    if kind == 1:
        return rng.random() < 0.5
    if kind == 2:
        return rng.choice(SPECIAL_INTS) if rng.random() < 0.3 else rng.randint(-10**6, 10**6)
    if kind == 3:
        return rand_float(rng)
    if kind in (4, 5):
        return rand_text(rng)
    if kind in (6, 7):
        return [rand_json(rng, depth + 1) for _ in range(rng.randrange(4))]
    return {rand_key(rng, set()): rand_json(rng, depth + 1) for _ in range(rng.randrange(4))}


def rand_dict(rng: random.Random) -> dict:
    return {rand_key(rng, set()): rand_json(rng, 1) for _ in range(rng.randrange(1, 4))}


def rand_bytes(rng: random.Random) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + bytes(rng.randrange(256) for _ in range(rng.randrange(0, 40)))


def make_frames(rng: random.Random, page, models) -> list:
    """Split the root panel a few times; some panels get ints for their box, a polygon, bowed edges, rounded corners;
    some nodes are "free" (drawn panels in reading order)."""
    from genko import frames as framemod

    root = page.frames[0]
    for _ in range(rng.randrange(0, 5)):
        leaves = page.leaf_frames()
        target = rng.choice(leaves)
        try:
            page.split_frame(target.id, rng.choice(["horizontal", "vertical"]),
                             rng.choice([0.5, 0.25, 0.6, 1 / 3, 0.75]), rng.choice([4, 3.5, 5, 0]))
        except ValueError:
            pass
    nodes = []

    def walk(node):
        nodes.append(node)
        for child in node.children:
            walk(child)

    walk(root)
    for node in nodes:
        if not node.children:
            r = rng.random()
            if r < 0.2:
                node.rect = models.Rect(int(node.rect.x), int(node.rect.y), int(node.rect.width) or 1, int(node.rect.height) or 1)
            elif r < 0.35:
                pts = framemod.corners(node.rect)
                dx = rng.choice([0, 3, 7.5])
                pts = [(pts[0][0] + dx, pts[0][1]), pts[1], pts[2], (pts[3][0] - dx, pts[3][1])]
                if rng.random() < 0.5:
                    pts = [(int(x), int(y)) for x, y in pts]
                node.poly = pts
            if rng.random() < 0.2:
                node.curves = [rng.choice([0, 1, 2.5, -1.5]) for _ in range(len(framemod.shape(node)))]
            if rng.random() < 0.2:
                node.corner_mm = rng.choice([2, 3.5])
            if rng.random() < 0.3:
                node.panel = rand_dict(rng)
            if rng.random() < 0.2:
                node.line = {"kind": rng.choice(["solid", "double", "dashed"]), "rgb": [rng.randrange(256)] * 3}
        node.bleed = rng.random() < 0.2
        node.clip = rng.random() < 0.8
        node.border_mm = rng.choice([0.8, 1, 0.5, 1.25])
        node.custom = rng.random() < 0.2
        if node.children and rng.random() < 0.2:
            node.split = {"a": [0.0, 0.5], "b": [1.0, 0.5], "gutter_mm": rng.choice([4.0, 3])}
    if rng.random() < 0.3:
        # a "free" node: panels drawn one by one, read in rows
        free = models.Frame(id=models.new_id(), rect=root.rect, split_axis="free")
        kids = []
        for _ in range(rng.randrange(2, 6)):
            x, y = rng.uniform(root.rect.x, root.rect.x + 100), rng.uniform(root.rect.y, root.rect.y + 150)
            w, h = rng.choice([20, 35.5, 50]), rng.choice([30, 42.25, 60])
            child = models.Frame(id=models.new_id(), rect=models.Rect(x, y, w, h))
            if rng.random() < 0.4:
                child.poly = [(x, y), (x + w, y + 5), (x + w, y + h), (x, y + h - 5)]
            kids.append(child)
        free.children = kids
        return [free]
    return [root]


def make_strokes(rng: random.Random, models) -> list:
    out = []
    for _ in range(rng.randrange(0, 12)):
        n = rng.randrange(1, 14)
        points = [(rand_float(rng, True), rand_float(rng, True)) for _ in range(n)]
        if rng.random() < 0.1:
            points = [(rand_float(rng), rand_float(rng)) for _ in range(n)]
        pressure = [rng.random() for _ in range(n)] if rng.random() < 0.6 else []
        if pressure and rng.random() < 0.15:
            pressure = pressure[:-1] or [0.5, 0.5]  # (a pressure list of another length is kept, not used)
        out.append(models.Stroke(
            id=models.new_id(), points=points, pressure=pressure,
            width_mm=rng.choice([0.35, 0.5, 1.0, 0.1 + 0.2, 2.675]),
            kind=rng.choice(["gpen", "maru", "pencil", "brush", "毛筆"]),
            rgb=rng.choice([None, (200, 10, 10), (0, 0, 0)]),
            opacity=rng.choice([1.0, 0.5, 0.25, 1 / 3]),
            rotation=[rng.uniform(-180, 180) for _ in range(n)] if rng.random() < 0.2 else [],
            pressure_opacity=rng.choice([0.0, 0.0, 0.3, 1.0])))
    return out


def make_layers(rng: random.Random, page, models, store) -> None:
    from genko.models import Layer, LayerKind, LayerRole

    for layer in page.layers:
        if layer.kind == LayerKind.STROKES:
            layer.strokes = make_strokes(rng, models)
    leaf_ids = [f.id for f in page.leaf_frames()]
    for _ in range(rng.randrange(0, 5)):
        kind = rng.choice(list(LayerKind))
        role = rng.choice(list(LayerRole))
        layer = Layer(id=models.new_id(), role=role, kind=kind, exportable=rng.random() < 0.7, visible=rng.random() < 0.8)
        layer.title = rand_text(rng)
        layer.blend = rng.choice(["normal", "multiply", "screen"])
        layer.opacity = rng.choice([1.0, 0.5, 1, 0.75])
        layer.angle = rng.choice([45.0, 30, 0.1 + 0.2])
        layer.clip = rng.random() < 0.2
        layer.lock_alpha = rng.random() < 0.2
        layer.locked = rng.random() < 0.2
        layer.panel_clip = rng.random() < 0.7
        layer.panel_each = rng.random() < 0.3
        layer.reference = rng.random() < 0.2
        layer.color_prints = rng.random() < 0.2
        if rng.random() < 0.3:
            layer.color = tuple(rng.randrange(256) for _ in range(3))
        if rng.random() < 0.2:
            layer.material_id = rand_text(rng) or "m1"
        if rng.random() < 0.2:
            layer.parent_id = models.new_id()
        if kind == LayerKind.STROKES:
            layer.strokes = make_strokes(rng, models)
        elif kind == LayerKind.RASTER:
            layer.raster_png = rand_bytes(rng)
            if rng.random() < 0.6:
                layer.patches = [{"id": models.new_id(), "box": [rand_num(rng), rand_num(rng), 10, 12.5],
                                  "mode": rng.choice(["mask", "image"]), "png": rand_bytes(rng),
                                  "rgb": [1, 2, 3], "opacity": rng.choice([1.0, 0.5])}
                                 for _ in range(rng.randrange(1, 3))]
        elif kind == LayerKind.FILL:
            layer.fill_rgb = (rng.randrange(256), rng.randrange(256), rng.randrange(256))
            if rng.random() < 0.5:
                layer.fill = {"rgb": list(layer.fill_rgb)} if rng.random() < 0.5 else {
                    "gradient": {"from": [0, 0], "to": [10, 20.5], "rgb_from": [0, 0, 0], "rgb_to": [255, 255, 255]}}
        elif kind == LayerKind.TONE:
            layer.lpi = rng.choice([60, 60.0, 85.5, None])
            layer.density = rng.choice([0.3, 30, None, 1 / 3])
            layer.tone = {"pattern": rng.choice(["dot", "line"]), "gradient": rng.random() < 0.5}
            if rng.random() < 0.5:
                layer.region = [(rand_num(rng), rand_num(rng)) for _ in range(rng.randrange(3, 6))]
        elif kind == LayerKind.ADJUST:
            layer.adjust = {"kind": rng.choice(["levels", "hue"]), "amount": rand_float(rng, True)}
        elif kind == LayerKind.PLACED:
            layer.asset = store.put_bytes(rand_bytes(rng), ".png")
            layer.frame_id = rng.choice(leaf_ids) if leaf_ids else None
            layer.placement_mm = models.Rect(rand_num(rng), rand_num(rng), rand_num(rng), rand_num(rng))
            layer.fit = rng.choice(["cover", "contain", "stretch"])
            layer.clip_to = rng.choice(["frame", "bleed", "none"])
            layer.source = rng.choice([None, {"candidate": models.new_id(), "request": "r1"}])
            layer.finish = rng.choice([None, {"mono": True, "threshold": 128}])
        if rng.random() < 0.2:
            layer.effect = {"border": {"width_mm": 0.5, "rgb": [255, 255, 255]}}
        if rng.random() < 0.15:
            layer.screen = {"pattern": "dot", "lpi": 60, "angle": 45.0}
        if rng.random() < 0.15 and kind != LayerKind.PLACED:
            layer.source = {"from": rand_text(rng)}
        if rng.random() < 0.2:
            layer.mask = {"png": rand_bytes(rng) + b"mask", "enabled": rng.random() < 0.7}
        page.layers.append(layer)


def make_story(rng: random.Random, episode, models) -> None:
    for page in episode.pages:
        for _ in range(rng.randrange(0, 4)):
            leaves = page.leaf_frames()
            line = episode.add_line(page.index, rand_text(rng) or "セリフ", speaker=rand_text(rng),
                                    frame_id=rng.choice([None] + [f.id for f in leaves]),
                                    x_mm=rand_num(rng), y_mm=rand_num(rng), w_mm=rng.choice([40, 30.5]),
                                    h_mm=rng.choice([20, 60.25]), balloon=rng.choice(["speech", "thought", "none"]))
            line.wrap = rng.choice(["vertical", "horizontal"])
            line.ruby = rng.choice(["", "とうきょう"])
            if rng.random() < 0.5:
                line.ruby_runs = [("東京", "とうきょう")] + ([("a", "b", "c")] if rng.random() < 0.3 else [])
            if rng.random() < 0.3:
                line.emphasis_runs = ["へ", "東"]
            if rng.random() < 0.3:
                line.style_runs = [["本当", {"scale": 1.4, "bold": 2}], ["x", {"rgb": [1, 2, 3]}]]
            if rng.random() < 0.3:
                line.style = {"font": "mincho", "size_mm": rng.choice([4, 4.5]), "tcy": True}
            if rng.random() < 0.3:
                line.tail = (rand_num(rng), rand_num(rng))
            if rng.random() < 0.2:
                line.tails = [{"to": [1.0, 2.0], "via": None, "width_mm": 1.5}]
            if rng.random() < 0.2:
                line.path = [(rand_num(rng), rand_num(rng)) for _ in range(rng.randrange(3, 6))]


def make_random_book(rng: random.Random, dest: Path) -> None:
    from genko import models
    from genko.assets import AssetStore
    from genko.io import save_episode
    from genko.models import Binding, LayerRole, PageSpec

    spec = rng.choice([PageSpec.b4_comic, PageSpec.b5_doujin, PageSpec.a5_doujin, PageSpec.a4_mono, PageSpec.webtoon,
                       lambda: PageSpec.custom(200.5, 280, 180, 260.25, 3, 12, 12.5, 10, 8),
                       lambda: PageSpec.publisher("Shueisha"), lambda: PageSpec.publisher("someone")])()
    binding = rng.choice([Binding.RIGHT, Binding.LEFT])
    episode = models.new_episode(rand_text(rng) or "無題", rng.randint(1, 99), rng.randint(1, 5), spec, binding)
    store = AssetStore(dest)
    episode.start_side = rng.choice([None, "left", "right"])
    episode.strict_gates = rng.random() < 0.3
    episode.autosave = rng.random() < 0.3
    episode.font_path = rng.choice(["", "/fonts/明朝.otf"])
    episode.brush_rgb = (rng.randrange(256), rng.randrange(256), rng.randrange(256))
    episode.brush_width_mm = rng.choice([0.35, 0.5, 1.0, 0.1 + 0.2])
    episode.brush_stabilize = rng.randrange(0, 5)
    episode.brush_taper = rng.random() < 0.5
    episode.brush_curve = rng.choice(["linear", "soft", "hard"])
    if rng.random() < 0.3:
        episode.brush_custom = {"my-pen": {"base": "gpen", "width_mm": 0.4, "taper": True}}
    if rng.random() < 0.4:
        episode.nombre = {"on": True, "start": rng.randint(1, 5), "size_mm": 3.5}
    episode.tickets = [rand_dict(rng) for _ in range(rng.randrange(0, 3))]
    episode.studio = rand_dict(rng) if rng.random() < 0.5 else {}
    episode.bible.plot = rand_text(rng)
    episode.bible.characters = [{"id": models.new_id(), "name": rand_text(rng), "locked": rng.random() < 0.5}
                                for _ in range(rng.randrange(0, 3))]
    episode.bible.constraints = [rand_text(rng) for _ in range(rng.randrange(0, 3))]
    for _ in range(rng.randrange(0, 4)):
        episode.extra[rand_key(rng, KNOWN_TOP)] = rand_json(rng)
    for i, page in enumerate(episode.pages):
        page.frames = make_frames(rng, page, models)
        make_layers(rng, page, models, store)
        page.note = rand_text(rng)
        page.name_ok = rng.random() < 0.5
        page.art_ok = rng.random() < 0.3
        page.stage = rng.choice(["name", "ink", "finish"])
        page.numero = rng.random() < 0.8
        page.spread_with = rng.choice([None, page.index + 1])
        page.onion_from = rng.choice([None, 1])
        page.lt_threshold = rng.choice([None, 128, 0.5])
        page.plan = rng.choice([None, {"turn_role": "hook", "slots": [1, 2]}])
        page.effects = [{"id": models.new_id(), "kind": rng.choice(["speed", "flash"]), "frame_id": None,
                         "params": rng.choice([{}, {"count": 40, "jitter": 0.25}])} for _ in range(rng.randrange(0, 3))]
        page.rulers = [{"kind": "perspective", "vp": [rand_num(rng), rand_num(rng)]} for _ in range(rng.randrange(0, 2))]
        page.ruler = rng.choice([None, {"vp": [1, 2]}])
        page.prims = [{"id": models.new_id(), "kind": "cube", **({"scene": "s1"} if rng.random() < 0.5 else {})}
                      for _ in range(rng.randrange(0, 2))]
        if rng.random() < 0.4:
            page.fills = {LayerRole.BG: (255, 255, 255)} if rng.random() < 0.5 else {LayerRole.TONE: (10, 20, 30),
                                                                                    LayerRole.BG: (1, 2, 3)}
        for _ in range(rng.randrange(0, 3)):
            page.extra[rand_key(rng, KNOWN_PAGE)] = rand_json(rng)
        if rng.random() < 0.2:
            page.extra["assignee"] = "ai:" + rand_text(rng)
        if rng.random() < 0.15:
            page.extra["anim"] = {"fps": 12, "frames": 24, "tracks": [{"layer": "x"}]}
        if i == len(episode.pages) - 1 and rng.random() < 0.3:
            page.extra["cover"] = {"kind": rng.choice(["front", "back", "jacket", "obi"]), "spine_mm": 12.5, "flap_mm": 80}
    if rng.random() < 0.5:
        episode.page_locks = {episode.pages[0].id: "human:作者"}
        if rng.random() < 0.5:
            episode.page_locks[str(episode.pages[-1].index)] = "ai:hermes"  # (v2 keyed locks by page number)
    make_story(rng, episode, models)
    save_episode(episode, dest, actor="human:作者")


def _tiny_png(rgba=(200, 30, 30, 255), size=(4, 3)) -> bytes:
    import io

    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGBA", size, rgba).save(buf, format="PNG")
    return buf.getvalue()


def make_opsbook(dest: str) -> None:
    """The book the op contract cases run on (native/tests/contract/ops_cases.json names its ids; they come from
    the counting ids, so the book is the same every time): six pages and a front cover, with split, cut, drawn,
    stacked and slanted panels, panel briefs, placed art, lines with tails, pen / paint / tone / fill / folder layers,
    patches, rulers of every kind (and the old single one), a spread, the book's own brushes, a page lock and
    tickets."""
    import base64

    from genko import models
    from genko.assets import AssetStore
    from genko.headless import apply_ops
    from genko.io import save_episode
    from genko.models import Binding, Layer, LayerKind, LayerRole, PageSpec, Rect

    fresh_process_state(True)
    root = Path(dest)
    root.mkdir(parents=True, exist_ok=True)
    store = AssetStore(root)
    episode = models.new_episode("操作の試験", 3, 6, PageSpec.b5_doujin(), Binding.RIGHT)

    def run(ops, agent="genko"):
        reply = apply_ops(episode, ops, agent=agent)
        if not reply.get("ok"):
            raise SystemExit(f"make-opsbook: {ops}: {reply}")
        return reply

    def page(n):  # (apply_ops replaces the pages with their copies: look them up again)
        return next(p for p in episode.pages if p.index == n)

    # page 1: a stacked split, its top half split again; a brief on a panel; lines with tails; placed art
    run([{"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.4, "gutter_mm": 5}])
    top = page(1).frames[0].children[0]
    run([{"op": "split_frame", "page": 1, "frame_id": top.id, "axis": "vertical", "ratio": 0.5, "gutter_mm": 3}])
    leaves1 = page(1).leaf_frames()
    leaves1[0].panel = {"shot": "close-up", "notes": ["目線"]}
    leaves1[2].panel = {"shot": "wide"}
    run([{"op": "add_line", "page": 1, "text": "こんにちは", "frame_id": leaves1[0].id, "x_mm": 120, "y_mm": 40,
          "w_mm": 20, "h_mm": 30, "tails": [{"to": [118, 75], "via": [121, 72]}]},
         {"op": "add_line", "page": 1, "text": "外の台詞", "x_mm": 30, "y_mm": 200, "tail": [60, 230]},
         {"op": "add_line", "page": 1, "text": "枠の台詞", "frame_id": leaves1[2].id, "x_mm": 40, "y_mm": 150,
          "tails": [{"to": [50, 170], "vias": [[45, 160], [48, 165]]}]}])
    art = store.put_bytes(_tiny_png(), ".png")
    placed = Layer(id=models.new_id(), role=LayerRole.USER, kind=LayerKind.PLACED, title="配置画像", asset=art,
                   frame_id=leaves1[1].id, placement_mm=Rect(20, 30, 60.5, 40), fit="contain", clip_to="frame",
                   source={"candidate": "c1", "request": "r1"})
    page(1).layers.append(placed)
    run([{"op": "name_ok", "page": 1}], agent="human:作者")
    # page 2: drawn panels (a box and a polygon); the spread 2–3
    run([{"op": "add_frame", "page": 2, "rect": [20, 25, 70, 90], "id": "drawn-a"},
         {"op": "add_frame", "page": 2, "points": [[100, 25], [170, 30], [165, 120], [95, 110]], "id": "drawn-b"},
         {"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])
    # page 3: a brief on the root, then a slanted cut; a line in the lower panel; a tone with a patch
    page(3).frames[0].panel = {"shot": "establishing"}
    run([{"op": "cut_frame", "page": 3, "p0": [10, 120], "p1": [200, 140], "gutter_mm": 6}])
    run([{"op": "add_line", "page": 3, "text": "下の台詞", "x_mm": 60, "y_mm": 180, "w_mm": 30, "h_mm": 25,
          "tails": [{"to": [70, 220]}]},
         {"op": "add_tone", "page": 3, "id": "tone-1", "area": {"poly": [[30, 40], [90, 40], [90, 90]]}}])
    # page 4: strokes, pen / paint / fill / folder layers, a locked layer, patches, a raster; the old single ruler
    run([{"op": "add_stroke", "page": 4, "layer": "name", "points": [[30, 30], [60, 40], [90, 35]]},
         {"op": "add_stroke", "page": 4, "layer": "name", "points": [[30, 80, 0.4], [70, 90, 0.8], [110, 85, 0.6]]},
         {"op": "add_stroke", "page": 4, "layer": "ink", "points": [[40, 50, 0.5], [80, 52, 0.9], [120, 60, 0.7],
                                                                   [150, 90, 0.3]], "width_mm": 0.6},
         {"op": "add_stroke", "page": 4, "layer": "ink", "points": [[40, 120], [160, 125]], "rgb": [10, 20, 200]},
         {"op": "add_stroke", "page": 4, "layer": "ink", "points": [[100, 100], [100, 160]], "kind": "maru",
          "rotation": [0, 45], "opacity": 0.5, "pressure_opacity": 0.4},
         {"op": "add_layer", "page": 4, "kind": "pen", "id": "pen-1", "name": "線画"},
         {"op": "add_stroke", "page": 4, "layer_id": "pen-1", "points": [[20, 20, 0.5], [40, 25, 0.6], [60, 22, 0.7],
                                                                        [80, 30, 0.8], [100, 28, 0.9]]},
         {"op": "add_layer", "page": 4, "kind": "paint", "id": "paint-1", "name": "塗り"},
         {"op": "fill_area", "page": 4, "layer_id": "paint-1", "area": {"poly": [[10, 10], [50, 10], [50, 40]]},
          "rgb": [250, 200, 0]},
         {"op": "add_layer", "page": 4, "kind": "fill", "id": "fill-1", "rgb": [240, 240, 255]},
         {"op": "add_layer", "page": 4, "kind": "folder", "id": "folder-1", "name": "フォルダー"},
         {"op": "add_layer", "page": 4, "kind": "pen", "id": "pen-2", "parent": "folder-1", "after": "folder-1"},
         {"op": "add_layer", "page": 4, "kind": "adjust", "id": "adjust-1", "adjust": {"kind": "levels", "black": 10}},
         {"op": "add_layer", "page": 4, "kind": "pen", "id": "locked-1"},
         {"op": "set_layer", "page": 4, "id": "locked-1", "locked": True},
         {"op": "put_raster", "page": 4, "layer": "bg", "png_base64": base64.b64encode(_tiny_png((255, 255, 255, 255))).decode()},
         {"op": "set_ruler", "page": 4, "kind": "perspective", "points": [[100, 40]]}])
    # page 5: a ruler of every kind; a line drawn; locked by an AI
    root5 = page(5).frames[0].id
    rulers = [{"kind": "line", "points": [[20, 100], [180, 110]], "id": "r-line"},
              {"kind": "curve", "points": [[20, 150], [60, 140], [100, 160], [140, 150]], "id": "r-curve", "frame_id": root5},
              {"kind": "parallel", "angle": 30, "id": "r-parallel"},
              {"kind": "concentric", "points": [[100, 150]], "ratio": 0.5, "angle": 20, "id": "r-circle"},
              {"kind": "radial", "points": [[100, 140]], "id": "r-radial"},
              {"kind": "perspective", "points": [[100, -50]], "id": "r-persp"},
              {"kind": "perspective", "points": [[-200, 60], [400, 60]], "id": "r-persp2", "grid": 6},
              {"kind": "symmetry", "points": [[104, 0], [104, 283]], "id": "r-sym"},
              {"kind": "symmetry", "points": [[100, 140], [100, 100]], "copies": 3, "mirror": True, "id": "r-sym3",
               "layer_id": "pen-5"},
              {"kind": "guide", "axis": "h", "at": 50, "id": "r-guide"},
              {"kind": "parallel_curve", "points": [[30, 60], [80, 50], [130, 70]], "id": "r-pcurve"},
              {"kind": "multi_curve", "points": [[30, 230], [100, 220], [170, 235]],
               "points2": [[30, 260], [100, 250], [170, 262]], "id": "r-mcurve"},
              {"kind": "radial_curve", "points": [[40, 100], [80, 90], [120, 110]], "center": [100, 200], "id": "r-rcurve"},
              {"kind": "rect", "points": [[50, 50], [150, 120]], "angle": 10, "id": "r-rect"},
              {"kind": "ellipse", "points": [[60, 160], [140, 220]], "id": "r-ellipse"},
              {"kind": "polygon", "points": [[20, 20], [60, 25], [50, 70]], "id": "r-polygon"}]
    run([{"op": "add_layer", "page": 5, "kind": "pen", "id": "pen-5"}]
        + [{"op": "add_ruler", "page": 5, **ruler} for ruler in rulers]
        + [{"op": "add_stroke", "page": 5, "layer": "ink", "points": [[30, 200], [90, 210]]},
           {"op": "lock_page", "page": 5, "agent": "ai:other"}])
    # page 6: stacked splits (made before cuts were stored: no "split"); a line in the bottom panel
    stack = page(6)
    a, b = stack.split_frame(stack.frames[0].id, axis="horizontal", ratio=0.5, gutter_mm=4)
    stack.split_frame(a.id, axis="vertical", ratio=0.4, gutter_mm=3)
    run([{"op": "add_line", "page": 6, "text": "積み", "x_mm": 50, "y_mm": 160, "frame_id": b.id}])
    # a front cover at the end (page 7)
    run([{"op": "add_cover", "kind": "front"}])
    episode.brush_custom = {"my_soft": {"label": "やわらか", "base": "gpen", "post_smooth": 2, "width_mm": 0.6},
                            "my_mix": {"label": "混色", "mix": 0.4, "stretch": 0.5},
                            "my_bad": {"label": "", "width_mm": 0.5}}
    episode.tickets = [{"id": "t1", "page_index": 3, "page_id": page(3).id, "role": "bg", "status": "open"},
                       {"id": "t2", "page_index": 5, "page_id": page(5).id, "role": "ink", "status": "open"}]
    episode.nombre = {"start": 1}
    save_episode(episode, root, actor="human:作者")


def make_drawbook(dest: str) -> None:
    """The v3 book test_contract_drawn_by_ops draws after the ops: two A4 pages with nothing the C++ build does not draw
    yet (no lines, nombres off): page 1 split in three panels, page 2 one panel with a fill layer and a pen layer."""
    from genko import models
    from genko.headless import apply_ops
    from genko.io import save_episode
    from genko.models import Binding, PageSpec

    fresh_process_state(True)
    root = Path(dest)
    root.mkdir(parents=True, exist_ok=True)
    episode = models.new_episode("線の試験", 1, 2, PageSpec.a4_mono(), Binding.RIGHT)
    episode.nombre = {"show": False}

    def run(ops):
        reply = apply_ops(episode, ops, agent="human:作者")
        if not reply.get("ok"):
            raise SystemExit(f"make-drawbook: {ops}: {reply}")

    root1 = episode.pages[0].frames[0].id
    run([{"op": "split_frame", "page": 1, "frame_id": root1, "axis": "horizontal", "ratio": 0.45, "gutter_mm": 6},
         {"op": "name_ok", "page": 1}])
    top = episode.pages[0].frames[0].children[0].id
    run([{"op": "split_frame", "page": 1, "frame_id": top, "axis": "vertical", "ratio": 0.4, "gutter_mm": 3},
         {"op": "add_layer", "page": 2, "kind": "fill", "id": "fill-2", "rgb": [235, 240, 250]},
         {"op": "add_layer", "page": 2, "kind": "pen", "id": "pen-2", "name": "線画"},
         {"op": "set_frame", "page": 2, "frame_id": episode.pages[1].frames[0].id, "corner_mm": 6,
          "line": {"kind": "double", "rgb": [40, 40, 90]}}])
    save_episode(episode, root, actor="human:作者")


def _picture_png(size, shapes, mode="RGBA", background=(0, 0, 0, 0)) -> bytes:
    """A small picture drawn with Pillow: [(kind, box, colour)] with kind rectangle, ellipse or line."""
    from PIL import Image, ImageDraw

    im = Image.new("RGBA", size, background)
    d = ImageDraw.Draw(im)
    for kind, box, colour in shapes:
        if kind == "line":
            d.line(box, fill=colour, width=6)
        else:
            getattr(d, kind)(box, fill=colour)
    buf = io.BytesIO()
    (im if mode == "RGBA" else im.convert(mode)).save(buf, format="PNG")
    return buf.getvalue()


def make_rasterbook(dest: str) -> None:
    """The book the raster op contract tests run on (native/tests/contract/raster_cases.json names its layers; ids are
    counted, so it is the same book every time): four small pages (70 × 95 mm) with lines that close areas for the
    fills, paint layers with pixels, pen layers, a folder, fill, gradient and correction layers, a mask, a locked
    layer, a reference layer and a kept area. Pages 1, 2 and 4 carry nothing the C++ build does not draw yet (no
    nombre, no lines, no tones); page 3 carries a nombre, a line and a tone (for what the C++ build refuses as not yet
    ported)."""
    import base64

    from genko import models
    from genko.headless import apply_ops
    from genko.io import save_episode
    from genko.models import Binding, PageSpec

    fresh_process_state(True)
    root = Path(dest)
    root.mkdir(parents=True, exist_ok=True)
    episode = models.new_episode("塗りの試験", 1, 4, PageSpec.custom(70, 95, 60, 85, 3, 8, 8, 7, 6), Binding.RIGHT)

    def run(ops, agent="human:作者"):
        reply = apply_ops(episode, ops, agent=agent)
        if not reply.get("ok"):
            raise SystemExit(f"make-rasterbook: {ops}: {reply}")

    def b64png(data: bytes) -> str:
        return base64.b64encode(data).decode("ascii")

    def loop(points):
        return [list(p) for p in points] + [list(points[0])]

    circle = [(45 + 7 * math.cos(math.tau * k / 24), 30 + 7 * math.sin(math.tau * k / 24)) for k in range(24)]
    paint = _picture_png((551, 748), [("rectangle", (60, 80, 220, 260), (200, 40, 40, 255)),
                                      ("ellipse", (300, 120, 480, 300), (30, 90, 200, 200)),
                                      ("line", (80, 500, 470, 640), (20, 20, 20, 255)),
                                      ("rectangle", (330, 420, 345, 435), (10, 10, 10, 255))])
    small = _picture_png((120, 160), [("ellipse", (10, 10, 110, 150), (240, 180, 20, 255))], mode="RGB")
    root1 = episode.pages[0].frames[0].id
    run([{"op": "split_frame", "page": 1, "frame_id": root1, "axis": "vertical", "ratio": 0.5, "gutter_mm": 3},
         {"op": "name_ok", "page": 1}])
    run([{"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0, "points": loop([(15, 20), (30, 20), (30, 40), (15, 40)]),
          "width_mm": 0.8},
         {"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0, "points": loop(circle), "width_mm": 0.6},
         {"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0,
          "points": [[15, 50], [30, 50], [30, 70], [15, 70], [15, 52.5]], "width_mm": 0.7},
         {"op": "add_stroke", "page": 1, "layer": "ink", "stabilize": 0, "points": [[38, 55], [55, 75]], "width_mm": 1.2,
          "rgb": [120, 120, 120]},
         {"op": "add_layer", "page": 1, "kind": "paint", "id": "paint-1", "name": "塗り"},
         {"op": "put_raster", "page": 1, "id": "paint-1", "png_base64": b64png(paint)},
         {"op": "fill_area", "page": 1, "layer_id": "paint-1", "area": {"poly": [[40, 60], [55, 60], [50, 72]]}, "rgb": [0, 160, 80]},
         {"op": "add_layer", "page": 1, "kind": "pen", "id": "pen-1", "name": "線画"},
         {"op": "add_stroke", "page": 1, "layer_id": "pen-1", "stabilize": 0, "points": [[14, 78, 0.4], [30, 74, 0.9], [52, 80, 0.6]],
          "width_mm": 1.0, "rgb": [30, 30, 160]},
         {"op": "add_stroke", "page": 1, "layer_id": "pen-1", "stabilize": 0, "points": [[40, 15], [56, 18], [50, 44]], "width_mm": 0.5},
         {"op": "add_layer", "page": 1, "kind": "folder", "id": "fold-1", "name": "フォルダー"},
         {"op": "add_layer", "page": 1, "kind": "pen", "id": "in-a", "parent": "fold-1", "after": "fold-1"},
         {"op": "add_stroke", "page": 1, "layer_id": "in-a", "stabilize": 0, "points": [[20, 60], [26, 66]], "width_mm": 1.5},
         {"op": "add_layer", "page": 1, "kind": "paint", "id": "in-b", "parent": "fold-1", "after": "in-a"},
         {"op": "fill_area", "page": 1, "layer_id": "in-b", "area": {"rect": [44, 48, 8, 6]}, "rgb": [250, 120, 0], "opacity": 0.6},
         {"op": "add_layer", "page": 1, "kind": "fill", "id": "fill-1", "rgb": [240, 230, 200]},
         {"op": "set_layer", "page": 1, "id": "fill-1", "opacity": 0.5, "blend": "multiply"},
         {"op": "add_layer", "page": 1, "kind": "adjust", "id": "adj-1", "adjust": {"kind": "levels", "black": 20, "white": 230}},
         {"op": "add_layer", "page": 1, "kind": "paint", "id": "mask-1"},
         {"op": "put_raster", "page": 1, "id": "mask-1", "png_base64": b64png(small)},
         {"op": "set_layer_mask", "page": 1, "id": "mask-1", "area": {"poly": [[10, 10], [40, 12], [30, 45]]}},
         {"op": "add_layer", "page": 1, "kind": "paint", "id": "lock-1"},
         {"op": "set_layer", "page": 1, "id": "lock-1", "locked": True},
         {"op": "add_layer", "page": 1, "kind": "pen", "id": "ref-1", "name": "参照"},
         {"op": "add_stroke", "page": 1, "layer_id": "ref-1", "stabilize": 0, "points": loop([(40, 50), (56, 50), (56, 66), (40, 66)]),
          "width_mm": 0.6},
         {"op": "set_layer", "page": 1, "id": "ref-1", "reference": True},
         {"op": "store_area", "page": 1, "name": "空", "area": {"rect": [12, 12, 20, 10]}},
         {"op": "store_area", "page": 1, "name": "丸", "area": {"ellipse": [40, 20, 12, 12]}}])
    # page 2: not approved (the name is what a fill looks at): name lines, a draft, ink shapes, a gradient layer under
    # them
    run([{"op": "add_stroke", "page": 2, "layer": "name", "stabilize": 0, "points": loop([(14, 14), (40, 14), (40, 36), (14, 36)]),
          "width_mm": 0.5},
         {"op": "add_stroke", "page": 2, "layer": "draft", "stabilize": 0, "points": [[20, 50], [50, 50], [50, 70]], "width_mm": 0.8},
         {"op": "add_stroke", "page": 2, "layer": "ink", "stabilize": 0, "points": loop([(18, 45), (48, 45), (48, 75), (18, 75)]),
          "width_mm": 0.7},
         {"op": "add_layer", "page": 2, "kind": "gradient", "id": "grad-2", "after": episode.pages[1].layers[0].id,
          "gradient": {"from": [10, 10], "to": [60, 80], "rgb_from": [255, 200, 200], "rgb_to": [200, 200, 255]}},
         {"op": "add_layer", "page": 2, "kind": "paint", "id": "paint-2"}])
    # page 3: what the C++ build does not draw yet: a nombre, a line, a tone
    run([{"op": "add_stroke", "page": 3, "layer": "ink", "stabilize": 0, "points": loop([(15, 20), (45, 20), (45, 50), (15, 50)]),
          "width_mm": 0.7},
         {"op": "add_line", "page": 3, "text": "台詞", "x_mm": 20, "y_mm": 60},
         {"op": "add_tone", "page": 3, "id": "tone-3", "area": {"poly": [[20, 25], [40, 25], [40, 45]]}},
         {"op": "add_layer", "page": 3, "kind": "paint", "id": "paint-3"}])
    # page 4: plain
    run([{"op": "add_stroke", "page": 4, "layer": "ink", "stabilize": 0, "points": loop([(14, 16), (54, 16), (54, 78), (14, 78)]),
          "width_mm": 1.0},
         {"op": "add_layer", "page": 4, "kind": "paint", "id": "blank-4"},
         {"op": "add_layer", "page": 4, "kind": "pen", "id": "multi-4"},
         {"op": "add_stroke", "page": 4, "layer_id": "multi-4", "stabilize": 0, "points": [[20, 30], [48, 60]], "width_mm": 3.0,
          "rgb": [200, 60, 60]},
         {"op": "set_layer", "page": 4, "id": "multi-4", "blend": "multiply", "opacity": 0.6}])
    for page in episode.pages:
        page.numero = page.index == 3
    save_episode(episode, root, actor="human:作者")


def show_cases(cases_path: str, book: str, only: str | None) -> int:
    """Each case of a contract case file (ops_cases.json, raster_cases.json) run on `book` as the tests run it: its
    last reply, flagged where it does not do what the case says (marked ok or failing; a failure in another op). For
    writing cases; the tests check the same."""
    import copy

    from genko.headless import apply_ops
    from genko.io import load_episode
    from genko.ops import ApplyError

    data = json.loads(Path(cases_path).read_text(encoding="utf-8"))
    flagged = 0
    for case in data["cases"]:
        if only and only not in case["n"] and only != case["op"]:
            continue
        steps = case.get("steps") or [{"ops": case["ops"], **({"agent": case["agent"]} if "agent" in case else {}),
                                       **({"dry_run": case["dry"]} if "dry" in case else {})}]
        fresh_process_state(True)
        episode = load_episode(Path(book))
        counting_ids(int(data.get("first_id", 1)))
        reply = {}
        for step in steps:
            try:
                reply = apply_ops(episode, copy.deepcopy(step["ops"]), dry_run=bool(step.get("dry_run")),
                                  agent=step.get("agent", "genko"))
            except ApplyError as exc:
                reply = {"ok": False, "error": str(exc)}
            except Exception as exc:
                reply = {"ok": False, "error": str(exc), "uncaught": type(exc).__name__}
        flag = "" if reply.get("ok") == case["ok"] else f"  <<< marked {case['ok']}"
        if (not reply.get("ok") and "uncaught" not in reply and not case.get("other") and not case["op"].startswith("_")
                and f"] {case['op']}: " not in reply.get("error", "")):
            flag += "  <<< in another op"
        flagged += bool(flag)
        shown = {k: v for k, v in reply.items() if k in ("ok", "error", "uncaught", "warnings", "results")}
        print(f"{case['n']} => {dumps(shown)[:400]}{flag}")
    print(f"{flagged} flagged")
    return 1 if flagged else 0


def filter_edges(out: str) -> None:
    """Pictures made to sit on the edges of the filters' float32 decisions, and filters.apply_filter of each, into OUT
    (cases.json: [{"name", "kind", "params", "input", "output"}], the pictures as PNG): despeckle's ink (numpy's float32
    luminance below 128) for every colour whose luminance is within 0.03 of 128, each pixel alone on paper (a speck
    taken away when it is ink) or alone in ink (a hole filled when it is not); lineart's ratio (a grey over the
    lightest grey around it, in float32, against the threshold) for the greys g and m whose g / m is next to it."""
    from PIL import Image

    from genko import filters

    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    cases = []

    def run(name, image, kind, params):
        image = image.convert("RGBA")
        image.save(root / f"{name}-in.png")
        filters.apply_filter(image, kind, dict(params)).save(root / f"{name}-out.png")
        cases.append({"name": name, "kind": kind, "params": params, "input": f"{name}-in.png", "output": f"{name}-out.png"})

    triples = []
    for r in range(256):
        for g in range(256):
            base = 0.299 * r + 0.587 * g
            b0 = int((128 - base) / 0.114)
            for b in range(b0 - 1, b0 + 3):
                if 0 <= b <= 255 and abs(base + 0.114 * b - 128) < 0.03:
                    triples.append((r, g, b))
    half = int(math.ceil(math.sqrt(len(triples))))
    for what, background in (("ink", (255, 255, 255, 255)), ("holes", (0, 0, 0, 255))):
        im = Image.new("RGBA", (2 * half, 2 * half), background)
        px = im.load()
        for k, (r, g, b) in enumerate(triples):
            px[(k % half) * 2, (k // half) * 2] = (r, g, b, 255)
        run(f"despeckle-{what}", im, "despeckle", {"size_px": 1.5, "what": what})
    for t in (0.55, 0.6, 0.65, 0.7, 0.72, 0.75, 0.8, 0.85, 0.9, 0.95):
        pairs = [(g, m) for m in range(1, 256) for g in range(m) if abs(g / m - t) <= 1.5 / m]
        cols = 64
        im = Image.new("RGB", (cols * 5, ((len(pairs) + cols - 1) // cols) * 5), (255, 255, 255))
        px = im.load()
        for k, (g, m) in enumerate(pairs):
            x0, y0 = (k % cols) * 5, (k // cols) * 5
            for dy in range(1, 4):
                for dx in range(1, 4):
                    px[x0 + dx, y0 + dy] = (m, m, m)
            px[x0 + 2, y0 + 2] = (g, g, g)
        run(f"lineart-{t}", im, "lineart", {"threshold": t, "radius": 3, "min_px": 0, "keep_solid": False})
    (root / "cases.json").write_text(dumps(cases), encoding="utf-8")


def make_random(out: str, seed: int, count: int) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        fresh_process_state(True)
        make_random_book(random.Random(seed * 1000 + i), root / f"book-{i:02d}.genko")


# --- random op sequences (the ops of M2-O1) ---------------------------------------------------------------------------

AGENTS = ["genko", "genko", "human:作者", "ai:hermes", "legacy:unknown", "ai:other"]
SEQ_FIRST_ID = 0x100000  # (new ids count from here: above every id a random book has)


class _Replay:
    """A sequence's book as test_contract_ops's Python side (steps_job) has it after each step, while the sequence is
    made: what an op will find in the book once the ops before it have changed it."""

    def __init__(self, book: Path, store: Path, first_id: int = SEQ_FIRST_ID):
        from genko.assets import AssetStore
        from genko.io import load_episode

        fresh_process_state(True)
        self.episode = load_episode(book)
        counting_ids(first_id)
        self.store = AssetStore(store)

    def layer_ids(self, step: dict, k: int):
        """The ids of the layers of the page that op k of `step` names, when op k runs (the ops before it in the
        batch applied); None when no op k runs (an op before fails) or there is no such page."""
        import copy

        import genko.models as models
        from genko.headless import apply_ops

        counter = models.uuid4
        mark = counter.next
        trial = copy.deepcopy(self.episode)
        try:
            if k > 0:
                apply_ops(trial, copy.deepcopy(step["ops"][:k]), dry_run=False, agent=step.get("agent", "genko"))
            index = int(step["ops"][k]["page"])
        except Exception:  # (the batch stops before op k, or op k names no page)
            return None
        finally:
            counter.next = mark  # (the ids the trial used are made again when the batch really runs)
        page = next((page for page in trial.pages if page.index == index), None)
        return [layer.id for layer in page.layers] if page is not None else None

    def apply(self, step: dict) -> None:
        """The step as steps_job applies it, with the payload and full snapshot it takes after each."""
        import copy

        from genko.headless import apply_ops
        from genko.io import _payload

        try:
            apply_ops(self.episode, copy.deepcopy(step["ops"]), dry_run=bool(step.get("dry_run")),
                      agent=step.get("agent", "genko"))
        except Exception:  # (refused, or one of the errors apply_ops lets through: the book is as it was)
            pass
        _payload(self.episode, self.store)
        _full_snapshot(self.episode)


def _book_pools(book: Path) -> dict:
    """What a sequence may name in a book: its pages, and on each its panels, layers and strokes."""
    from genko.io import load_episode
    from genko.models import LayerKind

    fresh_process_state(True)
    episode = load_episode(book)
    pages = []
    for page in episode.pages:
        nodes = []

        def walk(frame):
            nodes.append(frame)
            for child in frame.children:
                walk(child)

        for frame in page.frames:
            walk(frame)
        # (huge: a line with points far off the page — Python's eraser walks such a line in 0.2 mm steps and never
        # finishes; the sequences leave those layers alone)
        layers = [{"id": layer.id, "role": layer.role.value, "kind": layer.kind.value, "strokes": len(layer.strokes),
                   "pixels": bool(layer.raster_png) or (layer.kind == LayerKind.RASTER and bool(layer.patches)),
                   "huge": any(not abs(v) < 1e5 for s in layer.strokes for p in s.points for v in p[:2]),
                   "parent": layer.parent_id}
                  for layer in page.layers]
        pages.append({"index": page.index, "leaves": [f.id for f in page.leaf_frames()],
                      "splits": [f.id for f in nodes if f.children], "frames": [f.id for f in nodes], "layers": layers,
                      "w": float(page.spec.width_mm), "h": float(page.spec.height_mm)})
    huge = any(layer["huge"] for page in pages for layer in page["layers"])
    return {"pages": pages, "huge": huge}


def _sequence(rng: random.Random, pool: dict, replay: _Replay) -> list:
    """1 to 12 ops of M2-O1 for one book, in steps (most one op; some two or three; some dry runs), by several actors,
    with for_pages, strict_gates switched on and off, page locks and name approvals among them.

    Not made (the C++ build refuses them where Python goes on and breaks the book): a reorder_layers order that leaves
    a layer out, names one twice or one the page does not have (the order names the layers the page has when the op
    runs: `replay`); select_frame of what is not a panel of the page; a parent that is not a folder of the page;
    lock_page and unlock_page of a page the book does not have. The random draws for them are made as before, so the
    sequences keep their other ops. (What the ops before change in the book can still make one of the others refused:
    test_contract_ops stops comparing such a sequence there.)"""
    pages = pool["pages"]
    made = [f"{SEQ_FIRST_ID + k:012x}" for k in range(40)]  # (ids the sequence's own ops make)

    def pool_page(p):  # the page an op names, as the pool has it (None: not a page of the book)
        if isinstance(p, dict):
            return p
        return next((q for q in pages if str(q["index"]) == str(p)), None)

    def folder_or_none(p, layer_id, moved=None):  # layer_id when it is a folder of the page (not in `moved`)
        page = pool_page(p)
        layers = {layer["id"]: layer for layer in page["layers"]} if page else {}
        at, seen = layers.get(layer_id), set()
        if at is None or at["kind"] != "folder":
            return None
        while at is not None and at["id"] not in seen:  # (a folder is never put inside itself)
            if at["id"] == moved:
                return None
            seen.add(at["id"])
            at = layers.get(at["parent"] or "")
        return layer_id

    def pick(items, fallback="nope"):
        return rng.choice(items) if items and rng.random() < 0.9 else fallback

    def num(lo, hi, digits=None):
        v = rng.uniform(lo, hi)
        if digits is not None:
            v = round(v, digits)
        return rng.choice([v, v, v, int(v)])

    def page_of():
        if rng.random() < 0.06:
            return rng.choice([0, len(pages) + 1, "x", "1", None])
        return rng.choice(pages)

    def page_no(p):
        return p if not isinstance(p, dict) else p["index"]

    def points(p, n=None, pressure=None):
        n = n or rng.randint(2, 9)
        w, h = (p["w"], p["h"]) if isinstance(p, dict) else (200.0, 280.0)
        with_p = rng.random() < 0.5 if pressure is None else pressure
        out = []
        x, y = rng.uniform(-10, w), rng.uniform(-10, h)
        for _ in range(n):
            x, y = x + rng.uniform(-25, 25), y + rng.uniform(-25, 25)
            pt = [round(x, rng.choice([0, 1, 3, 6])), round(y, rng.choice([0, 1, 3, 6]))]
            if with_p:
                pt.append(round(rng.uniform(-0.1, 1.3), 3))
            out.append(pt)
        if rng.random() < 0.05:
            out[rng.randrange(len(out))] = rng.choice([[1], "ab", 5])
        return out

    def layer_of(p, safe=False):
        layers = p["layers"] if isinstance(p, dict) else []
        if safe:
            layers = [layer for layer in layers if not layer["pixels"] and not layer["huge"]]
            return rng.choice([layer["id"] for layer in layers] + ["nope"])
        return pick([layer["id"] for layer in layers] + made[:3])

    def frame_of(p, kind="leaves"):
        return pick((p[kind] if isinstance(p, dict) else []) + made[:6])

    def rgb():
        return rng.choice([[rng.randrange(256) for _ in range(3)], [1, 2], "123", [10, 20, 30, 40], None])

    def op_split(p):
        op = {"op": "split_frame", "page": page_no(p), "axis": rng.choice(["horizontal", "vertical", "vertical", "diag"])}
        if rng.random() < 0.6:
            op["frame_id"] = frame_of(p)
        if rng.random() < 0.6:
            op["ratio"] = num(0.05, 0.95, rng.choice([None, 2]))
        if rng.random() < 0.5:
            op["gutter_mm"] = num(0, 8)
        if rng.random() < 0.25:
            op["tilt_mm"] = num(-20, 20)
        if rng.random() < 0.3:
            op["force"] = True
        return op

    def op_cut(p):
        w, h = (p["w"], p["h"]) if isinstance(p, dict) else (200, 280)
        op = {"op": "cut_frame", "page": page_no(p), "p0": [num(0, w), num(0, h)], "p1": [num(0, w), num(0, h)]}
        if rng.random() < 0.5:
            y = rng.uniform(40, h - 40)
            op["p0"], op["p1"] = [0, round(y, 2)], [round(w, 2), round(y + rng.uniform(-30, 30), 2)]
        if rng.random() < 0.4:
            op["frame_id"] = frame_of(p)
        if rng.random() < 0.4:
            op["gutter_mm"] = num(0, 6)
        if rng.random() < 0.3:
            op["force"] = True
        return op

    def op_move_gutter(p):
        op = {"op": "move_gutter", "page": page_no(p), "frame_id": frame_of(p, "splits"), "delta_mm": num(-35, 35, 1)}
        if rng.random() < 0.4:
            op["index"] = rng.choice([0, 0, 1, 2, "0"])
        if rng.random() < 0.3:
            op["gutter_mm"] = num(0, 9)
        return op

    def op_merge(p):
        op = {"op": "merge_frame", "page": page_no(p), "frame_id": frame_of(p)}
        if rng.random() < 0.4:
            op["force"] = True
        return op

    def op_resize(p):
        w, h = (p["w"], p["h"]) if isinstance(p, dict) else (200, 280)
        x, y = rng.uniform(0, w * 0.6), rng.uniform(0, h * 0.6)
        return {"op": "resize_frame", "page": page_no(p), "frame_id": frame_of(p),
                "rect": {"x": num(x, x + 1, 2), "y": num(y, y + 1, 2), "width": num(5, w * 0.4, 2), "height": num(5, h * 0.4, 2)}}

    def op_set_frame(p):
        op = {"op": "set_frame", "page": page_no(p), "frame_id": frame_of(p, rng.choice(["leaves", "leaves", "frames"]))}
        for key in rng.sample(["bleed", "clip", "border_mm", "line", "corner_mm", "poly", "curves", "bow"], rng.randint(1, 3)):
            if key in ("bleed", "clip"):
                op[key] = rng.random() < 0.5
            elif key == "border_mm":
                op[key] = num(0, 3, 2)
            elif key == "line":
                op[key] = rng.choice([None, {"kind": rng.choice(["solid", "double", "dashed", "dotted", "rough", "wavy"]),
                                             "rgb": [1, 2, 3], "gap_mm": num(0, 12), "wobble_mm": num(0, 4)}])
            elif key == "corner_mm":
                op[key] = rng.choice([None, num(0, 60)])
            elif key == "poly":
                if rng.random() < 0.4:
                    op[key] = None
                else:
                    cx, cy = rng.uniform(40, 150), rng.uniform(40, 220)
                    op[key] = [[round(cx + rng.uniform(-40, 40), 2), round(cy + rng.uniform(-40, 40), 2)]
                               for _ in range(rng.randint(2, 6))]
            elif key == "curves":
                op[key] = rng.choice([None, [num(-4, 4, 2) for _ in range(rng.choice([4, 4, 3, 5]))]])
            else:
                op[key] = {"edge": rng.randint(-1, 4), "mm": num(-5, 5, 1)}
        return op

    def op_add_frame(p):
        op = {"op": "add_frame", "page": page_no(p)}
        if rng.random() < 0.5:
            op["rect"] = [num(0, 150), num(0, 200), num(-60, 80), num(-60, 80)]
        else:
            op["points"] = points(p, rng.randint(3, 14), False)
        if rng.random() < 0.2:
            op["id"] = rng.choice(["drawn-" + str(rng.randrange(3)), frame_of(p)])
        if rng.random() < 0.2:
            op["border_mm"] = num(0, 2)
        if rng.random() < 0.2:
            op["tolerance_mm"] = num(0.1, 3)
        return op

    def op_delete_frame(p):
        op = {"op": "delete_frame", "page": page_no(p), "frame_id": frame_of(p)}
        if rng.random() < 0.4:
            op["force"] = True
        return op

    def op_stroke(p):
        op = {"op": "add_stroke", "page": page_no(p), "points": points(p)}
        r = rng.random()
        if r < 0.35:
            op["layer"] = rng.choice(["ink", "name", "draft", "finish", "nope"])
        elif r < 0.6:
            op["layer_id"] = layer_of(p)
        for key in rng.sample(["stabilize", "taper", "pressure_gamma", "curve", "kind", "width_mm", "rgb", "opacity", "rotation",
                               "post_fit", "post_smooth", "snap_lines_mm", "space", "pressure_opacity", "taper_in_mm",
                               "stabilize_speed", "snap_ruler"], rng.randint(0, 4)):
            op[key] = {"stabilize": lambda: rng.choice([0, 3, 5, 9, "4"]), "taper": lambda: rng.random() < 0.7,
                       "pressure_gamma": lambda: num(0.1, 6), "curve": lambda: rng.choice(["gpen", "linear", "soft"]),
                       "kind": lambda: rng.choice(["gpen", "maru", "mili", "oil", "my-pen", "crayon", "fude"]),
                       "width_mm": lambda: num(0.1, 3, 2), "rgb": rgb, "opacity": lambda: num(-0.5, 1.5, 2),
                       "rotation": lambda: [num(-180, 180) for _ in range(rng.randint(1, 4))],
                       "post_fit": lambda: num(0.01, 4, 2), "post_smooth": lambda: rng.choice([0, 1, 3, 10]),
                       "snap_lines_mm": lambda: num(0.05, 12), "space": lambda: rng.choice(["page", "page", "spread", "x"]),
                       "pressure_opacity": lambda: num(0, 1.2, 2), "taper_in_mm": lambda: num(0, 90),
                       "stabilize_speed": lambda: True, "snap_ruler": lambda: True}[key]()
        return op

    def op_index(name, p):
        op = {"op": name, "page": page_no(p), "index": rng.choice([0, 0, 1, 2, 5, -1, "1"])}
        if rng.random() < 0.7:
            op["layer"] = rng.choice(["ink", "name", "name", "ink", "finish"])
        if name == "edit_stroke":
            op["points"] = points(p)
        if name == "simplify_stroke" and rng.random() < 0.5:
            op["epsilon_mm"] = num(0.01, 20)
        return op

    def op_erase(name, p):
        op = {"op": name, "page": page_no(p), "points": points(p, rng.randint(1, 5), False), "width_mm": num(0.3, 12, 1)}
        if rng.random() < 0.5 or pool["huge"]:  # (by role only where no layer of the book has a line far off)
            op["layer_id"] = layer_of(p, safe=True)
        else:
            op["layer"] = rng.choice(["ink", "name", "name", "draft", "finish"])
        if rng.random() < 0.5:
            op["mode"] = rng.choice(["cut", "whole", "to_crossing", "to_crossing", "smear"])
        if rng.random() < 0.15:
            op["texture"] = rng.choice(["hard", "soft", "rough", "glitter"])
        return op

    def op_add_layer(p):
        op = {"op": "add_layer", "page": page_no(p), "kind": rng.choice(["pen", "paint", "folder", "fill", "gradient",
                                                                          "adjust", "vector"])}
        if rng.random() < 0.3:
            op["id"] = rng.choice(["L" + str(rng.randrange(4)), layer_of(p)])
        if rng.random() < 0.3:
            op["after"] = layer_of(p)
        if rng.random() < 0.2:
            parent = folder_or_none(p, layer_of(p))
            if parent is not None:
                op["parent"] = parent
        if rng.random() < 0.2:
            op["blend"] = rng.choice(["multiply", "screen", "luminosity", "glow"])
        if op["kind"] == "fill" and rng.random() < 0.5:
            op["rgb"] = rgb()
        if op["kind"] == "gradient" and rng.random() < 0.5:
            op["gradient"] = {"from": [0, 0], "to": [num(10, 100), num(10, 200)], "shape": rng.choice(["linear", "radial", "x"]),
                              "stops": [[0, [0, 0, 0]], [num(0, 1.2, 2), [255, 0, 0], 0.5]]}
        if op["kind"] == "adjust" and rng.random() < 0.6:
            op["adjust"] = rng.choice([{"kind": "levels", "black": rng.choice([10, "x", 2.5])}, {"kind": "hue", "shift": 30},
                                       {"kind": "curve", "points": [[0, 0], [255, 255]]}, {"kind": "blur"}])
        return op

    def op_set_layer(p):
        op = {"op": "set_layer", "page": page_no(p)}
        if rng.random() < 0.8:
            op["id"] = layer_of(p)
        else:
            op["layer"] = rng.choice(["ink", "name", "bg", "draft", "tone", "xyz"])
        for key in rng.sample(["visible", "opacity", "exportable", "blend", "clip", "locked", "panel_clip", "panel_each", "name",
                               "color", "reference", "effect", "screen", "fill", "parent"], rng.randint(1, 3)):
            op[key] = {"opacity": num(0, 1.2, 2), "blend": rng.choice(["normal", "multiply", "dodge"]), "name": "名" + str(rng.randrange(9)),
                       "color": rgb(), "effect": rng.choice([None, {"border": {"width_mm": 1}}, {"glow": 1}]),
                       "screen": rng.choice([None, {"pattern": "dot", "lpi": 60}, {"lpi": 5}]),
                       "fill": rng.choice([None, {"rgb": [1, 2, 3]}]), "parent": rng.choice([None, layer_of(p)])
                       }.get(key, rng.random() < 0.5)
        if "parent" in op:
            op["parent"] = folder_or_none(p, op["parent"], op.get("id"))
        return op

    def op_set_layers(p):
        op = {"op": "set_layers", "page": page_no(p)}
        if rng.random() < 0.3:
            op["all"] = True
        else:
            op["ids"] = [layer_of(p) for _ in range(rng.randint(1, 3))]
        for key in rng.sample(["visible", "opacity", "locked", "blend", "color", "panel_clip"], rng.randint(0, 2)):
            op[key] = {"opacity": num(0, 1, 2), "blend": rng.choice(["multiply", "zz"]), "color": rgb()}.get(key, rng.random() < 0.5)
        return op

    def op_reorder_layers(p):
        ids = [layer["id"] for layer in (p["layers"] if isinstance(p, dict) else [])]
        rng.shuffle(ids)
        # (every layer once: the draws that once cut the order short or added an id are made and not used)
        if rng.random() < 0.3 and ids:
            rng.randint(0, len(ids))
        if rng.random() < 0.2:
            rng.choice(["nope", 5])
        if not isinstance(p, dict) and pool_page(p):  # (a page named by its text: its layers as they are)
            ids = [layer["id"] for layer in pool_page(p)["layers"]]
        return {"op": "reorder_layers", "page": page_no(p), "order": ids}

    def op_for_pages():
        inner = rng.choice([lambda: {"op": "set_note", "note": "毎"}, lambda: {"op": "set_note", "note": "選"},
                            lambda: {"op": "add_layer", "kind": "pen"}, lambda: {"op": "split_frame", "axis": "vertical"},
                            lambda: {"op": "add_stroke", "points": [[30, 40], [60, 70]]}, lambda: {"op": "name_ok"},
                            lambda: {"op": "set_layers", "all": True, "opacity": 0.5}])
        op = {"op": "for_pages", "ops": [inner() for _ in range(rng.randint(1, 2))]}
        r = rng.random()
        if r < 0.3:
            op["pages"] = "all"
        elif r < 0.6:
            op["pages"] = sorted({rng.randint(1, len(pages) + (1 if rng.random() < 0.1 else 0)) for _ in range(2)})
        return op

    def one_op():
        p = page_of()
        kind = rng.choice(["split_frame", "cut_frame", "move_gutter", "merge_frame", "resize_frame", "set_frame", "add_frame",
                           "delete_frame", "select_frame", "add_page", "delete_page", "duplicate_page", "reorder", "advance",
                           "name_ok", "lock_page", "unlock_page", "set_note", "set_meta", "set_autosave", "add_stroke",
                           "add_stroke", "add_stroke", "delete_stroke", "edit_stroke", "simplify_stroke", "erase",
                           "erase_raster", "add_layer", "delete_layer", "duplicate_layer", "set_layer", "set_layers",
                           "reorder_layers", "set_brush", "for_pages", "strict"])
        if kind == "split_frame":
            return op_split(p)
        if kind == "cut_frame":
            return op_cut(p)
        if kind == "move_gutter":
            return op_move_gutter(p)
        if kind == "merge_frame":
            return op_merge(p)
        if kind == "resize_frame":
            return op_resize(p)
        if kind == "set_frame":
            return op_set_frame(p)
        if kind == "add_frame":
            return op_add_frame(p)
        if kind == "delete_frame":
            return op_delete_frame(p)
        if kind == "select_frame":
            frame_id, page = frame_of(p), pool_page(p)
            if page and frame_id not in page["frames"]:  # (a panel of the page: its first, for an id it does not have)
                frame_id = page["leaves"][0] if page["leaves"] else page["frames"][0]
            return {"op": "select_frame", "page": page_no(p), "frame_id": frame_id}
        if kind == "add_page":
            op = {"op": "add_page", "count": rng.choice([1, 1, 2, 3, 0])}
            if rng.random() < 0.4:
                op["after"] = rng.randint(0, len(pages) + 1)
            return op
        if kind == "delete_page":
            return {"op": "delete_page", "page": page_no(p)}
        if kind == "duplicate_page":
            return {"op": "duplicate_page", "page": page_no(p), **({"next_to": True} if rng.random() < 0.5 else {})}
        if kind == "reorder":
            order = list(range(1, len(pages) + 1))
            rng.shuffle(order)
            if rng.random() < 0.15:
                order = order[:-1] or [1, 1]
            return {"op": "reorder", "order": order}
        if kind == "advance":
            return {"op": "advance", "page": page_no(p), "to": rng.choice(["name", "ink", "finish", "done"])}
        if kind == "name_ok":
            return {"op": "name_ok", **({"page": page_no(p)} if rng.random() < 0.7 else {})}
        if kind in ("lock_page", "unlock_page"):
            if p in (0, len(pages) + 1):  # (a page the book has; "x" and None name none, as in Python)
                p = pages[0]
            return {"op": kind, "page": page_no(p), **({"agent": rng.choice(AGENTS)} if kind == "lock_page" and rng.random() < 0.4 else {})}
        if kind == "set_note":
            return {"op": "set_note", "page": page_no(p), "note": rng.choice(["メモ", 5, None, "a\nb"])}
        if kind == "set_meta":
            return rng.choice([{"op": "set_meta", "title": "題" + str(rng.randrange(9))}, {"op": "set_meta", "binding": rng.choice(["left", "right", "up"])},
                               {"op": "set_meta", "start_side": rng.choice([None, "left", "right"])}, {"op": "set_meta", "episode": rng.choice([2, "3", "x"])}])
        if kind == "strict":
            return {"op": "set_meta", "strict_gates": rng.random() < 0.6}
        if kind == "set_autosave":
            return {"op": "set_autosave", "enabled": rng.random() < 0.5}
        if kind == "add_stroke":
            return op_stroke(p)
        if kind in ("delete_stroke", "edit_stroke", "simplify_stroke"):
            return op_index(kind, p)
        if kind in ("erase", "erase_raster"):
            return op_erase(kind, p)
        if kind == "add_layer":
            return op_add_layer(p)
        if kind == "delete_layer":
            return {"op": "delete_layer", "page": page_no(p), "id": layer_of(p)}
        if kind == "duplicate_layer":
            return {"op": "duplicate_layer", "page": page_no(p), "id": layer_of(p), **({"new_id": "dup-" + str(rng.randrange(3))} if rng.random() < 0.3 else {})}
        if kind == "set_layer":
            return op_set_layer(p)
        if kind == "set_layers":
            return op_set_layers(p)
        if kind == "reorder_layers":
            return op_reorder_layers(p)
        if kind == "set_brush":
            return {"op": "set_brush", **{k: v for k, v in (("rgb", rgb()), ("width_mm", num(0.1, 2, 2)), ("stabilize", rng.choice([0, 3, None])),
                                                            ("taper", rng.random() < 0.5), ("curve", rng.choice(["gpen", "linear", None])))
                                         if rng.random() < 0.5}}
        return op_for_pages()

    steps = []
    left = rng.randint(1, 12)
    while left > 0:
        size = min(left, rng.choice([1, 1, 1, 1, 2, 3]))
        left -= size
        step = {"ops": [one_op() for _ in range(size)], "agent": rng.choice(AGENTS)}
        if rng.random() < 0.1:
            step["dry_run"] = True
        for k, op in enumerate(step["ops"]):
            # a reorder_layers order drawn from the book as it was read: the layers the page has when it runs (those
            # still there in the order drawn, then the ones the ops before added)
            if op["op"] == "reorder_layers":
                now = replay.layer_ids(step, k)
                if now is not None and sorted(now) != sorted(op["order"]):
                    kept = [i for i in op["order"] if i in now]
                    op["order"] = kept + [i for i in now if i not in kept]
        replay.apply(step)
        steps.append(step)
    return steps


def make_sequences(out: str, books: str, seed: int, count: int) -> None:
    """`count` op sequences over the books in `books` (each sequence for one book, in turn): a JSON list of
    {"book", "first_id", "steps"} for test_contract_ops."""
    import tempfile

    paths = sorted(Path(books).glob("book-*.genko"))
    pools = [_book_pools(p) for p in paths]
    rng = random.Random(seed)
    sequences = []
    with tempfile.TemporaryDirectory() as scratch:
        for i in range(count):
            b = i % len(paths)
            replay = _Replay(paths[b], Path(scratch) / f"store-{i}")
            sequences.append({"book": str(paths[b]), "first_id": SEQ_FIRST_ID, "steps": _sequence(rng, pools[b], replay)})
    Path(out).write_text(dumps(sequences), encoding="utf-8")


# --- random op sequences (the ops of M3-A1, with the basic ones of M2) --------------------------------------------------

RASTER_FIRST_ID = 0x200000  # (new ids count from here: above every id a random book for drawing has)


def make_raster_books(out: str, seed: int, count: int) -> None:
    """`count` random books for drawing (render_harness.make_render_book: rasters of every PNG mode, patches, masks,
    blend modes, fills and gradients, corrections, panels) without what the C++ build does not draw yet (nombres, lines,
    tones, effect lines, 3D guides, layer screens): the books the raster op sequences run on."""
    import render_harness
    from genko.io import load_episode, save_episode
    from genko.models import LayerKind, LayerRole

    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        dest = root / f"book-{i:02d}.genko"
        fresh_process_state(True)
        render_harness.make_render_book(random.Random(seed * 1000 + i), dest, i)
        fresh_process_state(True)
        episode = load_episode(dest)
        episode.story = []
        for page in episode.pages:
            page.numero = False
            page.effects = []
            page.prims = []
            page.layers = [layer for layer in page.layers if layer.kind != LayerKind.TONE and layer.role != LayerRole.TONE]
            for layer in page.layers:
                layer.screen = None
        save_episode(episode, dest, actor="human:作者")


def _raster_pool(episode) -> list:
    """What an op may name in the book as it is now: each page's size, panels and layers."""
    from genko.models import LayerKind

    pages = []
    for page in episode.pages:
        layers = [{"id": layer.id, "kind": layer.kind.value, "role": layer.role.value, "parent": layer.parent_id}
                  for layer in page.layers]
        saved = (page.extra or {}).get("saved_areas")
        pages.append({"index": page.index, "w": float(page.spec.width_mm), "h": float(page.spec.height_mm),
                      "layers": layers, "folders": [layer.id for layer in page.layers if layer.kind == LayerKind.FOLDER],
                      "areas": sorted(saved) if isinstance(saved, dict) else []})
    return pages


def _raster_sequence(rng: random.Random, replay: _Replay) -> list:
    """1 to 10 ops for one book, in steps (most one op; some two or three; some dry runs), by several actors: the ops of
    M3-A1 mostly, with add_stroke, add_layer, set_layer, delete_layer, duplicate_layer, erase, name_ok, page locks and
    strict_gates among them. Each op is drawn from the book as the ops before it left it (`replay`), so most name what
    is there; some name what is not, or give what the op refuses."""

    def num(lo, hi, digits=2):
        return round(rng.uniform(lo, hi), digits)

    def pt(p, margin=5.0):
        return [num(-margin, p["w"] + margin, rng.choice([0, 1, 2])), num(-margin, p["h"] + margin, rng.choice([0, 1, 2]))]

    def inner_pt(p):
        return [num(p["w"] * 0.1, p["w"] * 0.9, 1), num(p["h"] * 0.1, p["h"] * 0.9, 1)]

    def points(p, lo=2, hi=7, pressure=None):
        n = rng.randint(lo, hi)
        x, y = inner_pt(p)
        with_p = rng.random() < 0.4 if pressure is None else pressure
        out = []
        for _ in range(n):
            x, y = x + rng.uniform(-15, 15), y + rng.uniform(-15, 15)
            q = [round(x, 2), round(y, 2)]
            if with_p:
                q.append(round(rng.uniform(0.1, 1.0), 2))
            out.append(q)
        return out

    def rgb():
        return rng.choice([[rng.randrange(256) for _ in range(3)], [rng.randrange(256) for _ in range(3)], None])

    def layer_id(p, kinds=None, fallback=0.05):
        layers = [layer for layer in p["layers"] if kinds is None or layer["kind"] in kinds]
        if not layers or rng.random() < fallback:
            return rng.choice(["nope", made[0]])
        return rng.choice(layers)["id"]

    def paintable(p):
        return layer_id(p, ("strokes", "raster"))

    def area(p, depth=0):
        r = rng.random()
        if r < 0.3 or depth > 1:
            cx, cy = inner_pt(p)
            out = {"poly": [[round(cx + rng.uniform(-25, 25), 2), round(cy + rng.uniform(-25, 25), 2)]
                            for _ in range(rng.randint(3, 6))]}
        elif r < 0.45:
            x, y = inner_pt(p)
            out = {"rect": [x - 10, y - 10, num(3, 40), num(3, 40)]}
        elif r < 0.6:
            x, y = inner_pt(p)
            out = {"ellipse": [x - 10, y - 10, num(3, 40), num(3, 40)]}
        elif r < 0.68:
            out = {"layer": paintable(p)}
        elif r < 0.73:
            out = {"saved": rng.choice(p["areas"] + ["a", "b"])}
        elif r < 0.77:
            out = {"all": True}
        elif r < 0.85:
            x, y = inner_pt(p)
            out = {"color": {"x_mm": x, "y_mm": y, **({"tolerance": rng.randrange(0, 100)} if rng.random() < 0.5 else {}),
                             **({"contiguous": rng.random() < 0.5} if rng.random() < 0.5 else {})}}
        else:
            out = {rng.choice(["union", "intersect", "subtract"]): [area(p, depth + 1) for _ in range(rng.randint(2, 3))]}
        if rng.random() < 0.1:
            out["invert"] = True
        if rng.random() < 0.1:
            out["grow_mm"] = num(-2, 3)
        if rng.random() < 0.1:
            out["feather_mm"] = num(0, 2)
        return out

    def picture_b64(p):
        import base64

        w = rng.randint(4, max(5, int(p["w"] / 25.4 * 120)))
        h = rng.randint(4, max(5, int(p["h"] / 25.4 * 120)))
        shapes = []
        for _ in range(rng.randint(1, 4)):
            x0, y0 = rng.uniform(-w * 0.2, w), rng.uniform(-h * 0.2, h)
            box = (x0, y0, x0 + rng.uniform(1, w), y0 + rng.uniform(1, h))
            shapes.append((rng.choice(["rectangle", "ellipse", "line"]), box,
                           tuple(rng.randrange(256) for _ in range(3)) + (rng.choice([255, 255, 160, 60]),)))
        background = rng.choice([(0, 0, 0, 0), (255, 255, 255, 255), (250, 240, 200, 255)])
        return base64.b64encode(_picture_png((w, h), shapes, rng.choice(["RGBA", "RGBA", "RGB", "L", "LA", "P"]),
                                             background)).decode("ascii")

    def op_convert(p):
        op = {"op": "convert_layer", "page": p["index"], "id": layer_id(p), "to": rng.choice(["paint", "paint", "pen", "x"])}
        if op["to"] == "pen" and rng.random() < 0.5:
            op["min_mm"] = num(0.2, 4)
        return op

    def op_merge_down(p):
        return {"op": "merge_down", "page": p["index"], "id": layer_id(p)}

    def op_merge_layers(p):
        ids = [layer_id(p, fallback=0.02) for _ in range(rng.randint(1, 3))]
        op = {"op": "merge_layers", "page": p["index"], "ids": ids}
        if rng.random() < 0.3:
            op["name"] = "まとめ" + str(rng.randrange(9))
        return op

    def op_merge_visible(p):
        op = {"op": "merge_visible", "page": p["index"]}
        if rng.random() < 0.4:
            op["copy"] = rng.random() < 0.5
        if rng.random() < 0.2:
            op["id"] = rng.choice(["vis-" + str(rng.randrange(3)), layer_id(p)])
        return op

    def op_move_layers(p):
        op = {"op": "move_layers", "page": p["index"], "ids": [layer_id(p, fallback=0.02) for _ in range(rng.randint(1, 2))]}
        r = rng.random()
        if r < 0.4:
            op["after"] = rng.choice(["bottom", layer_id(p)])
        if rng.random() < 0.35:
            op["parent"] = rng.choice(p["folders"] + [None, ""]) if p["folders"] else None
        return op

    def op_group(p):
        op = {"op": "group_layers", "page": p["index"], "ids": [layer_id(p, fallback=0.02) for _ in range(rng.randint(1, 3))]}
        if rng.random() < 0.4:
            op["id"] = "grp-" + str(rng.randrange(4))
        if rng.random() < 0.3:
            op["name"] = "組" + str(rng.randrange(9))
        return op

    def op_layer_mask(p):
        op = {"op": "set_layer_mask", "page": p["index"], "id": layer_id(p)}
        r = rng.random()
        if r < 0.4:
            op["area"] = area(p)
        elif r < 0.6:
            op["fill"] = rng.choice(["hide", "show", "hide", "maybe"])
        elif r < 0.7:
            op["delete"] = True
        elif r < 0.8:
            op["enabled"] = rng.random() < 0.5
        if rng.random() < 0.25:
            op["invert"] = True
        return op

    def op_paint_mask(p):
        op = {"op": "paint_mask", "page": p["index"], "id": layer_id(p), "points": points(p, 1, 5)}
        if rng.random() < 0.6:
            op["width_mm"] = num(0.5, 8)
        if rng.random() < 0.7:
            op["show"] = rng.random() < 0.5
        return op

    def op_put_raster(p):
        op = {"op": "put_raster", "page": p["index"], "png_base64": picture_b64(p)}
        if rng.random() < 0.5:
            op["id"] = layer_id(p, ("raster", "strokes"))
        elif rng.random() < 0.8:
            op["layer"] = rng.choice(["ink", "bg", "finish", "name", "draft"])
        return op

    def op_filter(p):
        kind = rng.choice(["blur", "sharpen", "hue", "levels", "curve", "mosaic", "bitonal", "motion_blur", "radial_blur",
                           "zoom_blur", "noise", "wave", "twirl", "lineart", "invert", "posterize", "threshold",
                           "gradient_map", "brightness_contrast", "despeckle", "glow", "rain", "sparkle"])
        params = {"blur": lambda: {"radius": num(0.5, 6)}, "sharpen": lambda: {"amount": num(0.5, 2), "radius": num(0.5, 3)},
                  "hue": lambda: {"shift": num(-180, 180), "saturation": num(0, 2), "value": num(0.5, 1.5)},
                  "levels": lambda: {"black": rng.randrange(0, 80), "white": rng.randrange(150, 256), "gamma": num(0.5, 2)},
                  "curve": lambda: rng.choice([{"gamma": num(0.4, 2.5)}, {"points": [[0, rng.randrange(60)], [128, rng.randrange(256)], [255, rng.randrange(180, 256)]]}]),
                  "mosaic": lambda: {"block": rng.randint(2, 20)}, "bitonal": lambda: {"threshold": rng.randrange(256)},
                  "motion_blur": lambda: {"distance": rng.randint(1, 15), "angle": num(-90, 90)},
                  "radial_blur": lambda: {"amount": num(0.02, 0.2), "cx": num(0, 1), "cy": num(0, 1)},
                  "zoom_blur": lambda: {"amount": num(0.02, 0.3)}, "noise": lambda: {"amount": num(0.05, 0.5), "mono": rng.random() < 0.5, "seed": rng.choice(["a", "b", 5])},
                  "wave": lambda: {"amplitude": num(1, 8), "wavelength": num(10, 60)},
                  "twirl": lambda: {"angle": num(-180, 180), "radius": num(0.1, 0.8)},
                  "lineart": lambda: rng.choice([{}, {"threshold": num(0.5, 0.95), "radius": rng.randint(2, 6), "drop_blue": rng.random() < 0.5}]),
                  "invert": dict, "posterize": lambda: {"levels": rng.randint(2, 6)}, "threshold": lambda: {"threshold": rng.randrange(256)},
                  "gradient_map": lambda: {"colors": [[rng.randrange(256) for _ in range(3)] for _ in range(rng.randint(2, 3))]},
                  "brightness_contrast": lambda: {"brightness": rng.randrange(-50, 50), "contrast": rng.randrange(-50, 50)},
                  "despeckle": lambda: rng.choice([{"size_px": rng.randint(2, 30)}, {"size_mm": num(0.2, 1.5), "what": rng.choice(["dark", "light", "both"])}]),
                  "glow": lambda: {"radius": num(1, 8), "threshold": rng.randrange(100, 250)},
                  "rain": lambda: {"count": rng.randint(5, 80), "length": num(5, 30), "seed": rng.choice(["r", 3])},
                  "sparkle": dict}[kind]()
        op = {"op": "filter_raster", "page": p["index"], "kind": kind, **params}
        if rng.random() < 0.85:
            op["id"] = layer_id(p, ("raster", "strokes"))
        else:
            op["layer"] = rng.choice(["ink", "bg", "name"])
        if rng.random() < 0.2:
            op["area"] = area(p)
        return op

    def fill_common(p, op):
        if rng.random() < 0.4:
            op["layer_id"] = paintable(p)
        if rng.random() < 0.4:
            op["rgb"] = rgb()
        if rng.random() < 0.2:
            op["opacity"] = num(0.2, 1)
        if rng.random() < 0.3:
            op["gap_mm"] = num(0, 2.5)
        if rng.random() < 0.2:
            op["expand_mm"] = num(0, 1)
        if rng.random() < 0.2:
            op["tolerance"] = rng.randrange(0, 101)
        if rng.random() < 0.15:
            op["reference"] = rng.choice(["page", "layer", "reference", "x"])
        if rng.random() < 0.1:
            op["ignore"] = rng.choice([["draft"], ["text"], "draft", ["draft", "text"]])
        return op

    def op_fill(p):
        x, y = inner_pt(p)
        return fill_common(p, {"op": "fill", "page": p["index"], "x_mm": x, "y_mm": y})

    def op_fill_area(p):
        op = {"op": "fill_area", "page": p["index"], "area": area(p)}
        if rng.random() < 0.5:
            op["layer_id"] = paintable(p)
        if rng.random() < 0.5:
            op["rgb"] = rgb()
        if rng.random() < 0.2:
            op["opacity"] = num(0.1, 1)
        return op

    def op_fill_enclosed(p):
        x, y = inner_pt(p)
        s = rng.uniform(10, 40)
        poly = [[round(x - s, 1), round(y - s, 1)], [round(x + s, 1), round(y - s, 1)], [round(x + s, 1), round(y + s, 1)],
                [round(x - s, 1), round(y + s, 1)]]
        return fill_common(p, {"op": "fill_enclosed", "page": p["index"], "poly": poly})

    def op_fill_gaps(p):
        op = {"op": "fill_gaps", "page": p["index"], "layer_id": paintable(p)}
        if rng.random() < 0.6:
            op["max_mm"] = num(0.5, 4)
        if rng.random() < 0.3:
            op["rgb"] = rgb()
        if rng.random() < 0.2:
            op["area"] = area(p)
        return op

    def op_flood(p):
        x, y = pt(p, 2)
        op = {"op": "flood_fill", "page": p["index"], "x_mm": x, "y_mm": y,
              "rgb": rng.choice([[rng.randrange(256) for _ in range(3)], [1, 2], [10, 20, 30, 40]])}
        if rng.random() < 0.7:
            op["layer"] = rng.choice(["ink", "bg", "name", "finish", "draft"])
        if rng.random() < 0.3:
            op["gap_mm"] = num(0, 2)
        return op

    def op_gradient(p):
        op = {"op": "gradient_fill", "page": p["index"], "from": inner_pt(p), "to": inner_pt(p)}
        if rng.random() < 0.5:
            op["layer_id"] = paintable(p)
        if rng.random() < 0.4:
            op["shape"] = rng.choice(["linear", "radial", "ellipse", "star"])
            if op["shape"] == "ellipse":
                op["ratio"] = num(0.2, 2)
        if rng.random() < 0.3:
            op["repeat"] = rng.choice(["none", "repeat", "mirror"])
        if rng.random() < 0.3:
            op["stops"] = [[num(0, 1), [rng.randrange(256) for _ in range(3)], num(0, 1)] for _ in range(rng.randint(2, 3))]
        else:
            for key in ("rgb_from", "rgb_to"):
                if rng.random() < 0.5:
                    op[key] = [rng.randrange(256) for _ in range(3)]
            for key in ("opacity_from", "opacity_to"):
                if rng.random() < 0.3:
                    op[key] = num(0, 1)
        if rng.random() < 0.25:
            op["area"] = area(p)
        return op

    def op_delete_area(p):
        return {"op": "delete_area", "page": p["index"], "layer_id": paintable(p), "area": area(p)}

    def op_transform(p):
        op = {"op": "transform_area", "page": p["index"], "layer_id": paintable(p), "area": area(p)}
        r = rng.random()
        if r < 0.55:
            a = math.radians(rng.uniform(-30, 30))
            s = rng.uniform(0.6, 1.4)
            op["matrix"] = [round(s * math.cos(a), 4), round(s * math.sin(a), 4), round(-s * math.sin(a), 4),
                            round(s * math.cos(a), 4), num(-10, 10), num(-10, 10)]
        elif r < 0.75:
            x, y = inner_pt(p)
            op["warp"] = {"perspective": [[round(x + dx + rng.uniform(-4, 4), 1), round(y + dy + rng.uniform(-4, 4), 1)]
                                          for dx, dy in ((-15, -15), (15, -15), (15, 15), (-15, 15))]}
        elif r < 0.9:
            x, y = inner_pt(p)
            op["warp"] = {"mesh": [[round(x + i * 10 + rng.uniform(-3, 3), 1), round(y + j * 10 + rng.uniform(-3, 3), 1)]
                                   for j in range(3) for i in range(3)]}
        else:
            op["matrix"] = rng.choice([[1, 0, 0, 1, 0], [1, 1, 1, 1, 0, 0]])
        if rng.random() < 0.3:
            op["interp"] = rng.choice(["nearest", "bicubic", "bilinear", "soft"])
        return op

    def op_paste(p):
        import base64

        items = {}
        if rng.random() < 0.7:
            items["strokes"] = [{"points": points(p), "width_mm": num(0.2, 2), **({"rgb": rgb()} if rng.random() < 0.4 else {})}
                                for _ in range(rng.randint(1, 3))]
        if rng.random() < 0.5 or not items:
            x, y = inner_pt(p)
            items["patches"] = [{"box": [x, y, num(2, 20), num(2, 20)], "mode": rng.choice(["mask", "image"]),
                                 "png": base64.b64encode(_picture_png((rng.randint(4, 40), rng.randint(4, 40)),
                                                                      [("ellipse", (2, 2, 30, 30), (200, 20, 20, 255))],
                                                                      rng.choice(["RGBA", "L"]))).decode("ascii"),
                                 **({"rgb": [rng.randrange(256) for _ in range(3)]} if rng.random() < 0.5 else {})}]
        op = {"op": "paste", "page": p["index"], "items": items}
        if rng.random() < 0.5:
            op["layer_id"] = paintable(p)
        if rng.random() < 0.5:
            op["matrix"] = [1, 0, 0, 1, num(-10, 10), num(-10, 10)]
        return op

    def op_store(p):
        return {"op": "store_area", "page": p["index"], "name": rng.choice(["a", "b", "空", " c "]), "area": area(p)}

    def op_forget(p):
        return {"op": "forget_area", "page": p["index"], "name": rng.choice(p["areas"] + ["a", "b", "zz"])}

    def op_paper(p):
        op = {"op": "set_paper", "rgb": rng.choice([[rng.randrange(256) for _ in range(3)], None, [300, 0, 0]])}
        if rng.random() < 0.6:
            op["page"] = p["index"]
        return op

    def op_timelapse(p):
        return {"op": "set_timelapse", **({"on": rng.random() < 0.6} if rng.random() < 0.7 else {})}

    def op_erase(p, name):
        op = {"op": name, "page": p["index"], "points": points(p, 1, 5), "width_mm": num(0.5, 8, 1)}
        if rng.random() < 0.8:
            op["layer_id"] = paintable(p)
        else:
            op["layer"] = rng.choice(["ink", "name", "finish"])
        if rng.random() < 0.4:
            op["texture"] = rng.choice(["hard", "soft", "rough"])
        return op

    def op_m2(p):
        kind = rng.choice(["add_stroke", "add_stroke", "add_layer", "add_layer", "set_layer", "delete_layer", "duplicate_layer",
                           "name_ok", "strict", "lock_page"])
        if kind == "add_stroke":
            op = {"op": "add_stroke", "page": p["index"], "points": points(p), "stabilize": 0}
            if rng.random() < 0.6:
                op["layer_id"] = paintable(p)
            else:
                op["layer"] = rng.choice(["ink", "name", "finish"])
            if rng.random() < 0.4:
                op["width_mm"] = num(0.3, 3)
            if rng.random() < 0.3:
                op["rgb"] = rgb()
            return op
        if kind == "add_layer":
            op = {"op": "add_layer", "page": p["index"], "kind": rng.choice(["pen", "paint", "paint", "folder", "fill", "gradient", "adjust"])}
            if rng.random() < 0.3:
                op["id"] = "L" + str(rng.randrange(4))
            if rng.random() < 0.3:
                op["after"] = layer_id(p)
            if op["kind"] == "adjust":
                op["adjust"] = rng.choice([{"kind": "levels", "black": 20}, {"kind": "hue", "shift": 40}, {"kind": "blur", "radius": 2},
                                           {"kind": "invert"}, {"kind": "mosaic", "block": 6}])
            return op
        if kind == "set_layer":
            op = {"op": "set_layer", "page": p["index"], "id": layer_id(p)}
            for key in rng.sample(["visible", "opacity", "blend", "clip", "locked", "reference", "parent"], rng.randint(1, 2)):
                op[key] = {"opacity": num(0, 1), "blend": rng.choice(["multiply", "screen", "normal", "overlay"]),
                           "parent": rng.choice(p["folders"] + [None]) if p["folders"] else None,
                           "locked": rng.random() < 0.3}.get(key, rng.random() < 0.6)
            return op
        if kind == "delete_layer":
            return {"op": "delete_layer", "page": p["index"], "id": layer_id(p)}
        if kind == "duplicate_layer":
            return {"op": "duplicate_layer", "page": p["index"], "id": layer_id(p)}
        if kind == "name_ok":
            return {"op": "name_ok", "page": p["index"]}
        if kind == "strict":
            return {"op": "set_meta", "strict_gates": rng.random() < 0.5}
        return {"op": rng.choice(["lock_page", "unlock_page"]), "page": p["index"]}

    makers = [op_convert, op_merge_down, op_merge_layers, op_merge_visible, op_move_layers, op_group, op_layer_mask,
              op_paint_mask, op_put_raster, op_filter, op_filter, op_fill, op_fill, op_fill_area, op_fill_enclosed,
              op_fill_gaps, op_flood, op_gradient, op_delete_area, op_transform, op_transform, op_paste, op_store,
              op_forget, op_paper, op_timelapse, lambda p: op_erase(p, "erase"), lambda p: op_erase(p, "erase_raster"),
              op_m2, op_m2, op_m2]
    made = [f"{RASTER_FIRST_ID + k:012x}" for k in range(4)]
    steps = []
    left = rng.randint(1, 10)
    while left > 0:
        size = min(left, rng.choice([1, 1, 1, 1, 2, 3]))
        left -= size
        pages = _raster_pool(replay.episode)
        step = {"ops": [rng.choice(makers)(rng.choice(pages)) for _ in range(size)], "agent": rng.choice(AGENTS)}
        if rng.random() < 0.1:
            step["dry_run"] = True
        replay.apply(step)
        steps.append(step)
    return steps


def make_raster_sequences(out: str, books: str, seed: int, count: int) -> None:
    """`count` op sequences of M3-A1 over the books in `books` (make-raster-books; each sequence for one book, in turn):
    a JSON list of {"book", "first_id", "steps"} for test_contract_raster_ops."""
    import tempfile

    paths = sorted(Path(books).glob("book-*.genko"))
    rng = random.Random(seed)
    sequences = []
    with tempfile.TemporaryDirectory() as scratch:
        for i in range(count):
            b = i % len(paths)
            replay = _Replay(paths[b], Path(scratch) / f"store-{i}", RASTER_FIRST_ID)
            sequences.append({"book": str(paths[b]), "first_id": RASTER_FIRST_ID, "steps": _raster_sequence(rng, replay)})
    Path(out).write_text(dumps(sequences), encoding="utf-8")


# --- numbers and JSON -------------------------------------------------------------------------------------------------


def bits(x: float) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", x))[0]


def rand_double(rng: random.Random) -> float:
    while True:
        r = rng.random()
        if r < 0.5:
            x = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        elif r < 0.8:
            x = rng.uniform(-1000, 1000)
        elif r < 0.9:
            x = round(rng.uniform(-1000, 1000), rng.randrange(0, 6))
        else:
            x = rng.choice(SPECIAL_FLOATS)
        if math.isfinite(x):
            return x


def numbers(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    reprs = [[bits(x), repr(x), format(x, "g")] for x in (rand_double(rng) for _ in range(count))]
    rounds = []
    for _ in range(count):
        x = rand_double(rng)
        if rng.random() < 0.5:
            x = rng.uniform(-1e4, 1e4)
        n = rng.randrange(-6, 12)
        try:
            rounds.append([bits(x), n, bits(round(x, n))])
        except OverflowError:
            rounds.append([bits(x), n, None])
    sums = []
    for _ in range(count // 4):
        items = []
        for _ in range(rng.randrange(0, 9)):
            items.append(rng.randint(-1000, 1000) if rng.random() < 0.3 else rng.choice([rng.uniform(-1e3, 1e3), 0.1, 0.2, 1e16, -1e16, 1.0, -0.0]))
        total = sum(items)
        sums.append([[["i", v] if isinstance(v, int) else ["f", bits(v)] for v in items],
                     ["i", total] if isinstance(total, int) else ["f", bits(total)]])
    hypots = []
    for _ in range(count // 4):
        a, b, c, d = (rng.uniform(-1e3, 1e3) * 10.0 ** rng.randint(-5, 5) for _ in range(4))
        hypots.append([bits(a), bits(b), bits(c), bits(d), bits(math.hypot(a, b)), bits(math.dist((a, b), (c, d)))])
    Path(out).write_text(dumps({"repr": reprs, "round": rounds, "sum": sums, "hypot": hypots}), encoding="utf-8")


def json_dumps_cases(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    cases = []
    for _ in range(count):
        value = rand_json(rng)
        if rng.random() < 0.5:
            value = {rand_key(rng, set()): rand_json(rng) for _ in range(rng.randrange(0, 6))}
        text = json.dumps(value, ensure_ascii=rng.random() < 0.3, indent=rng.choice([None, 1, 4]))
        cases.append({"text": text,
                      "indent2": json.dumps(value, ensure_ascii=False, indent=2),
                      "default": json.dumps(value, ensure_ascii=False),
                      "canonical": json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
                      "ascii": json.dumps(value, ensure_ascii=True)})
    Path(out).write_text(dumps(cases), encoding="utf-8")


JSON_ERRORS = ["", " ", "[", "]", "{", "}", "[1,]", "{\"a\":1,}", "{\"a\"", "{\"a\":", "{\"a\":1", "[1", "[1 2]",
               "{\"a\" 1}", "{1:2}", "\"abc", "\"a\\x\"", "\"\\u12\"", "\"\\u12G4\"", "\"a\nb\"", "nul", "tru", "fals",
               "-", "-a", "01", "1.", "1e", "1e+", ".5", "+1", "1 2", "{} x", "[] []", "\ufeff[]", "[\n1,\n2,\n]",
               "{\n  \"a\": [1,\n  2\n  x]}", "\"東京\" 1", "[\"東京\", ]", "NaN x", "[Infinity,",
               "{\"a\": 1, \"b\": }", "\t\r\n", "[1]\r\n x", "[\"\\ud83d\\ude00\", 1 2]", "{\"a\":1}}", "[-0, -]",
               "[1e400, 2] x", "{\"x\": \"\\t\"} \"y\""]


def json_errors(out: str) -> None:
    cases = []
    for text in JSON_ERRORS:
        try:
            json.loads(text)
            message = None
        except json.JSONDecodeError as exc:
            message = str(exc)
        cases.append([text, message])
    Path(out).write_text(dumps(cases), encoding="utf-8")


UNIT_JSON = ["null", "true", "false", "0", "-0", "-0.0", "1.0", "0.35", "1e-5", "1e16", "1e15", "0.1",
             "0.30000000000000004", "9223372036854775807", "-9223372036854775808", "18446744073709551615", "5e-324",
             "1.7976931348623157e308", "2.5e-7", "0.0001", "100.0", "1e22", "123.456", "\"日本語\"", "\"emoji 🙂\"",
             "\"\\u0000\\u0001\\u001f\\u007f\"", "\"\\n\\r\\t\\b\\f\\\"\\\\/\"", "\"\\u2028\\u2029\\u00e9\"", "[]", "{}",
             "[1, [2, [3, []]], {}]", "{\"b\": 1, \"a\": [true, null], \"c\": {\"z\": 1, \"y\": {}}}",
             "{\"日本\": \"語\", \"a\": \"\\ud83d\\ude00\", \"Z\": 0}", "[1.5, -2.25, 3e-10, -1e100]", "{\"\": \"\"}",
             "[0.1, 0.2, 0.30000000000000004, 1.1, 2.675]", "{\"x\": -1e-7, \"y\": 1e+300, \"z\": 12345678901234}",
             "[1e-4, 1e-5, 9999999999999998.0, 1e16, 12345.678e10]"]
UNIT_ROUND = [(2.675, 2), (0.125, 2), (0.375, 2), (-0.001, 2), (0.5, 0), (1.5, 0), (2.5, 0), (-0.5, 0), (-1.5, 0),
              (1234.5, -2), (50.0, -2), (150.0, -2), (250.0, -2), (0.0, 3), (-0.0, 3), (1.0005, 3), (1.0015, 3),
              (1.0025, 3), (5e-324, 3), (5e-324, 400), (1e300, -300), (123.456, -1), (123.456, 1), (123.456, 10),
              (0.1 + 0.2, 15), (0.1 + 0.2, 16), (0.1 + 0.2, 17), (1 / 3, 5), (2 / 3, 5), (-2 / 3, 5), (1e-7, 6),
              (5e-7, 6), (1.5e-7, 7), (2.5e-7, 7), (12345.6789, 0), (12345.6789, -3), (12345.6789, -5), (99.995, 2),
              (0.045, 2), (1.005, 2), (8.325, 2), (8.335, 2), (-8.335, 2), (4.5, -1), (45.0, -1), (55.0, -1),
              (95.0, -1), (95.0, -2), (149.99999999999997, -2), (180.0, 3), (465.2, 3), (0.0005, 3), (0.0015, 3),
              (1e16 + 2, -1), (1.7976931348623157e308, -307)]
UNIT_SUMS = [[0.1] * 10, [1, 0.1, 0.2], [0.1, 0.2, 0.3], [-0.0], [-0.0, -0.0], [], [1, 2, 3], [1e16, 1.0, -1e16],
             [1, 2.5], [2.5, 1], [3, 0.1, 0.2, 0.3], [1e100, 1.0, -1e100, 1e-100], [13, 13 + 184, 13 + 184, 13],
             [29.0, 179.0, 179.0, 29.0], [0.1, -0.1, 1e-17], [float(2**53), 1, 1]]


def unit_tables() -> dict:
    from genko import covers
    from genko.models import Binding, Page, PageSpec

    def json_case(text):
        value = json.loads(text)
        return {"text": text, "indent2": json.dumps(value, ensure_ascii=False, indent=2),
                "default": json.dumps(value, ensure_ascii=False),
                "canonical": json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
                "ascii": json.dumps(value, ensure_ascii=True)}

    def specs():
        out = {}
        makers = {"b4": PageSpec.b4_comic, "b5": PageSpec.b5_doujin, "a5": PageSpec.a5_doujin, "a4": PageSpec.a4_mono,
                  "webtoon": PageSpec.webtoon,
                  "custom": lambda: PageSpec.custom(200.5, 280, 180, 260.25, 3, 12, 12.5, 10, 8),
                  "shueisha": lambda: PageSpec.publisher(" Shueisha "), "other": lambda: PageSpec.publisher("x")}
        for name, make in makers.items():
            spec = make()
            pages = {}
            for binding in (Binding.RIGHT, Binding.LEFT):
                for index in (1, 2):
                    page = Page(index=index, spec=spec, frames=[], binding=binding)
                    r = page.inner_rect_mm()
                    rl = page.inner_rect_mm("left")
                    t = page.trim_rect_mm()
                    b = page.bleed_rect_mm()
                    pages[f"{binding.value}{index}"] = {
                        "side": page.side(), "edge": page.binding_edge(), "side_left": page.side("left"),
                        "inner": [r.x, r.y, r.width, r.height], "inner_start_left": [rl.x, rl.y, rl.width, rl.height],
                        "trim": [t.x, t.y, t.width, t.height], "bleed": [b.x, b.y, b.width, b.height],
                        "step": page.spread_step_mm()}
            out[name] = {"width_mm": spec.width_mm, "height_mm": spec.height_mm, "dpi": spec.dpi, "preset": spec.preset,
                         "trim_size": list(spec.trim_size()), "trim_origin": list(spec.trim_origin()),
                         "margins": spec.margins(), "frame_size": list(spec.frame_size()),
                         "describe": spec.describe(), "pages": pages}
        return out

    def cover_specs():
        out = []
        for book in (PageSpec.b4_comic(), PageSpec.a4_mono(), PageSpec.b5_doujin()):
            for cover in ({"kind": "front"}, {"kind": "jacket", "spine_mm": 12.5, "flap_mm": 80},
                          {"kind": "obi", "spine_mm": "7", "flap_mm": 0, "height_mm": 45.25}, {"kind": "jacket"}):
                spec = covers.spec_for(book, cover)
                out.append({"book": book.preset, "cover": cover, "width_mm": spec.width_mm, "height_mm": spec.height_mm,
                            "trim_w_mm": spec.trim_w_mm, "trim_h_mm": spec.trim_h_mm, "preset": spec.preset,
                            "margins_mm": spec.margins_mm})
        return out

    def rounded(x, n):
        try:
            return repr(round(x, n))
        except OverflowError:
            return "OverflowError"

    return {
        "json": [json_case(text) for text in UNIT_JSON],
        "round": [[repr(x), n, rounded(x, n)] for x, n in UNIT_ROUND],
        "sum": [[[["i", v] if isinstance(v, int) else ["f", repr(v)] for v in items],
                 (lambda t: ["i", t] if isinstance(t, int) else ["f", repr(t)])(sum(items))] for items in UNIT_SUMS],
        "specs": specs(),
        "covers": cover_specs(),
    }


def movie_info(path: str) -> dict:
    """A moving picture as Pillow plays it (or a folder of numbered PNGs, or an MP4 as ffprobe counts it): its
    format, size, frames (each one's RGB pixels as sha256, and how long it shows), loop."""
    import hashlib
    import shutil
    import subprocess

    from PIL import Image, ImageSequence

    target = Path(path)
    if target.is_dir():
        files = sorted(target.glob("frame_*.png"))
        frames = []
        sizes = []
        for f in files:
            with Image.open(f) as im:
                rgb = im.convert("RGB")
                frames.append(hashlib.sha256(rgb.tobytes()).hexdigest())
                sizes.append(list(rgb.size))
        return {"format": "frames", "names": [f.name for f in files], "sizes": sizes, "frames": frames}
    if target.suffix.lower() == ".mp4":
        tool = shutil.which("ffprobe")
        if tool is None:
            return {"format": "mp4", "probed": False}
        done = subprocess.run([tool, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                               "stream=nb_read_frames,r_frame_rate,width,height,codec_name,pix_fmt", "-of", "json", str(target)],
                              capture_output=True, text=True)
        stream = json.loads(done.stdout or "{}").get("streams", [{}])[0]
        return {"format": "mp4", "probed": True, **stream}
    with Image.open(target) as im:
        frames, durations, sizes = [], [], []
        for frame in ImageSequence.Iterator(im):
            rgb = frame.convert("RGB")
            frames.append(hashlib.sha256(rgb.tobytes()).hexdigest())
            durations.append(frame.info.get("duration"))
            sizes.append(list(rgb.size))
        return {"format": im.format, "size": list(im.size), "n_frames": getattr(im, "n_frames", 1), "loop": im.info.get("loop"),
                "durations": durations, "sizes": sizes, "frames": frames}


def movie_infos(out: str, paths: list[str]) -> None:
    Path(out).write_text(json.dumps({p: movie_info(p) for p in paths}, ensure_ascii=False) + "\n", encoding="utf-8")


def anim_movies(book: str, page: int, folder: str, out: str, formats: list[str], dpi: int) -> None:
    """anim.export of one page of a book in each format (at this dpi), and each file as Pillow plays it."""
    from genko import anim
    from genko.io import load_episode

    fresh_process_state(True)
    episode = load_episode(Path(book))
    target = next(p for p in episode.pages if p.index == page)
    results = {}
    for fmt in formats:
        dest = Path(folder) / ("frames" if fmt == "frames" else f"movie.{fmt}")
        try:
            written = anim.export(target, dest, episode=episode, dpi=dpi, fmt=fmt)
        except Exception as exc:  # (the reference's own refusal: its type and message)
            results[fmt] = {"error": [type(exc).__name__, str(exc)]}
            continue
        results[fmt] = movie_info(str(dest if fmt == "frames" else written))
    Path(out).write_text(json.dumps(results, ensure_ascii=False) + "\n", encoding="utf-8")


def plugin_run(config: str, kind: str, src: str, dest: str, params: str) -> None:
    """plugins.run (Python's own, in this process) on a picture, with the plugins of a config folder."""
    import os

    os.environ["GENKO_CONFIG_DIR"] = config
    from PIL import Image

    from genko import plugins

    with Image.open(src) as im:
        out = plugins.run(kind, im.convert("RGBA"), json.loads(params))
    out.save(dest)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("snapshot")
    p.add_argument("book")
    p.add_argument("--full", action="store_true")
    p.add_argument("--ids", action="store_true")
    p = sub.add_parser("resave")
    p.add_argument("book")
    p.add_argument("dest")
    p.add_argument("--ids", action="store_true")
    p = sub.add_parser("batch")
    p.add_argument("jobs")
    for name in ("make-random", "numbers", "json-dumps"):
        p = sub.add_parser(name)
        p.add_argument("out")
        p.add_argument("--seed", type=int, default=1)
        p.add_argument("--count", type=int, default=50)
    p = sub.add_parser("json-errors")
    p.add_argument("out")
    sub.add_parser("unit-tables")
    p = sub.add_parser("restore")
    p.add_argument("book")
    p.add_argument("--actor", default="genko")
    p.add_argument("--redo", action="store_true")
    p.add_argument("--force", action="store_true")
    for name in ("lock-hold", "lock-try"):
        p = sub.add_parser(name)
        p.add_argument("book")
        p.add_argument("--agent", default="genko")
    p = sub.add_parser("upgrade")
    p.add_argument("book")
    for name in ("make-opsbook", "make-drawbook", "make-rasterbook"):
        p = sub.add_parser(name)
        p.add_argument("out")
    p = sub.add_parser("make-sequences")
    p.add_argument("out")
    p.add_argument("--books", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=300)
    p = sub.add_parser("show-cases")
    p.add_argument("cases")
    p.add_argument("book")
    p.add_argument("--only")
    p = sub.add_parser("filter-edges")
    p.add_argument("out")
    p = sub.add_parser("make-raster-books")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=15)
    p = sub.add_parser("make-raster-sequences")
    p.add_argument("out")
    p.add_argument("--books", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=150)
    p = sub.add_parser("movie-info")
    p.add_argument("out")
    p.add_argument("paths", nargs="*")
    p = sub.add_parser("anim-movies")
    p.add_argument("book")
    p.add_argument("page", type=int)
    p.add_argument("folder")
    p.add_argument("out")
    p.add_argument("--formats", default="gif,png,webp,frames,mp4")
    p.add_argument("--dpi", type=int, default=20)
    p = sub.add_parser("plugin-run")
    for name in ("config", "kind", "src", "dest", "params"):
        p.add_argument(name)
    args = parser.parse_args(argv)
    if args.cmd == "plugin-run":
        plugin_run(args.config, args.kind, args.src, args.dest, args.params)
        return 0
    if args.cmd == "movie-info":
        movie_infos(args.out, args.paths)
        return 0
    if args.cmd == "anim-movies":
        anim_movies(args.book, args.page, args.folder, args.out, args.formats.split(","), args.dpi)
        return 0
    if args.cmd == "show-cases":
        return show_cases(args.cases, args.book, args.only)
    if args.cmd == "filter-edges":
        filter_edges(args.out)
        return 0
    if args.cmd == "make-raster-books":
        make_raster_books(args.out, args.seed, args.count)
        return 0
    if args.cmd == "make-raster-sequences":
        make_raster_sequences(args.out, args.books, args.seed, args.count)
        return 0
    if args.cmd == "make-opsbook":
        make_opsbook(args.out)
        return 0
    if args.cmd == "make-drawbook":
        make_drawbook(args.out)
        return 0
    if args.cmd == "make-rasterbook":
        make_rasterbook(args.out)
        return 0
    if args.cmd == "make-sequences":
        make_sequences(args.out, args.books, args.seed, args.count)
        return 0
    if args.cmd == "unit-tables":
        sys.stdout.write(json.dumps(unit_tables(), ensure_ascii=False, indent=1) + "\n")
        return 0
    if args.cmd == "restore":
        sys.stdout.write(dumps(restore_book(args.book, args.actor, args.redo, args.force)) + "\n")
        return 0
    if args.cmd == "lock-hold":
        return lock_hold(args.book, args.agent)
    if args.cmd == "lock-try":
        sys.stdout.write(dumps(lock_try(args.book, args.agent)) + "\n")
        return 0
    if args.cmd == "upgrade":
        upgrade(args.book)
        return 0
    if args.cmd == "snapshot":
        sys.stdout.write(dumps(snapshot_of(args.book, args.full, args.ids)) + "\n")
    elif args.cmd == "resave":
        resave(args.book, args.dest, args.ids)
    elif args.cmd == "batch":
        run_batch(args.jobs)
    elif args.cmd == "make-random":
        make_random(args.out, args.seed, args.count)
    elif args.cmd == "numbers":
        numbers(args.out, args.seed, args.count)
    elif args.cmd == "json-dumps":
        json_dumps_cases(args.out, args.seed, args.count)
    elif args.cmd == "json-errors":
        json_errors(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
