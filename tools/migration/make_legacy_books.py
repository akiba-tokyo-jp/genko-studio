#!/usr/bin/env python3
"""Make the legacy test books (v1, v2, v3) with the real old writers, for the C++ reader and converter tests.

The books are written by the Python code of three commits of this repository:
  v1  5b54b42  Initial commit (project.json without "version"; name/ink strokes as point lists)
  v2  3210633  M1 (version 2; layers with inline strokes; rasters under pages/NNN/)
  v3  1b7b1d7  the migration baseline (version 3; assets by hash; studio/journal.jsonl; studio/audit.jsonl)

Usage (from the repository root, on a machine with git and a Python that has Pillow and numpy):
    python3 tools/migration/make_legacy_books.py native/tests/data/legacy

Each book gets the same story: Japanese lines with ruby and an emoji, two pages, a split panel, pen lines with
pressure, and (v2, v3) a painted raster layer. The v3 book also has a saved history (three commits, an undo and
a redo), an approval change in the audit, a strokes blob, a placed image asset, an unknown top-level key and an
unknown page key. MANIFEST.json lists every file with its sha256; the C++ tests check the books against it.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMITS = {"v1": "5b54b42", "v2": "3210633", "v3": "1b7b1d7030d4188d7ebd5e398d010b3ed98b220f"}

V1 = r'''
from pathlib import Path
from genko.models import PageSpec, new_episode, LayerRole
from genko.io import save_episode
ep = new_episode("旧原稿 v1", 1, 2, PageSpec.a4_mono())
ep.pages[0].name_ok = True
ep.pages[0].split_frame(ep.pages[0].frames[0].id, "horizontal", 0.5, 4.0)
ep.pages[0].name_strokes = [[(10.0, 10.0), (20.5, 22.25), (31.125, 40.0)]]
ep.pages[0].ink_strokes = [[(50.0, 60.0), (70.0, 80.0)]]
ep.add_line(1, "始めよう。東京へ行く🙂", speaker="主人公")
ep.add_line(2, "二ページ目", speaker="相手")
save_episode(ep, Path(OUT))
'''

V2 = r'''
import io
from pathlib import Path
from PIL import Image
from genko.models import PageSpec, new_episode, LayerRole, Stroke
from genko.io import save_episode
ep = new_episode("旧原稿 v2", 1, 2, PageSpec.b4_comic())
page = ep.pages[0]
page.name_ok = True
page.split_frame(page.frames[0].id, "vertical", 0.4, 5.0)
ink = next(l for l in page.layers if l.role == LayerRole.INK)
ink.strokes = [Stroke(id="s1", points=[(30.0, 30.0), (60.25, 45.5), (90.0, 40.0)], pressure=[0.2, 0.9, 0.5], width_mm=0.5),
               Stroke(id="s2", points=[(100.0, 120.0), (140.0, 160.0)], pressure=[], width_mm=0.35, kind="maru")]
bg = next(l for l in page.layers if l.role == LayerRole.BG)
img = Image.new("RGBA", (64, 48), (0, 0, 0, 0))
for x in range(64):
    img.putpixel((x, x * 47 // 63), (10, 20, 30, 255))
buf = io.BytesIO(); img.save(buf, format="PNG")
bg.raster_png = buf.getvalue()
bg.raster_relpath = "pages/001/bg.png"
line = ep.add_line(1, "東京と東京", speaker="主人公")
line.ruby_runs = [("東京", "とうきょう")]
line.wrap = "vertical"
ep.extra["future_top"] = {"kept": True}
page.extra["future_page"] = [1, 2, 3]
save_episode(ep, Path(OUT))
'''

V3 = r'''
import io, json
from pathlib import Path
from PIL import Image
from genko.models import PageSpec, new_episode, LayerRole
from genko.io import save_episode, load_episode
from genko.ops import apply_ops
from genko.journal import restore
from genko.lock import ProjectLock
dest = Path(OUT)
ep = new_episode("旧原稿 v3", 1, 2, PageSpec.b5_doujin())
ep.extra["future_top"] = {"kept": True, "n": 12345678901234}
ep.pages[1].extra["future_page"] = {"a": [1, 2]}
save_episode(ep, dest, actor="human:作者")
ep = load_episode(dest)
apply_ops(ep, [
    {"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 4},
    {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 30, 0.2], [60.25, 45.5, 0.9], [90, 40, 0.5]], "width_mm": 0.5, "stabilize": 0},
    {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[100, 120], [140, 160]], "width_mm": 0.35, "rgb": [200, 10, 10], "stabilize": 0},
    {"op": "add_line", "page": 1, "text": "東京と東京へ🙂", "speaker": "主人公", "x_mm": 120, "y_mm": 40, "w_mm": 30, "h_mm": 60,
     "wrap": "vertical", "ruby_runs": [["東京", "とうきょう"]], "emphasis_runs": ["へ"]},
], agent="human:作者")
save_episode(ep, dest, actor="human:作者")
ep = load_episode(dest)
img = Image.new("RGB", (32, 24), (240, 240, 240))
for x in range(32):
    img.putpixel((x, 12), (0, 0, 0))
buf = io.BytesIO(); img.save(buf, format="PNG")
import base64
apply_ops(ep, [{"op": "put_raster", "page": 2, "layer": "bg", "png_base64": base64.b64encode(buf.getvalue()).decode()},
               {"op": "name_ok", "page": 1}], agent="human:作者")
save_episode(ep, dest, actor="human:作者")
ep = load_episode(dest)
apply_ops(ep, [{"op": "set_note", "page": 2, "note": "AI のメモ"}], agent="ai:hermes")
save_episode(ep, dest, actor="ai:hermes")
with ProjectLock(dest, agent="ai:hermes"):
    restore(dest, actor="ai:hermes")
with ProjectLock(dest, agent="ai:hermes"):
    restore(dest, actor="ai:hermes", redo=True)
'''


def export_tree(commit: str, into: Path) -> Path:
    data = subprocess.run(["git", "-C", str(ROOT), "archive", commit, "src/genko"], check=True,
                          capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(into, filter="data")
    return into / "src"


def build(version: str, script: str, out: Path, python: str, work: Path) -> None:
    src = export_tree(COMMITS[version], work / version)
    env = {**os.environ, "PYTHONPATH": str(src), "GENKO_CONFIG_DIR": str(work / f"config-{version}"),
           "GENKO_USER": "作者", "PYTHONDONTWRITEBYTECODE": "1", "PYTHONHASHSEED": "0"}
    code = "OUT = " + repr(str(out)) + "\n" + script
    subprocess.run([python, "-c", code], check=True, env=env)


def manifest(root: Path) -> dict:
    files = {}
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel == "MANIFEST.json" or rel.endswith("project.lock"):
            continue
        files[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"commits": COMMITS, "files": files}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    target = Path(sys.argv[1]).resolve()
    target.mkdir(parents=True, exist_ok=True)
    python = os.environ.get("GENKO_PYREF", sys.executable)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for version, script in (("v1", V1), ("v2", V2), ("v3", V3)):
            out = target / f"book-{version}.genko"
            if out.exists():
                raise SystemExit(f"{out} exists; remove it first")
            build(version, script, out, python, work)
    for lock in target.rglob("project.lock"):
        lock.unlink()
    (target / "MANIFEST.json").write_text(json.dumps(manifest(target), ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({"books": sorted(p.name for p in target.glob("*.genko")), "files": len(manifest(target)["files"])},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
