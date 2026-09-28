"""Rulers and backgrounds as CLIP STUDIO has them: shape rulers, a selection from a ruler, the perspective
grid, and the 3D camera and the perspective ruler matched both ways."""

from __future__ import annotations

import math

import pytest

from genko import persp3d, rulers
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


def test_a_line_runs_round_a_rectangle_keeping_its_corners():
    rect = {"kind": "rect", "points": [[10, 10], [50, 30]]}
    rulers.validate(rect)
    out = rulers.snap([[12, 9.5], [30, 10.5], [49, 11], [50.5, 20], [49, 29]], [rect])
    pts = [tuple(round(v, 2) for v in p[:2]) for p in out]
    assert (50.0, 10.0) in pts and pts[-1] == (50.0, 29.0)
    assert all(min(abs(x - 10), abs(x - 50)) < 1e-6 or min(abs(y - 10), abs(y - 30)) < 1e-6 for x, y in pts)
    big = {"kind": "rect", "points": [[10, 10], [90, 70]]}
    far = rulers.snap([[50, 40], [55, 42]], [big])  # (started in the middle, far from the outline: free)
    assert far == [[50, 40], [55, 42]]


def test_ellipse_and_polygon_rulers():
    ellipse = {"kind": "ellipse", "points": [[0, 0], [40, 20]]}
    out = rulers.snap([[20, 0.5], [35, 3], [39.5, 10]], [ellipse])
    for x, y in (p[:2] for p in out):
        assert ((x - 20) / 20) ** 2 + ((y - 10) / 10) ** 2 == pytest.approx(1, abs=0.02)
    tri = {"kind": "polygon", "points": [[0, 0], [30, 0], [15, 20]]}
    rulers.validate(tri)
    line = rulers.outline(tri)[0]
    assert line[0] == line[-1] and len(line) == 4
    with pytest.raises(ValueError):
        rulers.validate({"kind": "rect", "points": [[0, 0], [0.2, 10]]})


def test_the_perspective_grid():
    one = {"kind": "perspective", "points": [[90, 80]], "grid": 8}
    lines = rulers.perspective_grid(one, (180, 250))
    to_vp = [ln for ln in lines if ln[0] == (90.0, 80.0)]
    across = [ln for ln in lines if ln[0][1] == ln[1][1]]
    assert len(to_vp) == 9 and across  # (lines to the point, and the depths across)
    depths = sorted(ln[0][1] for ln in across)
    gaps = [b - a for a, b in zip(depths, depths[1:])]
    assert all(b >= a - 1e-6 for a, b in zip(gaps, gaps[1:]))  # (further back, closer together)
    two = {"kind": "perspective", "points": [[-80, 70], [260, 70]], "grid": 6}
    assert len(rulers.perspective_grid(two, (180, 250))) == 14
    assert rulers.perspective_grid({"kind": "perspective", "points": [[1, 1]]}, (10, 10)) == []


def test_the_3d_and_the_perspective_ruler_both_ways():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_prim3d", "page": 1, "kind": "box", "id": "b", "pos": [90, 130, 0], "size": [40, 40, 40], "rot": [0, 0, 0]},
                        {"op": "set_camera", "page": 1, "tip": 0.2, "turn": 0.6, "focal_mm": 350}])
    page = episode.pages[0]
    camera = page.extra["camera"]
    want = persp3d.vanishing_points(page.prims[0], camera)
    apply_ops(episode, [{"op": "ruler_from_3d", "page": 1, "id": "p", "grid": 10}])
    ruler = next(r for r in episode.pages[0].rulers if r["id"] == "p")
    assert ruler["kind"] == "perspective" and ruler["grid"] == 10
    assert ruler["points"][0] == pytest.approx(list(want["x"])) and ruler["points"][1] == pytest.approx(list(want["z"]))
    # the camera turned elsewhere, then brought back to the ruler (its two points)
    apply_ops(episode, [{"op": "set_camera", "page": 1, "tip": 0.0, "turn": -0.4, "focal_mm": 600},
                        {"op": "edit_ruler", "page": 1, "id": "p", "points": ruler["points"][:2]},
                        {"op": "camera_from_ruler", "page": 1, "id": "p"}])
    back = persp3d.vanishing_points(episode.pages[0].prims[0], episode.pages[0].extra["camera"])
    assert math.dist(back["x"], ruler["points"][0]) < 1.0 and math.dist(back["z"], ruler["points"][1]) < 1.0
    apply_ops(episode, [{"op": "add_ruler", "page": 1, "kind": "line", "id": "l", "points": [[0, 0], [10, 10]]}])
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "camera_from_ruler", "page": 1, "id": "l"}])


def test_shape_rulers_and_a_selection_from_a_ruler_in_the_app(tmp_path):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    from genko.app.main import MainWindow

    window = MainWindow()
    try:
        page = window._current()
        window.apply_ops([{"op": "add_ruler", "page": page.index, "kind": "ellipse", "id": "e", "points": [[20, 20], [80, 60]]}])
        window.canvas.selected_ruler_id = "e"
        window._ruler_selection()
        sel = window.canvas.selection
        assert sel and len(sel["area"]["poly"]) > 20
        xs = [p[0] for p in sel["area"]["poly"]]
        assert min(xs) == pytest.approx(20, abs=0.1) and max(xs) == pytest.approx(80, abs=0.1)
        window.canvas.ruler_kind = "rect"
        assert window.canvas._draft_ruler([[10, 10], [40, 30]])["points"] == [[10, 10], [40, 30]]
        window.canvas.ruler_kind = "polygon"
        assert window.canvas._draft_ruler([[0, 0], [0, 0], [10, 0], [5, 8]])["points"] == [[0, 0], [10, 0], [5, 8]]
        app.processEvents()
    finally:
        window.close()
