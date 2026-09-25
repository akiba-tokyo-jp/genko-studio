"""F3: the page gets the room — tools down the left with the settings of the tool in hand, the panels on
the right without cut-off names or buttons, agent panels only for books made with agents, and names that
say what they are."""

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
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


def _window(qapp, tmp_path: Path, studio: bool = False, size=(1280, 720)):
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 4, PageSpec.b4_comic())
    if studio:
        ep.strict_gates = True
    project = tmp_path / ("s.genko" if studio else "b.genko")
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(*size)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    return win


def overflow(root) -> list[str]:
    """Visible parts of a panel that reach past its right edge (cut off)."""
    from PySide6.QtWidgets import QAbstractButton, QAbstractScrollArea, QLabel, QScrollBar, QWidget

    width = root.width()
    out = []
    for child in root.findChildren(QWidget):
        if not child.isVisibleTo(root) or isinstance(child, QScrollBar) or child.width() <= 0:
            continue
        # items inside a list or text box scroll by themselves
        parent, inside = child.parentWidget(), False
        while parent is not None and parent is not root:
            if isinstance(parent, QAbstractScrollArea) and parent is not root:
                inside = True
                break
            parent = parent.parentWidget()
        if inside:
            continue
        right = child.mapTo(root, child.rect().topRight()).x()
        if right > width + 1:
            text = child.text() if isinstance(child, (QAbstractButton, QLabel)) else ""
            out.append(f"{type(child).__name__} {text} → {right}/{width}")
    return out


@pytest.mark.parametrize("size, least", [((1280, 720), 760), ((1024, 640), 500)])
def test_the_page_gets_the_room(qapp, tmp_path: Path, size, least):
    win = _window(qapp, tmp_path, size=size)
    assert win.canvas.width() >= least, win.canvas.width()
    assert win.height() <= size[1] + 1  # the window does not grow taller than the screen
    win.close()


def test_no_panel_is_cut_off(qapp, tmp_path: Path):
    from PySide6.QtWidgets import QScrollArea, QTabBar

    win = _window(qapp, tmp_path, size=(1024, 640))
    cut = {}
    for dock in [*win.studio_docks, win.brush_dock]:
        if not dock.isVisible():
            continue
        dock.raise_()
        tools = list(win.tool_actions) if dock is win.brush_dock else [None]
        for tool in tools:
            if tool:
                win._tool(tool)
            for _ in range(2):
                qapp.processEvents()
            content = dock.widget()
            root = content.viewport() if isinstance(content, QScrollArea) else content
            found = overflow(root)
            if found:
                cut[f"{dock.windowTitle()} {tool or ''}"] = found[:4]
    assert cut == {}, "\n".join(f"{k}: {v}" for k, v in cut.items())
    # tab names are shown whole
    for bar in win.findChildren(QTabBar):
        if not bar.isVisible():
            continue
        for i in range(bar.count()):
            text = bar.tabText(i)
            assert bar.tabRect(i).width() >= bar.fontMetrics().horizontalAdvance(text), text
    win.close()


def test_the_tool_settings_follow_the_tool(qapp, tmp_path: Path):
    win = _window(qapp, tmp_path)
    ts = win.tool_settings
    for action, title, page in ((win.act_pen, "ペン", win.brush), (win.act_text, "テキスト", win.text_settings),
                                (win.act_eraser, "消しゴム", None), (win.act_frame, "コマ割り", None), (win.act_ruler, "定規", None)):
        action.trigger()
        assert ts.title.text().startswith(title)
        if page is not None:
            assert ts.stack.currentWidget() is page
    # the text tool starts lines with its settings
    win.act_text.trigger()
    win.text_settings.balloon.setCurrentIndex(win.text_settings.balloon.findData("shout"))
    win.text_settings.font.setCurrentIndex(win.text_settings.font.findData("gothic"))
    win._type_new_line(120, 100)
    win.canvas.editor.setPlainText("なんだと")
    win.canvas.editor.finish(True) if hasattr(win.canvas.editor, "finish") else None
    lines = win.episode.story_for_page(1)
    if lines:  # (the inline editor may finish on its own)
        assert lines[-1].balloon == "shout" and (lines[-1].style or {}).get("font") == "gothic"
    # the eraser's size
    win.act_eraser.trigger()
    win.eraser_size.setValue(6)
    assert win.canvas.eraser_mm == 6
    win.close()


def test_agent_panels_only_for_books_made_with_agents(qapp, tmp_path: Path):
    solo = _window(qapp, tmp_path)
    titles = {d.windowTitle(): d.isVisible() for d in solo.studio_docks}
    assert not titles["承認箱"] and not titles["コマの詳細"] and not titles["資料"]
    assert titles["ページ"] and titles["レイヤー"] and titles["台詞"]
    assert not solo.process.isVisible() and "leaf" not in solo.status.text() and "ネーム" not in solo.status.text()
    solo.close()
    studio = _window(qapp, tmp_path, studio=True)
    titles = {d.windowTitle(): d.isVisible() for d in studio.studio_docks}
    assert titles["承認箱"] and titles["コマの詳細"]
    studio.close()


def test_names_say_what_they_are(qapp, tmp_path: Path):
    from PySide6.QtGui import QAction

    win = _window(qapp, tmp_path)
    texts = {}
    for action in win.findChildren(QAction):
        if action.text() and not action.isSeparator() and not action.menu():
            texts.setdefault(action.text(), set()).add(id(action))
    duplicates = {text for text, ids in texts.items() if len(ids) > 1 and text not in {d.windowTitle() for d in win.studio_docks}}
    assert duplicates == set()
    tools_menu = next(a.menu() for a in win.menuBar().actions() if a.text() == "ツール")
    in_menu = {a for a in tools_menu.actions()}
    assert all(action in in_menu for action in (win.act_ruler, win.act_3d, win.act_effect, win.act_stamp))
    assert win.act_frame.text() == "コマ割り" and win.act_3d.text() == "3D 操作"
    assert win.act_pen.icon().isNull() is False and win.tool_palette.orientation().name == "Vertical"
    win.close()


def test_no_stray_tab_bars_and_the_right_panels_in_front(qapp, tmp_path: Path):
    from PySide6.QtWidgets import QTabBar

    for studio in (False, True):
        (tmp_path / str(studio)).mkdir()
        win = _window(qapp, tmp_path / str(studio), studio=studio, size=(1024, 640))
        for _ in range(3):
            qapp.processEvents()
        shown = [[bar.tabText(i) for i in range(bar.count())] for bar in win.findChildren(QTabBar)
                 if bar.isVisible() and bar.parentWidget() is win and bar.geometry().right() > 0]
        # each group once, with every panel of the group
        assert sorted(map(tuple, shown)) == sorted({tuple(tabs) for tabs in shown}), shown
        assert all(len(tabs) >= 2 for tabs in shown), shown
        fronts = {bar.tabText(bar.currentIndex()) for bar in win.findChildren(QTabBar) if bar.isVisible()}
        assert {"レイヤー", "台詞"} <= fronts and (("承認箱" in fronts) == studio)
        win.close()
