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

G = "/src/build/linux-release/src/api/genko"
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
