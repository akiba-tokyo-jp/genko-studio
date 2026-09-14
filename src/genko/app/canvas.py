from __future__ import annotations

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from genko.models import Page, Rect


class PageCanvas(QWidget):
    """Paper view in millimetres. Draws name (blue) and ink (black)."""

    changed = Signal()

    def __init__(self) -> None:
        super().__init__()
        self.page: Page | None = None
        self._stroke: list[tuple[float, float]] = []
        self._scale = 2.4
        self.setMouseTracking(True)
        self.setMinimumSize(480, 640)

    def set_page(self, page: Page | None) -> None:
        self.page = page
        self._stroke = []
        self.update()

    def _to_mm(self, pos: QPointF) -> tuple[float, float]:
        return pos.x() / self._scale, pos.y() / self._scale

    def _pt(self, x: float, y: float) -> QPointF:
        return QPointF(x * self._scale, y * self._scale)

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#2b2b2b"))
        if self.page is None:
            return
        spec = self.page.spec
        painter.fillRect(
            0,
            0,
            int(spec.width_mm * self._scale),
            int(spec.height_mm * self._scale),
            QColor("#f6f1e4"),
        )
        inner = self.page.inner_rect_mm()
        painter.setPen(QPen(QColor("#c8b89a"), 1, Qt.PenStyle.DashLine))
        painter.drawRect(
            int(inner.x * self._scale),
            int(inner.y * self._scale),
            int(inner.width * self._scale),
            int(inner.height * self._scale),
        )
        painter.setPen(QPen(QColor("#111111"), 2))
        for frame in self.page.leaf_frames():
            self._draw_rect(painter, frame.rect)

        self._draw_strokes(painter, self.page.name_strokes, QColor("#3a6ea5"), 1.6)
        self._draw_strokes(painter, self.page.ink_strokes, QColor("#111111"), 2.2)
        if self._stroke:
            self._draw_strokes(painter, [self._stroke], QColor("#d35400"), 2.0)

    def _draw_rect(self, painter: QPainter, rect: Rect) -> None:
        painter.drawRect(
            int(rect.x * self._scale),
            int(rect.y * self._scale),
            int(rect.width * self._scale),
            int(rect.height * self._scale),
        )

    def _draw_strokes(
        self,
        painter: QPainter,
        strokes: list[list[tuple[float, float]]],
        color: QColor,
        width: float,
    ) -> None:
        pen = QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        for stroke in strokes:
            if len(stroke) < 2:
                continue
            path = QPainterPath(self._pt(*stroke[0]))
            for point in stroke[1:]:
                path.lineTo(self._pt(*point))
            painter.drawPath(path)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if self.page is None or event.button() != Qt.MouseButton.LeftButton:
            return
        self._stroke = [self._to_mm(event.position())]
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if not self._stroke:
            return
        self._stroke.append(self._to_mm(event.position()))
        self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if self.page is None or not self._stroke:
            return
        if self.page.stage == "ink":
            self.page.ink_strokes.append(self._stroke)
        else:
            self.page.name_strokes.append(self._stroke)
        self._stroke = []
        self.changed.emit()
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        delta = event.angleDelta().y()
        factor = 1.1 if delta > 0 else 0.9
        self._scale = min(8.0, max(0.8, self._scale * factor))
        self.update()
