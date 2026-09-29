"""The hand-drawing report (HD): bleed panels with round corners or a styled border, the tool chosen while a key
holds another, number fields and the wheel, the effect list per page, the tone's shift kept without Enter, the gaps
traced, 3D lines joined, pages taken in before the covers, grey fills on a monochrome page, dots on screen, the
chosen characters styled, and the page list keeping its pictures when a cover is added."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def _book(pages=2):
    return new_episode("t", 1, pages, PageSpec.b5_doujin())


def _window(tmp_path: Path, episode=None):
    from genko.app.main import MainWindow

    project = tmp_path / "hd.genko"
    save_episode(episode or _book(), project)
    return MainWindow(project)


def test_a_round_or_dashed_bleed_panel_runs_off_the_paper():
    from genko.placement import bleed_outline
    from genko.render import render_page

    ep = _book(1)
    apply_ops(ep, [{"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 5}])
    apply_ops(ep, [{"op": "split_frame", "page": 1, "axis": "vertical", "ratio": 0.5, "gutter_mm": 5,
                    "frame_id": ep.pages[0].leaf_frames()[0].id}])
    page = ep.pages[0]
    frame = page.leaf_frames()[0]  # (a quarter: two sides on the live edge, its inner corner rounded)
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": frame.id, "bleed": True, "corner_mm": 5,
                    "line": {"kind": "dashed"}}])
    page = ep.pages[0]  # (the book after the change)
    frame = page._find(frame.id)
    points = bleed_outline(page, frame)
    width, height = page.spec.paper_size() if hasattr(page.spec, "paper_size") else (page.spec.width_mm, page.spec.height_mm)
    assert any(x < 0 or y < 0 or x > width or y > height for x, y in points)  # (the live edge's corners off the paper)
    assert len(points) > 4  # (the inner corners still round)
    assert render_page(page, 60, mode="print", episode=ep).size[0] > 0


def test_a_tool_chosen_while_a_key_holds_another_stays(qapp):
    from genko.app.canvas import make_canvas

    canvas = make_canvas()
    canvas.tool = "eraser"
    canvas._held_tool = "pen"  # (Shift held: the eraser for now, the pen when it is let go)
    canvas.set_tool("vector")
    assert canvas.tool == "vector" and canvas._held_tool is None


def test_number_fields_select_their_value_and_the_wheel_leaves_unused_fields_alone(qapp):
    from PySide6.QtCore import QPoint, QPointF, Qt
    from PySide6.QtGui import QWheelEvent
    from PySide6.QtWidgets import QComboBox

    from genko.app import inputs

    inputs.install()
    box = QComboBox()
    box.addItems(["a", "b", "c"])
    box.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
    wheel = QWheelEvent(QPointF(5, 5), QPointF(5, 5), QPoint(0, 0), QPoint(0, -120), Qt.MouseButton.NoButton,
                        Qt.KeyboardModifier.NoModifier, Qt.ScrollPhase.NoScrollPhase, False)
    qapp.sendEvent(box, wheel)
    assert box.currentIndex() == 0


def test_the_effect_list_follows_the_page_and_the_tone_shift_is_kept_without_enter(qapp, tmp_path):
    ep = _book(2)
    apply_ops(ep, [{"op": "add_effect", "page": 2, "kind": "speed", "params": {"angle": 0, "count": 10}},
                   {"op": "add_tone", "page": 1, "frame_id": ep.pages[0].leaf_frames()[0].id, "density": 0.3}])
    window = _window(tmp_path, ep)
    try:
        materials = window.materials
        window._select_page(1)
        qapp.processEvents()
        materials.refresh()
        assert materials.effects.count() == 1
        window._select_page(0)
        materials.refresh()
        assert materials.effects.count() == 0
        tone = next(layer for layer in window._current().layers if getattr(layer.kind, "value", "") == "tone")
        window.set_target_layer(tone.id)
        materials.off_y.setValue(0.3)  # (typed, no Enter)
        assert materials._offset_timer.isActive()
        materials._offset_timer.timeout.emit()  # (the moment after typing)
        from genko.tones import settings

        now = next(layer for layer in window._current().layers if layer.id == tone.id)
        assert [round(float(v), 2) for v in settings(now)["offset_mm"]] == [0.0, 0.3]
    finally:
        window.close()


def test_fill_the_gaps_without_a_range_is_the_tracing_tool(qapp, tmp_path):
    window = _window(tmp_path)
    try:
        window.canvas.set_selection(None)
        window._fill_gaps()
        assert window.canvas.tool == "lassofill" and window.brush.lasso_mode.currentData() == "gaps"
    finally:
        window.close()


def test_the_lines_of_a_3d_box_are_joined():
    from genko.prim3d import join_lines

    assert join_lines([[(0, 0), (1, 0)], [(1, 0), (1, 1)], [(0, 1), (1, 1)], [(5, 5), (6, 6)]]) == \
        [[(0, 0), (1, 0), (1, 1), (0, 1)], [(5, 5), (6, 6)]]
    ep = _book(1)
    apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": "box", "id": "b1", "pos": [90, 120, 0], "size": 60}])
    ink_id = next(layer.id for layer in ep.pages[0].layers if hasattr(layer, "strokes") and layer.role.value == "ink")
    before = len(next(layer for layer in ep.pages[0].layers if layer.id == ink_id).strokes)
    apply_ops(ep, [{"op": "trace_prims", "page": 1, "layer_id": ink_id}])
    ink = next(layer for layer in ep.pages[0].layers if layer.id == ink_id)
    assert 1 <= len(ink.strokes) - before <= 9  # (a box: a few joined outlines, not twelve loose edges and more)


def test_pages_taken_from_another_book_go_before_the_covers(tmp_path):
    other = _book(2)
    save_episode(other, tmp_path / "other.genko")
    ep = _book(2)
    apply_ops(ep, [{"op": "add_cover", "kind": "front"}, {"op": "add_cover", "kind": "back"}])
    apply_ops(ep, [{"op": "import_pages", "from": str(tmp_path / "other.genko")}])
    from genko import covers

    kinds = [(covers.cover_of(p) or {}).get("kind") for p in ep.pages]
    assert kinds == [None, None, None, None, "front", "back"]


def test_a_gradient_on_a_monochrome_page_shows_in_grey():
    from genko.render import render_page

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    assert ep.pages[0].spec.expression != "color"
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "gradient",
                    "gradient": {"from": [0, 0], "to": [100, 0], "rgb_from": [220, 30, 30], "rgb_to": [30, 30, 220]}}])
    image = render_page(ep.pages[0], 30, mode="proof", episode=ep).convert("RGB")
    r, g, b = image.getpixel((image.width // 4, image.height // 2))
    assert r == g == b


def test_tones_can_be_seen_as_dots_on_screen():
    import numpy as np

    from genko import render

    ep = _book(1)
    apply_ops(ep, [{"op": "add_tone", "page": 1, "frame_id": ep.pages[0].leaf_frames()[0].id, "density": 0.4, "lpi": 30}])
    flat = np.asarray(render.render_page(ep.pages[0], 150, mode="proof", episode=ep).convert("L"))
    try:
        render.SCREEN_DOTS = True
        dots = np.asarray(render.render_page(ep.pages[0], 150, mode="proof", episode=ep).convert("L"))
    finally:
        render.SCREEN_DOTS = False
    mid = lambda a: int(((a > 40) & (a < 215)).sum())  # noqa: E731  (the greys)
    assert mid(dots) < mid(flat) / 3


def test_the_chosen_characters_are_styled_without_typing_the_notation(qapp):
    from PySide6.QtGui import QTextCursor
    from PySide6.QtWidgets import QPlainTextEdit

    from genko.app import text_style
    from genko.app.lettering import parse_marks

    edit = QPlainTextEdit()
    edit.setPlainText("なんだと")
    cursor = edit.textCursor()
    cursor.setPosition(0)
    cursor.setPosition(2, QTextCursor.MoveMode.KeepAnchor)
    edit.setTextCursor(cursor)
    assert text_style.wrap(edit, "大")
    assert edit.toPlainText() == "{大|なん}だと"
    text, _runs, _marks, styles = parse_marks(edit.toPlainText())
    assert text == "なんだと" and styles == [["なん", {"scale": 1.4}]]
    assert not text_style.wrap(edit, "太")  # (nothing chosen now)


def test_adding_a_cover_keeps_the_other_pictures_in_the_page_list(qapp, tmp_path):
    window = _window(tmp_path, _book(3))
    try:
        window.pages.finish_pictures()
        kept = dict(window.pages._thumbs)
        window.apply_ops([{"op": "add_cover", "kind": "front"}])
        assert all(window.pages._thumbs.get(page_id) is icon for page_id, icon in kept.items())
    finally:
        window.close()
