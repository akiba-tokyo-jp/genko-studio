"""M13: pens and brushes, colour, fills (gap closing, reference), selections (move / scale / turn / flip,
copy / paste, delete) and line fixes (pinch, width, erase to the crossing)."""

import math
import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import brushes  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    return ep


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


def _px(ep, x, y, dpi=100):
    return render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("RGB").getpixel((mm_to_px(x, dpi), mm_to_px(y, dpi)))


def _grey(ep, x, y, dpi=100):
    r, g, b = _px(ep, x, y, dpi)
    return (r + g + b) // 3


def _box(ep, x0, y0, x1, y1, gap=0.0, width=0.6):
    """A square drawn in four lines, with a gap of `gap` mm in the top side."""
    ink = _ink(ep).id
    top = [[[x0, y0], [x0 + (x1 - x0) / 2 - gap / 2, y0]], [[x0 + (x1 - x0) / 2 + gap / 2, y0], [x1, y0]]] if gap else [[[x0, y0], [x1, y0]]]
    sides = top + [[[x1, y0], [x1, y1]], [[x1, y1], [x0, y1]], [[x0, y1], [x0, y0]]]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "points": pts, "width_mm": width, "kind": "mili",
                    "stabilize": 0, "taper": False} for pts in sides])


# --- brushes -------------------------------------------------------------------------------------------


def test_every_brush_draws_and_keeps_its_kind(tmp_path: Path):
    ep = _book()
    ink = _ink(ep).id
    for i, key in enumerate(brushes.BRUSHES):
        y = 60 + i * 12
        apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": key, "stabilize": 0,
                        "points": [[40, y, 0.8], [120, y, 0.9], [160, y, 0.7]]}])
    kinds = [s.kind for s in _ink(ep).strokes]
    assert kinds == list(brushes.BRUSHES)
    for i, key in enumerate(brushes.BRUSHES):
        grey = _grey(ep, 100, 60 + i * 12, dpi=150)
        if key == "white":
            assert grey > 240  # white on white
        else:
            assert grey < 235, key
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    assert [s.kind for s in _ink(load_episode(project)).strokes] == kinds
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "crayon", "points": [[1, 1], [2, 2]]}])


def test_pressure_width_colour_and_opacity():
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "width_mm": 2, "rgb": [200, 30, 30],
                    "stabilize": 0, "taper": False, "points": [[40, 100], [160, 100]]},
                   {"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "marker", "width_mm": 3, "stabilize": 0,
                    "points": [[40, 140], [160, 140]]}])
    r, g, b = _px(ep, 100, 100)
    assert r > 150 and g < 90
    marker = _grey(ep, 100, 140)
    assert 60 < marker < 200  # a marker is see-through
    # a soft gamma makes a light touch thicker than a hard one
    soft = brushes._pressured([(0, 0, 0.3)], brushes.brush("gpen"))[0][2]
    assert 0 < soft < 1
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "gpen", "pressure_gamma": 0.5, "stabilize": 0,
                    "taper": False, "points": [[10, 10, 0.25], [20, 10, 0.25]]}])
    assert _ink(ep).strokes[-1].pressure[0] > 0.45


# --- fills ---------------------------------------------------------------------------------------------


def test_fill_stays_inside_lines_and_closes_small_gaps(tmp_path: Path):
    ep = _book()
    _box(ep, 60, 80, 120, 140)
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": ink, "x_mm": 90, "y_mm": 110, "rgb": [120, 120, 120]}])
    assert _grey(ep, 90, 110) < 140
    assert _grey(ep, 90, 150) > 240  # outside the square stays white
    # a square with a 1 mm gap: closed at 1.5 mm, leaks at 0
    ep2 = _book()
    _box(ep2, 60, 80, 120, 140, gap=1.0)
    apply_ops(ep2, [{"op": "fill", "page": 1, "layer_id": _ink(ep2).id, "x_mm": 90, "y_mm": 110, "rgb": [0, 0, 0], "gap_mm": 1.5}])
    assert _grey(ep2, 90, 110) < 60 and _grey(ep2, 90, 60) > 240
    ep3 = _book()
    _box(ep3, 60, 80, 120, 140, gap=1.0)
    apply_ops(ep3, [{"op": "fill", "page": 1, "layer_id": _ink(ep3).id, "x_mm": 90, "y_mm": 110, "rgb": [0, 0, 0], "gap_mm": 0}])
    assert _grey(ep3, 90, 60) < 60  # it ran out through the gap (to the panel's edge, not further)
    # the fill travels with the book
    project = tmp_path / "f.genko"
    save_episode(ep, project)
    again = load_episode(project)
    assert len(_ink(again).patches) == 1 and _grey(again, 90, 110) < 140
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": ink, "x_mm": 60, "y_mm": 110}])  # on the line


def test_fill_can_look_at_one_layer_only():
    ep = _book()
    _box(ep, 60, 80, 120, 140)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "色", "kind": "paint", "id": "paint1"}])
    # the paint layer has no lines: looking only at it, the fill runs to the panel border
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "paint1", "x_mm": 90, "y_mm": 110, "rgb": [0, 0, 0], "reference": "layer"}])
    assert _grey(ep, 90, 60) < 60
    ep2 = _book()
    _box(ep2, 60, 80, 120, 140)
    apply_ops(ep2, [{"op": "add_layer", "page": 1, "name": "色", "kind": "paint", "id": "paint1"},
                    {"op": "fill", "page": 1, "layer_id": "paint1", "x_mm": 90, "y_mm": 110, "rgb": [0, 0, 0], "reference": "page"}])
    assert _grey(ep2, 90, 60) > 240 and _grey(ep2, 90, 110) < 60


def test_fill_area_and_opacity():
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "fill_area", "page": 1, "layer_id": ink, "area": {"poly": [[50, 50], [100, 50], [100, 100], [50, 100]]},
                    "rgb": [0, 0, 0], "opacity": 0.5}])
    assert 90 < _grey(ep, 75, 75) < 170 and _grey(ep, 110, 75) > 240
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "fill_area", "page": 1, "layer_id": ink, "area": {"poly": [[1, 1], [2, 2]]}}])


# --- selections ------------------------------------------------------------------------------------------

SQUARE = {"poly": [[40, 40], [100, 40], [100, 100], [40, 100]]}


def _stroke_box(stroke):
    xs, ys = [p[0] for p in stroke.points], [p[1] for p in stroke.points]
    return min(xs), min(ys), max(xs), max(ys)


def test_move_scale_rotate_flip_a_selection():
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "width_mm": 1, "stabilize": 0, "taper": False,
                    "points": [[50, 70], [90, 70]]},
                   {"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "stabilize": 0, "taper": False,
                    "points": [[150, 150], [170, 150]]},
                   {"op": "fill_area", "page": 1, "layer_id": ink, "area": {"poly": [[60, 80], [80, 80], [80, 95], [60, 95]]}, "rgb": [0, 0, 0]}])
    # move by (+50, +10)
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": ink, "area": SQUARE, "matrix": [1, 0, 0, 1, 50, 10]}])
    moved, outside = _ink(ep).strokes[-1], _ink(ep).strokes[0]
    assert _stroke_box(outside) == (150, 150, 170, 150)  # the line outside the area stays
    assert _stroke_box(moved) == pytest.approx((100, 80, 140, 80))
    patch = _ink(ep).patches[0]
    assert patch["box"][0] == pytest.approx(110, abs=0.2) and patch["box"][1] == pytest.approx(90, abs=0.2)
    assert _grey(ep, 120, 97) < 60 and _grey(ep, 70, 87) > 240
    # scale ×2 about (120, 80): the line doubles in length and width
    area = {"poly": [[95, 75], [145, 75], [145, 110], [95, 110]]}
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": ink, "area": area, "matrix": [2, 0, 0, 2, -120, -80]}])
    line = next(s for s in _ink(ep).strokes if s.points[0][1] == pytest.approx(80))
    x0, _, x1, _ = _stroke_box(line)
    assert x1 - x0 == pytest.approx(80) and line.width_mm == pytest.approx(2)
    # a quarter turn about (120, 80) makes it upright
    area = {"poly": [[70, 70], [180, 70], [180, 140], [70, 140]]}
    c, s = 0.0, 1.0
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": ink, "area": area,
                    "matrix": [c, s, -s, c, 120 - c * 120 + s * 80, 80 - s * 120 - c * 80]}])
    line = next(st for st in _ink(ep).strokes if st.id == line.id)
    x0, y0, x1, y1 = _stroke_box(line)
    assert x1 - x0 == pytest.approx(0, abs=1e-6) and y1 - y0 == pytest.approx(80)
    # a flip keeps sizes
    before = _stroke_box(line)
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": ink, "area": {"poly": [[100, 30], [140, 30], [140, 130], [100, 130]]},
                    "matrix": [-1, 0, 0, 1, 240, 0]}])
    after = _stroke_box(next(st for st in _ink(ep).strokes if st.id == line.id))
    assert after[3] - after[1] == pytest.approx(before[3] - before[1])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": ink, "area": SQUARE, "matrix": [0, 0, 0, 0, 0, 0]}])


def test_delete_copy_paste_and_paint_pixels():
    from genko import selection

    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "stabilize": 0, "taper": False,
                    "points": [[50, 70], [90, 70]]},
                   {"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "stabilize": 0, "taper": False,
                    "points": [[150, 150], [170, 150]]}])
    import copy

    items = selection.items_to_json(selection.lift(copy.deepcopy(_ink(ep)), SQUARE, ep.pages[0]))
    assert len(items["strokes"]) == 1
    apply_ops(ep, [{"op": "delete_area", "page": 1, "layer_id": ink, "area": SQUARE}])
    assert len(_ink(ep).strokes) == 1
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "貼り付け", "kind": "pen", "id": "pasted"},
                   {"op": "paste", "page": 1, "layer_id": "pasted", "items": items, "matrix": [1, 0, 0, 1, 0, 100]}])
    pasted = next(layer for layer in ep.pages[0].layers if layer.id == "pasted")
    assert _stroke_box(pasted.strokes[0]) == pytest.approx((50, 170, 90, 170))
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "paste", "page": 1, "layer_id": "pasted", "items": {}}])
    # the pixels of a paint layer move too
    import base64
    import io

    from PIL import Image

    from genko.raster import WORKING_DPI

    spec = ep.pages[0].spec
    image = Image.new("RGBA", (mm_to_px(spec.width_mm, WORKING_DPI), mm_to_px(spec.height_mm, WORKING_DPI)), (0, 0, 0, 0))
    image.paste((0, 0, 0, 255), (mm_to_px(10, WORKING_DPI), mm_to_px(10, WORKING_DPI), mm_to_px(30, WORKING_DPI), mm_to_px(30, WORKING_DPI)))
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "色", "kind": "paint", "id": "paint1"},
                   {"op": "put_raster", "page": 1, "id": "paint1", "png_base64": base64.b64encode(buf.getvalue()).decode()}])
    assert _grey(ep, 20, 20) < 60
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": "paint1", "area": {"poly": [[0, 0], [60, 0], [60, 60], [0, 60]]},
                    "matrix": [1, 0, 0, 1, 100, 0]}])
    assert _grey(ep, 20, 20) > 240 and _grey(ep, 120, 20) < 60


def test_line_width_pinch_and_erase_to_crossing():
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "width_mm": 0.3, "stabilize": 0, "taper": False,
                    "points": [[50, 70], [70, 70], [90, 70]]}])
    apply_ops(ep, [{"op": "set_stroke_width", "page": 1, "layer_id": ink, "area": SQUARE, "width_mm": 1.2}])
    assert _ink(ep).strokes[0].width_mm == pytest.approx(1.2)
    sid = _ink(ep).strokes[0].id
    apply_ops(ep, [{"op": "reshape_stroke", "page": 1, "layer_id": ink, "stroke_id": sid, "points": [[50, 70], [70, 60], [90, 70]]}])
    assert _ink(ep).strokes[0].points[1] == pytest.approx((70, 60))
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_stroke_width", "page": 1, "layer_id": ink, "area": {"poly": [[200, 200], [210, 200], [210, 210]]},
                        "width_mm": 1}])
    # a cross: erasing the overhang of the horizontal line past the vertical one cuts it at the crossing
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "stabilize": 0, "taper": False,
                    "points": [[40, 100], [100, 100]]},
                   {"op": "add_stroke", "page": 1, "layer_id": ink, "kind": "mili", "stabilize": 0, "taper": False,
                    "points": [[80, 60], [80, 140]]}])
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink, "points": [[95, 98], [95, 102]], "width_mm": 1, "mode": "to_crossing"}])
    horizontal = [s for s in _ink(ep).strokes if abs(s.points[0][1] - s.points[-1][1]) < 1e-6]
    assert horizontal and max(p[0] for s in horizontal for p in s.points) == pytest.approx(80, abs=0.6)
    assert any(abs(s.points[0][0] - s.points[-1][0]) < 1e-6 for s in _ink(ep).strokes)  # the vertical one stays


def test_agents_use_the_same_tools_but_ink_waits_for_the_name_approval():
    from genko.ops import OPS_SCHEMA
    from genko.studio.service import AGENT_OPS

    tools = {"fill", "fill_area", "transform_area", "delete_area", "paste", "set_stroke_width", "reshape_stroke"}
    assert tools <= AGENT_OPS and tools <= {entry["op"] for entry in OPS_SCHEMA}
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ep.strict_gates = True
    ink = _ink(ep).id
    with pytest.raises(ApplyError, match="name_ok"):
        apply_ops(ep, [{"op": "fill_area", "page": 1, "layer_id": ink, "area": SQUARE}], agent="hermes")


# --- the window ---------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtCore import QSettings
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    settings = QSettings("Genko", "Genko Studio")
    for group in ("brush", "fill", "eraser"):
        settings.remove(group)
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    ep = new_episode("ペン", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    ink = next(layer for layer in win.current_page().layers if layer.role == LayerRole.INK)
    win.set_target_layer(ink.id)
    yield win
    win.close()
    for group in ("brush", "fill", "eraser"):  # the next tests start from the defaults
        settings.remove(group)


def _drag(canvas, a, b, steps=6, path=None):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton

    def ev(kind, p, button, buttons):
        return QMouseEvent(kind, p, canvas.mapToGlobal(p), button, buttons, Qt.KeyboardModifier.NoModifier)

    points = path or [(a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps) for i in range(steps + 1)]
    canvas.mousePressEvent(ev(QEvent.Type.MouseButtonPress, canvas._pt(*points[0]), left, left))
    for pt in points[1:]:
        canvas.mouseMoveEvent(ev(QEvent.Type.MouseMove, canvas._pt(*pt), left, left))
    canvas.mouseReleaseEvent(ev(QEvent.Type.MouseButtonRelease, canvas._pt(*points[-1]), left, none))


def _click(canvas, at):
    _drag(canvas, at, at, path=[at])


def _layer(win):
    return win.target_layer()


def test_brush_panel_draws_with_the_chosen_pen(window):
    window.brush.kinds.setCurrentRow(list(brushes.BRUSHES).index("maru"))
    window.brush.size.setValue(0.8)
    window.brush.set_colour((50, 140, 200))
    window.act_pen.trigger()
    _drag(window.canvas, (60, 100), (140, 100))
    stroke = _layer(window).strokes[-1]
    assert stroke.kind == "maru" and stroke.width_mm == pytest.approx(0.8) and tuple(stroke.rgb) == (50, 140, 200)
    window._nudge_brush(1)
    assert window.brush.size.value() == pytest.approx(1.2)
    # the eyedropper picks up the blue line
    window.act_picker.trigger()
    window.brush.set_colour((0, 0, 0))
    window.canvas.invalidate()
    window.canvas.repaint()
    _click(window.canvas, (100, 100))
    assert window.brush.rgb[2] > window.brush.rgb[0]


def test_fill_lasso_fill_and_selection_from_the_window(window):
    ep = window.episode
    ink = _layer(window).id
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": pts, "width_mm": 0.6, "kind": "mili", "stabilize": 0,
                       "taper": False} for pts in ([[60, 80], [120, 80]], [[120, 80], [120, 140]], [[120, 140], [60, 140]],
                                                   [[60, 140], [60, 80]])])
    window.brush.set_colour((0, 0, 0))
    window.act_fill.trigger()
    _click(window.canvas, (90, 110))
    assert len(_layer(window).patches) == 1
    assert _grey(window.episode, 90, 110) < 60
    window.act_lassofill.trigger()
    _drag(window.canvas, None, None, path=[(150, 60), (190, 60), (190, 100), (150, 100), (150, 61)])
    assert len(_layer(window).patches) == 2
    # a rectangle selection around the square, moved by dragging its inside
    window.act_marquee.trigger()
    _drag(window.canvas, (55, 75), (125, 145))
    assert window.canvas.selection is not None
    _drag(window.canvas, (90, 110), (90, 160))
    assert _grey(window.episode, 90, 110) > 240 and _grey(window.episode, 90, 160) < 60
    assert window.canvas.selection["outline"][0][1] == pytest.approx(125, abs=0.3)
    # flip, copy / paste onto a new layer, delete
    window.act_flip_h.trigger()
    count = len(window.current_page().layers)
    window.act_copy.trigger()
    window.act_paste.trigger()
    assert len(window.current_page().layers) == count + 1 and _layer(window).title == "貼り付け"
    assert _layer(window).strokes or _layer(window).patches
    window.act_delete_area.trigger()
    assert not _layer(window).strokes and not _layer(window).patches
    window.act_deselect.trigger()
    assert window.canvas.selection is None
    del ep


def test_wand_and_pinch_from_the_window(window):
    ink = _layer(window).id
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": pts, "width_mm": 0.6, "kind": "mili", "stabilize": 0,
                       "taper": False} for pts in ([[60, 80], [120, 80]], [[120, 80], [120, 140]], [[120, 140], [60, 140]],
                                                   [[60, 140], [60, 80]])])
    window.act_wand.trigger()
    _click(window.canvas, (90, 110))
    area = window.canvas.selection["area"]
    x, y, w, h = area["mask"]["box"]
    assert 55 < x < 65 and 55 < w < 65 and 55 < h < 65
    window.act_fill_selection.trigger()
    assert _grey(window.episode, 90, 110) < 100
    window.canvas.set_selection(None)
    window.act_reshape.trigger()
    before = [list(s.points) for s in _layer(window).strokes]
    _drag(window.canvas, (90, 80), (90, 70))
    top = next(s for s in _layer(window).strokes if s.points and abs(s.points[0][1] - 80) < 1e-6 and abs(s.points[-1][1] - 80) < 1e-6
               and s.points[0][0] == pytest.approx(60))
    assert top is not None
    assert [list(s.points) for s in _layer(window).strokes] != before


def test_erase_to_crossing_from_the_window(window):
    ink = _layer(window).id
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[40, 100], [100, 100]], "kind": "mili", "stabilize": 0,
                       "taper": False},
                      {"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[80, 60], [80, 140]], "kind": "mili", "stabilize": 0,
                       "taper": False}])
    window.brush.crossing.setChecked(True)
    window.act_eraser.trigger()
    _drag(window.canvas, (95, 98), (95, 102))
    right = max(p[0] for s in _layer(window).strokes for p in s.points if abs(p[1] - 100) < 1e-6)
    assert right == pytest.approx(80, abs=0.6)
    assert math.isfinite(right)
