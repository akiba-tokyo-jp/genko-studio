"""M2: v3 format, content-addressed assets, journal undo, page ids, spreads, strict gates."""

import base64
import io
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
from PIL import Image

from genko.__main__ import main
from genko.io import load_episode, save_episode
from genko.models import Binding, PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.render import render_page, render_spread

SRC = str(Path(__file__).resolve().parents[1] / "src")


def _png(rgb) -> str:
    buf = io.BytesIO()
    Image.new("RGB", (8, 8), rgb).save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


def _bg(ep, page_no):
    page = next(p for p in ep.pages if p.index == page_no)
    layer = next(layer for layer in page.layers if layer.role.value == "bg")
    return Image.open(io.BytesIO(layer.raster_png)).convert("RGB").getpixel((0, 0))


def test_v3_saves_assets_by_hash_and_no_stroke_coordinates(tmp_path: Path):
    ep = new_episode("t", 1, 16, PageSpec.b4_comic())
    for page in range(1, 17):
        apply_ops(ep, [{"op": "add_stroke", "page": page, "layer": "name", "points": [[10 + i, 10 + j] for j in range(20)]} for i in range(30)])
    img = Image.new("L", (400, 560), 255)
    for y in range(0, 560, 3):
        for x in range(0, 400, 2):
            img.putpixel((x, y), 0)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "draft", "png_base64": base64.b64encode(buf.getvalue()).decode()},
                   {"op": "lt_convert", "page": 1, "layer": "draft", "to": "name"}])
    save_episode(ep, tmp_path / "p.genko")
    text = (tmp_path / "p.genko" / "project.json").read_text(encoding="utf-8")
    data = json.loads(text)
    assert data["version"] == 3 and data["revision"] == 1
    assert len(text.encode()) < 1_000_000
    page = data["pages"][0]
    assert "texts" not in page and "name_strokes" not in page and "ink_strokes" not in page
    name = next(layer for layer in page["layers"] if layer["role"] == "name")
    assert name["strokes_blob"].startswith("sha256:") and "strokes" not in name
    loaded = load_episode(tmp_path / "p.genko")
    assert len(loaded.pages[1].name_strokes) == 30


def test_delete_page_then_put_raster_keeps_pixels(tmp_path: Path):
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "put_raster", "page": 2, "layer": "bg", "png_base64": _png((200, 0, 0))},
                   {"op": "put_raster", "page": 3, "layer": "bg", "png_base64": _png((0, 0, 200))}])
    save_episode(ep, tmp_path / "p.genko")
    ep = load_episode(tmp_path / "p.genko")
    apply_ops(ep, [{"op": "delete_page", "page": 1}])
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((0, 200, 0))}])
    save_episode(ep, tmp_path / "p.genko")
    ep = load_episode(tmp_path / "p.genko")
    assert _bg(ep, 1) == (0, 200, 0) and _bg(ep, 2) == (0, 0, 200)


def test_duplicate_page_gets_its_own_ids_and_lines_follow_new_frames(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "split_frame", "page": 1, "axis": "horizontal"}])
    leaf = ep.pages[0].leaf_frames()[0].id
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "a", "frame_id": leaf, "x_mm": 20, "y_mm": 20},
                   {"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((1, 2, 3))},
                   {"op": "duplicate_page", "page": 1}])
    clone = ep.pages[1]
    assert clone.id != ep.pages[0].id
    line = ep.story_for_page(2)[0]
    assert line.frame_id in {f.id for f in clone.leaf_frames()}
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((9, 9, 9))}])
    save_episode(ep, tmp_path / "p.genko")
    ep = load_episode(tmp_path / "p.genko")
    assert _bg(ep, 1) == (9, 9, 9) and _bg(ep, 2) == (1, 2, 3)


def test_locks_tickets_and_spreads_follow_pages():
    ep = new_episode("t", 1, 4, PageSpec.a4_mono())
    third = ep.pages[2].id
    apply_ops(ep, [{"op": "lock_page", "page": 3}, {"op": "add_ticket", "page": 3}, {"op": "set_spread", "page": 2, "with": 3}], agent="human:a")
    apply_ops(ep, [{"op": "delete_page", "page": 1}], agent="human:a")
    page = next(p for p in ep.pages if p.id == third)
    assert page.index == 2 and ep.page_locks == {third: "human:a"}
    assert ep.tickets[0]["page_index"] == 2 and ep.tickets[0]["page_id"] == third
    assert ep.pages[0].spread_with == 2
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_line", "page": 2, "text": "x"}], agent="ai:b")
    apply_ops(ep, [{"op": "reorder", "order": [3, 1, 2]}], agent="human:a")
    assert next(p for p in ep.pages if p.id == third).index == 3


def test_edit_line_persists_after_reload(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "before"}])
    save_episode(ep, tmp_path / "p.genko")
    ep = load_episode(tmp_path / "p.genko")
    assert ep.pages[0].texts[0] is ep.story[0]
    apply_ops(ep, [{"op": "edit_line", "id": ep.story[0].id, "text": "after"}])
    save_episode(ep, tmp_path / "p.genko")
    assert load_episode(tmp_path / "p.genko").pages[0].texts[0].text == "after"


def test_v2_project_is_read_and_upgraded_with_a_backup(tmp_path: Path):
    project = tmp_path / "old.genko"
    (project / "pages" / "001").mkdir(parents=True)
    Image.new("RGB", (4, 4), (5, 6, 7)).save(project / "pages" / "001" / "bg.png")
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    data = {
        "version": 2, "title": "old", "episode": 1, "binding": "right", "page_locks": {"1": "human:x"},
        "spec": {"width_mm": 210, "height_mm": 297, "dpi": 600, "bleed_mm": 3, "inner_margin_mm": 10},
        "pages": [{"index": 1, "frames": [{"id": ep.pages[0].frames[0].id, "rect": {"x": 13, "y": 13, "width": 184, "height": 271}}],
                   "layers": [{"id": "L1", "role": "bg", "kind": "raster", "raster_relpath": "pages/001/bg.png"}],
                   "texts": [{"id": "t1", "page_index": 1, "text": "hi"}]}],
        "story": [{"id": "t1", "page_index": 1, "text": "hi"}],
    }
    (project / "project.json").write_text(json.dumps(data), encoding="utf-8")
    loaded = load_episode(project)
    assert _bg(loaded, 1) == (5, 6, 7)
    assert loaded.page_locks == {loaded.pages[0].id: "human:x"}
    save_episode(loaded, project)
    assert json.loads((project / "project.v2.bak.json").read_text())["version"] == 2
    again = load_episode(project)
    assert again.pages[0].id == loaded.pages[0].id and _bg(again, 1) == (5, 6, 7)
    assert json.loads((project / "project.json").read_text())["version"] == 3


def test_undo_and_redo_work_across_processes(tmp_path: Path, capsys):
    project = tmp_path / "p.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.a4_mono()), project)
    ops = tmp_path / "ops.json"
    ops.write_text(json.dumps([{"op": "set_note", "page": 1, "note": "changed"}]))
    assert main(["apply", str(project), str(ops), "--agent", "human:a"]) == 0
    assert load_episode(project).pages[0].note == "changed"
    env = {**os.environ, "PYTHONPATH": SRC}
    other = subprocess.run([sys.executable, "-m", "genko", "undo", str(project), "--as", "human:b"], capture_output=True, env=env)
    assert other.returncode == 1  # someone else's change
    done = subprocess.run([sys.executable, "-m", "genko", "undo", str(project), "--as", "human:a"], capture_output=True, env=env)
    assert done.returncode == 0, done.stdout
    assert load_episode(project).pages[0].note == ""
    assert main(["redo", str(project), "--as", "human:a"]) == 0
    assert load_episode(project).pages[0].note == "changed"
    capsys.readouterr()


def test_expect_revision_detects_a_concurrent_save(tmp_path: Path, capsys):
    project = tmp_path / "p.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.a4_mono()), project)
    ops = tmp_path / "ops.json"
    ops.write_text(json.dumps([{"op": "set_note", "page": 1, "note": "x"}]))
    assert main(["apply", str(project), str(ops), "--expect-revision", "1"]) == 0
    assert main(["apply", str(project), str(ops), "--expect-revision", "1"]) == 1
    assert "revision conflict" in capsys.readouterr().out


def test_gc_keeps_referenced_assets_and_doctor_reports_missing_ones(tmp_path: Path, capsys):
    from genko.maintenance import doctor, gc

    project = tmp_path / "p.genko"
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((1, 1, 1))}])
    save_episode(ep, project)
    stray = project / "assets" / "ff" / ("ff" * 32 + ".png")
    stray.parent.mkdir(parents=True, exist_ok=True)
    stray.write_bytes(b"x")
    old = time.time() - 3 * 86400
    for path in (project / "assets").rglob("*"):
        if path.is_file():
            os.utime(path, (old, old))
    result = gc(project, dry_run=False)
    assert [Path(p).name for p in result["removed"]] == [stray.name]
    assert _bg(load_episode(project), 1) == (1, 1, 1)
    assert doctor(project)["ok"]
    ref = json.loads((project / "project.json").read_text())["pages"][0]["layers"][0]["asset"]
    digest = ref.split(":")[1]
    (project / "assets" / digest[:2] / f"{digest}.png").unlink()
    assert not doctor(project)["ok"]


def test_put_raster_rejects_bytes_that_are_not_an_image():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": base64.b64encode(b"not a png").decode()}])


def test_right_binding_spreads_put_the_even_page_on_the_right():
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "put_raster", "page": 2, "layer": "bg", "png_base64": _png((250, 0, 0))}])
    img = render_spread(ep, 2, 3, dpi=36, mode="print")
    assert img.getpixel((img.width * 3 // 4, img.height // 2))[0] > 200  # page 2 on the right
    assert img.getpixel((img.width // 4, img.height // 2))[0] > 200 and img.getpixel((img.width // 4, img.height // 2))[1] > 200
    left = new_episode("t", 1, 3, PageSpec.a4_mono(), Binding.LEFT)
    apply_ops(left, [{"op": "put_raster", "page": 2, "layer": "bg", "png_base64": _png((250, 0, 0))}])
    img = render_spread(left, 2, 3, dpi=36, mode="print")
    assert img.getpixel((img.width // 4, img.height // 2))[1] < 50  # page 2 on the left


def test_set_spread_needs_facing_pages_when_strict():
    ep = new_episode("t", 1, 4, PageSpec.a4_mono())
    result = apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    assert any("one leaf" in w for w in result["warnings"])
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": None}, {"op": "set_meta", "strict_gates": True}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    apply_ops(ep, [{"op": "set_spread", "page": 2, "with": 3}])
    apply_ops(ep, [{"op": "set_meta", "start_side": "right"}])
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])


def test_spread_space_strokes_land_on_the_physical_page():
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])
    width = ep.spec.width_mm
    apply_ops(ep, [{"op": "add_stroke", "page": 2, "layer": "name", "space": "spread", "points": [[10, 10], [20, 20]]}])
    assert ep.pages[2].name_strokes and not ep.pages[1].name_strokes  # left half = page 3 in a right-bound book
    apply_ops(ep, [{"op": "add_stroke", "page": 3, "layer": "name", "space": "spread", "points": [[width + 10, 10], [width + 20, 20]]}])
    assert ep.pages[1].name_strokes


def test_strict_gates_block_printed_rasters_before_name_ok():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_meta", "strict_gates": True}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((0, 0, 0))}])
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "draft", "png_base64": _png((0, 0, 0))}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_line", "page": 1, "text": "x", "frame_id": ep.pages[0].frames[0].id}])
    apply_ops(ep, [{"op": "name_ok", "page": 1}], agent="human:a")
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((0, 0, 0))}])


def test_speaker_names_are_not_printed_and_opacity_zero_hides_a_layer():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "あ", "speaker": "話者の名前がここに出る", "x_mm": 50, "y_mm": 80, "w_mm": 12, "h_mm": 20, "wrap": "vertical"}])
    name = render_page(ep.pages[0], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[0], 72, mode="print", episode=ep)
    from genko.render import mm_to_px

    box = (mm_to_px(50, 72), mm_to_px(60, 72), mm_to_px(110, 72), mm_to_px(79, 72))
    assert name.crop(box).getextrema()[0][0] < 200
    assert printed.crop(box).getextrema()[0][0] > 200
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png((0, 0, 0))}, {"op": "set_layer", "page": 1, "layer": "bg", "opacity": 0}])
    assert render_page(ep.pages[0], 36, mode="print", episode=ep).getpixel((5, 5)) == (255, 255, 255)
