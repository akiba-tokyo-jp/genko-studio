"""How number fields and choice lists take input everywhere in the app: a number field entered is all selected
(typing replaces it, so "-10" or "4" goes in as typed), and the mouse wheel changes a field or a list only when
it is the one being used (scrolling a dialog past it leaves it alone).

The filter sits on those widgets only, not on the whole app: an app-wide filter would run Python for every
event of every widget (paints, mouse moves), which slowed drawing on a busy page. Widgets are found when a
window comes forward or the focus moves (watch() adds one by hand)."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QTimer
from PySide6.QtWidgets import QAbstractSpinBox, QApplication, QComboBox, QSlider, QWidget

_KINDS = (QAbstractSpinBox, QComboBox, QSlider)
_FOCUS_IN, _WHEEL = QEvent.Type.FocusIn, QEvent.Type.Wheel


class _Inputs(QObject):
    def eventFilter(self, watched, event) -> bool:  # noqa: N802
        kind = event.type()
        if kind == _FOCUS_IN and isinstance(watched, QAbstractSpinBox):
            QTimer.singleShot(0, watched.selectAll)
        elif kind == _WHEEL and not watched.hasFocus():
            event.ignore()  # (the wheel goes on to what holds the field: the dialog scrolls)
            return True
        return False


_filter: _Inputs | None = None


def watch(widget: QWidget) -> None:
    """The fields in `widget` (and itself) take input the app's way; each is set up once."""
    if _filter is None or widget is None:
        return
    found = [widget] if isinstance(widget, _KINDS) else []
    for kind in _KINDS:
        found += widget.findChildren(kind)
    for field in found:
        if not field.property("genko_inputs"):
            field.setProperty("genko_inputs", True)
            field.installEventFilter(_filter)


def _scan(*_args) -> None:
    app = QApplication.instance()
    focus = app.focusWidget() if app else None
    if focus is not None:
        watch(focus.window())
    active = app.activeWindow() if app else None
    if active is not None and (focus is None or active is not focus.window()):
        watch(active)


def install() -> None:
    """Once per app: number fields select their value when entered; the wheel leaves unused fields alone."""
    global _filter
    app = QApplication.instance()
    if app is None or _filter is not None:
        return
    _filter = _Inputs(app)
    app.focusChanged.connect(_scan)
    app.focusWindowChanged.connect(_scan)
    for top in app.topLevelWidgets():
        watch(top)
