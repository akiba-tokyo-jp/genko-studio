"""How number fields and choice lists take input everywhere in the app: a number field entered is all selected
(typing replaces it, so "-10" or "4" goes in as typed), and the mouse wheel changes a field or a list only when
it is the one being used (scrolling a dialog past it leaves it alone)."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, Qt, QTimer
from PySide6.QtWidgets import QAbstractSpinBox, QApplication, QComboBox, QSlider


class _Inputs(QObject):
    def eventFilter(self, watched, event) -> bool:  # noqa: N802
        kind = event.type()
        if kind == QEvent.Type.FocusIn and isinstance(watched, QAbstractSpinBox):
            reason = event.reason() if hasattr(event, "reason") else None
            if reason in (Qt.FocusReason.MouseFocusReason, Qt.FocusReason.TabFocusReason, Qt.FocusReason.BacktabFocusReason,
                          Qt.FocusReason.ShortcutFocusReason, Qt.FocusReason.OtherFocusReason):
                QTimer.singleShot(0, watched.selectAll)
        elif kind == QEvent.Type.Wheel and isinstance(watched, (QAbstractSpinBox, QComboBox, QSlider)):
            if not watched.hasFocus():
                event.ignore()  # (the wheel goes on to what holds the field: the dialog scrolls)
                return True
        return False


_filter: _Inputs | None = None


def install() -> None:
    """Once per app: number fields select their value when entered; the wheel leaves unused fields alone."""
    global _filter
    app = QApplication.instance()
    if app is None or _filter is not None:
        return
    _filter = _Inputs(app)
    app.installEventFilter(_filter)
