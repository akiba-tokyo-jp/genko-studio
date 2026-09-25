"""M12: panels — slanted and free-form panels, cuts at any angle, gutters that move, borders, templates."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import frames as geo  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    return new_episode("t", 1, 1, PageSpec.b4_comic())


def _grey(ep, x, y, dpi=100):
    return render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("L").getpixel((mm_to_px(x, dpi), mm_to_px(y, dpi)))


def test_axis_splits_stay_rectangles_and_remember_their_cut():
    ep = _book()
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.4, "gutter_mm": 6}])
    root = ep.pages[0].frames[0]
    a, b = root.children
    assert a.poly is None and b.poly is None
    assert abs(b.rect.y - (a.rect.y + a.rect.height) - 6) < 1e-6
    assert root.split["gutter_mm"] == 6 and root.split_axis == "horizontal"


def test_a_slanted_split_and_a_cut_at_any_angle():
    ep = _book()
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "vertical", "ratio": 0.5, "gutter_mm": 3, "tilt_mm": 30}])
    left, right = ep.pages[0].frames[0].children
    assert left.poly and right.poly and left.rect.x < right.rect.x
    r = ep.pages[0].inner_rect_mm()
    apply_ops(ep, [{"op": "cut_frame", "page": 1, "p0": [right.rect.x, r.y + 60], "p1": [r.x + r.width, r.y + 120], "gutter_mm": 5}])
    leaves = ep.pages[0].leaf_frames()
    assert len(leaves) == 3
    # reading order: right column top first, then its lower part, then the left column
    assert leaves[0].rect.x > leaves[2].rect.x and leaves[0].rect.y < leaves[1].rect.y
    # the gutter is white and the panels have borders along the slant
    top_right = leaves[0]
    cx, cy = geo.centroid(geo.shape(top_right))
    assert geo.contains(top_right, cx, cy) and ep.pages[0].frame_at(cx, cy).id == top_right.id
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "cut_frame", "page": 1, "frame_id": top_right.id, "p0": [0, 0], "p1": [0.5, 0.2]}])


def test_ink_and_placed_art_are_clipped_to_a_slanted_panel():
    ep = _book()
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "name_ok", "page": 1},
                   {"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "vertical", "ratio": 0.5, "gutter_mm": 8, "tilt_mm": 60}])
    left, right = ep.pages[0].frames[0].children
    # a thick line across the whole page, at mid height
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[5, 180], [250, 180]], "width_mm": 3}])
    # the cut's middle at mid height: the gutter there is white, the panels on either side are inked
    (p0, p1, g) = geo.cut_line(ep.pages[0].frames[0])
    mx = (p0[0] + p1[0]) / 2
    assert _grey(ep, mx, 180) > 200
    assert _grey(ep, mx - 20, 180) < 80 and _grey(ep, mx + 20, 180) < 80
    # a layer that runs out of the panels keeps its line in the gutter
    ink = next(layer for layer in ep.pages[0].layers if layer.role.value == "ink")
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": ink.id, "panel_clip": False}])
    assert _grey(ep, mx, 180) < 80


def test_moving_gutters_keeps_their_width_and_the_cuts_inside():
    ep = _book()
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.4, "gutter_mm": 6}])
    top, bottom = ep.pages[0].frames[0].children
    b = bottom.rect
    apply_ops(ep, [{"op": "cut_frame", "page": 1, "frame_id": bottom.id, "p0": [b.x, b.y + 40], "p1": [b.x + b.width, b.y + 70]}])
    before = ep.pages[0]._find(bottom.id)
    lower_before = geo.shape(before.children[1])
    top_height = ep.pages[0].frames[0].children[0].rect.height
    apply_ops(ep, [{"op": "move_gutter", "page": 1, "frame_id": root.id, "delta_mm": 20}])
    top, bottom = ep.pages[0].frames[0].children
    assert abs(bottom.rect.y - (top.rect.y + top.rect.height) - 6) < 1e-3  # the gutter kept its width
    assert abs(top.rect.height - (top_height + 20)) < 0.01
    lower_after = geo.shape(bottom.children[1])
    assert lower_after != lower_before  # the slanted cut moved with its tier
    assert all(bottom.rect.y - 1e-3 <= y for _, y in lower_after)
    # a new gutter width
    apply_ops(ep, [{"op": "move_gutter", "page": 1, "frame_id": root.id, "delta_mm": 0, "gutter_mm": 10}])
    top, bottom = ep.pages[0].frames[0].children
    assert abs(bottom.rect.y - (top.rect.y + top.rect.height) - 10) < 1e-3
    with pytest.raises(ApplyError, match="too small"):
        apply_ops(ep, [{"op": "move_gutter", "page": 1, "frame_id": root.id, "delta_mm": 400}])


def test_stack_gutters_from_agent_layouts_move_too():
    from genko.studio.layout import apply_layout

    ep = _book()
    apply_layout(ep, 1, {"template": "4tier_classic", "panels": [{"slot": s} for s in ("p1", "p2", "p3", "p4", "p5", "p6")]})
    gutters = geo.gutters(ep.pages[0].frames[0])
    assert len(gutters) >= 5
    first = next(g for g in gutters if g["horizontal"])
    node = ep.pages[0]._find(first["node"])
    kids = sorted(node.children, key=lambda f: f.rect.y)
    height = kids[0].rect.height
    apply_ops(ep, [{"op": "move_gutter", "page": 1, "frame_id": first["node"], "index": first["index"], "delta_mm": 10}])
    node = ep.pages[0]._find(first["node"])
    assert abs(sorted(node.children, key=lambda f: f.rect.y)[0].rect.height - height - 10) < 0.1


def test_free_form_panels_border_and_bleed(tmp_path: Path):
    ep = _book()
    frame = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": frame.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    top = ep.pages[0].frames[0].children[0]
    r = top.rect
    shape = [[r.x, r.y], [r.x + r.width, r.y], [r.x + r.width - 40, r.y + r.height], [r.x, r.y + r.height]]
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top.id, "poly": shape, "border_mm": 1.5}])
    top = ep.pages[0]._find(top.id)
    assert top.custom and len(top.poly) == 4 and top.border_mm == 1.5
    assert _grey(ep, r.x + r.width - 5, r.y + r.height - 5) > 200  # the cut-away corner is paper
    save_episode(ep, tmp_path / "a.genko")
    again = load_episode(tmp_path / "a.genko").pages[0]._find(top.id)
    assert again.custom and again.poly == top.poly
    # back to the cut's shape
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top.id, "poly": None}])
    assert ep.pages[0]._find(top.id).poly is None
    # no border
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top.id, "border_mm": 0}])
    assert _grey(ep, r.x + r.width / 2, r.y + 0.2, 300) > 200
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top.id, "poly": [[0, 0], [1, 1]]}])


def test_people_reshape_approved_pages_agents_do_not(tmp_path: Path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    episode = load_episode(project)
    node = next(f for f in [episode.pages[0].frames[0]] if f.children)
    with pytest.raises(ApplyError, match="approved"):
        apply_ops(episode, [{"op": "move_gutter", "page": 1, "frame_id": node.id, "delta_mm": 5}], agent="ai:x")
    apply_ops(episode, [{"op": "move_gutter", "page": 1, "frame_id": node.id, "delta_mm": 5}], agent="human:leaf")


# --- the app ------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("コマ", 1, 2, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def _drag(canvas, a, b, steps=6):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton

    def ev(kind, p, button, buttons):
        return QMouseEvent(kind, p, canvas.mapToGlobal(p), button, buttons, Qt.KeyboardModifier.NoModifier)

    p = canvas._pt(*a)
    canvas.mousePressEvent(ev(QEvent.Type.MouseButtonPress, p, left, left))
    for i in range(1, steps + 1):
        q = canvas._pt(a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps)
        canvas.mouseMoveEvent(ev(QEvent.Type.MouseMove, q, left, left))
    canvas.mouseReleaseEvent(ev(QEvent.Type.MouseButtonRelease, q, left, none))


def test_panel_tool_cuts_moves_gutters_and_reshapes(window):
    window.act_frame.trigger()
    canvas = window.canvas
    r = window.current_page().inner_rect_mm()
    _drag(canvas, (r.x - 3, r.y + 120), (r.x + r.width + 3, r.y + 122))  # from the margin, nearly level
    leaves = window.current_page().leaf_frames()
    assert len(leaves) == 2 and all(f.poly is None for f in leaves)  # snapped level
    top, bottom = sorted(leaves, key=lambda f: f.rect.y)
    assert abs(bottom.rect.y - (top.rect.y + top.rect.height) - window.gutter_mm("horizontal")) < 0.01
    _drag(canvas, (r.x + 110, r.y + 5), (r.x + 150, r.y + 110))  # a slanted cut in the top tier
    assert len(window.current_page().leaf_frames()) == 3
    gutter = next(g for g in canvas._gutters() if g["horizontal"])
    mid = ((gutter["p0"][0] + gutter["p1"][0]) / 2, (gutter["p0"][1] + gutter["p1"][1]) / 2)
    _drag(canvas, mid, (mid[0], mid[1] + 20))
    bottom = max(window.current_page().leaf_frames(), key=lambda f: f.rect.y)
    assert abs(bottom.rect.y - (r.y + 120 - window.gutter_mm("horizontal") / 2 + window.gutter_mm("horizontal") + 20)) < 1.5
    window._on_frame_selected(bottom.id)
    corner = (bottom.rect.x + bottom.rect.width, bottom.rect.y + bottom.rect.height)
    _drag(canvas, corner, (corner[0] - 30, corner[1] - 20))
    shaped = window.current_page()._find(bottom.id)
    assert shaped.custom and shaped.poly
    window.act_reset_shape.trigger()
    assert window.current_page()._find(bottom.id).poly is None


def test_templates_and_border_from_the_menu(window, monkeypatch):
    from PySide6.QtWidgets import QInputDialog

    from genko.app.dialogs import TemplateDialog

    dialog = TemplateDialog(window, window.episode, window.current_page())
    assert dialog.list.count() >= 6
    dialog.list.setCurrentRow(4)
    dialog.choose()
    window.apply_ops(dialog.ops)
    leaves = window.current_page().leaf_frames()
    assert len(leaves) >= 5
    window._on_frame_selected(leaves[0].id)
    monkeypatch.setattr(QInputDialog, "getDouble", staticmethod(lambda *a, **k: (1.2, True)))
    window.act_border.trigger()
    assert window.current_page()._find(leaves[0].id).border_mm == 1.2
    window.act_bleed.trigger()
    assert window.current_page()._find(leaves[0].id).bleed
