"""The screen's look (UI-A): one set of colours in two lights, calm neutral greys with a single accent, used by the
palette, one style sheet and the icons, so every panel looks of a piece.

- mode: system (the computer's light or dark, followed as it changes), light or dark
- brightness: -2 … +2 steps on the greys (like the interface brightness of CLIP STUDIO or Photoshop's four greys)
- surround: the canvas's surround, a neutral grey (auto follows the light) so it does not sway how the page's
  greys look
- mono icons: the icons in grey only (no accent colour competing with the art)
- hints: the panels' explanations shown, or kept in tooltips (off: a quieter screen)
"""

from __future__ import annotations

from dataclasses import dataclass, replace

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QGuiApplication, QPalette
from PySide6.QtWidgets import QApplication, QWidget

MODES = [("パソコンの設定のまま", "system"), ("明るい", "light"), ("暗い", "dark")]


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
    accent_soft: str  # the chosen row's background
    danger: str
    ok: str
    surround: str    # around the page on the canvas


LIGHT = Tokens(False, window="#e3e4e6", panel="#eeeff1", base="#f8f8f9", raised="#f4f5f6", hover="#e2e4e8",
               border="#c9ccd1", divider="#d8dade", text="#1f2124", muted="#62666d", faint="#a3a7ad",
               accent="#d9601f", accent_text="#ffffff", accent_soft="#f6dccb", danger="#c0392b", ok="#2f7d4f",
               surround="#9ea0a4")
DARK = Tokens(True, window="#232427", panel="#2b2c30", base="#1f2023", raised="#34363a", hover="#3b3d42",
              border="#44464c", divider="#393b40", text="#dcdde0", muted="#9a9ea6", faint="#62656b",
              accent="#e07a45", accent_text="#1b1b1d", accent_soft="#4a3429", danger="#e0685a", ok="#6cc18f",
              surround="#3a3b3e")
GREYS = ("window", "panel", "base", "raised", "hover", "border", "divider", "surround")


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


def style_sheet(t: Tokens) -> str:
    """One style sheet for every panel: flat surfaces, thin dividers, small radii, one accent."""
    return f"""
QMainWindow, QDialog {{ background: {t.window}; }}
QMainWindow::separator {{ background: {t.window}; width: 3px; height: 3px; }}
QMainWindow::separator:hover {{ background: {t.accent_soft}; }}
QDockWidget {{ color: {t.text}; titlebar-close-icon: none; }}
QDockWidget::title {{ background: {t.panel}; padding: 5px 8px; border-bottom: 1px solid {t.divider}; text-align: left; }}
QDockWidget > QWidget {{ background: {t.panel}; }}
QWidget#panelBody {{ background: {t.panel}; }}
QTabWidget::pane {{ border: none; border-top: 1px solid {t.divider}; background: {t.panel}; }}
QTabBar {{ qproperty-drawBase: 0; }}
QTabBar::tab {{ background: transparent; color: {t.muted}; padding: 4px 7px; border: none; border-bottom: 2px solid transparent; }}
QTabBar::tab:hover {{ color: {t.text}; }}
QTabBar::tab:selected {{ color: {t.text}; border-bottom: 2px solid {t.accent}; }}
QPushButton {{ background: {t.raised}; color: {t.text}; border: 1px solid {t.border}; border-radius: 4px; padding: 3px 6px; }}
QPushButton:hover {{ background: {t.hover}; }}
QPushButton:pressed, QPushButton:checked {{ background: {t.accent_soft}; border-color: {t.accent}; }}
QPushButton:disabled {{ color: {t.faint}; background: {t.panel}; border-color: {t.divider}; }}
QPushButton[primary="true"] {{ background: {t.accent}; color: {t.accent_text}; border-color: {t.accent}; font-weight: 600; }}
QPushButton[primary="true"]:hover {{ background: {_shift(t.accent, 1)}; }}
QPushButton[primary="true"]:disabled {{ background: {t.panel}; color: {t.faint}; border-color: {t.divider}; }}
QToolButton {{ background: transparent; color: {t.text}; border: 1px solid transparent; border-radius: 5px; padding: 3px; }}
QToolButton:hover {{ background: {t.hover}; }}
QToolButton:checked, QToolButton:pressed {{ background: {t.accent_soft}; border-color: {t.accent}; }}
QToolButton[panel="true"] {{ background: {t.raised}; border: 1px solid {t.border}; padding: 3px 6px; }}
QToolButton[panel="true"]:hover {{ background: {t.hover}; }}
QToolBar {{ background: {t.window}; border: none; spacing: 1px; padding: 1px; }}
QToolBar::separator {{ background: {t.divider}; width: 1px; height: 1px; margin: 4px 6px; }}
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background: {t.base}; color: {t.text}; border: 1px solid {t.border}; border-radius: 4px; padding: 1px 3px;
    selection-background-color: {t.accent}; selection-color: {t.accent_text}; }}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{ border-color: {t.accent}; }}
QComboBox::drop-down {{ border: none; width: 18px; }}
QComboBox QAbstractItemView {{ background: {t.base}; border: 1px solid {t.border}; selection-background-color: {t.accent_soft};
    selection-color: {t.text}; }}
QListWidget, QListView, QTreeWidget, QTreeView, QTableWidget, QTableView {{
    background: {t.base}; color: {t.text}; border: 1px solid {t.divider}; border-radius: 6px; outline: none; }}
QListWidget::item, QTreeWidget::item {{ padding: 3px 4px; border-radius: 4px; }}
QListWidget::item:hover, QTreeWidget::item:hover {{ background: {t.hover}; }}
QListWidget::item:selected, QTreeWidget::item:selected {{ background: {t.accent_soft}; color: {t.text}; }}
QHeaderView::section {{ background: {t.panel}; color: {t.muted}; border: none; border-bottom: 1px solid {t.divider}; padding: 3px 6px; }}
QScrollBar:vertical {{ background: transparent; width: 9px; margin: 1px; }}
QScrollBar:horizontal {{ background: transparent; height: 9px; margin: 1px; }}
QScrollBar::handle {{ background: {t.border}; border-radius: 3px; min-height: 24px; min-width: 24px; }}
QScrollBar::handle:hover {{ background: {t.muted}; }}
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {{ background: none; width: 0; height: 0; }}
QSlider::groove:horizontal {{ height: 4px; background: {t.border}; border-radius: 2px; }}
QSlider::sub-page:horizontal {{ background: {t.accent}; border-radius: 2px; }}
QSlider::handle:horizontal {{ background: {t.raised}; border: 1px solid {t.border}; width: 14px; margin: -6px 0; border-radius: 7px; }}
QGroupBox {{ border: 1px solid {t.divider}; border-radius: 6px; margin-top: 10px; padding-top: 6px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 8px; padding: 0 4px; color: {t.muted}; }}
QMenuBar {{ background: {t.window}; color: {t.text}; }}
QMenuBar::item:selected {{ background: {t.hover}; border-radius: 4px; }}
QMenu {{ background: {t.panel}; color: {t.text}; border: 1px solid {t.border}; border-radius: 6px; padding: 4px; }}
QMenu::item {{ padding: 4px 22px 4px 18px; border-radius: 4px; }}
QMenu::item:selected {{ background: {t.accent_soft}; color: {t.text}; }}
QMenu::item:disabled {{ color: {t.faint}; }}
QMenu::separator {{ height: 1px; background: {t.divider}; margin: 4px 8px; }}
QToolTip {{ background: {t.raised}; color: {t.text}; border: 1px solid {t.border}; border-radius: 4px; padding: 4px 6px; }}
QStatusBar {{ background: {t.window}; color: {t.muted}; border-top: 1px solid {t.divider}; }}
QStatusBar QLabel {{ color: {t.muted}; }}
QSplitter::handle {{ background: {t.divider}; }}
QLabel[role="hint"] {{ color: {t.muted}; }}
QLabel[role="empty"] {{ color: {t.muted}; padding: 16px; }}
QLabel[role="error"] {{ color: {t.danger}; }}
QLabel[role="title"] {{ font-weight: 600; }}
QLabel[role="section"] {{ color: {t.muted}; font-weight: 600; padding-top: 4px; }}
QLabel[role="badge"] {{ border-radius: 8px; padding: 1px 8px; background: {t.hover}; color: {t.text}; }}
QLabel[role="badge-warn"] {{ border-radius: 8px; padding: 1px 8px; background: {t.accent_soft}; color: {t.text}; }}
QLabel[role="badge-ok"] {{ border-radius: 8px; padding: 1px 8px; background: {t.hover}; color: {t.ok}; }}
QWidget#launcher {{ background: {t.panel}; border: 1px solid {t.border}; border-radius: 6px; }}
QWidget#startCard {{ background: {t.panel}; border: 1px solid {t.divider}; border-radius: 8px; }}
"""


def apply(app: QApplication | None = None, which: str | None = None) -> Tokens:
    """Put the look on the whole application (Fusion draws the same on every system, so the colours hold)."""
    app = app or QApplication.instance()
    t = tokens(which)
    if app is None:
        return t
    if getattr(app, "_genko_tokens", None) == t:  # (unchanged: re-styling every open widget again is slow)
        return t
    if app.style().objectName().lower() != "fusion":
        app.setStyle("Fusion")
    app.setPalette(palette(t))
    app.setStyleSheet(style_sheet(t))
    app._genko_tokens = t
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

    if tip or (text is not None and button.text() and text != button.text()):
        button.setToolTip(tip or button.text())
    if text is not None:
        button.setText(text)
    button.setIcon(icon(name))
    _ICONED.append((button, name))
    return button


def refresh_icons() -> None:
    from genko.app.icons import icon

    alive = []
    for button, name in _ICONED:
        try:
            button.setIcon(icon(name))
        except RuntimeError:
            continue
        alive.append((button, name))
    _ICONED[:] = alive
