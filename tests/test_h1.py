"""H1: a thick book (32 pages, 1500 lines each) stays quick to draw on, undo, open and page through,
and a zoomed-in view is rendered at the zoom's resolution."""

import copy
import json
import os
import random
import sys
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import blobcache, journal  # noqa: E402
from genko import ops as opsmod  # noqa: E402
from genko import render as renders  # noqa: E402
from genko.headless import snapshot  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.assets import AssetStore  # noqa: E402
from genko.models import LayerRole, PageSpec, Stroke, coerce_stroke, new_episode, new_id, stroke_to_packed  # noqa: E402
from genko.ops import apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(page):
    return next(layer for layer in page.layers if layer.role == LayerRole.INK)


def _thick(pages: int = 32, lines: int = 1500):
    rng = random.Random(1)
    ep = new_episode("厚い本", 1, pages, PageSpec.b4_comic())
    for page in ep.pages:
        ink = _ink(page)
        for _ in range(lines):
            x, y = round(rng.uniform(20, 230), 3), round(rng.uniform(20, 340), 3)
            ink.strokes.append(Stroke(id=new_id(), points=[(round(x + i * 1.2, 3), round(y + rng.uniform(-2, 2), 3)) for i in range(20)],
                                      pressure=[0.7] * 20))
    return ep


# --- copying only what a change touches --------------------------------------------------------------


def _batches(ep):
    p1, p2 = ep.pages[0], ep.pages[1]
    return [
        [{"op": "add_stroke", "page": 1, "layer_id": _ink(p1).id, "points": [[30, 30], [80, 60]], "stabilize": 0}],
        [{"op": "add_line", "page": 2, "text": "やあ", "x_mm": 50, "y_mm": 60, "w_mm": 30, "h_mm": 40}],
        [{"op": "set_layer", "page": 2, "id": _ink(p2).id, "opacity": 0.5}],
        [{"op": "add_tone", "page": 1, "frame_id": p1.leaf_frames()[0].id, "density": 0.2}],
        [{"op": "delete_stroke", "page": 1, "layer": "ink", "index": 0}],
        [{"op": "set_note", "page": 3, "note": "ここは夜"}],
        [{"op": "set_spread", "page": 2, "with": 3}],
        [{"op": "add_stroke", "page": 2, "layer_id": _ink(p2).id, "points": [[240, 30], [300, 60]], "stabilize": 0}],
        [{"op": "add_page", "count": 1}],
        [{"op": "set_brush", "width_mm": 0.6}],
    ]


def test_copying_only_touched_pages_gives_the_same_book_as_copying_everything(monkeypatch):
    ep = _thick(4, 20)
    full = copy.deepcopy(ep)
    batches = _batches(ep)
    for batch in batches:
        apply_ops(ep, batch)
    monkeypatch.setattr(opsmod, "_touched_pages", lambda episode, ops: None)
    for batch in batches:
        apply_ops(full, batch)
    for a, b in zip(ep.pages, full.pages):
        assert [len(layer.strokes) for layer in a.layers] == [len(layer.strokes) for layer in b.layers]
        assert [layer.opacity for layer in a.layers] == [layer.opacity for layer in b.layers]
        assert (a.note, a.spread_with, len(a.texts)) == (b.note, b.spread_with, len(b.texts))
    assert snapshot(ep)["pages"][1]["story"][0]["text"] == "やあ"
    # line ids differ between the runs (new ids), so compare what the lines say
    assert [(line.text, line.page_index) for line in ep.story] == [(line.text, line.page_index) for line in full.story]


def test_a_stroke_copies_its_page_only_and_undo_brings_it_back():
    ep = _thick(8, 50)
    before = list(ep.pages)
    apply_ops(ep, [{"op": "add_stroke", "page": 3, "layer_id": _ink(ep.pages[2]).id, "points": [[30, 30], [80, 60]]}])
    assert ep.pages[2] is not before[2] and len(_ink(ep.pages[2]).strokes) == 51
    assert all(ep.pages[i] is before[i] for i in range(8) if i != 2)  # shared, not copied
    assert len(_ink(before[2]).strokes) == 50  # the old page is untouched: it is the undo
    apply_ops(ep, [{"op": "undo"}])
    assert ep.pages[2] is before[2]


def test_deleting_a_line_leaves_other_pages_alone():
    ep = _thick(4, 5)
    apply_ops(ep, [{"op": "add_line", "page": 2, "text": "消す", "id": "L1", "x_mm": 40, "y_mm": 40, "w_mm": 20, "h_mm": 30}])
    others = [ep.pages[i] for i in (0, 2, 3)]
    apply_ops(ep, [{"op": "delete_line", "id": "L1"}])
    assert [ep.pages[i] for i in (0, 2, 3)] == others and all(a is b for a, b in zip([ep.pages[i] for i in (0, 2, 3)], others))
    assert not ep.story_for_page(2)


def test_structure_changes_still_copy_everything():
    ep = _thick(4, 2)
    assert opsmod._touched_pages(ep, [{"op": "add_page", "count": 1}]) is None
    assert opsmod._touched_pages(ep, [{"op": "reorder", "order": [2, 1, 3, 4]}]) is None
    assert opsmod._touched_pages(ep, [{"op": "name_ok"}]) is None  # every page
    assert opsmod._touched_pages(ep, [{"op": "add_stroke", "page": 2}, {"op": "set_brush"}]) == {2}
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    assert opsmod._touched_pages(ep, [{"op": "add_stroke", "page": 1}]) == {1, 2}  # lines cross the gutter


# --- saving, reading and the journal -----------------------------------------------------------------


def test_packed_strokes_round_trip_and_old_blobs_still_read(tmp_path: Path):
    stroke = Stroke(id="s1", points=[(1.25, 2.5), (3.001, 4.75)], pressure=[0.5, 0.875], width_mm=0.6, kind="maru",
                    rgb=(10, 20, 30), opacity=0.4)
    back = coerce_stroke(json.loads(json.dumps(stroke_to_packed(stroke))))
    assert (back.points, back.pressure, back.width_mm, back.kind, back.rgb, back.opacity) == (
        stroke.points, stroke.pressure, 0.6, "maru", (10, 20, 30), 0.4)
    # a book saved before packing (lists of numbers) opens as before
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[10, 10], [20, 20]]}])
    project = tmp_path / "old.genko"
    save_episode(ep, project)
    data = json.loads((project / "project.json").read_text(encoding="utf-8"))
    store = AssetStore(project)
    layer = next(item for item in data["pages"][0]["layers"] if item.get("strokes_blob"))
    old = json.dumps([{"id": "o1", "points": [[10, 10, 0.7], [20, 20, 0.7]], "width_mm": 0.35, "kind": "gpen"}])
    layer["strokes_blob"] = store.put_bytes(old.encode("utf-8"), ".strokes.json")
    (project / "project.json").write_text(json.dumps(data), encoding="utf-8")
    blobcache.clear()
    loaded = load_episode(project)
    assert _ink(loaded.pages[0]).strokes[0].points == [(10.0, 10.0), (20.0, 20.0)]


def test_an_unchanged_layer_is_not_written_again(tmp_path: Path, monkeypatch):
    ep = _thick(3, 10)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    packed = []
    real = __import__("genko.io", fromlist=["x"]).stroke_to_packed
    monkeypatch.setattr("genko.io.stroke_to_packed", lambda s: packed.append(1) or real(s))
    apply_ops(ep, [{"op": "add_stroke", "page": 2, "layer_id": _ink(ep.pages[1]).id, "points": [[30, 30], [80, 60]]}])
    save_episode(ep, project)
    assert len(packed) == 11  # only page 2's ink layer (10 + 1 lines)


def test_reading_a_saved_layer_again_reuses_the_strokes(tmp_path: Path):
    ep = _thick(2, 10)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    first = load_episode(project)
    second = load_episode(project)
    assert _ink(first.pages[0]).strokes[0] is _ink(second.pages[0]).strokes[0]
    assert _ink(first.pages[0]).strokes is not _ink(second.pages[0]).strokes  # (lists are their own)


def test_a_big_batch_keeps_the_journal_line_short(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ops = [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[10 + i, 10 + k, 0.7] for k in range(30)]} for i in range(200)]
    apply_ops(ep, ops)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    line = journal.path(project).read_text(encoding="utf-8").splitlines()[-1]
    entry = json.loads(line)
    assert len(line) < 30_000 and entry["ops_asset"] and entry["ops"][0] == {"op": "add_stroke", "page": 1, "layer": "ink"}
    assert entry["ops_asset"] in journal.referenced_assets(project)
    full = AssetStore(project).get_bytes(entry["ops_asset"], ".ops.json")
    assert len(json.loads(full)) == 200


def test_the_journal_is_read_once(tmp_path: Path, monkeypatch):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    assert len(journal.entries(project)) == 1
    calls = []
    real = json.loads
    monkeypatch.setattr(journal.json, "loads", lambda s, *a, **k: calls.append(1) or real(s, *a, **k))
    journal.append(project, {"kind": "commit", "rev": 9})
    assert [e.get("rev") for e in journal.entries(project)][-1] == 9 and len(calls) == 1  # only the new line


def test_the_summary_counts_lines_without_listing_them():
    ep = _thick(2, 30)
    layers = len(ep.pages[0].layers)
    snap = snapshot(ep)
    assert snap["pages"][0]["ink_stroke_count"] == 30 and len(ep.pages[0].layers) == layers


# --- the screen -----------------------------------------------------------------------------------------


def test_small_pictures_draw_lines_plainly():
    ep = _thick(1, 200)
    t = time.perf_counter()
    image = renders.render_page(ep.pages[0], 12, mode="proof", episode=ep)
    assert time.perf_counter() - t < 0.5
    assert image.convert("L").getextrema()[0] < 128  # the lines are there


@pytest.fixture
def app():
    from PySide6.QtWidgets import QApplication

    return QApplication.instance() or QApplication([])


def test_zoomed_in_the_page_is_rendered_at_the_zoom(app, tmp_path: Path):
    from genko.app.canvas import BASE_DPI
    from genko.app.main import MainWindow

    ep = _thick(1, 40)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    window = MainWindow(project)
    window.resize(1280, 800)
    window.show()
    app.processEvents()
    canvas = window.canvas
    canvas.zoom_by((96 / 25.4) * 4 / canvas._scale)  # 400 %
    canvas._render_now()
    canvas.wait_detail()
    needed = canvas._scale * 25.4
    assert canvas._wanted_dpi() > BASE_DPI and canvas._rendered[0] == canvas._wanted_dpi()
    assert abs(canvas._rendered[0] - needed) <= 24
    assert canvas._rendered[1].width() == renders.mm_to_px(ep.pages[0].spec.width_mm, canvas._rendered[0])  # not stretched
    window.commit_now()
    window.close()


def test_a_page_not_seen_lately_shows_roughly_then_properly(app, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = _thick(3, 400)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    window = MainWindow(project)
    window.resize(1280, 800)
    window.show()
    app.processEvents()
    renders._STROKE_CACHE.clear()
    window.go_to_page(3)
    assert window.canvas._rough and window.canvas.background is not None
    window.canvas.wait_detail()
    assert not window.canvas._rough
    window.commit_now()
    window.close()


# --- the thick book ---------------------------------------------------------------------------------------


def test_a_thick_book_stays_quick(app, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = _thick()
    project = tmp_path / "thick.genko"
    save_episode(ep, project)
    blobcache.clear()
    renders._STROKE_CACHE.clear()
    journal._PARSED.clear()

    t = time.perf_counter()
    window = MainWindow(project)
    window.resize(1280, 800)
    window.show()
    app.processEvents()
    opened = time.perf_counter() - t

    ink = _ink(window.current_page()).id
    window.set_target_layer(ink)

    def stroke(i):
        window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[30 + i, 30], [80 + i, 60]], "stabilize": 0}])
        app.processEvents()

    stroke(0)
    times = []
    for i in (1, 2):
        t = time.perf_counter()
        stroke(i)  # (this one also saves the one before)
        times.append(time.perf_counter() - t)
    t = time.perf_counter()
    window._undo()
    app.processEvents()
    undo = time.perf_counter() - t
    t = time.perf_counter()
    window._redo()
    app.processEvents()
    redo = time.perf_counter() - t
    t = time.perf_counter()
    window.go_to_page(5)
    app.processEvents()
    turn = time.perf_counter() - t
    window.canvas.wait_detail()
    assert opened <= 3.0, opened
    assert min(times) <= 0.6, times
    assert undo <= 1.5 and redo <= 1.5, (undo, redo)
    assert turn <= 1.0, turn
    assert len(_ink(window.episode.pages[0]).strokes) == 1503
    window.commit_now()
    window.close()
