"""F5: the materials of manga lettering — 傍点, turned and slanted balloons and sound effects, balloons
drawn by hand, and Latin letters laid on their side in vertical text."""

import math
import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import balloons, fonts, tategaki  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def test_marks_are_typed_like_novel_sites_and_read_back():
    from genko.app.lettering import parse_marks, with_marks

    text, runs, marks, _styles = parse_marks("《《絶対》》に｜約束《やくそく》する")
    assert (text, runs, marks) == ("絶対に約束する", [["約束", "やくそく"]], ["絶対"])
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": text, "ruby_runs": runs, "emphasis_runs": marks, "id": "a"}])
    line = ep.story[0]
    assert with_marks(line) == "《《絶対》》に｜約束《やくそく》する"
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"emphasis_mark": "star"}}])


def _ink_columns(image, x0, x1):
    """Rows (px) with ink between x0 and x1."""
    alpha = image.split()[3]
    rows = set()
    for y in range(image.height):
        if any(alpha.getpixel((x, y)) > 100 for x in range(max(0, x0), min(image.width, x1))):
            rows.add(y)
    return rows


def test_emphasis_dots_sit_beside_their_characters_and_ruby_moves_out():
    em = 40
    face = fonts.face(None, fonts.DEFAULT_DIALOGUE)
    text = "今日は絶対に行く"
    plain = tategaki.compose(text, face.font(em), em, 2000, face=face)
    marked = tategaki.compose(text, face.font(em), em, 2000, face=face, emphasis_runs=["絶対"])
    mark_w = round(em * 0.36)
    assert marked.width == plain.width + mark_w
    # dots only beside 絶 and 対 (rows 3 and 4)
    rows = _ink_columns(marked, em, em + mark_w)
    assert rows and all(3 * em <= y < 5 * em for y in rows)
    assert any(3 * em <= y < 4 * em for y in rows) and any(4 * em <= y < 5 * em for y in rows)
    # with ruby on the same word: dots next to the characters, ruby outside the dots
    both = tategaki.compose(text, face.font(em), em, 2000, face=face, emphasis_runs=["絶対"], ruby_runs=[["絶対", "ぜったい"]])
    ruby_w = max(4, em // 2)
    assert both.width == em + mark_w + ruby_w
    assert _ink_columns(both, em, em + mark_w) and _ink_columns(both, em + mark_w, em + mark_w + ruby_w)
    # emphasis found by characters across columns
    cols = tategaki.columns_of(text, 4)
    assert tategaki.emphasis_cells(cols, ["絶対"]) == {(0, 3), (1, 0)}


def test_emphasis_dots_above_horizontal_text():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "とても大事な話", "wrap": "horizontal", "balloon": "none",
                    "x_mm": 30, "y_mm": 30, "w_mm": 80, "h_mm": 12, "style": {"size_mm": 6}}])
    line = ep.story[0]
    plain, _ = balloons.text_image(line, 300)
    line.emphasis_runs = ["大事"]
    marked, _ = balloons.text_image(line, 300)
    band = round(balloons.px(6, 300) * 0.36)
    assert marked.height == plain.height + band
    dots = [x for x in range(marked.width) for y in range(band) if marked.getpixel((x, y))[3] > 100]
    third = marked.width / 7  # 7 characters of about equal width
    assert dots and min(dots) > 2 * third and max(dots) < 5 * third + 2


def _dark(image, box):
    x0, y0, x1, y1 = box
    grey = image.convert("L")
    return [(x, y) for y in range(y0, y1) for x in range(x0, x1) if grey.getpixel((x, y)) < 110]


def test_a_turned_balloon_keeps_its_words_inside():
    dpi = 100
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "b", "text": "回っても中に収まる台詞です", "wrap": "vertical", "balloon": "speech",
                    "x_mm": 100, "y_mm": 150, "w_mm": 50, "h_mm": 30, "style": {"rotate_deg": 35}}])
    image = render_page(ep.pages[0], dpi, episode=ep)
    cx, cy = balloons.px(125, dpi), balloons.px(165, dpi)
    rx, ry = balloons.px(25, dpi), balloons.px(15, dpi)
    reach = int(math.hypot(rx, ry)) + 12
    ink = _dark(image, (cx - reach, cy - reach, cx + reach, cy + reach))
    assert len(ink) > 200
    a = math.radians(-35)
    for x, y in ink:
        dx, dy = x - cx, y - cy
        ux, uy = dx * math.cos(a) - dy * math.sin(a), dx * math.sin(a) + dy * math.cos(a)  # back to upright
        assert (ux / rx) ** 2 + (uy / ry) ** 2 <= 1.12, (x, y)
    # it really turned: an upright balloon's ink differs
    apply_ops(ep, [{"op": "edit_line", "id": "b", "style": {"rotate_deg": None}}])
    upright = render_page(ep.pages[0], dpi, episode=ep)
    assert set(_dark(upright, (cx - reach, cy - reach, cx + reach, cy + reach))) != set(ink)


def test_slanted_and_bowed_sound_effects():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "s", "text": "ドドド", "wrap": "horizontal", "balloon": "sfx",
                    "x_mm": 30, "y_mm": 30, "w_mm": 80, "h_mm": 25}])
    line = ep.story[0]
    flat, _ = balloons.text_image(line, 150)
    apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"skew_deg": 20}}])
    slanted, _ = balloons.text_image(ep.story[0], 150)
    assert slanted.width > flat.width and slanted.height == flat.height
    apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"skew_deg": None, "arc": 0.8}}])
    bowed, _ = balloons.text_image(ep.story[0], 150)
    assert bowed.height > flat.height
    # the middle rises above the ends
    alpha = bowed.split()[3]

    def top(x0, x1):
        return min(y for y in range(bowed.height) for x in range(x0, x1) if alpha.getpixel((x, y)) > 100)

    w = bowed.width
    assert top(w * 2 // 5, w * 3 // 5) < min(top(0, w // 5), top(w * 4 // 5, w)) - 3
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"arc": 3}}])


def test_a_balloon_drawn_by_hand_is_kept_and_drawn(tmp_path: Path):
    outline = [[40, 120], [75, 115], [85, 140], [70, 170], [45, 165], [35, 140]]
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "h", "text": "手描き", "wrap": "vertical", "path": outline}])
    line = ep.story[0]
    assert (line.x_mm, line.y_mm, line.w_mm, line.h_mm) == (35, 115, 50, 55)
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko")
    assert [list(p) for p in again.story[0].path] == outline
    image = render_page(again.pages[0], 150, episode=again)
    grey = image.convert("L")
    # the outline runs along the drawn edges, not the box's ellipse
    for (ax, ay), (bx, by) in zip(outline, outline[1:] + outline[:1]):
        mx, my = balloons.px((ax + bx) / 2, 150), balloons.px((ay + by) / 2, 150)
        assert min(grey.getpixel((mx + dx, my + dy)) for dx in range(-3, 4) for dy in range(-3, 4)) < 120
    # the drawn shape can go back to the ordinary one
    apply_ops(again, [{"op": "set_balloon_path", "id": "h", "path": None}])
    assert again.story[0].path is None
    with pytest.raises(ApplyError):
        apply_ops(again, [{"op": "set_balloon_path", "id": "h", "path": [[0, 0], [1, 1]]}])


def test_latin_words_lie_on_their_side_in_vertical_text():
    assert tategaki.ROT + "Genko" in tategaki.cells("これはGenkoです", latin=True)
    assert "OK" in tategaki.cells("OKです", latin=True)  # 2-3 letters stay 縦中横
    assert tategaki.ROT + "Mr. Smith" in tategaki.cells("Mr. Smithさん", latin=True)
    assert not any(c.startswith(tategaki.ROT) for c in tategaki.cells("これはGenkoです", latin=False))
    em = 40
    face = fonts.face(None, fonts.DEFAULT_DIALOGUE)
    lying = tategaki.compose("ABCDEFGH", face.font(em), em, 4000, face=face, latin=True)
    upright = tategaki.compose("ABCDEFGH", face.font(em), em, 4000, face=face, latin=False)
    assert lying.width <= em and lying.height < upright.height * 0.8
    # the letters lie across: the ink is taller than it is wide
    box = lying.getbbox()
    assert box[3] - box[1] > 2 * (box[2] - box[0])
    # a line can keep them upright
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "l", "text": "ABCDEFGH", "wrap": "vertical", "balloon": "none",
                    "x_mm": 30, "y_mm": 30, "w_mm": 10, "h_mm": 120, "style": {"size_mm": 6}}])
    side, _ = balloons.text_image(ep.story[0], 150)
    apply_ops(ep, [{"op": "edit_line", "id": "l", "style": {"latin": "upright"}}])
    stood, _ = balloons.text_image(ep.story[0], 150)
    assert stood.height > side.height


# --- the window --------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def test_the_balloon_pen_and_the_turn_handle(window):
    from test_m13 import _drag

    canvas = window.canvas
    window.act_text.trigger()
    window.text_settings.draw_balloon.setChecked(True)
    assert canvas.balloon_pen
    ring = [(100 + 25 * math.cos(t / 10 * math.tau), 150 + 18 * math.sin(t / 10 * math.tau)) for t in range(11)]
    _drag(canvas, None, None, path=ring)
    assert canvas.editor is not None
    canvas.editor.setPlainText("《《描いた》》形")
    canvas.editor.finish(True)
    line = window.episode.story_for_page(1)[-1]
    assert line.path and line.text == "描いた形" and line.emphasis_runs == ["描いた"]
    assert abs(line.x_mm - 75) < 1 and abs(line.w_mm - 50) < 1
    # turn it with the handle above it
    window.text_settings.draw_balloon.setChecked(False)
    window.act_select.trigger()
    canvas.selected_line_id = line.id
    cx, top = line.x_mm + line.w_mm / 2, line.y_mm - 7
    right = (line.x_mm + line.w_mm + 20, line.y_mm + line.h_mm / 2)
    _drag(canvas, None, None, path=[(cx, top), (cx + 10, top + 2), right])
    turned = window._line(line.id)
    assert abs(turned.style.get("rotate_deg", 0) - 90) < 3
    # the lines panel shows and sets it
    window.story.refresh()
    window.story.select(line.id)
    assert abs(window.story.rotate.value() - turned.style["rotate_deg"]) < 0.6
    window.story.skew.setValue(10)
    window.story._style_changed()
    assert window._line(line.id).style.get("skew_deg") == 10
