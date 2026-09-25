"""The app as a person uses it: fits a laptop screen, the view, tools, approvals, dialogs, words, review.html."""

import json
import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
QtWidgets = pytest.importorskip("PySide6.QtWidgets")

sys.path.insert(0, str(Path(__file__).parent))

from genko.app import exporting, wording  # noqa: E402
from genko.app.session import Session  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    try:
        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:  # no display libraries at all
        pytest.skip(f"Qt cannot start here: {exc}")
    return app


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")
    from PySide6.QtWidgets import QMessageBox

    # modal boxes would wait for a click that never comes offscreen
    shown = []
    for name in ("warning", "information"):
        monkeypatch.setattr(QMessageBox, name, lambda *a, _n=name, **k: shown.append((_n, a[2] if len(a) > 2 else "")))
    monkeypatch.setattr(QMessageBox, "exec", lambda self: 0)
    return shown


def _studio(root: Path):
    import test_m7_gui as m7

    return m7._project(root)


def _plain(root: Path, pages: int = 3) -> Path:
    project = root / "plain.genko"
    save_episode(new_episode("試し", 1, pages, PageSpec.b4_comic()), project)
    return project


def _window(qapp, project, size=(1280, 720)):
    from genko.app.main import MainWindow

    window = MainWindow(project)
    window.resize(*size)
    window.show()
    qapp.processEvents()
    return window


def _mouse(canvas, kind, pos, button, buttons):
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QMouseEvent

    return QMouseEvent(kind, pos, canvas.mapToGlobal(pos), button, buttons, Qt.KeyboardModifier.NoModifier)


# --- words -----------------------------------------------------------------------------------------


def test_errors_and_labels_are_in_plain_japanese():
    assert wording.error("ops[0] delete_page: cannot delete the last page") == "最後の 1 ページは削除できません"
    assert "承認済み" in wording.error("page 3: the name is approved; a person must revoke it before the layout changes (strict_gates)")
    assert wording.error("page 2 locked by ai:hermes") == "2 ページは ai:hermes が作業中です"
    assert wording.error("no page 9") == "9 ページはありません"
    assert wording.error("something new").startswith("この操作はできませんでした")
    assert wording.actor("ai:hermes") == "エージェント（hermes）" and wording.actor("human:leaf") == "leaf"
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    labels = [wording.layer_label(layer) for layer in ep.pages[0].layers]
    assert "ネーム" in labels and "ペン入れ" in labels

    class Placed:
        title, role, kind = "art 876604a32748", ep.pages[0].layers[0].role, type("K", (), {"value": "placed"})()

    assert wording.layer_label(Placed()) == "絵（配置）"


# --- session --------------------------------------------------------------------------------------------


def test_redo_puts_back_what_undo_took(tmp_path: Path):
    project = _plain(tmp_path)
    session = Session.open(project, "human:leaf")
    session.apply([{"op": "set_note", "page": 1, "note": "メモ"}])
    session.undo()
    assert session.episode.pages[0].note != "メモ"
    session.redo()
    assert session.episode.pages[0].note == "メモ"
    session.commit()
    session.undo()  # now through the journal on disk
    assert load_episode(project).pages[0].note != "メモ"
    session.redo()
    assert load_episode(project).pages[0].note == "メモ" and session.episode.pages[0].note == "メモ"


# --- exports ---------------------------------------------------------------------------------------------


@pytest.mark.parametrize("key", ["pdf", "png", "psd", "epub", "strip", "webtoon", "sns", "pack"])
def test_every_export_format_runs_from_the_app(tmp_path: Path, key: str):
    project = _plain(tmp_path, 2)
    episode = load_episode(project)
    result = exporting.run(episode, project, key, tmp_path / key, dpi=72, width=300, long_edge=400)
    assert result["ok"] and result["files"] and all(Path(f).exists() for f in result["files"])
    if key == "sns":
        assert all(f.endswith(".png") for f in result["files"])  # jpeg only when asked
        assert exporting.run(episode, project, key, tmp_path / "j", long_edge=400, jpeg=True)["files"][0].endswith(".jpg")


def test_official_export_needs_a_saved_book_and_preflight(tmp_path: Path):
    project = _plain(tmp_path, 1)
    episode = load_episode(project)
    assert not exporting.run(episode, None, "pdf", tmp_path / "o", official=True)["ok"]
    assert not exporting.run(episode, project, "psd", tmp_path / "o", official=True)["ok"]
    refused = exporting.run(episode, project, "pdf", tmp_path / "o", official=True, actor="human:leaf")
    assert not refused["ok"] and refused["errors"]


# --- the window --------------------------------------------------------------------------------------------


def test_the_window_fits_a_laptop_screen_and_the_page_fits_the_view(qapp, tmp_path: Path):
    agent, project = _studio(tmp_path)
    window = _window(qapp, project)
    hint = window.minimumSizeHint()
    assert hint.width() <= 1024 and hint.height() <= 640, hint
    canvas = window.canvas
    page = window.current_page()
    top_left, bottom_right = canvas._pt(0, 0), canvas._pt(page.spec.width_mm, page.spec.height_mm)
    assert 0 <= top_left.x() and bottom_right.x() <= canvas.width() and 0 <= top_left.y() and bottom_right.y() <= canvas.height()
    assert canvas.background is not None  # the page as it prints, lettering included
    before = canvas._scale
    window.act_zoom_in.trigger()
    assert canvas._scale > before and not canvas._fitted
    window.act_fit.trigger()
    assert canvas._scale == pytest.approx(before) and canvas._fitted
    # the status says where you are, in words
    assert "ページ" in window.status.text() and "leaf" in window.status.text() and "stage=" not in window.status.text()
    assert "ネーム" in window.pages.item(0).text()
    window.close()


def test_wheel_scrolls_and_ctrl_wheel_zooms(qapp, tmp_path: Path):
    from PySide6.QtCore import QPoint, QPointF, Qt
    from PySide6.QtGui import QWheelEvent

    window = _window(qapp, _plain(tmp_path))
    canvas = window.canvas

    def wheel(dy, mods):
        return QWheelEvent(QPointF(100, 100), canvas.mapToGlobal(QPointF(100, 100)), QPoint(0, 0), QPoint(0, dy),
                           Qt.MouseButton.NoButton, mods, Qt.ScrollPhase.NoScrollPhase, False)

    pan, scale = canvas._pan_y, canvas._scale
    canvas.wheelEvent(wheel(-120, Qt.KeyboardModifier.NoModifier))
    assert canvas._pan_y < pan and canvas._scale == scale
    canvas.wheelEvent(wheel(120, Qt.KeyboardModifier.ControlModifier))
    assert canvas._scale > scale
    window.close()


def test_select_tool_selects_and_pans_and_the_pen_only_draws_when_chosen(qapp, tmp_path: Path):
    from PySide6.QtCore import QEvent, QPointF, Qt

    window = _window(qapp, _plain(tmp_path))
    canvas = window.canvas
    page = window.current_page()
    assert canvas.tool == "select"
    inside = canvas._pt(page.inner_rect_mm().x + 20, page.inner_rect_mm().y + 20)
    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton
    canvas.mousePressEvent(_mouse(canvas, QEvent.Type.MouseButtonPress, inside, left, left))
    canvas.mouseReleaseEvent(_mouse(canvas, QEvent.Type.MouseButtonRelease, inside, left, none))
    assert window.current_page().selected_frame_id  # a click selects the panel
    strokes = len(window.target_layer().strokes)
    # a drag with the select tool moves the view and draws nothing
    pan = canvas._pan_x
    canvas.mousePressEvent(_mouse(canvas, QEvent.Type.MouseButtonPress, inside, left, left))
    canvas.mouseMoveEvent(_mouse(canvas, QEvent.Type.MouseMove, inside + QPointF(60, 0), left, left))
    canvas.mouseReleaseEvent(_mouse(canvas, QEvent.Type.MouseButtonRelease, inside + QPointF(60, 0), left, none))
    assert canvas._pan_x > pan and len(window.target_layer().strokes) == strokes
    window.act_pen.trigger()
    assert canvas.tool == "pen"
    canvas.mousePressEvent(_mouse(canvas, QEvent.Type.MouseButtonPress, inside, left, left))
    for step in range(1, 8):
        canvas.mouseMoveEvent(_mouse(canvas, QEvent.Type.MouseMove, inside + QPointF(step * 8, step * 4), left, left))
    canvas.mouseReleaseEvent(_mouse(canvas, QEvent.Type.MouseButtonRelease, inside + QPointF(56, 28), left, none))
    assert len(window.target_layer().strokes) == strokes + 1
    window.act_undo.trigger()
    assert len(window.target_layer().strokes) == strokes
    window.act_redo.trigger()
    assert len(window.target_layer().strokes) == strokes + 1
    window.close()


def test_deleting_a_page_asks_first(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    window = _window(qapp, _plain(tmp_path))
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.No)
    window.act_del_page.trigger()
    assert len(window.episode.pages) == 3
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    window.act_del_page.trigger()
    assert len(window.episode.pages) == 2
    window.close()


def test_approval_box_follows_the_page_and_opens_a_large_view(qapp, tmp_path: Path, monkeypatch):
    from genko.app import viewer

    agent, project = _studio(tmp_path)
    window = _window(qapp, project)
    box = window.approvals
    help_row = next(i for i, item in enumerate(box.items) if item.kind == "help")
    box.list.setCurrentRow(help_row)
    qapp.processEvents()
    assert window.current_page().index == 2  # the page the question is about
    assert box.back_button.text() == "返事を送る" and "返事" in box.reason.placeholderText()
    art_row = next(i for i, item in enumerate(box.items) if item.gate == "art")
    box.list.setCurrentRow(art_row)
    qapp.processEvents()
    assert window.current_page().index == 1 and "エージェント" in box.detail.text()
    shown = {}

    def fake_exec(self):
        shown["title"] = self.windowTitle()
        shown["items"] = len(self.view.scene().items())
        return 0

    monkeypatch.setattr(viewer.ViewerDialog, "exec", fake_exec)
    box.open_viewer()
    assert shown["title"].startswith("作画の承認") and shown["items"] >= 1
    window.panel_view.frame_id = window.current_page().leaf_frames()[0].id
    window.panel_view.refresh()
    window.panel_view.compare_candidates()
    assert shown["title"] == "候補を比べる"
    window.close()


def test_sheet_approval_shows_the_candidates_large(qapp, tmp_path: Path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    for cid in ("hina",):
        req = agent.generation_request("demo.genko", character_id=cid).data
        (Path(req["inbox"]) / "s.png").write_bytes(m5.fixture_images.sheet(req["request"]["size"]["suggested_px"]))
        agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s.png",
                                                                "origin": {"tool_id": "t"}}])
        agent.request_approval("demo.genko", "sheet", [], "", character_id=cid)
    window = _window(qapp, project)
    box = window.approvals
    box.list.setCurrentRow(0)
    qapp.processEvents()
    assert box.items[0].title.endswith("日向ひな")
    assert box.choices.isVisible() and not box.preview.isVisible() and box.choices.count() == 1
    assert box.choices.item(0).text() == "候補 1"
    window.close()


def test_new_manuscript_and_start_screen(qapp, tmp_path: Path):
    from genko.app.dialogs import NewProjectDialog, StartDialog
    from genko.app.main import remember_project

    dialog = NewProjectDialog()
    dialog.title.setText("夏の約束")
    dialog.pages.setValue(12)
    dialog.folder.setText(str(tmp_path))
    dialog.create()
    assert dialog.created == tmp_path / "夏の約束.genko"
    episode = load_episode(dialog.created)
    assert episode.title == "夏の約束" and len(episode.pages) == 12 and episode.pages[0].spec.width_mm == 257
    dialog.created = None
    dialog.create()  # the same name again is refused, nothing is overwritten
    assert dialog.created is None
    remember_project(tmp_path / "夏の約束.genko")
    start = StartDialog()
    assert start.list.count() == 1 and start.list.item(0).text().startswith("夏の約束")


def test_export_dialog_shows_only_the_options_of_the_format(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    from genko.app.dialogs import ExportDialog

    project = _plain(tmp_path, 1)
    window = _window(qapp, project)
    dialog = ExportDialog(window, window.episode, project, "human:leaf")
    dialog.show()
    dialog.format.setCurrentIndex(dialog.format.findData("webtoon"))
    qapp.processEvents()
    assert dialog.width.isVisible() and not dialog.dpi.isVisible() and dialog.official.isVisible()
    dialog.format.setCurrentIndex(dialog.format.findData("psd"))
    qapp.processEvents()
    assert dialog.dpi.isVisible() and not dialog.official.isVisible()
    dialog.dpi.setValue(72)
    dialog.folder.setText(str(tmp_path / "out"))
    monkeypatch.setattr(QMessageBox, "exec", lambda self: 0)
    dialog.run()
    assert dialog.result_["ok"] and all(f.endswith(".psd") for f in dialog.result_["files"])
    official = ExportDialog(window, window.episode, project, "human:leaf", official=True)
    keys = [official.format.itemData(i) for i in range(official.format.count())]
    assert "psd" not in keys and official.official.isChecked() and not official.official.isEnabled()
    window.close()


# --- review.html and the CLI -------------------------------------------------------------------------------


def test_review_page_reads_on_a_phone_and_points_to_the_app(tmp_path: Path):
    agent, project = _studio(tmp_path)
    result = agent.review_page("demo.genko")
    page = Path(result.data["path"]).read_text(encoding="utf-8")
    assert "name='viewport'" in page and "承認箱" in page and "human:名前" not in page
    assert "<details class='cmd'>" in page and "あなたを待っているもの" in page
    assert "作画の承認（1 ページ）" in page and "エージェント（test）" in page


def test_approving_from_the_shell_needs_no_as(tmp_path: Path, capsys):
    from genko.__main__ import main

    agent, project = _studio(tmp_path)
    assert main(["studio", "approve", str(project), "art", "--pages", "1"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["ok"] and load_episode(project).pages[0].art_ok
    from genko.studio import evaluate

    assert evaluate.audit(project)["changes_by_actor"].get("human:leaf")
