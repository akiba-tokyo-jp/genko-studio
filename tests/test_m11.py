"""M11: lettering — bundled faces (アンチック), vertical typesetting, balloon shapes, tails, joined balloons, the text tool."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import balloons, fonts  # noqa: E402
from genko.app import lettering  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402
from genko.tategaki import cells, columns_of, compose  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


# --- faces ---------------------------------------------------------------------------------------------


def test_bundled_faces_and_their_licences_ship():
    for key, (label, kana, other) in fonts.BUNDLED.items():
        assert kana.is_file() and other.is_file(), key
    assert len(list((fonts.DIR / "licenses").glob("*-OFL.txt"))) >= 6


def test_antique_sets_kana_in_mincho_and_kanji_in_gothic():
    face = fonts.face("antique")
    assert face.composite
    assert face.font(40, "あ").getname()[0] == "Zen Old Mincho"
    assert face.font(40, "漢").getname()[0] == "Zen Kaku Gothic New"
    assert face.font(40, "、").getname()[0] == "Zen Old Mincho"
    # a character the face lacks is swapped for its look-alike
    assert face.normalize("東京では――") == "東京では——"
    # a font file path works as a face; an unknown key falls back to the default
    assert fonts.face(str(fonts.MARU)).font(30, "あ").getname()[0] == "Zen Maru Gothic"
    assert fonts.face("nope").key == "antique"


# --- vertical typesetting --------------------------------------------------------------------------------


def test_tate_chu_yoko_for_short_numbers_and_bangs():
    assert cells("今日は12時に来るの!?") == ["今", "日", "は", "12", "時", "に", "来", "る", "の", "!?"]
    assert cells("１２３４５") == list("１２３４５")  # four or more digits stay upright one by one
    assert cells("第１２話！？") == ["第", "12", "話", "!?"]
    assert cells("12", tcy=False) == ["1", "2"]


def test_kinsoku_keeps_openers_off_the_end_and_closers_off_the_start():
    cols = columns_of("あいう「えお」。", 4)
    assert all(col[-1] != "「" for col in cols[:-1])
    assert all(col[0] not in "」。" for col in cols[1:])


def test_tracking_leading_and_ruby_change_the_block():
    font = fonts.face("antique").font(40)
    plain = compose("あいうえお\nかきく", font, 40, 400)
    spaced = compose("あいうえお\nかきく", font, 40, 400, tracking=0.25, leading=0.5)
    assert spaced.height > plain.height and spaced.width == plain.width + 20
    face = fonts.face("antique")
    mixed = compose("約束の日", face.font(40), 40, 400, face=face, ruby_runs=[["約束", "やくそく"]])
    assert mixed.width == 60  # a ruby band beside the column


# --- balloons ---------------------------------------------------------------------------------------------


def _page(*lines):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, **line} for line in lines])
    return ep


def _box(ep, dpi, x, y, w, h):
    return render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("L").crop(
        (mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi)))


@pytest.mark.parametrize("kind", [k for k in balloons.SHAPES if k != "none"])
def test_every_shape_draws_its_balloon_and_text(kind: str):
    ep = _page({"text": "来るの!?", "balloon": kind, "wrap": "vertical", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 44})
    box = _box(ep, 150, 55, 55, 40, 54)
    dark = sum(1 for v in box.get_flattened_data() if v < 100)
    assert dark > 150, kind
    # the text sits in the middle of its box
    mid = _box(ep, 150, 70, 70, 10, 24)
    assert min(mid.get_flattened_data()) < 80, kind


def test_border_width_and_no_fill():
    thin = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical", "style": {"border_mm": 0.2}})
    thick = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical", "style": {"border_mm": 1.2}})

    def left_edge(ep):
        row = _box(ep, 300, 58, 80, 6, 0.2).get_flattened_data()
        return sum(1 for v in row if v < 100)

    assert left_edge(thick) > left_edge(thin) * 3
    ep = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical", "style": {"fill": "none"}})
    assert ep.story[0].style == {"fill": "none"}


def test_joined_balloons_have_one_outline():
    lines = [{"text": "ねえ", "x_mm": 60, "y_mm": 60, "w_mm": 24, "h_mm": 30, "wrap": "vertical", "style": {"group": "g"}},
             {"text": "聞いてる？", "x_mm": 45, "y_mm": 78, "w_mm": 24, "h_mm": 36, "wrap": "vertical", "style": {"group": "g"}}]
    joined = _page(*lines)
    apart = _page(*[{**line, "style": {}} for line in lines])
    # where the two ellipses overlap, the joined version has no outline running through
    spot = (62, 84, 4, 4)
    assert min(_box(joined, 200, *spot).get_flattened_data()) > 200
    assert min(_box(apart, 200, *spot).get_flattened_data()) < 100


def test_tails_reach_their_tips_and_can_curve():
    straight = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical", "tails": [{"to": [75, 130]}]})
    curved = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical",
                    "tails": [{"to": [75, 130], "via": [100, 110]}]})
    assert min(_box(straight, 150, 73, 124, 4, 5).get_flattened_data()) < 100  # the tip is drawn
    # the curved tail bends out to the right: ink right of the straight one
    assert min(_box(curved, 150, 86, 105, 6, 8).get_flattened_data()) < 100
    assert min(_box(straight, 150, 86, 105, 6, 8).get_flattened_data()) > 200
    two = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "wrap": "vertical",
                 "tails": [{"to": [50, 120]}, {"to": [110, 120]}]})
    assert len(two.story[0].tails) == 2 and two.story[0].tail == (50.0, 120.0)


def test_style_ops_validate_merge_and_reset():
    ep = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40})
    line = ep.story[0]
    apply_ops(ep, [{"op": "edit_line", "id": line.id, "style": {"font": "mincho", "size_mm": 4, "rgb": [200, 0, 0]}}])
    apply_ops(ep, [{"op": "edit_line", "id": line.id, "style": {"size_mm": None, "outline_mm": 0.5}}])
    assert ep.story[0].style == {"font": "mincho", "rgb": [200, 0, 0], "outline_mm": 0.5}
    for bad in ({"colour": 1}, {"align": "diagonal"}, {"fill": "black"}):
        with pytest.raises(ApplyError):
            apply_ops(ep, [{"op": "edit_line", "id": line.id, "style": bad}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": line.id, "balloon": "heart"}])


def test_style_and_tails_are_saved(tmp_path: Path):
    ep = _page({"text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 40, "style": {"font": "hand"},
                "tails": [{"to": [50, 120], "via": [40, 100]}]})
    save_episode(ep, tmp_path / "a.genko")
    line = load_episode(tmp_path / "a.genko").story[0]
    assert line.style == {"font": "hand"} and line.tails == [{"to": [50.0, 120.0], "via": [40.0, 100.0]}]


def test_reorder_lines_changes_the_reading_order():
    ep = _page({"text": "一"}, {"text": "二"}, {"text": "三"})
    ids = [line.id for line in ep.story]
    apply_ops(ep, [{"op": "reorder_lines", "page": 1, "order": [ids[2], ids[0], ids[1]]}])
    assert [line.text for line in ep.story_for_page(1)] == ["三", "一", "二"]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "reorder_lines", "page": 1, "order": ids[:2]}])


def test_ruby_notation_round_trip():
    text, runs = lettering.parse_ruby("｜約束《やくそく》の日、東京《とうきょう》へ")
    assert text == "約束の日、東京へ" and runs == [["約束", "やくそく"], ["東京", "とうきょう"]]
    assert lettering.parse_ruby(lettering.with_ruby(text, runs)) == (text, runs)


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
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("写植", 1, 2, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def _press_release(canvas, x_mm, y_mm, to=None):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton

    def ev(kind, p, button, buttons):
        return QMouseEvent(kind, p, canvas.mapToGlobal(p), button, buttons, Qt.KeyboardModifier.NoModifier)

    p = canvas._pt(x_mm, y_mm)
    canvas.mousePressEvent(ev(QEvent.Type.MouseButtonPress, p, left, left))
    if to is not None:
        for i in range(1, 6):
            q = canvas._pt(x_mm + (to[0] - x_mm) * i / 5, y_mm + (to[1] - y_mm) * i / 5)
            canvas.mouseMoveEvent(ev(QEvent.Type.MouseMove, q, left, left))
        p = q
    canvas.mouseReleaseEvent(ev(QEvent.Type.MouseButtonRelease, p, left, none))


def test_text_tool_types_a_line_where_clicked(window):
    window.act_text.trigger()
    _press_release(window.canvas, 150, 100)
    editor = window.canvas.editor
    assert editor is not None and editor.isVisible()
    editor.setPlainText("約束《やくそく》したの!?")
    editor.finish(True)
    line = window.episode.story_for_page(1)[0]
    assert line.text == "約束したの!?" and line.ruby_runs == [("約束", "やくそく")] and line.wrap == "vertical"
    assert abs(line.x_mm + line.w_mm / 2 - 150) < 1 and abs(line.y_mm + line.h_mm / 2 - 100) < 1
    assert line.frame_id == window.current_page().leaf_frames()[0].id
    assert window.canvas.tool == "select" and window.canvas.selected_line_id == line.id
    # Esc drops a line being typed
    window.act_text.trigger()
    _press_release(window.canvas, 80, 200)
    window.canvas.editor.setPlainText("消える")
    window.canvas.editor.finish(False)
    assert len(window.episode.story_for_page(1)) == 1


def test_typing_over_a_balloon_keeps_its_centre(window):
    window.act_text.trigger()
    _press_release(window.canvas, 150, 100)
    window.canvas.editor.setPlainText("あ")
    window.canvas.editor.finish(True)
    line = window.episode.story_for_page(1)[0]
    centre = (line.x_mm + line.w_mm / 2, line.y_mm + line.h_mm / 2)
    window._edit_line_inline(line.id)
    window.canvas.editor.setPlainText("五年前の今日、ここで約束したの")
    window.canvas.editor.finish(True)
    line = window.episode.story_for_page(1)[0]
    assert line.text == "五年前の今日、ここで約束したの" and line.h_mm > 20
    assert abs(line.x_mm + line.w_mm / 2 - centre[0]) < 0.5 and abs(line.y_mm + line.h_mm / 2 - centre[1]) < 0.5


def test_handles_resize_and_bend_the_tail(window):
    ep_line = {"op": "add_line", "page": 1, "text": "あいう", "x_mm": 100, "y_mm": 60, "w_mm": 24, "h_mm": 36, "wrap": "vertical",
               "tails": [{"to": [95, 120]}]}
    window.apply_ops([ep_line])
    line = window.episode.story_for_page(1)[0]
    window.canvas.selected_line_id = line.id
    _press_release(window.canvas, 124, 96, to=(130, 106))  # the bottom-right corner
    line = window.episode.story_for_page(1)[0]
    assert abs(line.w_mm - 30) < 0.6 and abs(line.h_mm - 46) < 0.6
    cx, cy = line.x_mm + line.w_mm / 2, line.y_mm + line.h_mm / 2
    via = ((cx + 95) / 2, (cy + 120) / 2)
    _press_release(window.canvas, *via, to=(via[0] + 15, via[1]))  # the ◇ bend handle
    tail = window.episode.story_for_page(1)[0].tails[0]
    assert tail["to"] == [95.0, 120.0] and tail["via"][0] > via[0] + 10


def test_lines_panel_sets_the_face_and_reorders(window):
    window.apply_ops([{"op": "add_line", "page": 1, "text": "一", "x_mm": 100, "y_mm": 60, "w_mm": 20, "h_mm": 20},
                      {"op": "add_line", "page": 1, "text": "二", "x_mm": 60, "y_mm": 60, "w_mm": 20, "h_mm": 20}])
    story = window.story
    story.refresh()
    story.list.setCurrentRow(1)
    story.font.setCurrentIndex(story.font.findData("maru"))
    story._font_changed()
    story.size.setValue(6)
    story._style_changed()
    second = window.episode.story_for_page(1)[1]
    assert second.style["font"] == "maru" and second.style["size_mm"] == 6
    story._move(-1)
    assert [line.text for line in window.episode.story_for_page(1)] == ["二", "一"]
    story._reset_style()
    assert not {k: v for k, v in window.episode.story_for_page(1)[0].style.items() if k != "group"}
