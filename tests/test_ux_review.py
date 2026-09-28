"""The UI/UX review (docs/ux-review): tooltips with keys, Japanese Qt words, the approval box on the right, every
tool within reach on a short screen, the first steps on an empty book, AI と作る, and the panel reference for an AI."""

import json
import os
import sys
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def _window(qapp, tmp_path: Path, agent: bool = False, size=(1366, 768)):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    episode = new_episode("t", 1, 2, PageSpec.b4_comic())
    if agent:
        episode.strict_gates = True
        episode.tickets.append({"id": "t_help", "kind": "help", "status": "open", "text": "背景はどうしますか",
                                "page_index": 1, "created_by": "ai:test"})
    save_episode(episode, project)
    window = MainWindow(project)
    window.resize(*size)
    window.show()
    for _ in range(20):
        qapp.processEvents()
    return window


def test_tooltips_show_the_keys_as_they_are_now(qapp, tmp_path):
    from PySide6.QtGui import QKeySequence

    from genko.app import preferences

    window = _window(qapp, tmp_path)
    try:
        assert "Ctrl+Z" in window.act_undo.toolTip() and window.act_undo.toolTip().startswith("元に戻す")
        window.act_undo.setShortcut(QKeySequence("Ctrl+Alt+Z"))
        preferences.retip(window)
        assert "Ctrl+Alt+Z" in window.act_undo.toolTip()
    finally:
        window.close()


def test_qt_speaks_japanese(qapp):
    from PySide6.QtWidgets import QMessageBox

    from genko.app import theme

    theme.apply(qapp)
    box = QMessageBox(QMessageBox.Icon.Question, "t", "q", QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
    assert any("はい" in b.text() for b in box.buttons()) and any("いいえ" in b.text() for b in box.buttons())


def test_the_approval_box_sits_on_the_right_in_sight(qapp, tmp_path):
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QDockWidget

    window = _window(qapp, tmp_path, agent=True)
    try:
        window._settle_docks()
        for _ in range(20):
            qapp.processEvents()
        box = next(d for d in window.findChildren(QDockWidget) if d.windowTitle() == "承認箱")
        assert window.dockWidgetArea(box) == Qt.DockWidgetArea.RightDockWidgetArea
        assert box.isVisible() and box.height() >= 120  # (its list and at least a request in sight)
        quick = window.quick_dock
        assert not box.geometry().intersects(quick.geometry())  # (nothing lies over the approval box)
    finally:
        window.close()


def test_every_tool_is_within_reach_on_a_short_screen(qapp, tmp_path):
    window = _window(qapp, tmp_path, size=(1366, 700))
    try:
        window._fit_tools()
        for _ in range(10):
            qapp.processEvents()
        palette = window.tool_palette
        last = palette.widgetForAction(window.act_effect)
        assert last is not None and last.isVisible() and last.geometry().bottom() <= palette.height()
        assert palette.iconSize().width() >= 16
    finally:
        window.close()


def test_an_empty_book_shows_the_first_steps_until_something_is_made(qapp, tmp_path):
    window = _window(qapp, tmp_path)
    try:
        window._refresh_status()
        assert window.first_steps.isVisible()
        window.apply_ops([{"op": "add_line", "page": 1, "text": "やあ", "x_mm": 100, "y_mm": 100}])
        window._refresh_status()
        assert window.first_steps is not None and not window.first_steps.isVisible()
    finally:
        window.close()


def test_ai_link_texts_and_who_worked(tmp_path):
    from genko.app import ai_link

    where, text = ai_link.snippet("claude-desktop", tmp_path)
    config = json.loads(text)
    args = config["mcpServers"]["genko"]["args"]
    assert "mcp" in args and str(tmp_path) in args and "ai:claude-desktop" in args and "設定を編集" in where
    assert ai_link.snippet("claude-code", tmp_path)[1].startswith("claude mcp add genko -- ")
    assert "mcp_servers:" in ai_link.snippet("hermes", tmp_path)[1]
    book = tmp_path / "b.genko"
    (book / "studio").mkdir(parents=True)
    now = time.time()
    (book / "studio" / "presence.json").write_text(json.dumps({"actors": {
        "ai:hermes/9204": {"at": now - 60}, "ai:claude-code": {"at": now - 3 * 3600}, "human:leaf": {"at": now}}}))
    found = ai_link.recent_ai(book, now)
    assert [f["actor"] for f in found] == ["ai:hermes/9204", "ai:claude-code"] and found[0]["working"]
    words, working = ai_link.status_words(book)
    assert working and words.startswith("AI: hermes（9204） 作業中") and "ほか 1" in words
    assert ai_link.status_words(None) == ("AI と作る", False)
    assert "b.genko" in ai_link.first_request(book)


def test_the_ai_dialog_and_the_status_button(qapp, tmp_path):
    from genko.app.ai_link import AiDialog

    window = _window(qapp, tmp_path, agent=True)
    try:
        assert window.ai_button.text() == "AI と作る"
        dialog = AiDialog(window, window.path)
        assert json.dumps(str(tmp_path.resolve()), ensure_ascii=False)[1:-1] in dialog.text.toPlainText()  # (JSON doubles each \)
        dialog.client.setCurrentIndex(1)
        assert dialog.text.toPlainText().startswith("claude mcp add genko")
        dialog.close()
    finally:
        window.close()


def test_a_panel_reference_for_the_ai(qapp, tmp_path):
    window = _window(qapp, tmp_path)
    try:
        page = window.current_page()
        window.apply_ops([{"op": "split_frame", "page": 1, "frame_id": page.frames[0].id, "axis": "horizontal"}])
        second = window.current_page().leaf_frames()[1]
        words = window.panel_reference(second.id)
        assert words.startswith("1 ページ目の 2 コマ目") and f'frame_id "{second.id}"' in words
    finally:
        window.close()


def test_the_lines_list_shows_the_kind_first(qapp, tmp_path):
    window = _window(qapp, tmp_path)
    try:
        window.apply_ops([{"op": "add_line", "page": 1, "text": "とても長い台詞がここに入ります", "x_mm": 100, "y_mm": 100,
                           "balloon": "narration"}])
        window.story.refresh()
        assert window.story.list.item(0).text().startswith("1.〔ナレーション〕")
    finally:
        window.close()


def test_nine_menus_and_the_tabs_whole_on_a_1366_screen(qapp, tmp_path):
    from PySide6.QtWidgets import QTabBar

    window = _window(qapp, tmp_path, agent=True, size=(1366, 768))
    try:
        window._settle_docks()
        for _ in range(20):
            qapp.processEvents()
        assert len([a for a in window.menuBar().actions() if a.menu()]) == 9
        bar = next(b for b in window.findChildren(QTabBar) if b.isVisible() and "レイヤー" in [b.tabText(i) for i in range(b.count())])
        assert all(bar.tabRect(i).right() <= bar.width() for i in range(bar.count()))  # (no tab behind the arrows)
    finally:
        window.close()
