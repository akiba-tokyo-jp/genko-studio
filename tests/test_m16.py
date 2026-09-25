"""M16: pages and the book — page order, copies, spreads, ノンブル (visible and hidden), the story
editor (all lines, pouring a script in) and the checks before sending the book to print."""

import os
import sys
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import checks, nombre  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book(pages=4):
    ep = new_episode("t", 1, pages, PageSpec.b4_comic())
    for page in ep.pages:
        apply_ops(ep, [{"op": "name_ok", "page": page.index}])
    return ep


def _dark(ep, page, box, dpi=150, mode="print"):
    x, y, w, h = box
    image = render_page(ep.pages[page - 1], dpi, mode=mode, episode=ep).convert("L")
    crop = np.asarray(image.crop((mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi))))
    return float((crop < 128).mean())


# --- pages ----------------------------------------------------------------------------------------------


def test_pages_insert_copy_and_move_with_their_lines():
    ep = _book(3)
    apply_ops(ep, [{"op": "add_line", "page": 2, "text": "二ページ目", "x_mm": 100, "y_mm": 60, "w_mm": 12, "h_mm": 30}])
    ids = [p.id for p in ep.pages]
    apply_ops(ep, [{"op": "add_page", "count": 2, "after": 1}])
    assert [p.index for p in ep.pages] == [1, 2, 3, 4, 5]
    assert [p.id for p in ep.pages][0] == ids[0] and [p.id for p in ep.pages][3] == ids[1]
    assert ep.story[0].page_index == 4  # the line moved with its page
    apply_ops(ep, [{"op": "duplicate_page", "page": 4, "next_to": True}])
    assert len(ep.pages) == 6 and ep.pages[4].id not in ids and [ln.page_index for ln in ep.story] == [4, 5]
    order = [p.index for p in ep.pages][::-1]
    apply_ops(ep, [{"op": "reorder", "order": order}])
    assert sorted(ln.page_index for ln in ep.story) == [2, 3]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_page", "count": 1, "after": 40}])


def test_nombre_positions_font_start_and_hidden(tmp_path: Path):
    ep = _book(2)
    spec = ep.pages[0].spec
    # default: printed at the bottom middle, and in proofs
    [item] = nombre.placements(ep, ep.pages[0])
    assert item["x_mm"] == pytest.approx(spec.width_mm / 2) and item["text"] == "1"
    area = (item["x_mm"] - 4, item["y_mm"] - 3, 8, 6)
    assert _dark(ep, 1, area) > 0.005 and _dark(ep, 1, area, mode="proof") > 0.005
    # outside: the fore-edge side changes with the page
    apply_ops(ep, [{"op": "set_nombre", "position": "bottom_outside", "start": 5, "hidden": True, "font": "mincho", "size_mm": 4}])
    first = nombre.placements(ep, ep.pages[0])
    second = nombre.placements(ep, ep.pages[1])
    assert first[0]["text"] == "5" and second[0]["text"] == "6"
    assert (first[0]["x_mm"] < spec.width_mm / 2) != (second[0]["x_mm"] < spec.width_mm / 2)
    hidden = first[1]
    assert hidden["hidden"] and hidden["size_mm"] == 2.0
    assert (hidden["x_mm"] < spec.width_mm / 2) != (first[0]["x_mm"] < spec.width_mm / 2)  # the binding side
    assert _dark(ep, 1, (hidden["x_mm"] - 2, hidden["y_mm"] - 2, 4, 4)) > 0.01
    # a page can hide its own; the settings travel with the book
    apply_ops(ep, [{"op": "set_nombre", "page": 2, "numero": False}])
    assert nombre.placements(ep, ep.pages[1]) == []
    project = tmp_path / "n.genko"
    save_episode(ep, project)
    again = load_episode(project)
    assert again.nombre["position"] == "bottom_outside" and again.nombre["start"] == 5 and not again.pages[1].numero
    for bad in ({"position": "middle"}, {"size_mm": 50}, {"font": "comic"}, {"start": -1}):
        with pytest.raises(ApplyError):
            apply_ops(ep, [{"op": "set_nombre", **bad}])


# --- checks ---------------------------------------------------------------------------------------------


def _codes(report):
    return {i["code"] for i in report["issues"]}


def test_checks_find_text_outside_overlapping_small_and_art_outside():
    ep = _book(2)
    spec = ep.pages[0].spec
    inner = ep.pages[0].inner_rect_mm()
    apply_ops(ep, [
        {"op": "add_line", "page": 1, "text": "外", "x_mm": spec.width_mm - 8, "y_mm": 40, "w_mm": 12, "h_mm": 20},
        {"op": "add_line", "page": 1, "text": "枠外", "x_mm": inner.x - 4, "y_mm": 60, "w_mm": 10, "h_mm": 20},
        {"op": "add_line", "page": 1, "text": "重なり一", "x_mm": 100, "y_mm": 100, "w_mm": 20, "h_mm": 30},
        {"op": "add_line", "page": 1, "text": "重なり二", "x_mm": 105, "y_mm": 105, "w_mm": 20, "h_mm": 30},
        {"op": "add_line", "page": 1, "text": "とても長い台詞を小さなフキダシに無理に詰め込んでみたらどうなるでしょうか", "x_mm": 60,
         "y_mm": 150, "w_mm": 8, "h_mm": 10},
    ])
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[-20, 50], [40, 50]], "kind": "mili"}])
    report = checks.book(ep)
    codes = _codes(report)
    assert {"text_outside_trim", "text_outside_frame", "text_overlap", "text_too_small", "art_outside_page", "empty_page"} <= codes
    assert not report["ok"] and report["errors"] >= 1
    outside = next(i for i in report["issues"] if i["code"] == "text_outside_trim")
    assert outside["page"] == 1 and outside["box"] and outside["target"]["kind"] == "line"
    assert next(i for i in report["issues"] if i["code"] == "empty_page")["page"] == 2
    # grouped balloons may overlap
    lines = [ln for ln in ep.story if ln.text.startswith("重なり")]
    apply_ops(ep, [{"op": "edit_line", "id": ln.id, "style": {"group": "g"}} for ln in lines])
    assert "text_overlap" not in _codes(checks.book(ep))


def test_checks_low_resolution_pictures():
    from PIL import Image

    ep = _book(1)
    small = Image.new("RGBA", (100, 100), (0, 0, 0, 255))
    import io

    buf = io.BytesIO()
    small.save(buf, format="PNG")
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    ink.patches.append({"id": "p", "box": [50, 50, 50, 50], "mode": "image", "png": buf.getvalue(), "opacity": 1.0})
    low = next(i for i in checks.book(ep)["issues"] if i["code"] == "low_dpi")
    assert "51 dpi" in low["message"] and low["box"] == [50, 50, 50, 50]


def test_studio_preflight_is_folded_in(tmp_path: Path):
    ep = _book(1)
    ep.strict_gates = True
    project = tmp_path / "s.genko"
    save_episode(ep, project)
    report = checks.book(load_episode(project), project)
    assert "page_not_finished" in _codes(report)


# --- the script parser -------------------------------------------------------------------------------------


def test_parse_a_script():
    from genko.app.story_editor import parse_script

    rows = parse_script("太郎「おはよう」\n花子：遅いよ\n\n# 3\n（また寝坊した…）\nナレ：翌朝\nただの台詞\n---\n次のページ\n5ページ\n五", start_page=2)
    assert [(r["page"], r["speaker"], r["text"], r["balloon"]) for r in rows] == [
        (2, "太郎", "おはよう", "speech"), (2, "花子", "遅いよ", "speech"), (3, "", "また寝坊した…", "thought"),
        (3, "", "翌朝", "narration"), (3, "", "ただの台詞", "speech"), (4, "", "次のページ", "speech"), (5, "", "五", "speech")]


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
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    ep = _book(3)
    root = ep.pages[1].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 2, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def test_page_list_has_pictures_and_reorders(window):
    from PySide6.QtCore import Qt

    window.pages.finish_pictures()
    assert window.pages.count() == 3 and not window.pages.item(0).icon().isNull()
    ids = [p.id for p in window.episode.pages]
    window.go_to_page(1)
    window.act_page_down.trigger()
    assert [p.id for p in window.episode.pages] == [ids[1], ids[0], ids[2]]
    assert window.current_page().id == ids[0]  # the page moved and stays selected
    assert window.pages.item(1).data(Qt.ItemDataRole.UserRole + 1) == ids[0]
    window.act_dup_page.trigger()
    assert len(window.episode.pages) == 4 and window.current_page().index == 3
    window.act_spread.trigger()
    assert window.current_page().spread_with == 4 and "見開き 3–4" in window.pages.item(2).text()
    window.act_page_nombre.trigger()
    assert not window.current_page().numero and "ノンブルなし" in window.pages.item(2).text()
    window.act_add_page.trigger()
    assert len(window.episode.pages) == 5 and window.current_page().index == 4


def test_story_editor_edits_and_pours_a_script(window):
    window.open_story_editor()
    editor = window.story_editor
    editor.pour_rows([{"page": 2, "speaker": "太郎", "text": "おはよう", "balloon": "speech"},
                      {"page": 2, "speaker": "花子", "text": "遅いよ", "balloon": "speech"},
                      {"page": 5, "speaker": "", "text": "翌朝", "balloon": "narration"}])
    editor.apply()
    ep = window.episode
    assert len(ep.pages) == 5  # the script ran past the last page
    page2 = ep.story_for_page(2)
    assert [ln.text for ln in page2] == ["おはよう", "遅いよ"]
    top, bottom = sorted(ep.pages[1].leaf_frames(), key=lambda f: f.rect.y)
    assert page2[0].frame_id == top.id and page2[1].frame_id == bottom.id  # one per panel, in reading order
    assert all(ln.x_mm and ln.w_mm for ln in page2)
    # edit a line in the table and reorder
    rows = editor.rows()
    r = next(i for i, row in enumerate(rows) if row["text"] == "遅いよ")
    editor.table.item(r, 2).setText("｜遅刻《ちこく》だよ")
    editor.table.setCurrentCell(r, 2)
    editor.move(-1)
    editor.apply()
    page2 = window.episode.story_for_page(2)
    assert page2[0].text == "遅刻だよ" and page2[0].ruby_runs and page2[1].text == "おはよう"
    # delete, then undo the whole batch at once
    editor.table.setCurrentCell(0, 2)
    editor.delete_rows()
    editor.apply()
    assert len(window.episode.story) == 2
    window.act_undo.trigger()
    assert len(window.episode.story) == 3


def test_checks_panel_lists_problems_and_shows_them(window):
    window.apply_ops([{"op": "add_line", "page": 3, "text": "はみ出し", "x_mm": 250, "y_mm": 40, "w_mm": 12, "h_mm": 20}])
    window.act_checks.trigger()
    panel = window.checks
    assert panel.list.count() >= 1 and "止まる問題" in panel.summary.text()
    item = next(panel.list.item(i) for i in range(panel.list.count()) if "仕上がり線" in panel.list.item(i).text())
    window.go_to_page(1)
    panel._show(item)
    assert window.current_page().index == 3 and window.canvas.highlight_box == [250, 40, 12, 20]
    assert window.canvas.selected_line_id == window.episode.story_for_page(3)[0].id
    window.apply_ops([{"op": "move_line", "id": window.canvas.selected_line_id, "x_mm": 200}])
    window._refresh_visible_docks()  # the side panels catch up a moment after an edit
    assert "最新" in panel.summary.text()
