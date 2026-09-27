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


def _alone(qapp) -> None:
    """Earlier tests leave their windows alive; a new look re-polishes every one of them, so clear them first."""
    from PySide6.QtCore import QCoreApplication, QEvent

    for widget in qapp.topLevelWidgets():
        widget.close()
        widget.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    qapp.processEvents()


# --- the look ---------------------------------------------------------------------------------------------------


def test_light_and_dark_and_the_brightness_step(qapp):
    from genko.app import theme
    from genko.app.preferences import settings

    _alone(qapp)
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
    from PySide6.QtGui import QIcon

    on = (32, 32, QIcon.Mode.Normal, QIcon.State.On)  # (the tool in hand: its picture in the accent, or grey)
    settings().setValue("ui/mono_icons", "true")
    grey = icons.icon("pen").pixmap(*on).toImage()
    settings().setValue("ui/mono_icons", "false")
    colour = icons.icon("pen").pixmap(*on).toImage()
    assert icons.icon("pen").pixmap(32, 32).toImage() != colour  # (not in hand: the plain ink)
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

    _alone(qapp)
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


# --- the finish (D1–D4) --------------------------------------------------------------------------------------


def test_the_bundled_letters_and_their_three_steps(qapp):
    from genko.app import theme
    from genko.app.preferences import settings

    theme.apply(qapp)
    assert theme.ui_family() and "Plex" in theme.ui_family() and qapp.font().family() == theme.ui_family()
    sheet = qapp.styleSheet()
    assert 'QLabel[role="section"]' in sheet and 'QLabel[role="title"] { font-weight: 700' in sheet
    settings().setValue("ui/ui_font", "system")
    theme.apply(qapp)
    assert qapp.font().family() != theme.ui_family()
    settings().setValue("ui/ui_font", "genko")
    theme.apply(qapp)
    assert qapp.font().family() == theme.ui_family()


def test_the_chosen_row_is_a_clean_grey(qapp):
    from genko.app import theme

    for which in ("light", "dark"):
        chosen = theme.QColor(theme.tokens(which, 0).selected)
        assert chosen.hslSaturation() < 60 and not 0 <= chosen.hslHue() <= 60  # (a cool grey, not a muddy orange)


def test_the_top_bar_is_pictures_and_the_panel_is_rows(window):
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QLabel, QPushButton

    assert window.command_bar.toolButtonStyle() == Qt.ToolButtonStyle.ToolButtonIconOnly
    assert all(not a.icon().isNull() for a in window.command_bar.actions() if not a.isSeparator())
    window.act_frame.trigger()
    page = window.tool_settings.stack.currentWidget()
    sections = [label.text() for label in page.findChildren(QLabel) if label.property("role") == "section"]
    assert {"割る", "枠線", "形"} <= set(sections)
    rows = [b for b in page.findChildren(QPushButton) if b.property("row")]
    assert rows and len({b.iconSize().width() for b in rows}) == 1
    window.act_split_h.setEnabled(False)
    assert not next(b for b in rows if b.text().startswith("横に割る")).isEnabled()
    window.act_split_h.setEnabled(True)


def test_layers_have_eyes_and_even_rows(window):
    panel = window.layers
    panel.refresh()
    assert panel.list.objectName() == "layerList" and "layerList::indicator:checked" in window.styleSheet() + \
        __import__("PySide6.QtWidgets", fromlist=["QApplication"]).QApplication.instance().styleSheet()
    heights = {panel.list.visualItemRect(panel.list.item(i)).height() for i in range(panel.list.count())}
    assert len(heights) == 1


def test_the_rarely_used_panels_wait_in_the_window_menu(window):
    assert not window.sub_dock.isVisible() and not window.timeline_dock.isVisible()
    window.show_dock("タイムライン")
    assert window.timeline_dock.isVisible()


def test_the_icons_are_lucide_with_a_few_of_genkos_own(qapp):
    from genko.app import icons

    assert all((icons.LUCIDE_DIR / f"{file}.svg").exists() for file in icons.LUCIDE.values())
    assert (icons.LUCIDE_DIR / "LICENSE").read_text().startswith("ISC License")
    for name in ("pen", "text", "effect", "gradient", "kind_tone"):
        image = icons.icon(name).pixmap(32, 32).toImage()
        assert any(image.pixelColor(x, y).alpha() for x in range(32) for y in range(32)), name


def test_the_layer_settings_fold_away(window):
    panel = window.layers
    assert not panel.details.isVisible() and panel.details_toggle.text().startswith("▸")
    panel.details_toggle.click()
    assert panel.details.isVisible() and panel.details_toggle.text().startswith("▾")
    panel.details_toggle.click()


def test_a_question_makes_the_reply_the_main_button(qapp, tmp_path):
    import sys

    from genko.app.main import MainWindow

    sys.path.insert(0, str(Path(__file__).parent))
    import test_m7_gui as m7

    _agent, project = m7._project(tmp_path)
    window = MainWindow(project)
    window.show()
    qapp.processEvents()
    box = window.approvals
    kinds = [i.kind for i in box.items]
    box.list.setCurrentRow(kinds.index("help"))
    assert box.back_button.property("primary") and not box.approve_button.property("primary")
    box.list.setCurrentRow(kinds.index("gate"))
    assert box.approve_button.property("primary") and not box.back_button.property("primary")
    window.close()


# --- UI-F ---------------------------------------------------------------------------------------------------


def test_the_brush_list_shows_each_brushs_line(window):
    brush = window.brush
    icons = [brush.kinds.item(i).icon() for i in range(brush.kinds.count())]
    assert icons and all(not icon.isNull() for icon in icons)
    image = icons[0].pixmap(72, 20).toImage()
    assert any(image.pixelColor(x, 10).alpha() for x in range(image.width()))  # (a line runs across)


def test_the_tool_settings_fold_to_the_tools_name(window, qapp):
    ts = window.tool_settings
    window.act_frame.trigger()
    ts.fold.click()
    qapp.processEvents()
    assert ts.folded and not ts.stack.isVisible() and ts.title.text().startswith("コマ割り")
    assert window.brush_dock.maximumHeight() < 100
    ts.fold.click()
    qapp.processEvents()
    assert ts.stack.isVisible() and window.brush_dock.maximumHeight() > 10000


def test_an_empty_lines_list_says_what_goes_there(window):
    notes = [label for label in window.story.list.viewport().findChildren(QLabel_()) if "台詞がありません" in label.text()]
    assert notes and notes[0].isVisibleTo(window.story.list)


def test_the_layers_top_rows_only_when_needed(window):
    panel = window.layers
    panel.refresh()
    assert not panel.search.isVisibleTo(panel) and not panel.target.isVisibleTo(panel)


def QLabel_():  # noqa: N802
    from PySide6.QtWidgets import QLabel

    return QLabel


# --- stage 1: motion and the system's glass -------------------------------------------------------------------


def test_zoom_glides_but_the_view_is_already_there(window, qapp):
    from genko.app.preferences import settings

    canvas = window.canvas
    before = canvas._scale
    window.act_zoom_in.trigger()
    assert abs(canvas._scale - before * 1.25) < 1e-6  # (what the pen touches is the new view at once)
    assert canvas._glide is not None and canvas._glide_motion.state() == canvas._glide_motion.State.Running
    canvas.grab()  # (drawn part way without trouble)
    canvas._glide_motion.setCurrentTime(170)
    assert canvas._glide is None
    settings().setValue("ui/reduce_motion", "true")
    window.act_fit.trigger()
    assert getattr(canvas, "_glide", None) is None  # (fewer moving things: straight there)
    settings().setValue("ui/reduce_motion", "false")


def test_a_page_let_go_slides_to_a_stop(window, qapp):
    import time

    from PySide6.QtCore import QPointF

    canvas = window.canvas
    canvas._start_pan(QPointF(100, 100))
    canvas._pan_speed = QPointF(12, 0)
    canvas._pan_time = time.monotonic()
    canvas._panning = False
    x = canvas._pan_x
    canvas._coast()
    for _ in range(5):
        canvas._coast_step()
    assert canvas._pan_x > x + 30
    for _ in range(60):
        canvas._coast_step()
    assert not canvas._coast_timer.isActive()


def test_the_round_menu_opens_outward(window, qapp):
    from PySide6.QtCore import QPointF

    window.act_pen.trigger()
    window._context_menu("", QPointF(400, 300))
    menu = window.radial
    assert menu.grow < 1.0 and menu.property("glass_wanted") and not menu.mask().isEmpty()
    menu._opening.setCurrentTime(140)
    assert menu.grow == 1.0
    menu.grab()
    menu.close()


def test_the_system_glass_only_where_the_system_has_it(qapp):
    import sys

    from PySide6.QtWidgets import QMenu

    from genko.app import glass

    if sys.platform.startswith("linux"):
        assert glass.available() == "" and not glass.install(qapp)
    menu = QMenu()
    assert glass.wants(menu)
    assert glass.apply(menu) is False or glass.available()


# --- stage 2: the page on the graphics card -----------------------------------------------------------------------


def test_the_gpu_canvas_is_the_same_canvas_on_opengl(qapp):
    from PySide6.QtOpenGLWidgets import QOpenGLWidget

    from genko.app import canvas as cv

    gpu = cv._gpu_class()
    assert issubclass(gpu, QOpenGLWidget) and "paintGL" in gpu.__dict__ and "paintEvent" not in gpu.__dict__
    made = gpu()
    got = []
    made.changed.connect(lambda: got.append(True))
    made.changed.emit()
    assert got and made._QtBase is QOpenGLWidget and hasattr(made, "glide") and hasattr(made, "fit_page")


def test_the_page_is_drawn_by_the_processor_where_there_is_no_real_card(qapp):
    from genko.app import canvas as cv

    assert not cv.gpu_available()  # (the offscreen test screen)
    assert type(cv.make_canvas()).__name__ == "PageCanvas"
    for name in ("llvmpipe (LLVM 17.0.6, 256 bits)", "Microsoft Basic Render Driver", "Software Rasterizer"):
        assert cv.software_renderer(name)
    for name in ("NVIDIA GeForce RTX 4070/PCIe/SSE2", "AMD Radeon Pro 5500M OpenGL Engine", "Intel(R) Iris(R) Xe Graphics",
                 "Apple M2"):
        assert not cv.software_renderer(name)
