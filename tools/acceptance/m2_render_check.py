#!/opt/pyref/bin/python
"""Hermes の独立確認（M2 描画）: 作業者の試験を使わず、C++ の `genko render` と Python の render_page を直接比べる。

コンテナ内で実行: /src/build/linux-release/src/api/genko を使う。原稿は固定 seed で Python 側から作る（台詞・トーン・効果線・
3D・配置画像・ノンブルなど M2 で未移植の要素は入れない）。結果は差の画素数・最大差を表示する。
"""
import json, os, random, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, "/src/src")
from PIL import Image, ImageChops  # noqa: E402

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402

G = os.environ.get("GENKO_BIN", "/src/build/linux-release/src/api/genko")
BRUSHES = ["gpen", "maru", "kabura", "mili", "pencil", "fude", "marker", "airbrush", "fill_pen", "white", "fx",
           "calligraphy", "water", "spray", "stipple", "dotline", "dashline", "lace", "grass", "leaves", "hearts", "stars"]
BLENDS = ["normal", "multiply", "screen", "overlay", "darken", "lighten", "add", "subtract", "difference"]


def make_book(seed: int, root: Path) -> Path:
    rnd = random.Random(seed)
    spec = rnd.choice([PageSpec.b4_comic(), PageSpec.a4_mono(), PageSpec.b5_doujin()])
    ep = new_episode(f"確認 {seed}", 1, rnd.randint(1, 2), spec)
    for page in ep.pages:
        page.numero = False  # (page numbers are lettering: M4, not this check)
    ops = []
    for page in ep.pages:
        idx = page.index
        if rnd.random() < 0.8:
            ops.append({"op": "split_frame", "page": idx, "axis": rnd.choice(["horizontal", "vertical"]),
                        "ratio": round(rnd.uniform(0.3, 0.7), 3), "gutter_mm": rnd.choice([2, 4, 6])})
        ops.append({"op": "name_ok", "page": idx})
        for _ in range(rnd.randint(3, 25)):
            n = rnd.randint(2, 30)
            x0, y0 = rnd.uniform(20, 160), rnd.uniform(20, 230)
            pts = []
            for k in range(n):
                pts.append([round(x0 + k * rnd.uniform(-3, 3), 3), round(y0 + k * rnd.uniform(-3, 3), 3),
                            round(rnd.uniform(0.1, 1.0), 3)])
            op = {"op": "add_stroke", "page": idx, "layer": rnd.choice(["ink", "ink", "name", "bg"]), "points": pts,
                  "kind": rnd.choice(BRUSHES), "width_mm": round(rnd.uniform(0.2, 3.0), 2), "stabilize": rnd.choice([0, 0, 3])}
            if rnd.random() < 0.3:
                op["rgb"] = [rnd.randint(0, 255) for _ in range(3)]
            if rnd.random() < 0.2:
                op["opacity"] = round(rnd.uniform(0.2, 1.0), 2)
            ops.append(op)
    apply_ops(ep, ops, agent="human:確認")
    if os.environ.get("HERMES_TONES"):
        # M3-B: tones, effect lines and rulers (drawn as pen lines), placed with Python's own ops
        more = []
        for page in ep.pages:
            idx = page.index
            for _ in range(rnd.randint(1, 3)):
                tone = {"op": "add_tone", "page": idx, "density": round(rnd.uniform(0.05, 0.6), 2),
                        "lpi": rnd.choice([42.5, 55, 60, 85]), "angle": rnd.choice([0, 15, 45, 30.5]),
                        "pattern": rnd.choice(["dot", "line", "cross", "noise", "flat", "check", "brick", "wave", "grid",
                                               "hatch", "star", "sand"])}
                if rnd.random() < 0.5:
                    x, y = rnd.uniform(20, 120), rnd.uniform(20, 180)
                    tone["area"] = {"poly": [[x, y], [x + rnd.uniform(20, 60), y], [x + 30, y + rnd.uniform(20, 70)]]}
                if rnd.random() < 0.3:
                    tone["dot_shape"] = rnd.choice(["round", "square", "diamond", "ellipse"])
                if rnd.random() < 0.25:
                    tone["gradient"] = {"shape": rnd.choice(["linear", "radial"]), "angle": rnd.choice([0, 90, 30]),
                                        "start": 0.1, "end": 0.9}
                more.append(tone)
            for _ in range(rnd.randint(0, 2)):
                kind = rnd.choice(["focus", "speed", "uni_flash", "beta_flash"])
                params = {"center": [rnd.uniform(60, 120), rnd.uniform(60, 180)], "count": rnd.randint(20, 90)}
                if kind == "speed":
                    params = {"angle": rnd.uniform(0, 180), "count": rnd.randint(15, 60), "length": rnd.uniform(0.3, 0.9)}
                more.append({"op": "add_effect", "page": idx, "kind": kind, "params": params})
            if rnd.random() < 0.5:
                more.append({"op": "add_ruler", "page": idx, "kind": rnd.choice(["line", "curve", "ellipse", "rect"]),
                             "points": [[rnd.uniform(20, 90), rnd.uniform(20, 120)], [rnd.uniform(100, 170), rnd.uniform(130, 240)],
                                        [rnd.uniform(40, 150), rnd.uniform(40, 200)]][: rnd.choice([2, 3])],
                             "id": f"r{idx}"})
                more.append({"op": "ruler_to_layer", "page": idx, "id": f"r{idx}", "width_mm": rnd.choice([0.3, 0.8])})
        result = apply_ops(ep, more, agent="human:確認")
        if not result.get("ok", True):
            raise SystemExit(f"tone ops failed: {result}")
    if os.environ.get("HERMES_3D"):
        # M3-C: 3D guides (drawn in proof and name), placed with Python's own ops
        more = []
        for page in ep.pages:
            idx = page.index
            for _ in range(rnd.randint(1, 3)):
                pos = [round(rnd.uniform(40, 140), 1), round(rnd.uniform(60, 200), 1), round(rnd.uniform(-40, 60), 1)]
                rot = [round(rnd.uniform(-0.6, 0.6), 3), round(rnd.uniform(-1.2, 1.2), 3), round(rnd.uniform(-0.3, 0.3), 3)]
                pick = rnd.choice(["prim", "prim", "figure", "head", "hand", "mannequin", "scene"])
                if pick == "prim":
                    kind = rnd.choice(["box", "cylinder", "stairs", "floor", "sphere", "cone", "prop"])
                    op = {"op": "add_prim3d", "page": idx, "kind": kind, "pos": pos, "rot": rot,
                          "size": rnd.choice([[40, 30, 20], 35, [60, 20, 40]])}
                    if kind == "prop":
                        op["prop"] = rnd.choice(["chair", "desk", "table", "bed", "door", "window", "shelf", "car"])
                    if kind == "stairs":
                        op["steps"] = rnd.randint(3, 9)
                    if kind == "floor":
                        op["lines"] = rnd.randint(4, 12)
                    if rnd.random() < 0.4:
                        op["focal_mm"] = rnd.choice([60, 120, 300])
                elif pick == "figure":
                    op = {"op": "add_figure", "page": idx, "pos": pos, "height_mm": rnd.choice([70, 90, 120]),
                          "preset": rnd.choice(["stand", "walk", "run", "sit", "point", "arms_up", "think", "kneel", "peace"])}
                    if rnd.random() < 0.5:
                        op["body"] = {"sex": rnd.choice(["male", "female"]), "heads": rnd.choice([5, 6.5, 8])}
                elif pick == "head":
                    op = {"op": "add_head", "page": idx, "pos": pos, "size_mm": rnd.choice([20, 30, 45]), "rot": rot}
                elif pick == "hand":
                    op = {"op": "add_hand", "page": idx, "pos": pos, "size_mm": rnd.choice([18, 25, 40]),
                          "side": rnd.choice(["l", "r"]), "pose": rnd.choice(["open", "relaxed", "fist", "point", "peace", "grip"]),
                          "rot": rot}
                elif pick == "mannequin":
                    op = {"op": "add_mannequin", "page": idx, "pos": pos, "height_mm": rnd.choice([70, 100]), "rot": rot,
                          "preset": rnd.choice(["stand", "walk", "run", "sit", "point", "look_back", "arms_up"])}
                else:
                    op = {"op": "add_scene", "page": idx, "kind": rnd.choice(["room", "classroom", "corridor", "street"]),
                          "pos": pos, "size": rnd.choice([1, 0.6]), "frame_id": False}
                more.append(op)
            if rnd.random() < 0.5:
                more.append({"op": "set_camera", "page": idx, "turn": round(rnd.uniform(-0.8, 0.8), 3),
                             "tip": round(rnd.uniform(-0.5, 0.5), 3), "focal_mm": rnd.choice([35, 120, 800])})
            if rnd.random() < 0.5:
                more.append({"op": "set_light", "page": idx, "dir": [round(rnd.uniform(-1, 1), 2), round(rnd.uniform(-1, 1), 2), 0.6],
                             "ambient": rnd.choice([0.2, 0.5])})
        result = apply_ops(ep, more, agent="human:確認")
        if not result.get("ok", True):
            raise SystemExit(f"3d ops failed: {result}")
    for page in ep.pages:
        for layer in page.layers:
            if rnd.random() < 0.3:
                layer.blend = rnd.choice(BLENDS)
            if rnd.random() < 0.2:
                layer.opacity = round(rnd.uniform(0.3, 1.0), 2)
    path = root / f"book-{seed}.genko"
    save_episode(ep, path, actor="human:確認")
    return path


def compare(book: Path, page: int, dpi: int, mode: str, work: Path):
    out = work / f"c-{book.stem}-{page}-{dpi}-{mode}.png"
    p = subprocess.run([G, "render", str(book), "--page", str(page), "--dpi", str(dpi), "--mode", mode, "--out", str(out)],
                       capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        return {"error": (p.stdout + p.stderr)[-300:]}
    ep = load_episode(book)
    pg = next(x for x in ep.pages if x.index == page)
    ref = render_page(pg, dpi, mode=mode, episode=ep).convert("RGB")
    got = Image.open(out).convert("RGB")
    if got.size != ref.size:
        return {"size": [got.size, ref.size]}
    diff = ImageChops.difference(got, ref)
    bbox = diff.getbbox()
    if bbox is None:
        return {"equal": True}
    hist = diff.convert("L").histogram()
    changed = sum(hist[1:])
    return {"equal": False, "changed": changed, "max": max(i for i, v in enumerate(hist) if v), "bbox": bbox}


def main() -> int:
    work = Path(tempfile.mkdtemp(prefix="hermes-m2r-"))
    os.environ.setdefault("GENKO_CONFIG_DIR", str(work / "config"))
    results = []
    for seed in range(int(sys.argv[1]) if len(sys.argv) > 1 else 12):
        book = make_book(1000 + seed, work)
        ep = load_episode(book)
        for page in [p.index for p in ep.pages]:
            for dpi, mode in ((72, "print"), (150, "proof"), (350, "print"), (150, "name")):
                r = compare(book, page, dpi, mode, work)
                results.append({"seed": 1000 + seed, "page": page, "dpi": dpi, "mode": mode, **r})
    bad = [r for r in results if not r.get("equal")]
    print(json.dumps({"renders": len(results), "not_equal": len(bad), "examples": bad[:8]}, ensure_ascii=False, indent=1))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
