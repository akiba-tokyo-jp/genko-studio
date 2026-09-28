"""A new line's ends meet the lines nearby (ベクター吸着), the eraser runs along a ruler, and paint is
erased softly or roughly."""

from __future__ import annotations

import os

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


def _pen(ep):
    return next(layer for layer in ep.pages[0].layers if layer.id == "p")


def test_a_new_lines_ends_meet_the_lines_nearby():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "p"},
                   {"op": "add_stroke", "page": 1, "layer_id": "p", "points": [[20, 50], [80, 50]], "stabilize": 0, "taper": False}])
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "p", "points": [[40, 20], [40, 30], [41, 48.8]], "stabilize": 0,
                    "taper": False, "snap_lines_mm": 2}])
    end = _pen(ep).strokes[-1].points[-1]
    assert end == pytest.approx((41.0, 50.0), abs=0.01)  # (onto the line below)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "p", "points": [[60, 20], [79, 51]], "stabilize": 0,
                    "taper": False, "snap_lines_mm": 2}])
    assert _pen(ep).strokes[-1].points[-1] == pytest.approx((80.0, 50.0), abs=0.01)  # (an end of a line first)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "p", "points": [[60, 20], [60, 40]], "stabilize": 0,
                    "taper": False, "snap_lines_mm": 2}])
    assert _pen(ep).strokes[-1].points[-1] == pytest.approx((60.0, 40.0))  # (too far: left where drawn)


def test_the_eraser_runs_along_a_ruler():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "p"},
                   {"op": "add_stroke", "page": 1, "layer_id": "p", "points": [[50, 20], [50, 80]], "stabilize": 0, "taper": False},
                   {"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 60], [90, 60]]}])
    # a wobbly eraser stroke near the ruler: along it, it cuts the line at y = 60
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": "p", "points": [[40, 63], [50, 57], [60, 64]], "width_mm": 1,
                    "snap_ruler": True}])
    pieces = sorted(_pen(ep).strokes, key=lambda s: s.points[0][1])
    assert len(pieces) == 2 and pieces[0].points[-1][1] == pytest.approx(59.5, abs=0.6)


def test_paint_is_erased_softly_or_roughly():
    from genko.render import layer_image

    def erased(texture):
        ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
        apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "c"},
                       {"op": "fill_area", "page": 1, "layer_id": "c", "area": {"poly": [[20, 20], [100, 20], [100, 100], [20, 100]]},
                        "rgb": [0, 0, 0]}])
        apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": "c", "points": [[30, 60], [90, 60]], "width_mm": 8,
                        "texture": texture}])
        page = ep.pages[0]
        alpha = layer_image(page, next(la for la in page.layers if la.id == "c"), 100, ep).split()[3]
        row = [alpha.getpixel((round(60 / 25.4 * 100), round(y / 25.4 * 100))) for y in (60, 62.5, 63.5, 70)]
        return row

    hard, soft, rough = erased("hard"), erased("soft"), erased("rough")
    assert hard[0] == 0 and hard[3] == 255
    assert soft[0] < 60 and 0 < soft[2] < 255  # (the edge fades)
    assert rough[3] == 255 and 0 < rough[0] < 255  # (a grain: some of the paint is left in the erased path)
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "erase", "page": 1, "layer": "ink", "points": [[1, 1], [2, 2]], "texture": "wet"}])
