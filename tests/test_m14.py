"""M14: rulers (straight, curve, parallel, concentric, radial, perspective, symmetry), the grid, and the
3D guides (a posable figure dragged by its joints, boxes)."""

import math
import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import mannequin, prim3d, rulers  # noqa: E402
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


def _draw(ep, points, **extra):
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": _ink(ep).id, "points": points, "stabilize": 0, "taper": False,
                    "kind": "mili", "snap_ruler": True, **extra}])
    return _ink(ep).strokes[-1]


def _wobbly(a, b, n=20, amp=1.5):
    return [[a[0] + (b[0] - a[0]) * i / (n - 1) + amp * math.sin(i), a[1] + (b[1] - a[1]) * i / (n - 1) + amp * math.cos(i * 1.3)]
            for i in range(n)]


def _off_line(points, a, b):
    """The largest distance of the points from the line through a and b."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = math.hypot(dx, dy)
    return max(abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / n for p in points)


def test_straight_ruler_takes_lines_drawn_near_it(tmp_path: Path):
    ep = _book()
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 160]], "id": "r1"}])
    near = _draw(ep, _wobbly((40, 108), (150, 145)))
    assert _off_line(near.points, (20, 100), (200, 160)) < 0.01
    far = _draw(ep, _wobbly((40, 250), (150, 260)))
    assert _off_line(far.points, (20, 100), (200, 160)) > 5  # too far away: drawn as it was
    # without snap_ruler the pen is free
    free = _draw(ep, _wobbly((40, 108), (150, 145)), snap_ruler=False)
    assert _off_line(free.points, (20, 100), (200, 160)) > 0.5
    # an inactive ruler lets go; the rulers travel with the book
    apply_ops(ep, [{"op": "edit_ruler", "page": 1, "id": "r1", "active": False}])
    assert _off_line(_draw(ep, _wobbly((40, 108), (150, 145))).points, (20, 100), (200, 160)) > 0.5
    project = tmp_path / "r.genko"
    save_episode(ep, project)
    again = load_episode(project).pages[0].rulers
    assert again[0]["id"] == "r1" and again[0]["active"] is False
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "line", "points": [[1, 1]]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "spiral"}])


def test_curve_parallel_radial_and_concentric():
    ep = _book()
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "curve", "points": [[30, 60], [90, 40], [150, 70], [200, 50]], "id": "c"}])
    curve = rulers.smooth_curve([[30, 60], [90, 40], [150, 70], [200, 50]])
    stroke = _draw(ep, _wobbly((50, 55), (140, 66)))
    assert max(rulers._nearest_on_polyline(p, curve)[0] for p in stroke.points) < 0.3
    apply_ops(ep, [{"op": "delete_ruler", "page": 1, "id": "c"},
                   {"op": "add_ruler", "page": 1, "kind": "parallel", "angle": 30}])
    stroke = _draw(ep, _wobbly((60, 200), (140, 250)))
    (x0, y0), (x1, y1) = stroke.points[0], stroke.points[-1]
    assert math.degrees(math.atan2(y1 - y0, x1 - x0)) == pytest.approx(30, abs=0.01)
    apply_ops(ep, [{"op": "delete_ruler", "page": 1},
                   {"op": "add_ruler", "page": 1, "kind": "radial", "points": [[120, 180]]}])
    stroke = _draw(ep, _wobbly((40, 100), (80, 150), amp=0.8))
    assert _off_line(stroke.points, stroke.points[0], (120, 180)) < 0.01
    apply_ops(ep, [{"op": "delete_ruler", "page": 1},
                   {"op": "add_ruler", "page": 1, "kind": "concentric", "points": [[120, 180]], "ratio": 0.5}])
    arc = [[120 + 40 * math.cos(a / 10) + 0.8 * math.sin(a), 180 + 20 * math.sin(a / 10)] for a in range(0, 25)]
    stroke = _draw(ep, arc)
    # every point is on the same ellipse (x/40)^2 + (y/20)^2 = k
    ks = [((x - 120) / 40) ** 2 + ((y - 180) / 20) ** 2 for x, y in stroke.points]
    assert max(ks) - min(ks) < 1e-3 and len(stroke.points) > 20


def test_perspective_picks_the_vanishing_point_or_upright():
    ep = _book()
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "perspective", "points": [[-100, 120], [350, 120]]}])
    toward_left = _draw(ep, _wobbly((120, 200), (60, 180), amp=0.6))
    assert _off_line(toward_left.points, toward_left.points[0], (-100, 120)) < 0.01
    toward_right = _draw(ep, _wobbly((120, 200), (180, 180), amp=0.6))
    assert _off_line(toward_right.points, toward_right.points[0], (350, 120)) < 0.01
    upright = _draw(ep, _wobbly((120, 200), (121, 260), amp=0.6))
    xs = [p[0] for p in upright.points]
    assert max(xs) - min(xs) < 0.01
    assert rulers.horizon({"kind": "perspective", "points": [[-100, 120], [350, 120]]})[1] == pytest.approx((1.0, 0.0))
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "perspective", "points": [[0, 0]] * 4}])


def test_symmetry_draws_the_line_again_and_panel_rulers_stay_in_their_panel():
    ep = _book()
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "symmetry", "points": [[120, 0], [120, 300]]}])
    count = len(_ink(ep).strokes)
    _draw(ep, [[80, 100], [100, 120], [110, 150]])
    strokes = _ink(ep).strokes
    assert len(strokes) == count + 2
    assert strokes[-1].points[0] == pytest.approx((160, 100))
    assert strokes[-1].kind == strokes[-2].kind and strokes[-1].id != strokes[-2].id
    apply_ops(ep, [{"op": "delete_ruler", "page": 1},
                   {"op": "add_ruler", "page": 1, "kind": "symmetry", "points": [[120, 150], [120, 100]], "copies": 6}])
    _draw(ep, [[150, 150], [160, 150]])
    assert len(_ink(ep).strokes) == count + 2 + 6
    # a ruler for one panel only
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "delete_ruler", "page": 1},
                   {"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 5}])
    top, bottom = sorted(ep.pages[0].leaf_frames(), key=lambda f: f.rect.y)
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "parallel", "angle": 0, "frame_id": top.id}])
    in_top = _draw(ep, _wobbly((60, top.rect.y + 20), (150, top.rect.y + 40)))
    assert in_top.points[0][1] == pytest.approx(in_top.points[-1][1])
    in_bottom = _draw(ep, _wobbly((60, bottom.rect.y + 20), (150, bottom.rect.y + 40)))
    assert abs(in_bottom.points[0][1] - in_bottom.points[-1][1]) > 5
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "parallel", "frame_id": "nope"}])


def test_grid_snap():
    assert rulers.snap_to_grid((12.4, 17.6), 5) == (10, 20)
    assert rulers.snap_to_grid((12.4, 17.6), 0) == (12.4, 17.6)


# --- 3D ------------------------------------------------------------------------------------------------


def test_a_figure_is_posed_by_dragging_its_joints():
    ep = _book()
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "id": "m", "pos": [120, 200, 0], "height_mm": 120}])
    prim = ep.pages[0].prims[0]
    for handle in mannequin.HANDLES:
        base_name = mannequin.HANDLES[handle][0]
        base = mannequin.skeleton(ep.pages[0].prims[0])["points"][base_name]
        target = (base[0] + 12, base[1] - 9)
        apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "drag": {"handle": handle, "to": list(target)}}])
        prim = ep.pages[0].prims[0]
        moved = mannequin.skeleton(prim)["points"]
        # the dragged part now points from its base toward the target
        got = math.atan2(moved[handle][1] - moved[base_name][1], moved[handle][0] - moved[base_name][0])
        want = math.atan2(target[1] - moved[base_name][1], target[0] - moved[base_name][0])
        assert math.cos(got - want) > 0.999, handle
    apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "drag": {"handle": "pelvis", "to": [60, 150]}}])
    assert ep.pages[0].prims[0]["pos"][:2] == [60, 150]
    # turned and tipped figures follow too
    apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "rot": [0.3, 2.5, 0.2]},
                   {"op": "pose_mannequin", "page": 1, "id": "m", "drag": {"handle": "r_hand", "to": [30, 100]}}])
    moved = mannequin.skeleton(ep.pages[0].prims[0])["points"]
    got = math.atan2(moved["r_hand"][1] - moved["r_elbow"][1], moved["r_hand"][0] - moved["r_elbow"][0])
    want = math.atan2(100 - moved["r_elbow"][1], 30 - moved["r_elbow"][0])
    assert math.cos(got - want) > 0.999
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "drag": {"handle": "tail", "to": [0, 0]}}])


def test_boxes_are_seen_in_perspective_and_traced_as_lines():
    ep = _book()
    apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": "box", "id": "b", "pos": [120, 180, 0], "size": [60, 40, 50],
                    "rot": [0.3, 0.6, 0]}])
    box = ep.pages[0].prims[0]
    edges = prim3d.edges(box)
    assert len(edges) == 12 and 6 <= sum(seen for *_, seen in edges) <= 9
    # the near face is drawn bigger than the far one (perspective)
    near = prim3d.project({**box, "rot": [0, 0, 0]})
    width_front = abs(near[4][0] - near[0][0])  # z = -d/2 (front) corners 0 and 4
    width_back = abs(near[5][0] - near[1][0])
    assert width_front > width_back
    apply_ops(ep, [{"op": "edit_prim", "page": 1, "id": "b", "pos": [100, 150, 0], "rot": [0, 0.3, 0]}])
    assert ep.pages[0].prims[0]["pos"] == [100, 150, 0]
    image = render_page(ep.pages[0], 100, mode="proof", episode=ep).convert("L")
    x, y, w, h = prim3d.bbox(ep.pages[0].prims[0])
    crop = image.crop((mm_to_px(x, 100), mm_to_px(y, 100), mm_to_px(x + w, 100) + 1, mm_to_px(y + h, 100) + 1))
    assert crop.getextrema()[0] < 200  # the box shows in the proof
    printed = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    assert printed.crop((mm_to_px(x, 100), mm_to_px(y, 100), mm_to_px(x + w, 100), mm_to_px(y + h, 100))).getextrema()[0] > 200
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "id": "m", "pos": [60, 200, 0]},
                   {"op": "trace_prims", "page": 1, "layer_id": _ink(ep).id}])
    traced = _ink(ep).strokes
    assert len(traced) >= 6 + 20 and all(s.kind == "pencil" for s in traced)
    apply_ops(ep, [{"op": "delete_prim", "page": 1, "id": "b"}])
    assert [p["id"] for p in ep.pages[0].prims] == ["m"]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "delete_prim", "page": 1, "id": "b"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": "sphere"}])


def test_agents_have_the_ruler_and_3d_tools():
    from genko.ops import OPS_SCHEMA
    from genko.studio.service import AGENT_OPS

    tools = {"add_ruler", "edit_ruler", "delete_ruler", "edit_prim", "delete_prim", "trace_prims"}
    assert tools <= AGENT_OPS and tools <= {entry["op"] for entry in OPS_SCHEMA}


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
    from PySide6.QtWidgets import QInputDialog, QMessageBox

    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    monkeypatch.setattr(QInputDialog, "getInt", lambda *a, **k: (6, True))
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(_book(), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    win.set_target_layer(_ink(win.episode).id)
    yield win
    win.close()


def _mouse(canvas, kind, at, modifiers=None):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton
    mods = modifiers or Qt.KeyboardModifier.NoModifier
    p = canvas._pt(*at)
    kinds = {"press": (QEvent.Type.MouseButtonPress, left, left), "move": (QEvent.Type.MouseMove, left, left),
             "release": (QEvent.Type.MouseButtonRelease, left, none), "double": (QEvent.Type.MouseButtonDblClick, left, left)}
    t, button, buttons = kinds[kind]
    event = QMouseEvent(t, p, canvas.mapToGlobal(p), button, buttons, mods)
    {"press": canvas.mousePressEvent, "move": canvas.mouseMoveEvent, "release": canvas.mouseReleaseEvent,
     "double": canvas.mouseDoubleClickEvent}[kind](event)


def _drag(canvas, path, modifiers=None):
    _mouse(canvas, "press", path[0], modifiers)
    for pt in path[1:]:
        _mouse(canvas, "move", pt, modifiers)
    _mouse(canvas, "release", path[-1], modifiers)


def _click(canvas, at):
    _mouse(canvas, "press", at)
    _mouse(canvas, "release", at)


def test_place_a_straight_ruler_and_draw_along_it(window):
    kinds = [kind for _, kind, *_ in window.ruler_kinds]
    window.ruler_actions[kinds.index("line")].trigger()
    assert window.canvas.tool == "ruler"
    _drag(window.canvas, [(30, 100), (80, 110), (200, 140)])
    ruler = window.current_page().rulers[0]
    assert ruler["kind"] == "line" and ruler["points"] == [[30, 100], [200, 140]]
    window.act_pen.trigger()
    _drag(window.canvas, _wobbly((50, 106), (150, 128)))
    stroke = _ink(window.episode).strokes[-1]
    assert _off_line(stroke.points, (30, 100), (200, 140)) < 0.05
    # snapping off: a free line
    window.act_snap.trigger()
    assert not window.canvas.snap_rulers
    _drag(window.canvas, _wobbly((50, 106), (150, 128)))
    assert _off_line(_ink(window.episode).strokes[-1].points, (30, 100), (200, 140)) > 0.5
    window.act_snap.trigger()
    # move the ruler's end by its handle
    window.act_ruler.trigger()
    _drag(window.canvas, [(200, 140), (200, 100)])
    assert window.current_page().rulers[0]["points"][1] == [200, 100]
    # delete it with Delete
    window.act_delete_area.trigger()
    assert not window.current_page().rulers


def test_perspective_curve_and_symmetry_from_the_menu(window):
    kinds = [(kind, opts) for _, kind, opts, _ in window.ruler_kinds]
    window.ruler_actions[kinds.index(("perspective", {"vps": 2}))].trigger()
    _click(window.canvas, (10, 120))
    assert not window.current_page().rulers  # waits for the second point
    _click(window.canvas, (240, 120))
    assert window.current_page().rulers[0]["points"] == [[10, 120], [240, 120]]
    window.act_clear_rulers.trigger()
    window.ruler_actions[kinds.index(("curve", {}))].trigger()
    for at in ((30, 60), (90, 40), (150, 70)):
        _click(window.canvas, at)
    _mouse(window.canvas, "double", (150, 70))
    curve = window.current_page().rulers[0]
    assert curve["kind"] == "curve" and len(curve["points"]) == 3
    window.act_clear_rulers.trigger()
    window.ruler_actions[kinds.index(("symmetry", {"ask": True}))].trigger()
    _drag(window.canvas, [(120, 150), (120, 100)])
    assert window.current_page().rulers[0]["copies"] == 6
    window.act_pen.trigger()
    before = len(_ink(window.episode).strokes)
    _drag(window.canvas, [(140, 150), (150, 150), (160, 152)])
    assert len(_ink(window.episode).strokes) == before + 6
    # the guides panel lists it; unchecking turns it off
    window.show_dock("定規・3D")
    window.guides.refresh()
    from PySide6.QtCore import Qt

    item = window.guides.rulers.item(0)
    assert "対称定規" in item.text()
    item.setCheckState(Qt.CheckState.Unchecked)
    assert window.current_page().rulers[0]["active"] is False


def test_grid_and_shift_straight_lines(window):
    from PySide6.QtCore import Qt

    window.act_grid.trigger()
    window.act_grid_snap.trigger()
    assert window.canvas.grid_visible and window.canvas.grid_snap
    window.act_pen.trigger()
    _drag(window.canvas, [(51, 49), (70, 60), (99, 81)], Qt.KeyboardModifier.ShiftModifier)
    stroke = _ink(window.episode).strokes[-1]
    assert stroke.points[0] == pytest.approx((50, 50)) and stroke.points[-1] == pytest.approx((100, 80))
    window.act_grid.trigger()
    window.act_grid_snap.trigger()


def test_pose_a_figure_and_turn_a_box_on_the_canvas(window):
    window.act_add_figure.trigger()
    page = window.current_page()
    figure = page.prims[0]
    assert window.canvas.tool == "3d" and window.canvas.selected_prim_id == figure["id"]
    points = mannequin.skeleton(figure)["points"]
    hand = points["r_hand"]
    target = (hand[0] - 25, hand[1] - 30)
    _drag(window.canvas, [hand, ((hand[0] + target[0]) / 2, (hand[1] + target[1]) / 2), target])
    moved = mannequin.skeleton(window.current_page().prims[0])["points"]
    got = math.atan2(moved["r_hand"][1] - moved["r_elbow"][1], moved["r_hand"][0] - moved["r_elbow"][0])
    want = math.atan2(target[1] - moved["r_elbow"][1], target[0] - moved["r_elbow"][0])
    assert math.cos(got - want) > 0.99
    # a preset from the menu
    from genko.app.guide_panel import PRESETS

    window.pose_actions[list(PRESETS).index("run")].trigger()
    assert window.current_page().prims[0].get("preset") == "run"
    # a box: move it, turn it with its round handle
    window.act_add_box.trigger()
    box = window.current_page().prims[-1]
    x, y, w, h = prim3d.bbox(box)
    centre = (box["pos"][0], box["pos"][1])
    _drag(window.canvas, [centre, (centre[0] + 10, centre[1] + 5), (centre[0] + 20, centre[1] + 10)])
    box = window.current_page().prims[-1]
    assert box["pos"][:2] == pytest.approx([centre[0] + 20, centre[1] + 10])
    x, y, w, h = prim3d.bbox(box)
    turn_handle = (x + w / 2, y - 8)
    before = list(box["rot"])
    _drag(window.canvas, [turn_handle, (turn_handle[0] + 20, turn_handle[1])])
    assert window.current_page().prims[-1]["rot"][1] == pytest.approx(before[1] + 0.5, abs=0.01)
    # trace everything as pencil lines on the ink layer
    count = len(_ink(window.episode).strokes)
    window.act_trace.trigger()
    assert len(_ink(window.episode).strokes) > count + 20
    # Delete removes the selected one
    window.act_delete_area.trigger()
    assert len(window.current_page().prims) == 1
