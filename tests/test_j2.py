"""J2: figures, every way to make and change a selection (for people and agents), guide lines pulled
from the scales, and the sub-view of reference pictures."""

import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import rulers, selops, selection  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(page):
    return next(layer for layer in page.layers if layer.role == LayerRole.INK)


def _book():
    return new_episode("t", 1, 1, PageSpec.b4_comic())


# --- figures ---------------------------------------------------------------------------------------------


@pytest.mark.parametrize("shape,extra", [
    ("line", {"points": [[40, 40], [120, 90]]}),
    ("polyline", {"points": [[40, 40], [80, 60], [60, 100]], "closed": True}),
    ("curve", {"points": [[40, 40], [80, 20], [120, 60], [160, 40]]}),
    ("rect", {"box": [40, 40, 80, 50], "radius_mm": 6}),
    ("ellipse", {"box": [40, 40, 80, 50]}),
    ("polygon", {"box": [40, 40, 80, 80], "sides": 6}),
])
def test_figures_draw_as_lines(shape, extra):
    ep = _book()
    ink = _ink(ep.pages[0])
    apply_ops(ep, [{"op": "add_shape", "page": 1, "layer_id": ink.id, "shape": shape, "width_mm": 0.8, **extra}])
    stroke = _ink(ep.pages[0]).strokes[-1]
    assert len(stroke.points) >= 2
    if shape in ("rect", "ellipse", "polygon") or extra.get("closed"):
        assert stroke.points[0] == stroke.points[-1]  # closed
    if shape == "curve":
        assert len(stroke.points) > 12  # smooth, not four corners


def test_a_figure_can_be_filled():
    ep = _book()
    ink = _ink(ep.pages[0])
    apply_ops(ep, [{"op": "add_shape", "page": 1, "layer_id": ink.id, "shape": "ellipse", "box": [40, 40, 60, 60],
                    "line": False, "fill": True, "rgb": [200, 0, 0]}])
    layer = _ink(ep.pages[0])
    assert not layer.strokes and layer.patches
    image = render_page(ep.pages[0], 50, mode="proof", episode=ep).convert("RGB")
    assert image.getpixel((round(70 / 25.4 * 50), round(70 / 25.4 * 50)))[0] > 150


def test_figures_refuse_nonsense():
    ep = _book()
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_shape", "page": 1, "layer": "ink", "shape": "star", "box": [0, 0, 1, 1]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_shape", "page": 1, "layer": "ink", "shape": "rect"}])


# --- areas (the selection language) --------------------------------------------------------------------------


def _covered(area, page, x, y):
    mask = selops.to_mask(area, page)
    return mask.getpixel((round(x / 25.4 * selops.SEL_DPI), round(y / 25.4 * selops.SEL_DPI))) > 127


def test_areas_join_invert_grow_and_feather():
    page = _book().pages[0]
    a = {"rect": [20, 20, 40, 40]}
    b = {"ellipse": [40, 40, 40, 40]}
    union = {"union": [a, b]}
    assert _covered(union, page, 25, 25) and _covered(union, page, 70, 60)
    minus = {"subtract": [a, b]}
    assert _covered(minus, page, 25, 25) and not _covered(minus, page, 55, 55)
    both = {"intersect": [a, b]}
    assert _covered(both, page, 55, 55) and not _covered(both, page, 25, 25)
    inverted = {"rect": [20, 20, 40, 40], "invert": True}
    assert _covered(inverted, page, 100, 100) and not _covered(inverted, page, 30, 30)
    grown = {"rect": [20, 20, 40, 40], "grow_mm": 3}
    assert _covered(grown, page, 18, 30) and not _covered({"rect": [20, 20, 40, 40]}, page, 18, 30)
    shrunk = {"rect": [20, 20, 40, 40], "grow_mm": -3}
    assert not _covered(shrunk, page, 21, 30)
    soft = selops.to_mask({"rect": [20, 20, 40, 40], "feather_mm": 2}, page)
    edge = soft.getpixel((round(20 / 25.4 * selops.SEL_DPI), round(40 / 25.4 * selops.SEL_DPI)))
    assert 20 < edge < 235  # half way at the edge


def test_areas_from_a_layer_and_a_colour():
    ep = _book()
    ink = _ink(ep.pages[0])
    apply_ops(ep, [{"op": "add_shape", "page": 1, "layer_id": ink.id, "shape": "rect", "box": [50, 50, 30, 30],
                    "line": False, "fill": True, "rgb": [0, 0, 200]}])
    page = ep.pages[0]
    drawn = {"layer": ink.id}
    assert _covered(drawn, page, 65, 65) and not _covered(drawn, page, 20, 20)
    colour = {"color": {"x_mm": 65, "y_mm": 65, "tolerance": 30}}
    mask = selops.to_mask(colour, page, ep)
    assert mask.getpixel((round(65 / 25.4 * 200), round(65 / 25.4 * 200))) == 255
    assert mask.getpixel((round(20 / 25.4 * 200), round(20 / 25.4 * 200))) == 0


def test_any_op_takes_the_richer_areas():
    ep = _book()
    ink = _ink(ep.pages[0])
    apply_ops(ep, [{"op": "fill_area", "page": 1, "layer_id": ink.id, "rgb": [0, 0, 0],
                    "area": {"subtract": [{"rect": [60, 80, 60, 60]}, {"ellipse": [75, 95, 30, 30]}]}}])
    image = render_page(ep.pages[0], 50, mode="proof", episode=ep).convert("L")
    px = lambda v: round(v / 25.4 * 50)  # noqa: E731
    assert image.getpixel((px(65), px(85))) < 80 and image.getpixel((px(90), px(110))) > 200  # a hole in the middle
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "fill_area", "page": 1, "layer_id": ink.id, "area": {"saved": "nope"}}])


def test_areas_can_be_kept_on_the_page(tmp_path: Path):
    ep = _book()
    apply_ops(ep, [{"op": "store_area", "page": 1, "name": "空", "area": {"rect": [0, 0, 100, 40]}}])
    assert "空" in ep.pages[0].extra["saved_areas"]
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    again = load_episode(project)
    assert "空" in again.pages[0].extra["saved_areas"]
    assert _covered({"saved": "空"}, again.pages[0], 50, 20)
    apply_ops(again, [{"op": "forget_area", "page": 1, "name": "空"}])
    assert "空" not in again.pages[0].extra["saved_areas"]


def test_a_selection_pen_stroke_is_an_area():
    area = selops.stroke_area([[40, 40], [80, 40]], 6)
    assert selection.contains(area, 60, 41) and not selection.contains(area, 60, 50)


def test_guides_snap_lines_near_them():
    ruler = {"id": "g", "kind": "guide", "axis": "h", "at": 100.0, "active": True}
    rulers.validate(ruler)
    snapped = rulers.snap([(20, 101.5), (120, 108)], [ruler])
    assert all(abs(p[1] - 100.0) < 1e-6 for p in snapped)
    far = rulers.snap([(20, 110), (120, 118)], [ruler])
    assert far[0][1] == 110
    ep = _book()
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "guide", "axis": "v", "at": 64}])
    assert ep.pages[0].rulers[0]["axis"] == "v"
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "guide", "axis": "z", "at": 1}])


# --- the app ------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(_book(), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_the_figure_tool_draws(window, qapp):
    window._tool("shape")
    window.shape_kind.setCurrentIndex(window.shape_kind.findData("ellipse"))
    window.canvas.shape_kind = "ellipse"
    window.canvas.shapeDrawn.emit({"shape": "ellipse", "box": [40, 40, 50, 30]})
    assert window.target_layer().strokes[-1].points[0] == window.target_layer().strokes[-1].points[-1]
    # a polyline by clicks, ended with Enter
    window.canvas.shape_kind = "polyline"
    for p in ((30, 30), (60, 50), (40, 90)):
        window.canvas._shape_press(*p, window.canvas._modifiers if hasattr(window.canvas, "_modifiers") else 0)
    assert window.canvas.finish_points()
    assert len(window.target_layer().strokes) == 2


def test_selections_join_with_shift_and_alt(window, qapp):
    canvas = window.canvas
    window._tool("rect")
    canvas._sel_how = "replace"
    canvas._selection_done({"poly": [[20, 20], [60, 20], [60, 60], [20, 60]]})
    canvas._sel_how = "add"
    canvas._selection_done({"poly": [[100, 100], [140, 100], [140, 140], [100, 140]]})
    area = canvas.selection["area"]
    page = window.current_page()
    assert _covered(area, page, 30, 30) and _covered(area, page, 120, 120)
    canvas._sel_how = "subtract"
    canvas._selection_done({"poly": [[100, 100], [140, 100], [140, 140], [100, 140]]})
    area = canvas.selection["area"]
    assert _covered(area, page, 30, 30) and not _covered(area, page, 120, 120)
    window._change_selection({"invert": True})
    assert not _covered(canvas.selection["area"], page, 30, 30)
    window._change_selection({"grow_mm": 2})
    window.canvas.set_selection(None)


def test_the_selection_pen_eraser_and_colour(window, qapp):
    canvas = window.canvas
    window._tool("selpen")
    assert canvas.marquee == "pen" and window.marquee_mode.currentData() == "selpen"
    window._selection_painted([[40, 40], [90, 40]], True)
    assert canvas.selection is not None
    window._selection_painted([[60, 20], [60, 70]], False)
    assert not _covered(canvas.selection["area"], window.current_page(), 60, 40)
    canvas.set_selection(None)
    canvas._sel_how = "replace"
    window._select_colour(150, 150)  # the white page
    assert canvas.selection is not None


def test_keep_and_bring_back_a_selection(window, qapp):
    window.canvas.set_selection({"poly": [[20, 20], [60, 20], [60, 60], [20, 60]]})
    assert window._keep_selection("髪")
    window.canvas.set_selection(None)
    window._fill_stock()
    assert "髪" in [a.text() for a in window.stock_menu.actions()]
    window._use_stock("髪")
    assert window.canvas.selection is not None


def test_the_launcher_follows_the_selection(window, qapp):
    canvas = window.canvas
    canvas.set_selection({"poly": [[40, 40], [90, 40], [90, 90], [40, 90]]})
    canvas.repaint()
    qapp.processEvents()
    assert canvas.launcher.isVisible()
    assert [label for label, _ in window.launcher_actions][:3] == ["塗る", "消す", "トーン"]
    canvas.set_selection(None)
    canvas.repaint()
    assert not canvas.launcher.isVisible()


def test_scales_and_guides(window, qapp):
    from PySide6.QtCore import QPointF

    canvas = window.canvas
    window.act_scale.trigger()
    assert canvas.show_scale
    placed = []
    canvas.rulerPlaced.connect(placed.append)
    assert canvas._guide_press(QPointF(200, 5))
    canvas._guide_move(QPointF(200, 300))
    canvas._guide_release(QPointF(200, 300))
    assert placed and placed[0]["kind"] == "guide" and placed[0]["axis"] == "h"
    assert any(r.get("kind") == "guide" for r in window.current_page().rulers)


def test_the_sub_view_gives_colours(window, qapp, tmp_path: Path):
    picture = tmp_path / "ref.png"
    Image.new("RGB", (40, 40), (10, 200, 30)).save(picture)
    assert window.subview.add(str(picture))
    assert window.sub_dock.windowTitle() == "サブビュー"
    view = window.subview.picture
    view.resize(200, 200)
    from PySide6.QtCore import QPointF

    colour = view.colour_at(QPointF(100, 100))
    assert colour == (10, 200, 30)
    view.picked.emit(colour)
    assert tuple(window.brush.rgb) == (10, 200, 30)
