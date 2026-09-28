"""M10: vector pen lines, the eraser, layers a person draws on, lines placed in panels, guides, image import."""

import os
import sys
from pathlib import Path

import pytest
from PIL import Image, ImageDraw

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.app import lettering  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerKind, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _dark_run(image, x_mm: float, y_from: float, y_to: float, dpi: int) -> int:
    """How many pixels are dark along a vertical cut at x (the line's thickness)."""
    grey = image.convert("L")
    x = mm_to_px(x_mm, dpi)
    return sum(1 for y in range(mm_to_px(y_from, dpi), mm_to_px(y_to, dpi)) if grey.getpixel((x, y)) < 128)


def _inked(width_mm: float = 1.0):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "name_ok", "page": 1},
                   {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 100], [140, 100]], "width_mm": width_mm}])
    return ep


# --- vector lines -------------------------------------------------------------------------------------


def test_pen_lines_stay_vectors_and_keep_their_width_at_any_resolution():
    ep = _inked(1.0)
    ink = ep.pages[0]._layer(LayerRole.INK)
    assert ink.raster_png is None and len(ink.strokes) == 1  # not baked
    for dpi in (150, 600):
        thick = _dark_run(render_page(ep.pages[0], dpi, mode="print", episode=ep), 90, 95, 105, dpi)
        assert abs(thick - mm_to_px(1.0, dpi)) <= 3, (dpi, thick)  # no pressure given: the full width
    # a thin pen is thin
    thin = _inked(0.2)
    assert _dark_run(render_page(thin.pages[0], 600, mode="print", episode=thin), 90, 95, 105, 600) <= mm_to_px(0.3, 600)


def test_colour_and_opacity_of_a_line():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "name_ok", "page": 1},
                   {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 100], [140, 100]], "width_mm": 2,
                    "rgb": [200, 30, 30]},
                   {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 150], [140, 150]], "width_mm": 2, "opacity": 0.5}])
    image = render_page(ep.pages[0], 100, mode="proof", episode=ep)
    red = image.getpixel((mm_to_px(90, 100), mm_to_px(100, 100)))
    half = image.getpixel((mm_to_px(90, 100), mm_to_px(150, 100)))
    assert red[0] > 150 and red[1] < 80
    assert 90 < half[0] < 170  # half black on white
    saved = ep.pages[0]._layer(LayerRole.INK).strokes
    assert saved[0].rgb == (200, 30, 30) and saved[1].opacity == 0.5


def test_layers_draw_in_their_order_and_hidden_ones_do_not():
    ep = _inked(3.0)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "name": "上", "id": "top"},
                   {"op": "add_stroke", "page": 1, "layer_id": "top", "points": [[90, 80], [90, 120]], "width_mm": 3,
                    "rgb": [255, 255, 255]}])
    crossing = render_page(ep.pages[0], 100, mode="print", episode=ep).getpixel((mm_to_px(90, 100), mm_to_px(100, 100)))
    assert crossing[0] > 200  # the white line on the layer in front covers the black one
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "top", "visible": False}])
    crossing = render_page(ep.pages[0], 100, mode="print", episode=ep).getpixel((mm_to_px(90, 100), mm_to_px(100, 100)))
    assert crossing[0] < 80


def test_panel_border_has_its_width():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    frame = ep.pages[0].leaf_frames()[0]
    image = render_page(ep.pages[0], 600, mode="print", episode=ep)
    x = frame.rect.x + frame.rect.width / 2
    band = _dark_run(image, x, frame.rect.y - 1, frame.rect.y + 3, 600)
    assert abs(band - mm_to_px(0.8, 600)) <= 2


# --- eraser and layers ------------------------------------------------------------------------------------


def test_eraser_cuts_lines_and_leaves_the_others_alone():
    ep = _inked(1.0)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 200], [140, 200]]}])
    ink = ep.pages[0]._layer(LayerRole.INK)
    untouched = ink.strokes[1].id
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[90, 90], [90, 110]], "width_mm": 4}])
    ink = ep.pages[0]._layer(LayerRole.INK)  # apply_ops swaps in a new copy of the book
    assert len(ink.strokes) == 3 and any(s.id == untouched for s in ink.strokes)
    image = render_page(ep.pages[0], 100, mode="print", episode=ep)
    assert image.getpixel((mm_to_px(90, 100), mm_to_px(100, 100)))[0] > 200
    assert image.getpixel((mm_to_px(60, 100), mm_to_px(100, 100)))[0] < 80
    # erasing a whole short line removes it
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[40, 200], [140, 200]], "width_mm": 4}])
    assert not any(s.id == untouched for s in ep.pages[0]._layer(LayerRole.INK).strokes)


def test_layers_a_person_adds_and_changes():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = ep.pages[0]._layer(LayerRole.INK)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "name": "人物", "id": "p1", "after": ink.id},
                   {"op": "add_layer", "page": 1, "kind": "paint", "name": "塗り", "id": "p2"},
                   {"op": "set_layer", "page": 1, "id": "p1", "name": "人物の線", "opacity": 0.5, "locked": True}])
    layers = ep.pages[0].layers
    order = [layer.id for layer in layers]
    assert order.index("p1") == order.index(ink.id) + 1
    p1 = next(layer for layer in layers if layer.id == "p1")
    assert p1.kind == LayerKind.STROKES and p1.title == "人物の線" and p1.opacity == 0.5 and p1.locked
    assert next(layer for layer in layers if layer.id == "p2").kind == LayerKind.RASTER
    with pytest.raises(ApplyError, match="locked"):
        apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "p1", "points": [[10, 10], [20, 20]]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "vector"}])


def test_locked_flag_is_saved(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "p1"}, {"op": "set_layer", "page": 1, "id": "p1", "locked": True}])
    save_episode(ep, tmp_path / "a.genko")
    assert next(layer for layer in load_episode(tmp_path / "a.genko").pages[0].layers if layer.id == "p1").locked


def test_studio_books_keep_printed_layers_until_the_name_is_approved(tmp_path: Path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    episode = load_episode(project)
    human._apply([{"op": "revoke", "gate": "name", "page": 1, "reason": "試す"}])
    episode = load_episode(project)
    apply_ops(episode, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "u1"}], agent="human:leaf")
    with pytest.raises(ApplyError, match="name_ok"):
        apply_ops(episode, [{"op": "add_stroke", "page": 1, "layer_id": "u1", "points": [[10, 10], [20, 20]]}], agent="human:leaf")


def test_a_filter_turns_pen_lines_into_pixels():
    ep = _inked(1.0)
    ink = ep.pages[0]._layer(LayerRole.INK)
    apply_ops(ep, [{"op": "filter_raster", "page": 1, "id": ink.id, "kind": "blur", "radius": 2}])
    ink = ep.pages[0]._layer(LayerRole.INK)
    assert not ink.strokes and ink.raster_png and ink.kind == LayerKind.RASTER


# --- lines placed by a person -------------------------------------------------------------------------------


def test_new_lines_go_inside_the_panel_vertical_and_right_to_left():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    page = ep.pages[0]
    frame = page.leaf_frames()[0]
    first = lettering.place_new(ep, page, frame, "5年前の今日、ここで約束したの")
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "5年前の今日、ここで約束したの", "frame_id": frame.id, **first}])
    second = lettering.place_new(ep, page, frame, "来るわけないよね", "thought")
    r = frame.rect
    for box in (first, second):
        assert box["wrap"] == "vertical"
        assert r.x <= box["x_mm"] and box["x_mm"] + box["w_mm"] <= r.x + r.width
        assert r.y <= box["y_mm"] and box["y_mm"] + box["h_mm"] <= r.y + r.height
    assert second["x_mm"] + second["w_mm"] <= first["x_mm"]  # the next line is to the left
    assert lettering.columns("5年前の今日、ここで約束したの", 7) == ["5年前の今", "日、ここで", "約束したの"]


def test_edit_line_switches_kind_and_direction():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "ドン", "x_mm": 50, "y_mm": 50, "w_mm": 12, "h_mm": 24, "wrap": "vertical"}])
    line = ep.story[0]
    apply_ops(ep, [{"op": "edit_line", "id": line.id, "balloon": "sfx", "wrap": "horizontal"}])
    line = ep.story[0]
    assert line.balloon == "sfx" and line.wrap == "horizontal"
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": line.id, "wrap": "diagonal"}])


def test_vertical_text_is_balanced_inside_its_balloon():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    frame = ep.pages[0].leaf_frames()[0]
    box = lettering.place_new(ep, ep.pages[0], frame, "5年前の今日、ここで約束したの")
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "5年前の今日、ここで約束したの", "frame_id": frame.id, **box}])
    image = render_page(ep.pages[0], 200, mode="print", episode=ep).convert("L")
    x0, y0 = mm_to_px(box["x_mm"], 200), mm_to_px(box["y_mm"], 200)
    x1, y1 = mm_to_px(box["x_mm"] + box["w_mm"], 200), mm_to_px(box["y_mm"] + box["h_mm"], 200)
    inside = image.crop((x0, y0, x1, y1))
    below = image.crop((x0, y1 + 2, x1, y1 + mm_to_px(15, 200)))
    assert min(inside.get_flattened_data()) < 50  # text and outline are in the box
    assert min(below.get_flattened_data()) > 200  # nothing hangs below it


# --- the app ---------------------------------------------------------------------------------------------------


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
    save_episode(new_episode("試し", 1, 3, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def _drag(canvas, points_mm):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton

    def ev(kind, p, button, buttons):
        return QMouseEvent(kind, p, canvas.mapToGlobal(p), button, buttons, Qt.KeyboardModifier.NoModifier)

    p = canvas._pt(*points_mm[0])
    canvas.mousePressEvent(ev(QEvent.Type.MouseButtonPress, p, left, left))
    for q in points_mm[1:]:
        p = canvas._pt(*q)
        canvas.mouseMoveEvent(ev(QEvent.Type.MouseMove, p, left, left))
    canvas.mouseReleaseEvent(ev(QEvent.Type.MouseButtonRelease, p, left, none))


def test_the_pen_draws_on_the_selected_layer_quickly(window):
    import time

    assert window.target_layer().role == LayerRole.INK  # a person alone starts on the ink, which prints
    window.show_dock("レイヤー")
    window.layers._add("pen", "ペン")
    user_id = window.target_layer().id
    assert window.target_layer().role == LayerRole.USER and "描く先" in window.layers.target.text()

    def user_strokes():
        return next(layer for layer in window.current_page().layers if layer.id == user_id).strokes

    window.act_pen.trigger()
    start = time.time()
    _drag(window.canvas, [(40 + i * 3, 80 + (i % 3) * 2) for i in range(20)])
    assert time.time() - start < 0.5
    assert len(user_strokes()) == 1 and not window.current_page().name_strokes
    # a tap is a dot
    _drag(window.canvas, [(60, 150)])
    assert len(user_strokes()) == 2
    # the eraser cuts the line on the same layer
    window.act_eraser.trigger()
    _drag(window.canvas, [(70, 70), (70, 75), (70, 80), (70, 85), (70, 90)])
    assert len(user_strokes()) == 3
    # a locked layer is not drawn on
    window.apply_ops([{"op": "set_layer", "page": 1, "id": user_id, "locked": True}])
    window.act_pen.trigger()
    _drag(window.canvas, [(40, 120), (60, 125), (80, 120)])
    assert len(user_strokes()) == 3 and "描けません" in window.last_notice


def test_lines_are_added_edited_and_deleted_in_the_panel(window, qapp):
    page = window.current_page()
    frame = page.leaf_frames()[0]
    window._on_frame_selected(frame.id)
    story = window.story
    story.speaker.setText("ひな")
    story.text.setPlainText("5年前の今日、ここで約束したの")
    story.add()
    line = window.episode.story_for_page(1)[0]
    assert line.frame_id == frame.id and line.wrap == "vertical"
    r = frame.rect
    assert r.x < line.x_mm < r.x + r.width and r.y < line.y_mm < r.y + r.height
    assert story.current_id() == line.id and window.canvas.selected_line_id == line.id
    story.kind.setCurrentIndex(story.kind.findData("shout"))
    story.text.setPlainText("来るわけ\nないよね")
    story.apply_edit()
    line = window.episode.story_for_page(1)[0]
    assert line.balloon == "shout" and line.text == "来るわけ\nないよね"
    story.delete()
    assert window.episode.story_for_page(1) == []


def test_image_goes_into_the_selected_panel(window, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QFileDialog

    picture = tmp_path / "pic.jpg"
    img = Image.new("RGB", (400, 300), "white")
    ImageDraw.Draw(img).ellipse((100, 50, 300, 250), outline="black", width=8)
    img.save(picture)
    monkeypatch.setattr(QFileDialog, "getOpenFileName", staticmethod(lambda *a, **k: (str(picture), "")))
    frame = window.current_page().leaf_frames()[0]
    window._on_frame_selected(frame.id)
    window._import_image()
    placed = [layer for layer in window.current_page().layers if layer.kind == LayerKind.PLACED]
    assert placed and placed[0].frame_id == frame.id and placed[0].title == "pic"
    window.commit_now()
    assert any(layer.kind == LayerKind.PLACED for layer in load_episode(window.path).pages[0].layers)


def test_guides_can_be_hidden(window):
    assert window.canvas.show_guides
    window.act_guides.trigger()
    assert not window.canvas.show_guides
