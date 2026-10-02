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
                                       "first_id"?, "store", "out", "digest"?, "reread"?} (batches one after another
                                       on the book in memory, as a session: see steps_job)
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
  make-sequences OUT --books DIR --seed N --count K
                                       K random op sequences (1 to 12 ops, in batches by various actors, some dry
                                       runs, for_pages, strict_gates, page locks) over the make-random books in DIR
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
    "revision"). With "digest", the snapshot and payload (and the reply's snapshot) are replaced by the sha256 of
    their json.dumps. With "reread" (a new folder), the book is then saved there with save_episode and read back as
    a new process would (ids counted from 1 again with "ids"): one more record, {"reread": true, "full",
    "payload"}."""
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
    records = []
    for step in job["steps"]:
        try:
            reply = apply_ops(episode, copy.deepcopy(step["ops"]), dry_run=bool(step.get("dry_run")),
                              agent=step.get("agent", "genko"))
        except ApplyError as exc:
            reply = {"ok": False, "error": str(exc)}
        except Exception as exc:  # (apply_ops lets these through: Python's command line stops with a traceback)
            reply = {"ok": False, "error": str(exc), "uncaught": type(exc).__name__}
        payload = _payload(episode, store)
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
        payload = _payload(again, store)
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


def run_batch(jobs_path: str) -> None:
    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    for job in jobs:
        if job["op"] == "snapshot":
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


def make_random(out: str, seed: int, count: int) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        fresh_process_state(True)
        make_random_book(random.Random(seed * 1000 + i), root / f"book-{i:02d}.genko")


# --- random op sequences (the ops of M2-O1) ---------------------------------------------------------------------------

AGENTS = ["genko", "genko", "human:作者", "ai:hermes", "legacy:unknown", "ai:other"]
SEQ_FIRST_ID = 0x100000  # (new ids count from here: above every id a random book has)


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
                   "huge": any(not abs(v) < 1e5 for s in layer.strokes for p in s.points for v in p[:2])}
                  for layer in page.layers]
        pages.append({"index": page.index, "leaves": [f.id for f in page.leaf_frames()],
                      "splits": [f.id for f in nodes if f.children], "frames": [f.id for f in nodes], "layers": layers,
                      "w": float(page.spec.width_mm), "h": float(page.spec.height_mm)})
    huge = any(layer["huge"] for page in pages for layer in page["layers"])
    return {"pages": pages, "huge": huge}


def _sequence(rng: random.Random, pool: dict) -> list:
    """1 to 12 ops of M2-O1 for one book, in steps (most one op; some two or three; some dry runs), by several actors,
    with for_pages, strict_gates switched on and off, page locks and name approvals among them."""
    pages = pool["pages"]
    made = [f"{SEQ_FIRST_ID + k:012x}" for k in range(40)]  # (ids the sequence's own ops make)

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
            op["parent"] = layer_of(p)
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
        if rng.random() < 0.3 and ids:
            ids = ids[:rng.randint(0, len(ids))]
        if rng.random() < 0.2:
            ids.append(rng.choice(["nope", 5]))
        return {"op": "reorder_layers", "page": page_no(p), "order": ids}

    def op_for_pages():
        inner = rng.choice([lambda: {"op": "set_note", "note": "毎"}, lambda: {"op": "select_frame", "frame_id": "x"},
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
            return {"op": "select_frame", "page": page_no(p), "frame_id": frame_of(p)}
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
        steps.append(step)
    return steps


def make_sequences(out: str, books: str, seed: int, count: int) -> None:
    """`count` op sequences over the books in `books` (each sequence for one book, in turn): a JSON list of
    {"book", "first_id", "steps"} for test_contract_ops."""
    paths = sorted(Path(books).glob("book-*.genko"))
    pools = [_book_pools(p) for p in paths]
    rng = random.Random(seed)
    sequences = []
    for i in range(count):
        b = i % len(paths)
        sequences.append({"book": str(paths[b]), "first_id": SEQ_FIRST_ID, "steps": _sequence(rng, pools[b])})
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
    p = sub.add_parser("make-opsbook")
    p.add_argument("out")
    p = sub.add_parser("make-sequences")
    p.add_argument("out")
    p.add_argument("--books", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=300)
    args = parser.parse_args(argv)
    if args.cmd == "make-opsbook":
        make_opsbook(args.out)
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
