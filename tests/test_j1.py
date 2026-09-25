"""J1: the screen made one's own — workspaces, quick access, the command bar, command search, the dark
screen, the cursor, tools held with Alt / Ctrl, and two-finger gestures."""

import os
import sys
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


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow
    from genko.app.preferences import settings

    store = settings()
    for key in ("ui/quick", "ui/commandbar", "ui/theme", "ui/cursor", "keys/alt_tool", "keys/ctrl_tool", "workspaces"):
        store.remove(key)
    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()
    from genko.app import workspace

    for key in ("ui/quick", "ui/commandbar", "ui/theme", "ui/cursor", "keys/alt_tool", "keys/ctrl_tool", "workspaces"):
        store.remove(key)
    workspace.apply_theme("system")


def test_command_search_finds_and_runs(window, qapp):
    from genko.app import workspace

    found = [a.text() for a in workspace.search(window, "見開き")]
    assert any("見開き" in t for t in found)
    assert workspace.where(window, window.act_mirror).startswith("表示")
    dialog = window._find_command()
    dialog.query.setText("左右反転して見る")
    assert dialog.results.count() >= 1
    before = window.act_mirror.isChecked()
    dialog.run()
    assert window.act_mirror.isChecked() != before
    assert window.act_find_command.shortcut().toString() == "Ctrl+Shift+F"


def test_the_command_bar_can_be_changed(window):
    from genko.app import workspace

    names = [a.text() for a in window.command_bar.actions() if not a.isSeparator()]
    assert names[:2] == ["元に戻す", "やり直す"] and "書き出し…" in names
    workspace.keep("ui/commandbar", ["印刷…", "|", "ペン"])
    workspace.fill_commandbar(window)
    names = [a.text() for a in window.command_bar.actions() if not a.isSeparator()]
    assert names == ["印刷…", "ペン"]


def test_quick_access_holds_chosen_commands(window, qapp):
    from genko.app import workspace

    assert window.quick_dock.windowTitle() == "クイックアクセス"
    labels = [b.text() for b in window.quick_access.buttons]
    assert "ペン" in labels and "集中線" in labels
    workspace.keep("ui/quick", ["消しゴム", "印刷…"])
    window.quick_access.refresh()
    assert [b.text() for b in window.quick_access.buttons] == ["消しゴム", "印刷…"]
    window.quick_access.buttons[0].click()
    assert window.canvas.tool == "eraser"


def test_workspaces_are_kept_and_brought_back(window, qapp):
    from genko.app import workspace

    assert window._save_workspace("ペン入れ")
    assert "ペン入れ" in workspace.workspaces()
    window.quick_dock.hide()
    assert workspace.load_workspace(window, "ペン入れ")
    qapp.processEvents()
    window._fill_workspaces()
    titles = [a.text() for a in window.workspace_menu.actions()]
    assert "ペン入れ" in titles and "はじめの配置に戻す" in titles
    workspace.delete_workspace("ペン入れ")
    assert "ペン入れ" not in workspace.workspaces()


def test_the_dark_screen(window, qapp):
    from PySide6.QtGui import QPalette
    from PySide6.QtWidgets import QApplication

    from genko.app import workspace

    workspace.apply_theme("dark")
    assert QApplication.instance().palette().color(QPalette.ColorRole.Window).lightness() < 80
    workspace.apply_theme("light")
    assert QApplication.instance().palette().color(QPalette.ColorRole.Window).lightness() > 180


def test_held_alt_is_the_eyedropper_for_a_moment(window, qapp):
    canvas = window.canvas
    window._tool("pen")
    canvas.hold_modifier("alt", True)
    assert canvas.tool == "picker"
    canvas.hold_modifier("alt", False)
    assert canvas.tool == "pen"
    canvas.modifier_tools = {"alt": "", "ctrl": "move"}
    canvas.hold_modifier("alt", True)
    assert canvas.tool == "pen"
    canvas.hold_modifier("ctrl", True)
    assert canvas.tool == "move"
    canvas.hold_modifier("ctrl", False)
    assert canvas.tool == "pen"


def test_the_cursor_kind(window, qapp):
    from PySide6.QtCore import Qt

    canvas = window.canvas
    window._tool("pen")
    canvas.cursor_kind = "circle"
    canvas._update_cursor()
    assert canvas.cursor().shape() == Qt.CursorShape.BlankCursor
    canvas.cursor_kind = "cross"
    canvas._update_cursor()
    assert canvas.cursor().shape() == Qt.CursorShape.CrossCursor


def test_two_fingers_zoom_turn_and_move(window, qapp):
    from PySide6.QtCore import QPointF

    canvas = window.canvas
    scale, turn, pan = canvas._scale, canvas.rotation, canvas._pan_x
    canvas.pinch(1.5, 0, QPointF(300, 300), QPointF(0, 0))
    assert canvas._scale > scale
    canvas.pinch(1.0, 20, QPointF(300, 300), QPointF(0, 0))
    assert abs(canvas.rotation - turn - 20) < 1e-6
    canvas.pinch(1.0, 0, QPointF(300, 300), QPointF(40, 0))
    assert canvas._pan_x != pan


def test_the_preferences_hold_the_screen_settings(window, qapp):
    from genko.app.preferences import PreferencesDialog, settings

    dialog = PreferencesDialog(window)
    dialog.theme.setCurrentIndex(dialog.theme.findData("dark"))
    dialog.cursor.setCurrentIndex(dialog.cursor.findData("dot"))
    dialog.alt_tool.setCurrentIndex(dialog.alt_tool.findData("eraser"))
    dialog.save()
    assert settings().value("ui/theme") == "dark" and window.canvas.cursor_kind == "dot"
    assert window.canvas.modifier_tools["alt"] == "eraser"
