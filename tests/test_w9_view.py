"""The view and transforms as CLIP STUDIO has them: the zoom typed in, the magnifier, the view upside down,
the transform's pivot and numbers, and a mesh of any grid."""

from __future__ import annotations

import math

import pytest
from PySide6.QtCore import QPointF

from genko import warp


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path):
    from genko.app.main import MainWindow

    win = MainWindow()
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def test_a_mesh_of_any_grid():
    pts = [[x * 5.0, y * 10.0] for y in range(2) for x in range(3)]  # (3 across, 2 down)
    pts[1] = [5.0, 2.0]  # the top middle pulled down
    go = warp.mapping((0, 0, 10, 10), {"mesh": pts, "grid": [3, 2]})
    assert go(5, 0) == pytest.approx((5.0, 2.0)) and go(10, 10) == pytest.approx((10.0, 10.0))
    with pytest.raises(warp.WarpError):
        warp.mapping((0, 0, 10, 10), {"mesh": pts})  # (six points need their grid)
    square = [[x * 2.5, y * 2.5] for y in range(5) for x in range(5)]
    assert warp.mapping((0, 0, 10, 10), {"mesh": square, "grid": [5, 5]})(3, 3) == pytest.approx((3.0, 3.0))
    with pytest.raises(warp.WarpError):
        warp.mapping((0, 0, 10, 10), {"mesh": square})


def test_the_numbers_turn_about_the_pivot():
    from genko.app.main import transform_matrix

    a, b, c, d, e, f = transform_matrix((10, 10), angle=90)
    x, y = a * 20 + c * 10 + e, b * 20 + d * 10 + f
    assert (x, y) == pytest.approx((10, 20))  # (a quarter turn clockwise on the page, about (10, 10))
    a, b, c, d, e, f = transform_matrix((0, 0), dx=5, sx=2, sy=0.5)
    assert (a * 3 + e, d * 4 + f) == pytest.approx((11, 2))


def test_zoom_typed_in_upside_down_and_the_magnifier(window, qapp):
    canvas = window.canvas
    canvas.set_zoom_percent(200)
    assert abs(canvas.zoom_percent() - 200) <= 1
    window._refresh_zoom()
    assert window.zoom_box.value() == canvas.zoom_percent()
    window.zoom_box.setValue(50)
    assert abs(canvas.zoom_percent() - 50) <= 1
    window.act_view_flip_v.trigger()
    assert canvas.flipped_v and "上下反転" in window.zoom_label.text()
    mid = QPointF(canvas.width() / 2, canvas.height() / 2)
    assert canvas._ev(QPointF(mid.x(), mid.y() - 50)).y() == pytest.approx(mid.y() + 50)
    canvas.reset_view()
    assert not canvas.flipped_v
    window._tool("zoom")
    before = canvas.zoom_percent()
    canvas.zoom_to_rect(20, 20, 60, 50)
    assert canvas.zoom_percent() > before
    x0, y0 = canvas._to_mm(QPointF(0, 0))
    x1, y1 = canvas._to_mm(QPointF(canvas.width(), canvas.height()))
    assert x0 <= 20 and x1 >= 60 and y0 <= 20 and y1 >= 50  # (the area fills the view)


def test_the_pivot_moves_and_the_selection_turns_about_it(window):
    canvas = window.canvas
    canvas.set_tool("marquee")
    canvas.set_selection({"poly": [[20, 20], [60, 20], [60, 40], [20, 40]]})
    assert canvas.selection_pivot() == pytest.approx((40, 30))
    assert not any(h[0] == "pivot" for h in canvas._sel_handles())  # (the middle is for moving the selection)
    canvas.pivot_mode = True
    assert canvas._marquee_press(canvas._pt(20, 20))
    canvas._sel_drag = None
    assert canvas.sel_pivot == pytest.approx([20, 20]) and not canvas.pivot_mode
    assert next(h for h in canvas._sel_handles() if h[0] == "pivot")[2] == pytest.approx((20, 20))
    canvas._sel_drag = {"kind": "rotate", "key": "r", "start": (20, 10), "box": canvas._sel_box()}
    a, b, c, d, e, f = canvas._sel_matrix((30, 20))  # (a quarter turn about the top-left corner)
    assert (a * 20 + c * 20 + e, b * 20 + d * 20 + f) == pytest.approx((20, 20))
    assert math.isclose(abs(b), 1, abs_tol=1e-6)
    canvas._sel_drag = None
    assert canvas.start_warp("mesh", 3, 1)
    assert len(canvas.warp["points"]) == 8 and canvas.warp["grid"] == [4, 2]
    got = []
    canvas.selectionWarped.connect(got.append)
    canvas.finish_warp()
    assert got and got[0]["grid"] == [4, 2]
    canvas.set_selection({"poly": [[20, 20], [60, 20], [60, 40], [20, 40]]})
    assert canvas.sel_pivot is None
