"""The screen's look (UI-A): one set of colours in two lights, calm neutral greys with a single accent, used by the
palette, one style sheet and the icons, so every panel looks of a piece.

- mode: system (the computer's light or dark, followed as it changes), light or dark
- brightness: -2 … +2 steps on the greys (like the interface brightness of CLIP STUDIO or Photoshop's four greys)
- surround: the canvas's surround, a neutral grey (auto follows the light) so it does not sway how the page's
  greys look
- mono icons: the icons in grey only (no accent colour competing with the art)
- hints: the panels' explanations shown, or kept in tooltips (off: a quieter screen)
- letters (D4): IBM Plex Sans JP (bundled, SIL OFL) on every system, or the computer's own; four steps:
  見出し (a panel's or a tool's name: larger and bold; a dialog's title larger still), 本文 (labels and values),
  補足 (explanations and section names: quiet grey, never below MIN_PT) and 数字 (counts: bold, in the text colour)
"""

from __future__ import annotations

from dataclasses import dataclass, replace

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication, QPalette
from PySide6.QtWidgets import QApplication, QWidget

MODES = [("パソコンの設定のまま", "system"), ("明るい", "light"), ("暗い", "dark")]
MIN_PT = 9.0  # (the smallest letters on the screen: about 12 px at 100 %)
MIN_TARGET = 24  # (the smallest thing to click, in px)


@dataclass(frozen=True)
class Tokens:
    dark: bool
    window: str      # the frame behind the panels
    panel: str       # a panel's face
    base: str        # fields and lists
    raised: str      # buttons
    hover: str
    border: str
    divider: str
    text: str
    muted: str       # explanations and secondary labels
    faint: str       # disabled
    accent: str      # the one colour: the current tool, the chosen item, the main button
    accent_text: str
    accent_soft: str  # a soft accent wash (warnings, a divider being dragged)
    selected: str    # the chosen row or tool: a clean neutral (an orange tint on a dark grey turns muddy)
    danger: str
    ok: str
    surround: str    # around the page on the canvas


LIGHT = Tokens(False, window="#e3e4e6", panel="#eeeff1", base="#f8f8f9", raised="#f4f5f6", hover="#e2e4e8",
               border="#c9ccd1", divider="#d8dade", text="#1f2124", muted="#62666d", faint="#a3a7ad",
               accent="#d9601f", accent_text="#ffffff", accent_soft="#f6dccb", selected="#d9dde4", danger="#c0392b",
               ok="#2f7d4f", surround="#9ea0a4")
DARK = Tokens(True, window="#232427", panel="#2b2c30", base="#1f2023", raised="#34363a", hover="#3b3d42",
              border="#44464c", divider="#393b40", text="#dcdde0", muted="#9a9ea6", faint="#62656b",
              accent="#e07a45", accent_text="#1b1b1d", accent_soft="#4a3429", selected="#41444b", danger="#e0685a",
              ok="#6cc18f", surround="#3a3b3e")
GREYS = ("window", "panel", "base", "raised", "hover", "border", "divider", "surround", "selected")


def _settings():
    from genko.app.preferences import settings

    return settings()


def mode() -> str:
    value = str(_settings().value("ui/theme", "system") or "system")
    return value if value in dict((k, 1) for _l, k in MODES) else "system"


def brightness() -> int:
    try:
        return max(-2, min(2, int(_settings().value("ui/brightness", 0))))
    except (TypeError, ValueError):
        return 0


def mono_icons() -> bool:
    return str(_settings().value("ui/mono_icons", "false")).lower() in ("1", "true", "yes")


def show_hints() -> bool:
    return str(_settings().value("ui/hints", "false")).lower() in ("1", "true", "yes")


def surround_setting() -> str:
    """"auto" or a grey level 0…255."""
    value = str(_settings().value("ui/surround", "auto") or "auto")
    return value if value == "auto" or value.isdigit() else "auto"


FONT_CHOICES = [("Genko の標準（IBM Plex Sans JP）", "genko"), ("パソコンの書体", "system")]


def font_choice() -> str:
    value = str(_settings().value("ui/ui_font", "genko") or "genko")
    return value if value in ("genko", "system") else "genko"


_FAMILY: list = []


def ui_family() -> str | None:
    """The bundled screen letters, registered once (None when they cannot be loaded)."""
    if not _FAMILY:
        from pathlib import Path

        family = None
        folder = Path(__file__).resolve().parent.parent / "fonts" / "ui"
        for path in sorted(folder.glob("*.ttf")):
            found = QFontDatabase.applicationFontFamilies(QFontDatabase.addApplicationFont(str(path)))
            family = family or (found[0] if found else None)
        _FAMILY.append(family)
    return _FAMILY[0]


_SYSTEM_FONT: list = []


def apply_font_family(app: QApplication) -> None:
    """The screen's letters: the bundled family (the same on every computer) or the computer's own."""
    if not _SYSTEM_FONT:
        _SYSTEM_FONT.append(QFont(app.font()))
    family = ui_family() if font_choice() == "genko" else None
    font = QFont(app.font())
    target = family or _SYSTEM_FONT[0].family()
    if font.family() != target:
        font.setFamily(target)
        font.setHintingPreference(QFont.HintingPreference.PreferNoHinting if family else font.hintingPreference())
        app.setFont(font)


def base_pt(app: QApplication | None = None) -> float:
    app = app or QApplication.instance()
    size = app.font().pointSizeF() if app is not None else -1
    return size if size > 0 else 9.0


def system_dark() -> bool:
    app = QGuiApplication.instance()
    if app is None:
        return False
    try:
        return app.styleHints().colorScheme() == Qt.ColorScheme.Dark
    except AttributeError:  # (an older Qt: the palette the system gave)
        return app.palette().color(QPalette.ColorRole.Window).lightness() < 128


def is_dark(which: str | None = None) -> bool:
    which = which or mode()
    return system_dark() if which == "system" else which == "dark"


def _shift(hex_colour: str, step: int) -> str:
    colour = QColor(hex_colour)
    h, s, lightness, a = colour.getHsl()
    colour.setHsl(h, s, max(0, min(255, lightness + step * 7)), a)
    return colour.name()


def tokens(which: str | None = None, step: int | None = None) -> Tokens:
    """The colours now: the light or dark set, its greys moved by the brightness, the surround as chosen."""
    base = DARK if is_dark(which) else LIGHT
    step = brightness() if step is None else step
    moved = replace(base, **{name: _shift(getattr(base, name), step) for name in GREYS}) if step else base
    chosen = surround_setting()
    if chosen != "auto":
        level = max(0, min(255, int(chosen)))
        moved = replace(moved, surround=QColor(level, level, level).name())
    return moved


def accent() -> QColor:
    """The accent now, for drawing on the canvas (from the look put on the application: no settings read, so a
    paint stays quick)."""
    app = QApplication.instance()
    key = getattr(app, "_genko_tokens", None) if app is not None else None
    return QColor(key[0].accent if key else LIGHT.accent)


def surround() -> QColor:
    return QColor(tokens().surround)


def palette(t: Tokens) -> QPalette:
    p = QPalette()
    roles = {
        QPalette.ColorRole.Window: t.window, QPalette.ColorRole.WindowText: t.text, QPalette.ColorRole.Base: t.base,
        QPalette.ColorRole.AlternateBase: t.panel, QPalette.ColorRole.ToolTipBase: t.raised, QPalette.ColorRole.ToolTipText: t.text,
        QPalette.ColorRole.PlaceholderText: t.faint, QPalette.ColorRole.Text: t.text, QPalette.ColorRole.Button: t.raised,
        QPalette.ColorRole.ButtonText: t.text, QPalette.ColorRole.BrightText: t.danger, QPalette.ColorRole.Highlight: t.accent,
        QPalette.ColorRole.HighlightedText: t.accent_text, QPalette.ColorRole.Link: t.accent, QPalette.ColorRole.Mid: t.border,
        QPalette.ColorRole.Midlight: t.hover, QPalette.ColorRole.Dark: t.divider, QPalette.ColorRole.Light: t.base,
    }
    for role, value in roles.items():
        p.setColor(role, QColor(value))
    for role in (QPalette.ColorRole.Text, QPalette.ColorRole.ButtonText, QPalette.ColorRole.WindowText):
        p.setColor(QPalette.ColorGroup.Disabled, role, QColor(t.faint))
    return p


def _rgba(hex_colour: str, alpha: float) -> str:
    colour = QColor(hex_colour)
    return f"rgba({colour.red()}, {colour.green()}, {colour.blue()}, {alpha:g})"


def style_sheet(t: Tokens, pt: float = 9.0) -> str:
    """One style sheet for every panel: flat surfaces, thin dividers, small radii, one accent; the four steps of
    letters (見出し・本文・補足・数字: the module's note)."""
    big, head, small = f"{pt + 5:g}pt", f"{pt + 2:g}pt", f"{max(MIN_PT, pt - 1):g}pt"
    return f"""
QMainWindow, QDialog {{ background: {t.window}; }}
QMainWindow::separator {{ background: {t.window}; width: 3px; height: 3px; }}
QMainWindow::separator:hover {{ background: {t.accent_soft}; }}
QDockWidget {{ color: {t.text}; titlebar-close-icon: none; }}
QDockWidget::title {{ background: {t.panel}; padding: 5px 8px; border-bottom: 1px solid {t.divider}; text-align: left; font-weight: 500; }}
QDockWidget > QWidget {{ background: {t.panel}; }}
QWidget#panelBody {{ background: {t.panel}; }}
QTabWidget::pane {{ border: none; border-top: 1px solid {t.divider}; background: {t.panel}; }}
QTabBar {{ qproperty-drawBase: 0; }}
QTabBar::tab {{ background: transparent; color: {t.muted}; padding: 6px 8px; border: none; border-bottom: 2px solid transparent; }}
QTabBar::tab:hover {{ color: {t.text}; }}
QTabBar::tab:selected {{ color: {t.text}; border-bottom: 2px solid {t.accent}; font-weight: 500; }}
QPushButton {{ background: {t.raised}; color: {t.text}; border: 1px solid {t.border}; border-radius: 6px; padding: 4px 8px; }}
QPushButton:hover {{ background: {t.hover}; }}
QPushButton:pressed, QPushButton:checked {{ background: {t.selected}; border-color: {t.accent}; }}
QPushButton:disabled {{ color: {t.faint}; background: {t.panel}; border-color: {t.divider}; }}
QPushButton[primary="true"] {{ background: {t.accent}; color: {t.accent_text}; border-color: {t.accent}; font-weight: 600; }}
QPushButton[primary="true"]:hover {{ background: {_shift(t.accent, 1)}; }}
QPushButton[primary="true"]:disabled {{ background: {t.panel}; color: {t.faint}; border-color: {t.divider}; }}
QPushButton[row="true"] {{ background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 6px;
    text-align: left; }}
QPushButton[row="true"]:hover {{ background: {t.hover}; }}
QPushButton[iconbtn="true"] {{ background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 0; }}
QPushButton[iconbtn="true"]:hover {{ background: {t.hover}; }}
QPushButton[iconbtn="true"]:pressed {{ background: {t.selected}; }}
QPushButton[iconbtn="true"]::menu-indicator {{ width: 0; image: none; }}
QPushButton[chip="true"] {{ background: transparent; border: 1px solid {t.divider}; border-radius: 9px; padding: 1px 4px; color: {t.muted}; }}
QPushButton[chip="true"]:hover {{ background: {t.hover}; color: {t.text}; }}
QPushButton[quiet="true"] {{ color: {t.muted}; }}
QWidget#toolPage QCheckBox {{ padding: 4px 0 4px 7px; spacing: 8px; }}
QWidget#toolPage QLabel[role="section"] {{ padding-left: 7px; }}
QPushButton[row="true"]::menu-indicator {{ width: 0; image: none; }}
QPushButton[row="true"]:pressed {{ background: {t.selected}; border-color: {t.selected}; }}
QPushButton[row="true"]:disabled {{ color: {t.faint}; background: transparent; border-color: transparent; }}
QToolButton {{ background: transparent; color: {t.text}; border: 1px solid transparent; border-radius: 5px; padding: 3px; }}
QToolButton:hover {{ background: {t.hover}; }}
QToolButton:checked, QToolButton:pressed {{ background: {t.selected}; border-color: {t.selected}; }}
QToolButton[panel="true"] {{ background: {t.raised}; border: 1px solid {t.border}; padding: 3px 6px; }}
QToolButton[panel="true"]:hover {{ background: {t.hover}; }}
QToolBar {{ background: {t.window}; border: none; spacing: 1px; padding: 1px; }}
QToolBar#commands {{ padding: 2px 4px; spacing: 2px; }}
QToolBar#commands QToolButton {{ padding: 4px; }}
QToolBar::separator {{ background: {t.divider}; width: 1px; height: 1px; margin: 4px 6px; }}
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background: {t.base}; color: {t.text}; border: 1px solid {t.border}; border-radius: 6px; padding: 2px 3px;
    selection-background-color: {t.accent}; selection-color: {t.accent_text}; }}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{ border-color: {t.accent}; }}
QComboBox::drop-down {{ border: none; width: 18px; }}
QSpinBox::up-button, QDoubleSpinBox::up-button {{ subcontrol-origin: border; subcontrol-position: top right; width: 16px;
    border: none; border-left: 1px solid {t.divider}; border-top-right-radius: 4px; background: transparent; }}
QSpinBox::down-button, QDoubleSpinBox::down-button {{ subcontrol-origin: border; subcontrol-position: bottom right; width: 16px;
    border: none; border-left: 1px solid {t.divider}; border-bottom-right-radius: 4px; background: transparent; }}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {{
    background: {t.hover}; }}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {{ image: url({_indicator("caret_up", t.muted)}); width: 10px; height: 10px; }}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {{ image: url({_indicator("caret_down", t.muted)}); width: 10px; height: 10px; }}
QComboBox QAbstractItemView {{ background: {t.base}; border: 1px solid {t.border}; selection-background-color: {t.selected};
    selection-color: {t.text}; }}
QListWidget, QListView, QTreeWidget, QTreeView, QTableWidget, QTableView {{
    background: {t.base}; color: {t.text}; border: 1px solid {t.divider}; border-radius: 6px; outline: none; }}
QListWidget::item, QTreeWidget::item {{ padding: 4px 8px; border-radius: 4px; }}
QListWidget::item:hover, QTreeWidget::item:hover {{ background: {t.hover}; }}
QListWidget::item:selected, QTreeWidget::item:selected {{ background: {t.selected}; color: {t.text}; }}
QHeaderView::section {{ background: {t.panel}; color: {t.muted}; border: none; border-bottom: 1px solid {t.divider}; padding: 3px 6px; }}
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 0; }}
QScrollBar:horizontal {{ background: transparent; height: 10px; margin: 0; }}
QScrollBar::handle {{ background: {t.border}; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 1px; }}
QScrollBar::handle:hover {{ background: {t.muted}; }}
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {{ background: none; width: 0; height: 0; }}
QSlider::groove:horizontal {{ height: 4px; background: {t.border}; border-radius: 2px; }}
QSlider::sub-page:horizontal {{ background: {t.accent}; border-radius: 2px; }}
QSlider::handle:horizontal {{ background: {t.raised}; border: 1px solid {t.border}; width: 18px; margin: -8px 0; border-radius: 9px; }}
QGroupBox {{ border: 1px solid {t.divider}; border-radius: 6px; margin-top: 10px; padding-top: 6px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 8px; padding: 0 4px; color: {t.muted}; }}
QMenuBar {{ background: {t.window}; color: {t.text}; }}
QMenuBar::item:selected {{ background: {t.hover}; border-radius: 4px; }}
QMenu {{ background: {t.panel}; color: {t.text}; border: 1px solid {t.border}; border-radius: 6px; padding: 4px; }}
QMenu::item {{ padding: 4px 22px 4px 18px; border-radius: 4px; }}
QMenu::item:selected {{ background: {t.selected}; color: {t.text}; }}
QMenu::item:disabled {{ color: {t.faint}; }}
QMenu::separator {{ height: 1px; background: {t.divider}; margin: 4px 8px; }}
QToolTip {{ background: {t.raised}; color: {t.text}; border: 1px solid {t.border}; border-radius: 4px; padding: 4px 6px; }}
QStatusBar {{ background: {t.window}; color: {t.muted}; border-top: 1px solid {t.divider}; padding: 0 8px; }}
QStatusBar::item {{ border: none; }}
QStatusBar QLabel {{ color: {t.muted}; }}
QSplitter::handle {{ background: {t.divider}; }}
QLabel[role="hint"] {{ color: {t.muted}; }}
QLabel[role="empty"] {{ color: {t.muted}; padding: 16px; }}
QLabel[role="error"] {{ color: {t.danger}; }}
QLabel[role="title"] {{ font-weight: 700; font-size: {big}; }}
QLabel[role="heading"] {{ font-weight: 700; font-size: {head}; }}
QLabel[role="section"] {{ color: {t.muted}; font-weight: 500; font-size: {small}; padding-top: 8px; padding-bottom: 2px; }}
QLabel[role="label"] {{ color: {t.muted}; }}
QLabel[role="caption"] {{ color: {t.muted}; font-size: {small}; }}
QLabel[role="figure"] {{ color: {t.text}; font-weight: 700; }}
QLabel[role="badge"] {{ border-radius: 8px; padding: 1px 8px; background: {t.hover}; color: {t.text}; }}
QLabel[role="badge-warn"] {{ border-radius: 8px; padding: 1px 8px; background: {t.accent_soft}; color: {t.text}; }}
QLabel[role="badge-ok"] {{ border-radius: 8px; padding: 1px 8px; background: {t.hover}; color: {t.ok}; }}
QTabBar::close-button {{ image: url({_indicator("close", t.muted)}); subcontrol-position: right; border-radius: 4px;
    padding: 1px; margin: 2px; }}
QTabBar::close-button:hover {{ image: url({_indicator("close", t.text)}); background: {t.hover}; }}
QListWidget#layerList::indicator {{ width: 18px; height: 18px; padding: 3px; margin-right: 1px; }}
QListWidget#layerList::indicator:checked {{ image: url({_indicator("eye", t.text)}); }}
QListWidget#layerList::indicator:unchecked {{ image: url({_indicator("eye_off", t.faint)}); }}
QMenu[glass="true"] {{ background: {_rgba(t.panel, 0.62)}; border: 1px solid {_rgba(t.border, 0.7)}; }}
QToolTip[glass="true"], QLabel[glass="true"] {{ background: {_rgba(t.raised, 0.66)}; }}
QDialog[glass="true"] {{ background: {_rgba(t.window, 0.7)}; }}
QFrame#dialogCard {{ background: {t.panel}; border: 1px solid {t.divider}; border-radius: 8px; }}
QWidget#dialogFooter {{ border-top: 1px solid {t.divider}; }}
QWidget#launcher {{ background: {t.panel}; border: 1px solid {t.border}; border-radius: 6px; }}
QLabel#startNotice {{ background: {t.accent_soft}; color: {t.text}; border-radius: 6px; padding: 8px; }}
QWidget#firstSteps {{ background: {t.panel}; border-bottom: 1px solid {t.divider}; }}
QWidget#startCard {{ background: {t.panel}; border: 1px solid {t.divider}; border-radius: 8px; }}
QPushButton#actionCard {{ background: {t.panel}; border: 1px solid {t.divider}; border-radius: 12px; padding: 0; text-align: left; }}
QPushButton#actionCard:hover {{ background: {t.hover}; border-color: {t.border}; }}
QPushButton#actionCard:pressed {{ background: {t.selected}; }}
QPushButton#actionCard[main="true"] {{ border: 2px solid {t.accent}; }}
QListWidget#recentBooks {{ background: transparent; border: none; }}
QListWidget#recentBooks::item, QListWidget#recentBooks::item:hover, QListWidget#recentBooks::item:selected {{ background: transparent; }}
"""


def empty_note(view, text: str):
    """Quiet words in an empty list, saying what will appear there and how (they go when the first row comes)."""
    from PySide6.QtWidgets import QLabel

    note = QLabel(text, view.viewport())
    note.setWordWrap(True)
    note.setAlignment(Qt.AlignmentFlag.AlignCenter)
    role(note, "empty")
    note.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

    def follow(*_args) -> None:
        try:
            note.setGeometry(view.viewport().rect().adjusted(8, 8, -8, -8))
            note.setVisible(view.model().rowCount() == 0)
        except RuntimeError:  # (the list is gone)
            pass

    model = view.model()
    for signal in (model.rowsInserted, model.rowsRemoved, model.modelReset, model.layoutChanged):
        signal.connect(follow)
    view.viewport().installEventFilter(_Resize(view.viewport(), follow))
    follow()
    return note


def japanese(app: QApplication) -> bool:
    """Qt's own words in Japanese (はい・いいえ, the file and colour dialogs, text fields' menus): the screen is
    Japanese whatever language the computer is set to."""
    if getattr(app, "_genko_translator", None) is not None:
        return True
    from PySide6.QtCore import QLibraryInfo, QTranslator

    translator = QTranslator(app)
    if not translator.load("qtbase_ja", QLibraryInfo.path(QLibraryInfo.LibraryPath.TranslationsPath)):
        return False
    app.installTranslator(translator)
    app._genko_translator = translator
    return True


def name_buttons(root: QWidget) -> None:
    """Buttons that show only a picture get their tooltip's first line as their name for screen readers (one pass
    over a window when it is built: no watching of events, so drawing stays as fast)."""
    from PySide6.QtWidgets import QAbstractButton

    for button in root.findChildren(QAbstractButton):
        if not button.text() and not button.accessibleName() and button.toolTip():
            button.setAccessibleName(button.toolTip().split("\n")[0].strip())


class _Resize(QObject):
    """Keeps an empty list's note the size of the list."""

    def __init__(self, parent, callback) -> None:
        super().__init__(parent)
        self.callback = callback

    def eventFilter(self, _obj, event) -> bool:  # noqa: N802
        if event.type() == QEvent.Type.Resize:
            self.callback()
        return False


def still_icon(pixmap):
    """A picture that stays itself when its row is chosen (Qt tints chosen icons with the accent otherwise)."""
    from PySide6.QtGui import QIcon

    icon = QIcon()
    for mode in (QIcon.Mode.Normal, QIcon.Mode.Selected, QIcon.Mode.Active):
        icon.addPixmap(pixmap, mode)
    return icon


def _indicator(name: str, colour: str) -> str:
    """A small picture for a style sheet (the eye beside each layer), kept as a file the sheet can point to."""
    import tempfile
    from pathlib import Path

    from genko.app.icons import _icon

    path = Path(tempfile.gettempdir()) / "genko-ui" / f"{name}_{colour.lstrip('#')}.png"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        _icon(name, colour, colour).pixmap(32, 32).save(str(path))
    return path.as_posix()


def apply(app: QApplication | None = None, which: str | None = None) -> Tokens:
    """Put the look on the whole application (Fusion draws the same on every system, so the colours hold)."""
    app = app or QApplication.instance()
    t = tokens(which)
    if app is None:
        return t
    apply_font_family(app)
    key = (t, base_pt(app), app.font().family())
    if getattr(app, "_genko_tokens", None) == key:  # (unchanged: re-styling every open widget again is slow)
        return t
    if app.style().objectName().lower() != "fusion":
        app.setStyle("Fusion")
    app.setPalette(palette(t))
    app.setStyleSheet(style_sheet(t, base_pt(app)))
    app._genko_tokens = key
    from genko.app import glass

    glass.install(app)  # (the system's frosted glass for menus and the like, where there is one)
    japanese(app)
    if not getattr(app, "_genko_follows_system", False):
        try:
            app.styleHints().colorSchemeChanged.connect(lambda _scheme: mode() == "system" and _refresh_all(app))
            app._genko_follows_system = True
        except AttributeError:
            pass
    return t


def _refresh_all(app: QApplication) -> None:
    apply(app)
    for widget in app.topLevelWidgets():
        refresh = getattr(widget, "refresh_icons", None)
        if callable(refresh):
            refresh()
        widget.update()


# --- panel helpers -------------------------------------------------------------------------------------


def role(widget: QWidget, name: str) -> QWidget:
    """Give a label its part in the look (hint, empty, error, title, section, badge…)."""
    widget.setProperty("role", name)
    style = widget.style()
    style.unpolish(widget)
    style.polish(widget)
    return widget


_HINTS: list = []


def hint(label) -> QWidget:
    """An explanation in a panel: quiet grey, and hidden with its words kept as the panel's tooltip when the
    person turns the explanations off."""
    role(label, "hint")
    _HINTS.append(label)
    _show_hint(label, show_hints())
    return label


def _show_hint(label, on: bool) -> None:
    try:
        label.setVisible(on)
        parent = label.parentWidget()
        if parent is not None and not on and label.text() and not parent.toolTip():
            parent.setToolTip(label.text())
    except RuntimeError:  # (the label was deleted with its panel)
        pass


def set_hints(on: bool) -> None:
    _settings().setValue("ui/hints", "true" if on else "false")
    alive = []
    for label in _HINTS:
        try:
            label.objectName()
        except RuntimeError:
            continue
        _show_hint(label, on)
        alive.append(label)
    _HINTS[:] = alive


def primary(button) -> QWidget:
    """The main button of a panel or dialog, in the accent."""
    return role_prop(button, "primary", True)


def role_prop(widget, key: str, value) -> QWidget:
    widget.setProperty(key, value)
    widget.style().unpolish(widget)
    widget.style().polish(widget)
    return widget


_ICONED: list = []


def iconic(button, name: str, text: str | None = None, tip: str | None = None):
    """A button with its picture (and a shorter label, the words moved to its tooltip)."""
    from genko.app.icons import icon

    if tip or (text is not None and button.text() and text != button.text() and not button.toolTip()):
        button.setToolTip(tip or button.text())  # (a tooltip already written stays: it says more than the label)
    if text is not None:
        button.setText(text)
    button.setIcon(icon(name))
    if not button.text() and button.toolTip():  # (a picture alone: its words for a screen reader)
        button.setAccessibleName(button.toolTip().split("\n")[0].strip())
    _ICONED.append((button, name))
    return button


def refresh_icons() -> None:
    from genko.app.icons import colours, icon

    inks = colours()  # (once: the settings are read for the look, not for every button)
    alive = []
    for button, name in _ICONED:
        try:
            button.setIcon(icon(name, inks))
        except RuntimeError:
            continue
        alive.append((button, name))
    _ICONED[:] = alive
