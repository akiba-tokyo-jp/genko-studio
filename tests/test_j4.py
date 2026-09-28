"""J4: vector lines edited point by point, joined, cut and recoloured; the leftover spots filled; the
colour panel (square and hue, numbers, main / sub / transparent, sets, history, between, near, the layer
eyedropper)."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


def _two_lines():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _ink(ep)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "stabilize": 0, "points": [[40, 100, 0.5], [80, 100, 0.6], [120, 100, 0.7]]},
                   {"op": "add_stroke", "page": 1, "layer_id": ink.id, "stabilize": 0, "points": [[125, 100], [160, 110]]}])
    return ep


def test_control_points_move_come_and_go():
    ep = _two_lines()
    ink = _ink(ep)
    first = ink.strokes[0].id
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "move_point", "stroke_id": first, "index": 1,
                    "to": [80, 90]}])
    assert _ink(ep).strokes[0].points[1] == (80.0, 90.0)
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "add_point", "stroke_id": first, "at": [100, 96]}])
    stroke = _ink(ep).strokes[0]
    assert len(stroke.points) == 4 and len(stroke.pressure) == 4
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "delete_point", "stroke_id": first, "index": 2}])
    assert len(_ink(ep).strokes[0].points) == 3
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "delete_point", "stroke_id": first, "index": 9}])


def test_lines_join_cut_recolour_and_go():
    ep = _two_lines()
    ink = _ink(ep)
    a, b = ink.strokes[0].id, ink.strokes[1].id
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "connect", "ids": [a, b]}])
    joined = _ink(ep).strokes
    assert len(joined) == 1 and joined[0].points[0] == (40.0, 100.0) and joined[0].points[-1] == (160.0, 110.0)
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "cut", "stroke_id": a, "at": [60, 100]}])
    parts = _ink(ep).strokes
    assert len(parts) == 2 and parts[0].points[-1] == parts[1].points[0] and parts[0].id != parts[1].id
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "recolor", "ids": [parts[1].id], "rgb": [0, 0, 220]}])
    assert _ink(ep).strokes[1].rgb == (0, 0, 220) and _ink(ep).strokes[0].rgb is None
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "delete", "ids": [parts[0].id]}])
    assert len(_ink(ep).strokes) == 1
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": ink.id, "action": "connect", "ids": [parts[1].id]}])


def test_leftover_spots_are_filled():
    from genko.render import layer_image

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "c"},
                   {"op": "fill_area", "page": 1, "layer_id": "c", "rgb": [220, 180, 150],
                    "area": {"subtract": [{"rect": [60, 80, 60, 60]}, {"ellipse": [88, 108, 3, 3]}]}}])
    layer = next(item for item in ep.pages[0].layers if item.id == "c")
    hole = (round(89.5 / 25.4 * 150), round(109.5 / 25.4 * 150))
    assert layer_image(ep.pages[0], layer, 150, ep).getpixel(hole)[3] < 50
    apply_ops(ep, [{"op": "fill_gaps", "page": 1, "layer_id": "c", "max_mm": 2}])
    layer = next(item for item in ep.pages[0].layers if item.id == "c")
    filled = layer_image(ep.pages[0], layer, 150, ep).getpixel(hole)
    assert filled[3] > 200 and filled[:3] == (220, 180, 150)
    outside = (round(40 / 25.4 * 150), round(40 / 25.4 * 150))
    assert layer_image(ep.pages[0], layer, 150, ep).getpixel(outside)[3] == 0
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "empty"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "fill_gaps", "page": 1, "layer_id": "empty"}])


def test_colours_between_and_near():
    from genko.app.colours import between, near

    grid = between([(255, 255, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)], 5)
    assert grid[0][0] == (255, 255, 255) and grid[0][4] == (255, 0, 0) and grid[4][4] == (0, 0, 0)
    assert grid[2][2] != grid[0][0]
    rows = near((200, 80, 40))
    assert len(rows) == 5 and rows[2][2] == (200, 80, 40)


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from PySide6.QtCore import QSettings

    from genko.app.main import MainWindow

    for key in ("colour/sub", "colour/history", "colour/corners"):
        QSettings("Genko", "Genko Studio").remove(key)
    project = tmp_path / "b.genko"
    save_episode(_two_lines(), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_the_vector_tool(window, qapp):
    from PySide6.QtCore import Qt

    canvas = window.canvas
    window.set_target_layer(_ink(window.episode).id)
    window._tool("vector")
    none = Qt.KeyboardModifier.NoModifier
    canvas._vector_press(80, 100.2, none)
    first = _ink(window.episode).strokes[0].id
    assert canvas.vector_ids == [first]
    canvas._vector_press(80, 100, none)  # on its middle point: a drag starts
    canvas._vector_move(80, 92)
    canvas._vector_release()
    assert _ink(window.episode).strokes[0].points[1] == (80.0, 92.0)
    canvas._vector_press(140, 104.3, Qt.KeyboardModifier.ShiftModifier)
    assert len(canvas.vector_ids) == 2
    window.brush.set_colour((10, 120, 10))
    window._vector_selected("recolor")
    assert all(s.rgb == (10, 120, 10) for s in _ink(window.episode).strokes)
    window._vector_selected("connect")
    assert len(_ink(window.episode).strokes) == 1
    window.act_vector_cut.trigger()
    canvas._vector_press(60, 100, none)
    assert len(_ink(window.episode).strokes) == 2
    window.act_vector_cut.trigger()
    canvas._vector_press(50, 100, none)
    canvas.vector_point = None
    assert canvas.vector_delete()
    assert len(_ink(window.episode).strokes) == 1


def test_the_colour_panel(window, qapp):
    panel = window.colours
    panel.choose((200, 40, 40))
    assert tuple(window.brush.rgb) == (200, 40, 40)
    assert panel.hex.text() == "#C82828" and [s.value() for s in panel.spins] == [200, 40, 40]
    panel.hex.setText("#1020F0")
    panel._from_hex()
    assert tuple(window.brush.rgb) == (16, 32, 240)
    panel.square.sat, panel.square.val = 0.0, 1.0
    panel._from_square()
    assert tuple(window.brush.rgb) == (255, 255, 255)
    panel.choose((0, 0, 0))
    window.act_swap_colour.trigger()
    assert tuple(window.brush.rgb) == (255, 255, 255) and panel.sub_rgb == (0, 0, 0)
    panel.remember((1, 2, 3))
    assert panel.history[0] == (1, 2, 3) and panel.history_grid.count() >= 1
    panel.new_set("ポスター")
    panel.add_to_set()
    from genko.app.colours import load_sets

    assert (255, 255, 255) in load_sets()["ポスター"]
    panel.set_corner(0)
    assert panel.corners[0] == (255, 255, 255)


def test_drawing_with_the_transparent_colour_takes_away(window, qapp):
    ink = _ink(window.episode)
    window.set_target_layer(ink.id)
    window._tool("pen")
    window.colours.transparent.setChecked(True)
    window._on_stroke([[80, 95, 0.7], [80, 105, 0.7]])
    assert len(_ink(window.episode).strokes) == 3  # the first line was cut in two
    window.colours.transparent.setChecked(False)


def test_the_eyedropper_can_read_the_layer(window, qapp):
    window.apply_ops([{"op": "add_layer", "page": 1, "kind": "paint", "id": "c"},
                      {"op": "fill_area", "page": 1, "layer_id": "c", "rgb": [30, 160, 90], "area": {"rect": [60, 120, 30, 30]}}])
    window.set_target_layer("c")
    assert window.layer_colour_at(70, 130) == (30, 160, 90)
    assert window.layer_colour_at(20, 20) is None
    window.colours.pick_source.setCurrentIndex(1)
    window.colours.pick_source.activated.emit(1)
    assert window.canvas.pick_source == "layer"
