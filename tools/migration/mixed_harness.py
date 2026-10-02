#!/usr/bin/env python3
"""The Python reference for the C++ contract test of the milestones together (M3-I; native/tests/contract/
test_contract_m3_mixed.cpp): random books carrying tones, effect lines, rulers and 3D besides panels, layers and pen
lines, and random op sequences mixing the ops of tones, effect lines, rulers and 3D with M2's (add_stroke, erase,
add_layer, split_frame, …), for the test to give to `genko apply` and to `python -m genko apply`. Development only;
nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/mixed_harness.py <command> ...

Commands:
  make-books OUT --seed N --count K
        K random v3 books (OUT/book-NN.genko) on small pages (quick to draw at 150 dpi): panels (some slanted), pen
        lines, tone layers, effect lines, rulers and layer screens (tone_harness.make_book), user layers (pen, paint,
        fill) and 3D (figures, heads, hands, models, boxes, scenes, mannequins, a camera, a light, a perspective
        ruler); some books with strict_gates or a page locked. Nothing the C++ build does not draw yet (lines and
        balloons, nombres — not on pages added later either —, placed pictures) and no pen line whose look depends
        on its id (pencil and brush grain, spray, …): the lines `genko apply` makes get new random ids on each side.
  sequences OUT --books DIR --seed N --steps S
        for each book of DIR, S random steps (1 to 3 ops each, an agent, sometimes a dry run), each made from the
        book as Python leaves it after the steps before (applied here with apply_ops). An op names only what has the
        same id on both sides: what the book had and what an op named (a panel cut, a page added or a layer copied
        without a new id get random ids). Left out: what the C++ build refuses on purpose (docs/cpp-migration/
        SPEC.md COMP-01a: a number that is not finite left in the book, a lock on a page the book does not have, a
        selection that is no panel of its page, an order of layers leaving one out — each op of a step is tried in
        turn here and a step with one is drawn again —, delete_tone of a layer that is not a tone), what it does not
        have yet (selections other than rect, ellipse, polygon and mask, erasing paint, colour mixing), undo
        (Python's `genko apply` undoes only its own session's changes) and pen kinds whose look depends on the
        line's id.
        OUT (JSON): [{"book", "steps": [{"ops", "agent", "dry_run"}]}].
"""
from __future__ import annotations

import argparse
import base64
import copy
import json
import random
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / "src", Path(__file__).resolve().parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import geom3d_harness  # noqa: E402
import pyref_harness  # noqa: E402
import tone_harness  # noqa: E402

# Pen kinds whose lines look the same whatever their id (pencil and fude have a grain; spray, stipple, grass, leaves
# and stars scatter their stamps: both drawn with the line's id as the seed).
STEADY_KINDS = ["gpen", "maru", "kabura", "mili", "marker", "airbrush", "fill_pen", "white", "fx", "calligraphy", "water",
                "dotline", "dashline", "lace", "hearts"]
AGENTS = ["genko"] * 5 + ["human:作者", "ai:hermes"]

TONE_OPS = ["add_tone", "set_tone", "delete_tone", "add_effect", "edit_effect", "delete_effect", "effect_to_layer",
            "add_ruler", "edit_ruler", "delete_ruler", "set_ruler", "ruler_to_layer"]
THREED_OPS = ["add_figure", "pose_figure", "add_head", "add_hand", "import_model", "set_camera", "set_light", "render_prims",
              "add_mannequin", "pose_mannequin", "add_prim3d", "add_scene", "edit_prim", "delete_prim", "trace_prims",
              "ruler_from_3d", "camera_from_ruler"]
M2_OPS = (["add_stroke"] * 3 + ["erase"] * 2 + ["add_layer", "set_layer", "split_frame"] * 2 +
          ["delete_stroke", "edit_stroke", "simplify_stroke", "delete_layer", "duplicate_layer", "set_layers", "reorder_layers",
           "cut_frame", "move_gutter", "add_frame", "delete_frame", "merge_frame", "resize_frame", "set_frame", "select_frame",
           "add_page", "delete_page", "duplicate_page", "reorder", "advance", "set_brush", "set_note", "set_meta", "name_ok",
           "lock_page", "unlock_page", "set_autosave", "for_pages"])


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


def page_xyz(rng: random.Random, page) -> list:
    w, h = float(page.spec.width_mm), float(page.spec.height_mm)
    return [round(rng.uniform(4, w - 4), 2), round(rng.uniform(4, h - 4), 2), rng.choice([0, 0, round(rng.uniform(-20, 60), 1)])]


# --- random books ---------------------------------------------------------------------------------------------------


def threed_ops(rng: random.Random, page) -> list:
    """add_* ops that put random 3D on a page (with explicit ids), sized for a small page; sometimes a camera, a light
    and a perspective ruler."""
    from genko import mannequin, mesh3d, prim3d

    ops = []
    for n in range(rng.randrange(1, 4)):
        pid = f"q{page.index}{n:02d}"
        pos = page_xyz(rng, page)
        kind = rng.choice(["figure", "figure", "head", "hand", "model", "prim", "prim", "scene", "mannequin"])
        if kind == "figure":
            op = {"op": "add_figure", "page": page.index, "id": pid, "pos": pos, "height_mm": rng.choice([30, 45, 70])}
            if rng.random() < 0.6:
                op["rot"] = [round(rng.uniform(-1, 1), 3) for _ in range(3)]
            if rng.random() < 0.5:
                op["preset"] = rng.choice(list(mesh3d.FIGURE_PRESETS))
            if rng.random() < 0.4:
                op["body"] = geom3d_harness.rand_body(rng)
            if rng.random() < 0.3:
                op["joints"] = geom3d_harness.rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
            if rng.random() < 0.3:
                op["hands"] = geom3d_harness.rand_hands(rng)
        elif kind in ("head", "hand"):
            op = {"op": f"add_{kind}", "page": page.index, "id": pid, "pos": pos, "size_mm": rng.choice([10, 18, 28]),
                  "rot": [round(rng.uniform(-2, 2), 3) for _ in range(3)]}
            if kind == "hand":
                op.update(side=rng.choice(["l", "r"]), pose=rng.choice(list(mesh3d.HAND_POSES)))
        elif kind == "model":
            op = {"op": "import_model", "page": page.index, "id": pid, "pos": pos, "size_mm": rng.choice([12, 25])}
            if rng.random() < 0.6:
                op["obj"] = geom3d_harness.rand_obj(rng)
            else:
                op["glb"] = base64.b64encode(geom3d_harness.glb(*geom3d_harness.TETRA, extra_nodes=rng.random() < 0.5)).decode("ascii")
        elif kind == "prim":
            op = {"op": "add_prim3d", "page": page.index, "id": pid, "pos": pos,
                  "kind": rng.choice(["box", "cylinder", "stairs", "floor", "sphere", "cone", "prop"]),
                  "size": [round(rng.uniform(6, 30), 1) for _ in range(3)] if rng.random() < 0.7 else rng.choice([12, 20.5])}
            if op["kind"] == "prop":
                op["prop"] = rng.choice(list(prim3d.PROPS))
            if rng.random() < 0.5:
                op["rot"] = [round(rng.uniform(-1.5, 1.5), 3) for _ in range(3)]
            if rng.random() < 0.3:
                op["focal_mm"] = rng.choice([150, 400, 1000])
        elif kind == "scene":
            op = {"op": "add_scene", "page": page.index, "id": pid, "kind": rng.choice(list(prim3d.SCENES))}
            size = rng.choice([None, 60, [70, 40, 90]])
            if size is not None:
                op["size"] = size
            if rng.random() < 0.5:
                op["frame_id"] = False
        else:
            op = {"op": "add_mannequin", "page": page.index, "id": pid, "pos": pos, "height_mm": rng.choice([30, 45, 60])}
            if rng.random() < 0.6:
                op["preset"] = rng.choice(list(mannequin.PRESETS))
            if rng.random() < 0.4:
                op["rot"] = [round(rng.uniform(-1, 1), 3), round(rng.uniform(-3, 3), 3), round(rng.uniform(-0.5, 0.5), 3)]
        ops.append(op)
    if rng.random() < 0.4:
        ops.append({"op": "set_camera", "page": page.index, "turn": round(rng.uniform(-1, 1), 3), "tip": round(rng.uniform(-0.5, 0.5), 3),
                    "focal_mm": rng.choice([60, 250, 900])})
    if rng.random() < 0.3:
        ops.append({"op": "set_light", "page": page.index, "dir": [round(rng.uniform(0.1, 1), 2) * rng.choice([1, -1]) for _ in range(3)],
                    "ambient": round(rng.uniform(0, 0.8), 2)})
    if rng.random() < 0.3:
        w, h = float(page.spec.width_mm), float(page.spec.height_mm)
        ops.append({"op": "add_ruler", "page": page.index, "kind": "perspective", "id": f"pr{page.index}",
                    "points": [[round(rng.uniform(-w, 2 * w), 1), round(rng.uniform(-h / 2, h), 1)] for _ in range(rng.choice([1, 2, 3]))]})
    return ops


def make_book(rng: random.Random, dest: Path) -> None:
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode

    with tempfile.TemporaryDirectory() as tmp:
        pyref_harness.fresh_process_state(True)
        base = Path(tmp) / "base.genko"
        tone_harness.make_book(rng, base, True)  # panels, pen lines, tones, effect lines, rulers, screens
        episode = load_episode(base)
    episode.nombre = {"show": False}  # (no nombre on any page, not on one added later either: nombres are drawn in M4)
    ops = []
    for page in episode.pages:
        if rng.random() < 0.5:
            ops.append({"op": "add_layer", "page": page.index, "kind": "pen", "id": f"pen{page.index}"})
            for _ in range(rng.randrange(1, 4)):
                ops.append({"op": "add_stroke", "page": page.index, "layer_id": f"pen{page.index}",
                            "points": [tone_harness.rand_xy(rng, page) for _ in range(rng.choice([2, 3, 5]))],
                            "width_mm": rng.choice([0.4, 1.2, 3]), "kind": rng.choice(STEADY_KINDS)})
        if rng.random() < 0.5:
            ops.append({"op": "add_layer", "page": page.index, "kind": "paint", "id": f"paint{page.index}"})
        if rng.random() < 0.15:
            ops.append({"op": "add_layer", "page": page.index, "kind": "fill", "id": f"fill{page.index}", "blend": "multiply",
                        "rgb": rng.choice([[235, 225, 250], [215, 240, 220]])})
        ops += threed_ops(rng, page)
    apply_ops(episode, ops, agent="human:作者")
    episode.strict_gates = rng.random() < 0.2
    if len(episode.pages) > 1 and rng.random() < 0.15:
        episode.page_locks = {episode.pages[-1].id: "ai:other"}
    save_episode(episode, dest, actor="human:作者")


def make_books(out: str, seed: int, count: int) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        make_book(random.Random(seed * 1000 + i), root / f"book-{i:02d}.genko")


# --- random op sequences --------------------------------------------------------------------------------------------


def ids_of(episode) -> set:
    """Every id the book holds: pages, panels, layers, effects, rulers, 3D."""
    out = {page.id for page in episode.pages}

    def walk(frame):
        out.add(frame.id)
        for child in frame.children:
            walk(child)

    for page in episode.pages:
        for frame in page.frames:
            walk(frame)
        out.update(layer.id for layer in page.layers)
        for items in (page.effects, page.rulers, page.prims):
            out.update(item["id"] for item in items if isinstance(item, dict) and isinstance(item.get("id"), str))
    return out


def strings_in(value) -> set:
    if isinstance(value, str):
        return {value}
    if isinstance(value, dict):
        value = list(value.values())
    if isinstance(value, list):
        out = set()
        for item in value:
            out |= strings_in(item)
        return out
    return set()


def frame_ids(page) -> set:
    out = set()

    def walk(frame):
        out.add(frame.id)
        for child in frame.children:
            walk(child)

    for frame in page.frames:
        walk(frame)
    return out


NONFINITE = re.compile(r'(?<![\w"])(NaN|-?Infinity)(?![\w"])')


def finite(episode) -> bool:
    """Whether the book as Python saves it holds finite numbers only (its JSON gets NaN and Infinity otherwise)."""
    from genko.io import save_episode

    with tempfile.TemporaryDirectory() as tmp:
        book = Path(tmp) / "b.genko"
        save_episode(copy.deepcopy(episode), book)
        return not any(NONFINITE.search(f.read_text(encoding="utf-8")) for f in book.rglob("*.json"))


def refused_on_purpose(episode, op) -> bool:
    """Whether the C++ build refuses this op on purpose where Python takes it (docs/cpp-migration/SPEC.md COMP-01a,
    M2/M3): a lock on a page the book does not have, a selection that is no panel of its page, an order of layers that
    does not list each of the page's layers once, a mannequin op that would rewrite another kind (covered separately
    as an unchanged refusal by test_ops_3d and Hermes's m3_kind_check)."""
    from genko.ops import _op_page_index

    if not isinstance(op, dict) or op.get("op") not in ("lock_page", "unlock_page", "select_frame", "reorder_layers", "pose_mannequin"):
        return False
    index = _op_page_index(episode, op)
    page = next((p for p in episode.pages if p.index == index), None) if index is not None else None
    if op["op"] in ("lock_page", "unlock_page"):
        return index is not None and page is None
    if page is None:
        return False  # (refused on both sides: no such page)
    if op["op"] == "pose_mannequin":
        prim = next((p for p in page.prims if isinstance(p, dict) and p.get("id") == op.get("id")), None)
        return prim is not None and prim.get("kind") != "mannequin"
    if op["op"] == "select_frame":
        frame = op.get("frame_id")
        return not (isinstance(frame, str) and frame in frame_ids(page))
    order = op.get("order") or []
    ids = [layer.id for layer in page.layers]
    return not (isinstance(order, list) and all(isinstance(item, str) for item in order) and len(set(ids)) == len(ids) and
                sorted(order) == sorted(ids))


def trial(episode, ops: list, agent: str) -> str:
    """How a step goes, tried op by op on a copy of the book: "refused" (Python refuses it or stops with a traceback,
    and so does the C++ build), "again" (the C++ build would refuse an op on purpose, or the book would hold a number
    that is not finite), else "ok"."""
    from genko.headless import apply_ops

    book = copy.deepcopy(episode)
    for op in ops:
        if refused_on_purpose(book, op):
            return "again"
        try:
            apply_ops(book, [copy.deepcopy(op)], agent=agent)
        except Exception:  # noqa: BLE001 (ApplyError, or a traceback in Python's command line)
            return "refused"
        if not finite(book):
            return "again"
    return "ok"


class MixGen(tone_harness.Gen):
    """Random ops made from the book as it is, naming only what has the same id on both sides (`stable`)."""

    def __init__(self, rng: random.Random, episode, stable: set, agent: str) -> None:
        super().__init__(rng, episode, set())
        self.stable = stable
        self.agent = agent

    # --- what an op names -------------------------------------------------------------------------------------------

    def page(self):
        # (mostly a page the step's agent may change: one locked by another is refused at once)
        free = [page for page in self.ep.pages if self.ep.page_locks.get(page.id) in (None, self.agent)]
        if free and self.rng.random() < 0.85:
            return self.rng.choice(free)
        return self.rng.choice(self.ep.pages)

    def layer_id(self, page, tone: bool | None = None):
        layers = [layer for layer in page.layers if layer.id in self.stable and
                  (tone is None or tone == (layer.role.value == "tone" or layer.kind.value == "tone"))]
        if not layers or self.rng.random() < 0.08:
            return self.rng.choice(["nope", "", None])
        return self.rng.choice(layers).id

    def frame_id(self, page):
        leaves = [frame for frame in page.leaf_frames() if frame.id in self.stable]
        if not leaves or self.rng.random() < 0.08:
            return self.rng.choice(["no-panel", False])
        return self.rng.choice(leaves).id

    def effect_id(self, page):
        ids = [e["id"] for e in page.effects if isinstance(e, dict) and e.get("id") in self.stable]
        if not ids or self.rng.random() < 0.1:
            return self.rng.choice(["no-effect", None])
        return self.rng.choice(ids)

    def ruler_id(self, page):
        ids = [r["id"] for r in page.rulers if isinstance(r, dict) and r.get("id") in self.stable]
        if not ids or self.rng.random() < 0.1:
            return self.rng.choice(["no-ruler", None])
        return self.rng.choice(ids)

    def prim_ids(self, page, kinds=None) -> list:
        return [p["id"] for p in page.prims if isinstance(p, dict) and p.get("id") in self.stable and
                (kinds is None or p.get("kind") in kinds)]

    def prim_id(self, page, kinds=None):
        ids = self.prim_ids(page, kinds) or self.prim_ids(page)
        if not ids or self.rng.random() < 0.06:
            return self.rng.choice(["nothing", None])
        return self.rng.choice(ids)

    def num(self, values, bad=()):
        # (no "inf": a number that is not finite is refused by the C++ build on purpose)
        return super().num(values, [b for b in bad if b not in ("inf", "-inf", "nan")])

    def new_id(self, prefix: str) -> str:
        return f"{prefix}{self.rng.randrange(10000):04d}"

    def pos(self, page) -> list:
        return page_xyz(self.rng, page)

    def to(self, page) -> list:
        w, h = float(page.spec.width_mm), float(page.spec.height_mm)
        return [round(self.rng.uniform(0, w), 2), round(self.rng.uniform(0, h), 2)]

    def points(self, page, pressure: bool | None = None) -> list:
        pts = [self.xy(page) for _ in range(self.rng.choice([2, 3, 4, 6]))]
        if pressure if pressure is not None else self.rng.random() < 0.3:
            pts = [p + [round(self.rng.uniform(0.1, 1), 2)] for p in pts]
        return pts

    # --- tones, effect lines and rulers (tone_harness.Gen), an effect always named ----------------------------------

    def add_effect(self):
        op = super().add_effect()
        op.setdefault("id", "e-" + str(self.rng.randrange(1000)))  # (an effect's lines are drawn with its id as the seed)
        return op

    # --- 3D ------------------------------------------------------------------------------------------------------------

    def add_figure(self):
        from genko import mesh3d

        rng = self.rng
        page = self.page()
        op = {"op": "add_figure", "page": self.page_no(page), "pos": self.pos(page), "height_mm": rng.choice([30, 50, 80, 9, 401, "45"])}
        if rng.random() < 0.8:
            op["id"] = self.new_id("n") if rng.random() < 0.9 else self.prim_id(page)
        for key, make in (("preset", lambda: rng.choice(list(mesh3d.FIGURE_PRESETS) + ["dance"])),
                          ("body", lambda: geom3d_harness.rand_body(rng) if rng.random() < 0.9 else {"heads": 20}),
                          ("joints", lambda: geom3d_harness.rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))),
                          ("hands", lambda: geom3d_harness.rand_hands(rng) if rng.random() < 0.9 else {"r": "wave"}),
                          ("rot", lambda: [round(rng.uniform(-2, 2), 3) for _ in range(3)])):
            if rng.random() < 0.3:
                op[key] = make()
        if rng.random() < 0.2:
            op["frame_id"] = self.frame_id(page)
        return op

    def pose_figure(self):
        from genko import mesh3d, threeops

        rng = self.rng
        page = self.page()
        op = {"op": "pose_figure", "page": self.page_no(page), "id": self.prim_id(page, ("figure", "hand"))}
        choice = rng.random()
        if choice < 0.2:
            op["joints"] = geom3d_harness.rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
        elif choice < 0.3:
            op["set_joints"] = geom3d_harness.rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
        elif choice < 0.4:
            op["preset"] = rng.choice(list(mesh3d.FIGURE_PRESETS))
        elif choice < 0.5:
            op["body"] = geom3d_harness.rand_body(rng) or {"sex": "female"}
        elif choice < 0.6:
            op["hands"] = geom3d_harness.rand_hands(rng) or {"l": "fist"}
        elif choice < 0.7:
            op["pose"] = rng.choice(list(mesh3d.HAND_POSES) + ["wave"])
            if rng.random() < 0.5:
                op["curls"] = [round(rng.random(), 2) for _ in range(5)]
        elif choice < 0.85:
            op["drag"] = {"handle": rng.choice(["pelvis", *threeops.HANDLES, "tail"]), "to": self.to(page)}
        else:
            op["drag"] = {"handle": rng.choice(list(threeops.IK_CHAINS) + ["head"]), "ik": True, "to": self.to(page)}
        if rng.random() < 0.2:
            op["height_mm"] = rng.choice([40, 90])
        if rng.random() < 0.2:
            op["rot"] = [round(rng.uniform(-1, 1), 3) for _ in range(3)]
        return op

    def head_or_hand(self, name: str):
        from genko import mesh3d

        rng = self.rng
        page = self.page()
        op = {"op": name, "page": self.page_no(page), "pos": self.pos(page) if rng.random() < 0.95 else "x"}
        if rng.random() < 0.8:
            op["size_mm"] = rng.choice([10, 18, 30])
        if name == "add_hand":
            if rng.random() < 0.5:
                op["side"] = rng.choice(["l", "r", "x"])
            if rng.random() < 0.6:
                op["pose"] = rng.choice(list(mesh3d.HAND_POSES) + ["wave"])
        if rng.random() < 0.4:
            op["rot"] = [round(rng.uniform(-2, 2), 3) for _ in range(3)]
        if rng.random() < 0.7:
            op["id"] = self.new_id("n")
        return op

    def add_head(self):
        return self.head_or_hand("add_head")

    def add_hand(self):
        return self.head_or_hand("add_hand")

    def import_model(self):
        rng = self.rng
        page = self.page()
        op = {"op": "import_model", "page": self.page_no(page), "pos": self.pos(page), "id": self.new_id("m"),
              "size_mm": rng.choice([12, 25])}
        k = rng.random()
        if k < 0.6:
            op["obj"] = geom3d_harness.rand_obj(rng) if rng.random() < 0.85 else rng.choice(["v 0 0 0", "   ", 5])
        elif k < 0.9:
            op["glb"] = (base64.b64encode(geom3d_harness.glb(*geom3d_harness.TETRA, extra_nodes=rng.random() < 0.5)).decode("ascii")
                         if rng.random() < 0.85 else "!!!")
        else:
            op["gltf"] = '{"buffers": [{"uri": "model.bin"}]}'
        return op

    def set_camera(self):
        rng = self.rng
        page = self.page()
        op = {"op": "set_camera", "page": self.page_no(page)}
        if rng.random() < 0.15:
            op["off"] = True
        for key in ("turn", "tip", "roll"):
            if rng.random() < 0.6:
                op[key] = round(rng.uniform(-1.2, 1.2), 5)
        if rng.random() < 0.5:
            op["focal_mm"] = rng.choice([20, 300, 5000, 10, 9000, 333.3])
        if rng.random() < 0.3:
            op["target"] = rng.choice([self.to(page), None, [5], [10.5, 20, 30]])
        return op

    def set_light(self):
        rng = self.rng
        page = self.page()
        op = {"op": "set_light", "page": self.page_no(page)}
        if rng.random() < 0.7:
            op["dir"] = rng.choice([[round(rng.uniform(-1, 1), 3) for _ in range(3)], [0, 0, 0], [1, 1]])
        if rng.random() < 0.6:
            op["ambient"] = rng.choice([0.2, 0.0, 1.5, -1, 0.65])
        return op

    def prims_op(self, name: str):
        rng = self.rng
        page = self.page()
        op = {"op": name, "page": self.page_no(page)}
        roll = rng.random()
        if roll < 0.7:
            op["layer_id"] = self.layer_id(page)
        elif roll < 0.85:
            op["layer"] = rng.choice(["ink", "name", "draft", "bg", "nope"])
        prims = self.prim_ids(page)
        if prims and rng.random() < 0.4:
            op["ids"] = rng.sample(prims, min(len(prims), rng.randrange(1, 3)))
        if name == "trace_prims" or rng.random() < 0.3:
            # (trace_prims draws in pencil when not told, whose grain follows the new lines' random ids)
            op["kind"] = rng.choice(["mili", "gpen", "maru", "oil", "laser"])
        if rng.random() < 0.3:
            op["width_mm"] = rng.choice([0.2, 1, "0.5"])
        if rng.random() < 0.2:
            op["rgb"] = [10, 20, 200]
        if name == "render_prims":
            if rng.random() < 0.3:
                op["surfaces"] = False
            if rng.random() < 0.2:
                op["lines"] = False
            if rng.random() < 0.2:
                op["tone"] = rng.choice([{"lpi": 50}, {"lpi": 200}, {"pattern": "line", "angle": 30}])
            if rng.random() < 0.2:
                op["light"] = [0.5, -1, -0.3]
            if rng.random() < 0.2:
                op["ambient"] = 0.5
        return op

    def render_prims(self):
        return self.prims_op("render_prims")

    def trace_prims(self):
        return self.prims_op("trace_prims")

    def add_mannequin(self):
        from genko import mannequin

        rng = self.rng
        page = self.page()
        op = {"op": "add_mannequin", "page": self.page_no(page), "pos": self.pos(page)}
        if rng.random() < 0.7:
            op["height_mm"] = rng.choice([30, 50, 70])
        if rng.random() < 0.4:
            op["preset"] = rng.choice(list(mannequin.PRESETS) + ["dance"])
        if rng.random() < 0.3:
            op["rot"] = [round(rng.uniform(-1, 1), 3) for _ in range(3)]
        if rng.random() < 0.7:
            op["id"] = self.new_id("n")
        return op

    def pose_mannequin(self):
        from genko import mannequin

        rng = self.rng
        page = self.page()
        op = {"op": "pose_mannequin", "page": self.page_no(page), "id": self.prim_id(page, ("mannequin",))}
        c = rng.random()
        if c < 0.3:
            op["joints"] = geom3d_harness.rand_joints(rng, mannequin.JOINTS, ("yaw", "pitch"))
        elif c < 0.45:
            op["preset"] = rng.choice(list(mannequin.PRESETS))
        elif c < 0.8:
            op["drag"] = {"handle": rng.choice(["pelvis", *mannequin.HANDLES, "tail"]), "to": self.to(page)}
        else:
            op["height_mm"] = rng.choice([30, 60])
        if rng.random() < 0.2:
            op["rot"] = [round(rng.uniform(-0.5, 0.5), 3), round(rng.uniform(-3, 3), 3), 0]
        if rng.random() < 0.2:
            op["pos"] = self.pos(page)
        return op

    def add_prim3d(self):
        from genko import prim3d

        rng = self.rng
        page = self.page()
        op = {"op": "add_prim3d", "page": self.page_no(page), "pos": self.pos(page),
              "kind": rng.choice(["box", "cylinder", "stairs", "floor", "sphere", "cone", "prop", "torus"])}
        if rng.random() < 0.6:
            op["size"] = rng.choice([[8, 12, 16], 14, "x", [10]])
        if op["kind"] == "prop":
            op["prop"] = rng.choice(list(prim3d.PROPS) + ["spaceship"])
        if rng.random() < 0.3:
            op["rot"] = [round(rng.uniform(-1.5, 1.5), 3) for _ in range(3)]
        if rng.random() < 0.7:
            op["id"] = self.new_id("n")
        if rng.random() < 0.2:
            op["frame_id"] = self.frame_id(page)
        return op

    def add_scene(self):
        from genko import prim3d

        rng = self.rng
        page = self.page()
        op = {"op": "add_scene", "page": self.page_no(page), "kind": rng.choice(list(prim3d.SCENES) + ["castle"])}
        if rng.random() < 0.4:
            op["size"] = rng.choice([60, [70, 40, 90], "x"])
        if rng.random() < 0.3:
            op["frame_id"] = rng.choice([self.frame_id(page), False, "nope"])
        if rng.random() < 0.7:
            op["id"] = self.new_id("n")
        return op

    def edit_prim(self):
        rng = self.rng
        page = self.page()
        w, h = float(page.spec.width_mm), float(page.spec.height_mm)
        op = {"op": "edit_prim", "page": self.page_no(page), "id": self.prim_id(page)}
        if rng.random() < 0.4:
            op["pos"] = [round(rng.uniform(0, w), 2), round(rng.uniform(0, h), 2)] + ([round(rng.uniform(-20, 60), 1)] if rng.random() < 0.7 else [])
        if rng.random() < 0.4:
            op["size"] = [round(rng.uniform(5, 40), 1) for _ in range(rng.choice([2, 3]))]
        if rng.random() < 0.4:
            op["rot"] = [round(rng.uniform(-2, 2), 3) for _ in range(rng.choice([2, 3]))]
        if rng.random() < 0.3:
            op["focal_mm"] = rng.choice([10, 300])
        return op

    def delete_prim(self):
        page = self.page()
        return {"op": "delete_prim", "page": self.page_no(page), "id": self.prim_id(page)}

    def ruler_from_3d(self):
        rng = self.rng
        page = self.page()
        op = {"op": "ruler_from_3d", "page": self.page_no(page)}
        if rng.random() < 0.8:
            op["prim_id"] = self.prim_id(page)
        if rng.random() < 0.6:
            op["id"] = self.new_id("r") if rng.random() < 0.9 else self.ruler_id(page)
        if rng.random() < 0.4:
            op["grid"] = rng.choice([0, 10, 60])
        return op

    def camera_from_ruler(self):
        rng = self.rng
        page = self.page()
        op = {"op": "camera_from_ruler", "page": self.page_no(page), "id": self.ruler_id(page)}
        if rng.random() < 0.6:
            op["prim_id"] = self.prim_id(page)
        return op

    # --- M2: pen lines, layers, panels, pages, the book ------------------------------------------------------------

    def add_stroke(self):
        rng = self.rng
        page = self.page()
        op = {"op": "add_stroke", "page": self.page_no(page), "points": self.points(page)}
        r = rng.random()
        if r < 0.45:
            op["layer_id"] = self.layer_id(page)
        elif r < 0.75:
            op["layer"] = rng.choice(["ink", "name", "ink"])
        if rng.random() < 0.5:
            op["kind"] = rng.choice(STEADY_KINDS)
        if rng.random() < 0.4:
            op["width_mm"] = rng.choice([0.3, 0.8, 2, 5])
        if rng.random() < 0.2:
            op["rgb"] = rng.choice([[200, 20, 20], [20, 20, 200], [128, 128, 128]])
        if rng.random() < 0.15:
            op["opacity"] = rng.choice([0.5, 1])
        if rng.random() < 0.15:
            op["taper"] = rng.random() < 0.5
        if rng.random() < 0.2:
            op["snap_ruler"] = True
        if rng.random() < 0.15:
            op["ruler_id"] = self.ruler_id(page)
        return op

    def role_layer(self, page, role: str):
        return next((layer for layer in page.layers if layer.role.value == role), None)

    def stroke_index(self, page, role: str):
        layer = self.role_layer(page, role)
        count = len(layer.strokes) if layer is not None else 0
        if count == 0 or self.rng.random() < 0.1:
            return self.rng.choice([0, 5, -1, "x"])
        return self.rng.randrange(count)

    def stroke_op(self, name: str) -> tuple[dict, object]:
        page = self.page()
        role = self.rng.choice(["ink", "name"])
        op = {"op": name, "page": self.page_no(page), "index": self.stroke_index(page, role)}
        if role == "ink" or self.rng.random() < 0.5:
            op["layer"] = role
        return op, page

    def delete_stroke(self):
        return self.stroke_op("delete_stroke")[0]

    def edit_stroke(self):
        op, page = self.stroke_op("edit_stroke")
        op["points"] = self.points(page, False) if self.rng.random() < 0.9 else [self.xy(page)]
        return op

    def simplify_stroke(self):
        op = self.stroke_op("simplify_stroke")[0]
        if self.rng.random() < 0.5:
            op["epsilon_mm"] = self.rng.choice([0.2, 0.8, 3])
        return op

    def erase(self):
        rng = self.rng
        page = self.page()
        op = {"op": rng.choice(["erase", "erase", "erase_raster"]), "page": self.page_no(page),
              "points": self.points(page, False), "width_mm": rng.choice([1, 2.5, 6])}
        # a pen or tone layer, or the ink or name layer — never paint: erasing pixels is not in the C++ build yet
        layers = [layer for layer in page.layers if layer.id in self.stable and layer.kind.value in ("strokes", "tone") and not layer.raster_png]
        roles = []
        for role in ("ink", "name"):
            layer = self.role_layer(page, role)
            if layer is None or not layer.raster_png:
                roles.append(role)
        if layers and (rng.random() < 0.5 or not roles):
            op["layer_id"] = rng.choice(layers).id
        elif roles:
            op["layer"] = rng.choice(roles)
        else:
            return self.add_stroke()
        if rng.random() < 0.3:
            op["mode"] = rng.choice(["cut", "to_crossing", "whole", "sideways"])
        if rng.random() < 0.2:
            op["snap_ruler"] = True
        if rng.random() < 0.1:
            op["soft"] = True
        return op

    def add_layer(self):
        rng = self.rng
        page = self.page()
        op = {"op": "add_layer", "page": self.page_no(page), "id": self.new_id("L-") if rng.random() < 0.92 else self.layer_id(page)}
        kind = rng.choice(["pen", "pen", "paint", "paint", "folder", "fill", "tone", None])
        if kind is not None:
            op["kind"] = kind
        if kind == "fill":
            op["rgb"] = rng.choice([[230, 220, 250], [200, 240, 210]])
            op["blend"] = "multiply"
        if rng.random() < 0.3:
            op["after"] = self.layer_id(page)
        if rng.random() < 0.2:
            op["name"] = rng.choice(["下描き", "影", ""])
        if rng.random() < 0.15:
            op["panel_each"] = rng.random() < 0.5
        return op

    def delete_layer(self):
        page = self.page()
        return {"op": "delete_layer", "page": self.page_no(page), "id": self.layer_id(page)}

    def duplicate_layer(self):
        page = self.page()
        op = {"op": "duplicate_layer", "page": self.page_no(page), "id": self.layer_id(page)}
        if self.rng.random() < 0.6:
            op["new_id"] = self.new_id("D-")
        return op

    LAYER_FIELDS = (("visible", [True, False]), ("opacity", [0.4, 1, 0.75]), ("blend", ["normal", "multiply", "screen", "darken"]),
                    ("locked", [True, False]), ("panel_clip", [True, False]), ("clip", [True, False]),
                    ("exportable", [True, False]), ("color", [[200, 50, 50], None]))

    def set_layer(self):
        rng = self.rng
        page = self.page()
        op = {"op": "set_layer", "page": self.page_no(page), "id": self.layer_id(page)}
        for key, values in self.LAYER_FIELDS + (("name", ["影", "線"]),):
            if rng.random() < 0.15:
                op[key] = rng.choice(values)
        if rng.random() < 0.15:
            op["screen"] = rng.choice([{"lpi": 40}, {"pattern": "line", "lpi": 50, "angle": 30}, None,
                                       {"pattern": "dot", "lpi": 60, "shape": "square"}])
        if rng.random() < 0.08:
            op["effect"] = rng.choice([{"border": {"width_mm": 0.5, "rgb": [255, 255, 255]}}, None])
        return op

    def set_layers(self):
        rng = self.rng
        page = self.page()
        op = {"op": "set_layers", "page": self.page_no(page)}
        if rng.random() < 0.2:
            op["all"] = True
        else:
            stable = [layer.id for layer in page.layers if layer.id in self.stable]
            op["ids"] = rng.sample(stable, min(len(stable), rng.randrange(1, 4))) if stable else ["nope"]
        for key, values in self.LAYER_FIELDS:
            if rng.random() < 0.25:
                op[key] = rng.choice(values)
        return op

    def reorder_layers(self):
        # (every layer of the page once: the C++ build refuses an order that leaves one out, which Python drops)
        page = self.page()
        if not all(layer.id in self.stable for layer in page.layers):
            return self.set_layer()
        order = [layer.id for layer in page.layers]
        self.rng.shuffle(order)
        return {"op": "reorder_layers", "page": page.index, "order": order}

    def leaf(self, page):
        leaves = [frame for frame in page.leaf_frames() if frame.id in self.stable]
        return self.rng.choice(leaves) if leaves else None

    def splits(self, page) -> list:
        out = []

        def walk(frame):
            if frame.children:
                if frame.id in self.stable:
                    out.append(frame.id)
                for child in frame.children:
                    walk(child)

        for frame in page.frames:
            walk(frame)
        return out

    def split_frame(self):
        rng = self.rng
        page = self.page()
        op = {"op": "split_frame", "page": self.page_no(page), "axis": rng.choice(["horizontal", "vertical"] * 10 + ["diagonal"]),
              "ratio": rng.choice([0.3, 0.5, 0.62]), "gutter_mm": rng.choice([2, 3, 4.5])}
        if rng.random() < 0.75:
            op["frame_id"] = self.frame_id(page)
        if rng.random() < 0.2:
            op["tilt_mm"] = rng.choice([3, -4, 6])
        return op

    def cut_frame(self):
        rng = self.rng
        page = self.page()
        frame = self.leaf(page)
        r = frame.rect if frame is not None else page.inner_rect_mm()
        x, y, w, h = float(r.x), float(r.y), float(r.width), float(r.height)
        if rng.random() < 0.5:  # across
            p0 = [round(x - 3, 2), round(y + h * rng.uniform(0.2, 0.8), 2)]
            p1 = [round(x + w + 3, 2), round(y + h * rng.uniform(0.2, 0.8), 2)]
        else:  # down
            p0 = [round(x + w * rng.uniform(0.2, 0.8), 2), round(y - 3, 2)]
            p1 = [round(x + w * rng.uniform(0.2, 0.8), 2), round(y + h + 3, 2)]
        op = {"op": "cut_frame", "page": self.page_no(page), "p0": p0, "p1": p1}
        if frame is not None and rng.random() < 0.7:
            op["frame_id"] = frame.id
        if rng.random() < 0.3:
            op["gutter_mm"] = rng.choice([2, 5])
        return op

    def move_gutter(self):
        rng = self.rng
        page = self.page()
        splits = self.splits(page)
        op = {"op": "move_gutter", "page": self.page_no(page),
              "frame_id": rng.choice(splits) if splits and rng.random() < 0.9 else self.frame_id(page),
              "delta_mm": rng.choice([-6, -2, 3, 8])}
        if rng.random() < 0.3:
            op["index"] = rng.choice([0, 1])
        if rng.random() < 0.3:
            op["gutter_mm"] = rng.choice([2, 6])
        return op

    def add_frame(self):
        rng = self.rng
        page = self.page()
        w, h = float(page.spec.width_mm), float(page.spec.height_mm)
        op = {"op": "add_frame", "page": self.page_no(page), "id": self.new_id("fr-")}
        if rng.random() < 0.6:
            op["rect"] = [round(rng.uniform(5, w / 2), 1), round(rng.uniform(5, h / 2), 1), round(rng.uniform(10, w / 2), 1),
                          round(rng.uniform(10, h / 2), 1)]
        else:
            cx, cy = rng.uniform(w * 0.3, w * 0.7), rng.uniform(h * 0.3, h * 0.7)
            shape = rng.choice([[(-15, -10), (15, -12), (12, 14), (-14, 10)], [(-10, 0), (0, -12), (10, 0), (0, 12), (-5, 5)]])
            op["points"] = [[round(cx + dx, 1), round(cy + dy, 1)] for dx, dy in shape]
        if rng.random() < 0.2:
            op["border_mm"] = rng.choice([0, 0.5, 1.5])
        return op

    def delete_frame(self):
        rng = self.rng
        page = self.page()
        splits = self.splits(page)
        op = {"op": "delete_frame", "page": self.page_no(page),
              "frame_id": rng.choice(splits) if splits and rng.random() < 0.2 else self.frame_id(page)}
        if rng.random() < 0.3:
            op["force"] = True
        return op

    def merge_frame(self):
        rng = self.rng
        page = self.page()
        splits = self.splits(page)
        op = {"op": "merge_frame", "page": self.page_no(page),
              "frame_id": rng.choice(splits) if splits and rng.random() < 0.8 else self.frame_id(page)}
        if rng.random() < 0.3:
            op["force"] = True
        return op

    def resize_frame(self):
        rng = self.rng
        page = self.page()
        frame = self.leaf(page)
        r = frame.rect if frame is not None else page.inner_rect_mm()
        return {"op": "resize_frame", "page": self.page_no(page), "frame_id": frame.id if frame is not None else "no-panel",
                "rect": {"x": round(float(r.x) + rng.uniform(-3, 3), 2), "y": round(float(r.y) + rng.uniform(-3, 3), 2),
                         "width": round(float(r.width) * rng.uniform(0.6, 1.1), 2), "height": round(float(r.height) * rng.uniform(0.6, 1.1), 2)}}

    def set_frame(self):
        rng = self.rng
        page = self.page()
        op = {"op": "set_frame", "page": self.page_no(page), "frame_id": self.frame_id(page)}
        # (no rough border: its wobble is drawn with the panel's id as the seed, and a cut panel's parts get new ids)
        for key, values in (("bleed", [True, False]), ("clip", [True, False, 0]), ("border_mm", [0, 0.5, 1.2, 2]), ("corner_mm", [0, 2, 4]),
                            ("line", [{"kind": "double", "gap_mm": 0.6}, {"kind": "dashed", "dash_mm": 2}, {"kind": "dotted"},
                                      {"kind": "solid", "rgb": [60, 60, 200]}, None])):
            if rng.random() < 0.3:
                op[key] = rng.choice(values)
        if rng.random() < 0.1:
            op["curves"] = rng.choice([[2, 0, -2, 0], None])
        if rng.random() < 0.08:
            op["bow"] = {"edge": rng.randrange(4), "mm": rng.choice([3, -2])}
        return op

    def select_frame(self):
        page = self.page()
        return {"op": "select_frame", "page": self.page_no(page), "frame_id": self.frame_id(page)}

    def add_page(self):
        op = {"op": "add_page"}
        if self.rng.random() < 0.3:
            op["after"] = self.rng.choice([0, 1, len(self.ep.pages)])
        return op

    def delete_page(self):
        return {"op": "delete_page", "page": self.page_no()}

    def duplicate_page(self):
        op = {"op": "duplicate_page", "page": self.page_no()}
        if self.rng.random() < 0.4:
            op["next_to"] = True
        return op

    def reorder(self):
        order = list(range(1, len(self.ep.pages) + 1))
        self.rng.shuffle(order)
        if self.rng.random() < 0.1:
            order = order[:-1] or [2]
        return {"op": "reorder", "order": order}

    def advance(self):
        return {"op": "advance", "page": self.page_no(), "to": self.rng.choice(["name", "ink", "finish", "colour"])}

    def set_brush(self):
        rng = self.rng
        op = {"op": "set_brush"}
        if rng.random() < 0.5:
            op["rgb"] = rng.choice([[200, 10, 10], [20, 20, 20], [10, 90, 200]])
        if rng.random() < 0.4:
            op["width_mm"] = rng.choice([0.4, 1.5])
        if rng.random() < 0.3:
            op["taper"] = rng.random() < 0.5
        return op

    def set_meta(self):
        rng = self.rng
        op = {"op": "set_meta"}
        if rng.random() < 0.6:
            op["strict_gates"] = rng.random() < 0.4
        if rng.random() < 0.3:
            op["title"] = rng.choice(["新しい題", "t"])
        if rng.random() < 0.1:
            op["binding"] = rng.choice(["left", "right"])
        return op

    def lock_page(self):
        op = {"op": "lock_page", "page": self.page_no()}
        if self.rng.random() < 0.6:
            op["agent"] = self.rng.choice(["genko", "human:作者", "ai:hermes", "genko", "ai:other"])
        return op

    def for_pages(self):
        rng = self.rng
        inner = rng.choice([[{"op": "set_note", "note": "全"}], [{"op": "add_layer", "kind": "pen", "id": self.new_id("F-")}],
                            [{"op": "name_ok"}]])
        return {"op": "for_pages", "pages": rng.choice(["all", [1], [1, 2]]), "ops": inner}

    def one(self) -> dict:
        r = self.rng.random()
        names = TONE_OPS if r < 0.36 else THREED_OPS if r < 0.66 else M2_OPS
        return getattr(self, self.rng.choice(names))()


def run_sequences(out: str, books: str, seed: int, steps: int) -> None:
    from genko.headless import apply_ops
    from genko.io import load_episode

    result = []
    for k, book in enumerate(sorted(str(p) for p in Path(books).glob("book-*.genko"))):
        rng = random.Random(seed * 100003 + k)
        pyref_harness.fresh_process_state(True, 900000000000)  # (ids here unlike the book's: 900000000000, …)
        episode = load_episode(Path(book))
        stable = ids_of(episode)
        sequence = {"book": book, "steps": []}
        for _ in range(steps):
            for _attempt in range(100):
                agent = rng.choice(AGENTS)
                gen = MixGen(rng, episode, stable, agent)
                ops = [gen.one() for _ in range(1 if rng.random() < 0.8 else rng.choice([2, 3]))]
                dry_run = rng.random() < 0.05
                outcome = trial(episode, ops, agent)
                if outcome == "again":
                    continue
                if outcome == "ok" and not dry_run:
                    after = copy.deepcopy(episode)
                    try:
                        apply_ops(after, copy.deepcopy(ops), agent=agent)
                    except Exception:  # noqa: BLE001 (the batch refused as a whole: the book stays as it was)
                        pass
                    else:
                        episode = after
                        stable |= ids_of(episode) & strings_in(ops)
                break
            else:
                raise SystemExit(f"{book}: no step the C++ build would not refuse on purpose")
            sequence["steps"].append({"ops": ops, "agent": agent, "dry_run": dry_run})
        result.append(sequence)
    Path(out).write_text(dumps(result), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make-books")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=32)
    p = sub.add_parser("sequences")
    p.add_argument("out")
    p.add_argument("--books", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--steps", type=int, default=10)
    args = parser.parse_args(argv)
    if args.cmd == "make-books":
        make_books(args.out, args.seed, args.count)
    elif args.cmd == "sequences":
        run_sequences(args.out, args.books, args.seed, args.steps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
