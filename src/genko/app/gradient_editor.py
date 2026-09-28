"""多色のグラデーション: the colours of a gradient in a row (any number, each with where it sits and how
see-through it is), with ready-made rows (空・夕焼け・虹・セピア) to start from."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtGui import QColor, QLinearGradient, QPainter
from PySide6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QGridLayout,
    QHBoxLayout,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

PRESETS = {
    "空": [[0.0, [70, 130, 210], 1.0], [0.6, [150, 200, 240], 1.0], [1.0, [235, 245, 255], 1.0]],
    "夕焼け": [[0.0, [40, 50, 110], 1.0], [0.45, [210, 90, 90], 1.0], [0.75, [250, 170, 80], 1.0], [1.0, [255, 230, 170], 1.0]],
    "夜明け": [[0.0, [20, 30, 70], 1.0], [0.5, [120, 110, 170], 1.0], [1.0, [250, 200, 180], 1.0]],
    "虹": [[0.0, [230, 60, 60], 1.0], [0.2, [245, 160, 50], 1.0], [0.4, [240, 230, 70], 1.0], [0.6, [80, 190, 90], 1.0],
          [0.8, [70, 120, 220], 1.0], [1.0, [140, 80, 200], 1.0]],
    "セピア": [[0.0, [40, 25, 15], 1.0], [0.5, [150, 110, 70], 1.0], [1.0, [245, 230, 205], 1.0]],
    "黒から透明": [[0.0, [20, 20, 20], 1.0], [1.0, [20, 20, 20], 0.0]],
}


class _Bar(QWidget):
    def __init__(self, editor) -> None:
        super().__init__(editor)
        self.editor = editor
        self.setMinimumHeight(22)

    def paintEvent(self, event) -> None:  # noqa: N802
        p = QPainter(self)
        for y in range(0, self.height(), 6):  # (see-through shows as squares)
            for x in range(0, self.width(), 6):
                p.fillRect(x, y, 6, 6, QColor(210, 210, 210) if (x // 6 + y // 6) % 2 else QColor(245, 245, 245))
        grad = QLinearGradient(0, 0, self.width(), 0)
        for pos, rgb, opacity in self.editor.stops():
            grad.setColorAt(pos, QColor(rgb[0], rgb[1], rgb[2], round(255 * opacity)))
        p.fillRect(self.rect(), grad)
        p.end()


class StopsEditor(QWidget):
    """The colours of a gradient: each a colour, a place (0..100 %) and how strong it is; ＋ adds one."""

    changed = Signal()

    def __init__(self, stops=None, parent=None, opacity: bool = True) -> None:
        super().__init__(parent)
        self.with_opacity = opacity
        self._stops = [list(s) for s in (stops or PRESETS["黒から透明"])]
        self.bar = _Bar(self)
        self.presets = QComboBox()
        self.presets.addItem("見本から選ぶ…", "")
        for name in PRESETS:
            self.presets.addItem(name, name)
        self.presets.activated.connect(lambda _: self._preset())
        self.rows = QGridLayout()
        add = QPushButton("＋ 色を足す")
        add.clicked.connect(self._add)
        top = QHBoxLayout()
        top.addWidget(self.presets, 1)
        top.addWidget(add)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.bar)
        layout.addLayout(top)
        layout.addLayout(self.rows)
        self._fill()

    def stops(self) -> list[list]:
        return sorted(([float(p), [int(v) for v in rgb], float(o)] for p, rgb, o in self._stops), key=lambda s: s[0])

    def set_stops(self, stops) -> None:
        self._stops = [list(s) for s in stops]
        self._fill()
        self.changed.emit()

    def _preset(self) -> None:
        name = self.presets.currentData()
        if name:
            self.set_stops(PRESETS[name])
        self.presets.setCurrentIndex(0)

    def _add(self) -> None:
        stops = self.stops()
        a, b = stops[-2], stops[-1]
        mid = [(a[0] + b[0]) / 2, [round((x + y) / 2) for x, y in zip(a[1], b[1])], (a[2] + b[2]) / 2]
        self.set_stops([*stops[:-1], mid, b])

    def _fill(self) -> None:
        while self.rows.count():
            item = self.rows.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        self._stops = self.stops()
        for i, (pos, rgb, opacity) in enumerate(self._stops):
            colour = QPushButton()
            colour.setFixedWidth(46)
            colour.setStyleSheet("background: rgb({},{},{})".format(*rgb))
            colour.setToolTip("色を選ぶ")
            colour.clicked.connect(lambda _=False, k=i: self._pick(k))
            where = QDoubleSpinBox()
            where.setRange(0, 100)
            where.setSuffix(" %")
            where.setValue(round(pos * 100, 1))
            where.setToolTip("はじめ（0 %）から終わり（100 %）のどこに置くか")
            where.valueChanged.connect(lambda v, k=i: self._set(k, 0, v / 100))
            self.rows.addWidget(colour, i, 0)
            self.rows.addWidget(where, i, 1)
            if self.with_opacity:
                strong = QDoubleSpinBox()
                strong.setRange(0, 100)
                strong.setSuffix(" %")
                strong.setValue(round(opacity * 100))
                strong.setToolTip("濃さ（0 % で透明）")
                strong.valueChanged.connect(lambda v, k=i: self._set(k, 2, v / 100))
                self.rows.addWidget(strong, i, 2)
            remove = QPushButton("×")
            remove.setFixedWidth(28)
            remove.setEnabled(len(self._stops) > 2)
            remove.clicked.connect(lambda _=False, k=i: self._remove(k))
            self.rows.addWidget(remove, i, 3)
        self.bar.update()

    def _set(self, index: int, part: int, value) -> None:
        self._stops[index][part] = value
        self.bar.update()
        self.changed.emit()

    def _pick(self, index: int) -> None:
        rgb = self._stops[index][1]
        colour = QColorDialog.getColor(QColor(*rgb), self, "色")
        if colour.isValid():
            self._stops[index][1] = [colour.red(), colour.green(), colour.blue()]
            self._fill()
            self.changed.emit()

    def _remove(self, index: int) -> None:
        if len(self._stops) > 2:
            del self._stops[index]
            self._fill()
            self.changed.emit()


def stops_from(spec: dict) -> list[list]:
    """A gradient's colours as stops, from its stops or its two ends."""
    if spec.get("stops"):
        return [list(s) for s in spec["stops"]]
    return [[0.0, list(spec.get("rgb_from") or [20, 20, 20]), float(spec.get("opacity_from", 1.0))],
            [1.0, list(spec.get("rgb_to") or [255, 255, 255]), float(spec.get("opacity_to", 1.0))]]

