"""The numbers of a filter or a colour adjustment, with a tone curve to drag and the result shown on the page
while the numbers change (プレビュー)."""

from __future__ import annotations

from PySide6.QtCore import QPointF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QLabel,
    QWidget,
)

CHANNELS = [("RGB（全部）", "rgb"), ("赤", "r"), ("緑", "g"), ("青", "b")]


class CurveEditor(QWidget):
    """トーンカーブ: click to add a point, drag to move it, right-click (or drag off the side) to take it away.
    The two ends always stay."""

    changed = Signal()
    SIDE = 200

    def __init__(self, points=None, parent=None) -> None:
        super().__init__(parent)
        self.setFixedSize(self.SIDE + 2, self.SIDE + 2)
        self.points: list[list[float]] = [list(map(float, p)) for p in (points or [[0, 0], [255, 255]])]
        self._drag: int | None = None
        self.setToolTip("クリックで点を足す・ドラッグで動かす・右クリックで消す")

    def _to_view(self, x: float, y: float) -> QPointF:
        return QPointF(1 + x / 255 * self.SIDE, 1 + (255 - y) / 255 * self.SIDE)

    def _to_value(self, pos) -> tuple[float, float]:
        x = min(255.0, max(0.0, (pos.x() - 1) / self.SIDE * 255))
        y = min(255.0, max(0.0, 255 - (pos.y() - 1) / self.SIDE * 255))
        return round(x), round(y)

    def _near(self, pos) -> int | None:
        for i, (x, y) in enumerate(self.points):
            p = self._to_view(x, y)
            if abs(p.x() - pos.x()) <= 6 and abs(p.y() - pos.y()) <= 6:
                return i
        return None

    def mousePressEvent(self, event) -> None:  # noqa: N802
        pos = event.position()
        index = self._near(pos)
        if event.button() == Qt.MouseButton.RightButton:
            if index is not None and 0 < index < len(self.points) - 1:
                del self.points[index]
                self.update()
                self.changed.emit()
            return
        if index is None:
            x, y = self._to_value(pos)
            if any(abs(px - x) < 3 for px, _ in self.points):
                return
            self.points.append([x, y])
            self.points.sort()
            index = next(i for i, p in enumerate(self.points) if p == [x, y])
            self.changed.emit()
        self._drag = index
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if self._drag is None:
            return
        x, y = self._to_value(event.position())
        i = self._drag
        lo = self.points[i - 1][0] + 1 if i > 0 else 0
        hi = self.points[i + 1][0] - 1 if i < len(self.points) - 1 else 255
        if i == 0:
            hi = min(hi, 254)
        self.points[i] = [min(hi, max(lo, x)), y]
        self.update()
        self.changed.emit()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        self._drag = None

    def paintEvent(self, event) -> None:  # noqa: N802
        from genko.filters import curve_table

        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), QColor(250, 250, 250))
        p.setPen(QPen(QColor(215, 215, 215), 1))
        for k in range(1, 4):
            v = 1 + k * self.SIDE / 4
            p.drawLine(QPointF(v, 1), QPointF(v, self.SIDE + 1))
            p.drawLine(QPointF(1, v), QPointF(self.SIDE + 1, v))
        p.setPen(QPen(QColor(160, 160, 160), 1))
        p.drawRect(0, 0, self.SIDE + 1, self.SIDE + 1)
        table = curve_table(self.points)
        path = QPainterPath(self._to_view(0, table[0]))
        for x in range(1, 256):
            path.lineTo(self._to_view(x, table[x]))
        p.setPen(QPen(QColor(30, 30, 30), 1.5))
        p.drawPath(path)
        p.setBrush(QColor(255, 255, 255))
        for x, y in self.points:
            p.drawEllipse(self._to_view(x, y), 4, 4)
        p.end()


def histogram_pixmap(counts, width: int = 202, height: int = 56):
    """ヒストグラム: how many pixels have each lightness (0 left, 255 right), as bars."""
    from PySide6.QtGui import QPixmap

    pixmap = QPixmap(width, height)
    pixmap.fill(QColor(250, 250, 250))
    top = max(1, sorted(counts)[-2] if len(counts) > 1 else max(counts))  # (one huge bar, often paper, does not flatten the rest)
    p = QPainter(pixmap)
    p.setPen(QPen(QColor(90, 90, 90), 1))
    for i, n in enumerate(counts):
        x = 1 + i * (width - 2) / 255
        h = min(height - 2, round((height - 2) * n / top))
        if h:
            p.drawLine(QPointF(x, height - 1), QPointF(x, height - 1 - h))
    p.end()
    return pixmap


def ask(parent, kind: str, fields, now: dict | None = None, preview=None, title: str = "フィルターの強さ",
        histogram=None) -> dict | None:
    """The numbers (None: the person stopped). `fields`: (key, label, lo, hi, default) for a number, or
    (key, label, [(label, value), …], default) for a choice. `preview(params | None)`: called while the numbers
    change (None: take the preview away)."""
    now = dict(now or {})
    dialog = QDialog(parent)
    dialog.setWindowTitle(title)
    form = QFormLayout(dialog)
    if histogram and kind in ("levels", "curve"):
        chart = QLabel()
        chart.setPixmap(histogram_pixmap(list(histogram)[:256]))
        chart.setToolTip("このレイヤーの明るさの分布（左が黒、右が白）")
        form.addRow("分布", chart)
    getters: dict = {}
    signals = []
    for key, label, *rest in fields:
        if isinstance(rest[0], list):
            box = QComboBox()
            for text, value in rest[0]:
                box.addItem(text, value)
            box.setCurrentIndex(max(0, box.findData(now.get(key, rest[1]))))
            getters[key] = box.currentData
            signals.append(box.currentIndexChanged)
        else:
            lo, hi, value = rest
            box = QDoubleSpinBox()
            box.setRange(lo, hi)
            box.setDecimals(3 if hi <= 1 else 2 if hi <= 10 else 1 if hi <= 300 else 0)
            box.setSingleStep(0.01 if hi <= 1 else 0.1 if hi <= 10 else 1)
            box.setValue(float(now.get(key, value)))
            getters[key] = lambda b=box: round(b.value(), 3)
            signals.append(box.valueChanged)
        form.addRow(label, box)
    curve = None
    if kind == "curve":
        curve = CurveEditor(now.get("points") or [[0, 0], [255, 255]])
        form.addRow("曲線", curve)
        form.addRow("", QLabel("横が元の明るさ、縦がかけた後。上に持ち上げると明るく"))
        signals.append(curve.changed)
    check = None
    timer = QTimer(dialog)
    timer.setSingleShot(True)
    timer.setInterval(250)

    def values() -> dict:
        out = {key: get() for key, get in getters.items()}
        if curve is not None:
            out["points"] = [[int(x), int(y)] for x, y in curve.points]
            out.pop("gamma", None)
        return out

    if preview is not None:
        check = QCheckBox("プレビュー（ページで見る）")
        check.setChecked(True)
        form.addRow("", check)

        def show() -> None:
            try:
                preview(values() if check.isChecked() else None)
            except Exception:  # (a preview that cannot be made must not stop the dialog)
                preview(None)

        timer.timeout.connect(show)
        for signal in signals:
            signal.connect(lambda *_: timer.start())
        check.toggled.connect(lambda _: timer.start())
        timer.start(0)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
    buttons.button(QDialogButtonBox.StandardButton.Ok).setText("かける")
    buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    dialog.values = values  # (tests read the numbers without closing)
    dialog.curve = curve
    dialog.preview_check = check
    ok = dialog.exec()
    timer.stop()
    if preview is not None:
        preview(None)
    return values() if ok else None
