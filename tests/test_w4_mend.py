"""Mending inked lines: widen or narrow only where traced, one point's width, fewer points, a part redrawn
by tracing over it, its width redrawn, ends joined along a trace, a pinch that keeps the ends."""

from __future__ import annotations

import math
import os

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


def _book(*lines):
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "p"}] + [
        {"op": "add_stroke", "page": 1, "layer_id": "p", "points": pts, "width_mm": 0.6, "taper": False, "stabilize": 0}
        for pts in lines])
    return ep


def _strokes(ep):
    return next(layer for layer in ep.pages[0].layers if layer.id == "p").strokes


def _mend(ep, **op):
    apply_ops(ep, [{"op": "trace_edit", "page": 1, "layer_id": "p", **op}])


def _straight(x0, x1, y=50.0, n=41):
    return [[x0 + (x1 - x0) * i / (n - 1), y, 0.7] for i in range(n)]


def test_widen_and_narrow_only_where_traced():
    ep = _book(_straight(20, 60))
    _mend(ep, action="widen", points=[[38, 48], [42, 52]], radius_mm=2, amount=0.5)
    pressure = _strokes(ep)[0].pressure
    assert max(pressure) > 0.9 and pressure[0] == pytest.approx(0.7) and pressure[-1] == pytest.approx(0.7)
    _mend(ep, action="narrow", points=[[20, 49], [22, 51]], radius_mm=2, amount=0.8)
    assert _strokes(ep)[0].pressure[0] < 0.5
    with pytest.raises(ApplyError):
        _mend(ep, action="widen", points=[[100, 100], [110, 110]])


def test_one_points_width_and_fewer_points():
    ep = _book(_straight(20, 60))
    stroke = _strokes(ep)[0]
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": "p", "action": "set_pressure", "stroke_id": stroke.id, "index": 5,
                    "pressure": 1.2}])
    assert _strokes(ep)[0].pressure[5] == 1.2 and _strokes(ep)[0].pressure[4] == pytest.approx(0.7)
    apply_ops(ep, [{"op": "vector_edit", "page": 1, "layer_id": "p", "action": "simplify", "stroke_id": stroke.id}])
    assert len(_strokes(ep)[0].points) < 41


def test_a_part_is_redrawn_by_tracing_over_it():
    ep = _book(_straight(20, 80))
    bump = [[40 + i, 50 - 5 * math.sin(math.pi * i / 20), 0.9] for i in range(21)]
    _mend(ep, action="redraw", points=bump, radius_mm=1.5)
    pts = _strokes(ep)[0].points
    assert pts[0] == (20.0, 50.0) and pts[-1] == (80.0, 50.0)
    assert min(y for _, y in pts) < 46  # (the middle took the traced bump)
    with pytest.raises(ApplyError):  # (a trace that does not start and end on the line)
        _mend(ep, action="redraw", points=[[30, 50], [30, 70]], radius_mm=1)


def test_the_width_is_redrawn_by_the_pens_pressure():
    ep = _book(_straight(20, 60))
    _mend(ep, action="redraw_width", points=[[30, 50, 0.2], [40, 50, 0.2]], radius_mm=1)
    pressure = _strokes(ep)[0].pressure
    assert pressure[15] == pytest.approx(0.2) and pressure[0] == pytest.approx(0.7)


def test_ends_along_a_trace_are_joined():
    ep = _book(_straight(20, 40), _straight(42, 60))
    _mend(ep, action="join", points=[[41, 45], [41, 55]], radius_mm=3, join_mm=5)
    strokes = _strokes(ep)
    assert len(strokes) == 1 and strokes[0].points[0][0] == 20 and strokes[0].points[-1][0] == 60


def test_the_pinch_keeps_the_ends_when_asked():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")
    from genko.app.canvas import PageCanvas

    ep = _book(_straight(20, 30, n=11))
    canvas = PageCanvas()
    canvas.strokes_for_reshape = lambda: _strokes(ep)
    canvas.reshape_radius_mm = 20
    canvas.reshape_pin_ends = True
    canvas._reshape_press(25, 50)
    canvas._reshape_move(25, 60)
    moved = canvas._reshape["points"]
    assert moved[0][1] == pytest.approx(50) and moved[-1][1] == pytest.approx(50)
    assert max(p[1] for p in moved) > 53
