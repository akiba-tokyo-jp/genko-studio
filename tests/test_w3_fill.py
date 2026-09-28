"""Fills: CLIP's enclose-and-fill, the colour margin and how far under the lines, draft and text left out,
and the gaps along a traced line."""

from __future__ import annotations

import os

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


def _square(x, y, s):
    return [[x, y], [x + s, y], [x + s, y + s], [x, y + s], [x, y]]


def _book(extra=()):
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ops = [{"op": "add_stroke", "page": 1, "layer": "ink", "points": _square(50, 50, 20), "width_mm": 0.5, "taper": False, "stabilize": 0},
           {"op": "add_stroke", "page": 1, "layer": "ink", "points": _square(90, 50, 20), "width_mm": 0.5, "taper": False, "stabilize": 0},
           {"op": "add_layer", "page": 1, "kind": "paint", "id": "c"}, *extra]
    apply_ops(ep, ops)
    return ep


def _at(ep, x, y):
    image = render_page(ep.pages[0], 100, mode="proof", episode=ep).convert("RGB")
    return image.getpixel((round(x / 25.4 * 100), round(y / 25.4 * 100)))


def test_only_what_the_lines_close_inside_the_lasso_is_filled():
    ep = _book()
    apply_ops(ep, [{"op": "fill_enclosed", "page": 1, "layer_id": "c", "poly": [[40, 40], [100, 40], [100, 80], [40, 80]],
                    "rgb": [200, 0, 0]}])
    assert _at(ep, 60, 60) == (200, 0, 0)  # (inside, and closed)
    assert _at(ep, 100, 60) == (255, 255, 255)  # (closed, but running out of the lasso)
    assert _at(ep, 45, 45) == (255, 255, 255)  # (inside the lasso, but open to the page)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "fill_enclosed", "page": 1, "layer_id": "c", "poly": [[20, 20], [30, 20], [30, 30]]}])


def test_draft_and_text_can_be_left_out_of_the_walls():
    ep = _book([{"op": "add_layer", "page": 1, "kind": "pen", "id": "d"},
                {"op": "set_layer", "page": 1, "id": "d", "exportable": False},
                {"op": "add_stroke", "page": 1, "layer_id": "d", "points": [[50, 60], [70, 60]], "width_mm": 0.6,
                 "taper": False, "stabilize": 0}])
    ep.pages[0].name_ok = True
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "c", "x_mm": 60, "y_mm": 55, "rgb": [0, 0, 200]}])
    assert _at(ep, 60, 65) != (0, 0, 200)  # (the draft line splits the square)
    ep = _book([{"op": "add_layer", "page": 1, "kind": "pen", "id": "d"},
                {"op": "set_layer", "page": 1, "id": "d", "exportable": False},
                {"op": "add_stroke", "page": 1, "layer_id": "d", "points": [[50, 60], [70, 60]], "width_mm": 0.6,
                 "taper": False, "stabilize": 0}])
    ep.pages[0].name_ok = True
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "c", "x_mm": 60, "y_mm": 55, "rgb": [0, 0, 200], "ignore": ["draft"]}])
    assert _at(ep, 60, 65) == (0, 0, 200)


def test_the_colour_margin_decides_what_a_grey_line_does():
    ep = _book([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[50, 60], [70, 60]], "width_mm": 0.6,
                 "taper": False, "stabilize": 0, "rgb": [150, 150, 150]}])
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "c", "x_mm": 60, "y_mm": 55, "rgb": [0, 150, 0], "tolerance": 60}])
    assert _at(ep, 60, 65) == (0, 150, 0)  # (a wide margin: the grey line is crossed)
    ep = _book([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[50, 60], [70, 60]], "width_mm": 0.6,
                 "taper": False, "stabilize": 0, "rgb": [150, 150, 150]}])
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "c", "x_mm": 60, "y_mm": 55, "rgb": [0, 150, 0], "tolerance": 30}])
    assert _at(ep, 60, 65) != (0, 150, 0)  # (a narrow margin: the grey line is a wall)


def test_the_brush_panel_gives_the_fill_its_settings():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")
    from genko.app.brush_panel import BrushPanel

    panel = BrushPanel()
    panel._loading = True  # (not kept in the person's settings)
    panel.tolerance.setValue(55)
    panel.expand.setValue(0.3)
    panel.skip_draft.setChecked(True)
    fields = panel.fill_fields()
    assert fields["tolerance"] == 55 and fields["expand_mm"] == 0.3 and fields["ignore"] == ["draft"]
    assert panel.lasso_mode.findData("enclosed") >= 0 and panel.lasso_mode.findData("gaps") >= 0
