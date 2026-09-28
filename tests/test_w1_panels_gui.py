"""The panel tool draws panels: a rectangle by dragging, a polygon corner by corner, a freehand outline;
a chosen panel is deleted alone; a layer's switch keeps its lines in the panel they begin in."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b5_doujin()), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.close()


def _drag(canvas, points_mm):
    canvas._frame_press(canvas._pt(*points_mm[0]))
    for p in points_mm[1:]:
        canvas._frame_move(canvas._pt(*p))
    canvas._frame_release()


def test_panels_are_drawn_three_ways_and_one_is_deleted(window):
    canvas = window.canvas
    window._tool("frame")
    index = window.frame_mode.findData("rect")
    window.frame_mode.setCurrentIndex(index)
    window.frame_mode.activated.emit(index)
    assert canvas.frame_mode == "rect"
    _drag(canvas, [(20, 20), (60, 50), (100, 80)])
    page = window._current()
    assert len(page.leaf_frames()) == 1 and page.frames[0].split_axis == "free"
    # corner by corner, closed on the first corner
    canvas.frame_mode = "poly"
    for p in [(110, 20), (180, 20), (170, 80), (110, 80)]:
        canvas._frame_press(canvas._pt(*p))
    canvas._frame_press(canvas._pt(110, 20))
    assert len(window._current().leaf_frames()) == 2 and not canvas._frame_poly
    # freehand
    canvas.frame_mode = "free"
    import math

    _drag(canvas, [(100 + 40 * math.cos(t / 40 * 2 * math.pi), 150 + 25 * math.sin(t / 40 * 2 * math.pi)) for t in range(41)])
    page = window._current()
    assert len(page.leaf_frames()) == 3
    # the chosen one goes, the others stay
    page.selected_frame_id = page.leaf_frames()[0].id
    gone = page.selected_frame_id
    window.act_delete_frame.trigger()
    assert gone not in [f.id for f in window._current().leaf_frames()] and len(window._current().leaf_frames()) == 2


def test_the_layer_switch_for_panel_by_panel(window):
    from PySide6.QtWidgets import QCheckBox

    switch = next(b for b in window.findChildren(QCheckBox) if b.text() == "描き始めたコマの中だけに描く")
    ink = next(layer for layer in window._current().layers if layer.role.value == "ink")
    window.set_target_layer(ink.id)
    assert ink.panel_each
    switch.click()
    ink = next(layer for layer in window._current().layers if layer.role.value == "ink")
    assert not ink.panel_each


def test_a_ruler_cuts_or_makes_a_panel_and_a_panel_becomes_the_selection(window):
    page = window._current()
    frame = page.leaf_frames()[0]
    r = frame.rect
    y = r.y + r.height / 2
    window.apply_ops([{"op": "add_ruler", "page": page.index, "kind": "line", "id": "cut",
                       "points": [[r.x + 10, y], [r.x + r.width - 10, y]]},
                      {"op": "add_ruler", "page": page.index, "kind": "concentric", "id": "ring",
                       "points": [[100, 100], [130, 100]]}])
    window.canvas.selected_ruler_id = "cut"
    window.act_ruler_frame.trigger()
    assert len(window._current().leaf_frames()) == 2
    window.canvas.selected_ruler_id = "ring"
    window.act_ruler_frame.trigger()
    page = window._current()
    assert len(page.leaf_frames()) == 3 and page.frames[0].split_axis == "free"
    ring = next(f for f in page.leaf_frames() if f.poly and len(f.poly) > 8)
    assert abs(ring.rect.width - 60) < 2
    page.selected_frame_id = ring.id
    window.act_frame_selection.trigger()
    area = window.canvas.selection
    assert area and len(area["area"]["poly"]) > 8


def test_own_layouts_come_first_in_the_template_dialog(window, tmp_path):
    from genko.app.dialogs import TemplateDialog
    from genko.studio import layout

    window.apply_ops([{"op": "add_frame", "page": 1, "rect": [20, 20, 80, 60]},
                      {"op": "add_frame", "page": 1, "rect": [110, 20, 70, 60]}])
    layout.save_user_template("上に二つ", window._current())
    dialog = TemplateDialog(window, window.episode, window._current())
    first = dialog.list.item(0)
    assert first.text() == "自分: 上に二つ" and dialog.list.count() > 1
    dialog.list.setCurrentItem(first)
    dialog.choose()
    assert any(op["op"] == "set_layout" for op in dialog.ops)
    dialog.close()
