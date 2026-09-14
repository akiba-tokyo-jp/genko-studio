from __future__ import annotations

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from genko.models import Page, Rect


class PageCanvas(QWidget):
    changed = Signal()
    strokeCommitted = Signal(list)
    frameSelected = Signal(str)

    def __init__(self) -> None:
        super().__init__()
        self.page: Page | None = None
        self._stroke: list[tuple[float, float]] = []
        self._scale = 2.4
        self._pan_x = 20.0
        self._pan_y = 20.0
        self._panning = False
        self._last_pos = QPointF()
        self.setMouseTracking(True)
        self.setMinimumSize(480, 640)

    def set_page(self, page: Page | None) -> None:
        self.page = page
        self._stroke = []
        self.update()

    def _to_mm(self, pos: QPointF) -> tuple[float, float]:
        return (pos.x() - self._pan_x) / self._scale, (pos.y() - self._pan_y) / self._scale

    def _pt(self, x: float, y: float) -> QPointF:
        return QPointF(x * self._scale + self._pan_x, y * self._scale + self._pan_y)

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#2b2b2b"))
        if self.page is None:
            return
        spec = self.page.spec
        origin = self._pt(0, 0)
        painter.fillRect(
            int(origin.x()),
            int(origin.y()),
            int(spec.width_mm * self._scale),
            int(spec.height_mm * self._scale),
            QColor("#f6f1e4"),
        )
        inner = self.page.inner_rect_mm()
        painter.setPen(QPen(QColor("#c8b89a"), 1, Qt.PenStyle.DashLine))
        self._draw_rect(painter, inner)
        for frame in self.page.leaf_frames():
            selected = frame.id == self.page.selected_frame_id
            painter.setPen(QPen(QColor("#1f6feb") if selected else QColor("#111111"), 3 if selected else 2))
            self._draw_rect(painter, frame.rect)
        self._draw_strokes(painter, self.page.name_strokes, QColor("#3a6ea5"), 1.6)
        self._draw_strokes(painter, self.page.ink_strokes, QColor("#111111"), 2.2)
        if self._stroke:
            self._draw_strokes(painter, [self._stroke], QColor("#d35400"), 2.0)

    def _draw_rect(self, painter: QPainter, rect: Rect) -> None:
        p = self._pt(rect.x, rect.y)
        painter.drawRect(int(p.x()), int(p.y()), int(rect.width * self._scale), int(rect.height * self._scale))

    def _draw_strokes(self, painter, strokes, color, width) -> None:
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
        if event.button() == Qt.MouseButton.MiddleButton:
            self._panning = True
            self._last_pos = event.position()
            return
        if self.page is None or event.button() != Qt.MouseButton.LeftButton:
            return
        self._stroke = [self._to_mm(event.position())]
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if self._panning:
            delta = event.position() - self._last_pos
            self._pan_x += delta.x()
            self._pan_y += delta.y()
            self._last_pos = event.position()
            self.update()
            return
        if not self._stroke:
            return
        self._stroke.append(self._to_mm(event.position()))
        self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.MiddleButton:
            self._panning = False
            return
        if self.page is None or not self._stroke:
            return
        if len(self._stroke) < 4:
            frame = self.page.frame_at(*self._stroke[0])
            if frame is not None:
                self.frameSelected.emit(frame.id)
            self._stroke = []
            self.update()
            return
        self.strokeCommitted.emit(list(self._stroke))
        self._stroke = []
        self.changed.emit()
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        delta = event.angleDelta().y()
        factor = 1.1 if delta > 0 else 0.9
        self._scale = min(8.0, max(0.8, self._scale * factor))
        self.update()
