"""UI-A and UI-B: the look (light, dark, as the computer is; the canvas surround; hints; icons; the start screen) and
comfort over long sessions (the page alone, stage layouts, the round menu, resting the eyes, fewer moving things)."""

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

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

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


# --- the look ---------------------------------------------------------------------------------------------------


def test_light_and_dark_and_the_brightness_step(qapp):
    from genko.app import theme
    from genko.app.preferences import settings

    light, dark = theme.tokens("light", 0), theme.tokens("dark", 0)
    assert not light.dark and dark.dark
    assert theme.QColor(light.window).lightness() > 180 and theme.QColor(dark.window).lightness() < 60
    brighter = theme.tokens("dark", 2)
    assert theme.QColor(brighter.panel).lightness() > theme.QColor(dark.panel).lightness()
    assert brighter.accent == dark.accent  # (only the greys move)
    settings().setValue("ui/theme", "dark")
    theme.apply(qapp)
    from PySide6.QtGui import QPalette

    assert qapp.palette().color(QPalette.ColorRole.Window).lightness() < 80
    assert "QDockWidget::title" in qapp.styleSheet()
    settings().setValue("ui/theme", "system")
    theme.apply(qapp)


def test_the_canvas_surround_is_a_neutral_grey(qapp):
    from genko.app import theme
    from genko.app.preferences import settings

    colour = theme.surround()
    assert abs(colour.red() - colour.green()) <= 6 and abs(colour.green() - colour.blue()) <= 6
    settings().setValue("ui/surround", "128")
    assert theme.surround().red() == 128
    settings().setValue("ui/surround", "auto")


def test_hints_can_go_into_tooltips(qapp):
    from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

    from genko.app import theme

    panel = QWidget()
    note = QLabel("この道具の説明", panel)
    QVBoxLayout(panel).addWidget(note)
    theme.hint(note)
    theme.set_hints(False)
    assert note.isHidden() and panel.toolTip() == "この道具の説明"
    theme.set_hints(True)
    assert not note.isHidden()


def test_icons_follow_the_look_and_can_be_grey(qapp):
    from genko.app import icons, theme
    from genko.app.preferences import settings

    assert not icons.icon("approve").isNull() and not icons.icon("duplicate").isNull()
    settings().setValue("ui/mono_icons", "true")
    grey = icons.icon("pen").pixmap(32, 32).toImage()
    settings().setValue("ui/mono_icons", "false")
    colour = icons.icon("pen").pixmap(32, 32).toImage()
    assert grey != colour and theme.mono_icons() is False


def test_the_start_screen_shows_covers(qapp, tmp_path):
    from genko.app.dialogs import StartDialog, cover_thumbnail
    from genko.app.main import remember_project

    project = tmp_path / "c.genko"
    save_episode(new_episode("表紙の本", 1, 1, PageSpec.b5_doujin()), project)
    remember_project(project)
    cover = cover_thumbnail(project)
    assert cover is not None and cover.height() == 180
    dialog = StartDialog()
    assert dialog.list.count() == 1 and dialog.list.item(0).text() == "表紙の本"
    dialog.close()


# --- comfort --------------------------------------------------------------------------------------------------


def test_tab_leaves_the_page_alone_and_brings_the_panels_back(window, qapp):
    from PySide6.QtWidgets import QDockWidget

    shown = [d for d in window.findChildren(QDockWidget) if d.isVisible()]
    assert shown
    window.canvas_only.toggle()
    qapp.processEvents()
    assert not any(d.isVisible() for d in shown) and window.canvas_only.on
    window.canvas_only._show_side("right")
    assert any(d.isVisible() for d in shown if window.canvas_only._side_of(d) == "right")
    window.canvas_only.toggle()
    qapp.processEvents()
    assert all(d.isVisible() for d in shown)


def test_each_stage_has_its_layout(window, qapp):
    from PySide6.QtWidgets import QDockWidget

    from genko.app import comfort

    comfort.apply_stage(window, "letter")
    qapp.processEvents()
    visible = {d.windowTitle() for d in window.findChildren(QDockWidget) if d.isVisible()}
    assert "台詞" in visible and "カラー" not in visible and "素材" not in visible
    comfort.apply_stage(window, "ink")
    qapp.processEvents()
    visible = {d.windowTitle() for d in window.findChildren(QDockWidget) if d.isVisible()}
    assert {"レイヤー", "カラー"} <= visible


def test_the_round_menu_at_the_pen(window, qapp):
    from PySide6.QtCore import QPointF

    from genko.app import comfort

    window.act_pen.trigger()
    window._context_menu("", QPointF(400, 300))
    menu = window.radial
    assert isinstance(menu, comfort.RadialMenu) and len(menu.actions_) >= 6
    target = next(i for i, a in enumerate(menu.actions_) if a is window.act_eraser)
    menu._choose(target)
    assert window.canvas.tool == "eraser"


def test_the_eyes_are_reminded_only_when_asked(window, qapp):
    from genko.app.preferences import settings

    rest = window.rest
    rest.worked = 10_000
    rest._count()
    assert rest.worked == 10_000  # (off by default: nothing counted, nothing shown)
    settings().setValue("ui/rest_minutes", 30)
    import time

    from genko.app import comfort

    comfort.last_input()
    comfort._input.last = time.monotonic()  # (the person is at work)
    shown = []
    rest.remind = lambda: shown.append(True)
    rest._count()
    assert shown and rest.worked == 0.0
    settings().setValue("ui/rest_minutes", 0)


def test_fewer_moving_things_and_letters_at_once(qapp):
    from PySide6.QtCore import Qt

    from genko.app import comfort
    from genko.app.preferences import settings

    settings().setValue("ui/reduce_motion", "true")
    comfort.apply_motion(qapp)
    assert not qapp.isEffectEnabled(Qt.UIEffect.UI_AnimateMenu)
    settings().setValue("ui/reduce_motion", "false")
    comfort.apply_motion(qapp)
    before = qapp.font().pointSize()
    settings().setValue("ui/font_pt", before + 2)
    comfort.apply_font(qapp)
    assert qapp.font().pointSize() == before + 2
    settings().setValue("ui/font_pt", before)
    comfort.apply_font(qapp)


# --- for manga work (UI-C) ----------------------------------------------------------------------------------------


def test_a_request_from_the_agent_brings_the_approval_box_forward(qapp, tmp_path):
    import sys

    from PySide6.QtWidgets import QDockWidget, QLabel

    from genko.app.main import MainWindow

    sys.path.insert(0, str(Path(__file__).parent))
    import test_m7_gui as m7

    agent, project = m7._project(tmp_path)
    window = MainWindow(project)
    window.resize(1500, 950)
    window.show()
    qapp.processEvents()
    window.show_dock("レイヤー")  # (the approval box behind another panel)
    window.canvas_only.toggle()
    agent.ask_human("demo.genko", "3 ページの背景をどうするか", page=3, item="gen_panel")
    window._on_disk_change(str(project / "project.json"))  # what the file watcher calls
    qapp.processEvents()
    box = window.approvals
    assert not window.canvas_only.on
    dock = next(d for d in window.findChildren(QDockWidget) if d.windowTitle() == "承認箱")
    assert dock.isVisible() and window._dock_visible(dock)
    assert box.current() is not None and box.current().pages == [3]
    notes = [n.text() for n in window.canvas.findChildren(QLabel) if n.text().startswith("承認の依頼が届きました")]
    assert notes
    window.close()


def test_a_phone_screen_over_a_vertical_scroll_book(qapp, tmp_path):
    from genko.app.main import MainWindow

    project = tmp_path / "w.genko"
    save_episode(new_episode("縦", 1, 1, PageSpec.webtoon()), project)
    win = MainWindow(project)
    win.show()
    qapp.processEvents()
    assert win.act_phone.isChecked() and win.canvas.phone_view  # (a tall strip: on without being asked)
    screens = win.canvas.phone_screens()
    assert len(screens) == 2 and abs(screens[0] - screens[1] + 80 * 844 / 390) < 0.01
    win.canvas._hover = (40.0, 100.0)
    win.canvas.grab()  # (drawn without trouble)
    win.act_phone.trigger()
    assert not win.canvas.phone_view
    win.close()


def test_the_phone_screen_stays_off_for_a_printed_page(window):
    assert not window.act_phone.isChecked() and not window.canvas.phone_view
