from __future__ import annotations

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from genko.models import Page, Rect, StoryLine
from genko.stroke import pack_point


class PageCanvas(QWidget):
    changed = Signal()
    strokeCommitted = Signal(list)
    frameSelected = Signal(str)
    textMoved = Signal(str, float, float)

    def __init__(self) -> None:
        super().__init__()
        self.page: Page | None = None
        self.lines: list[StoryLine] = []
        self._stroke: list[tuple[float, float]] = []
        self._scale = 2.4
        self._pan_x = 20.0
        self._pan_y = 20.0
        self._panning = False
        self._last_pos = QPointF()
        self._drag_line: StoryLine | None = None
        self.tool = "pen"
        self.setMouseTracking(True)
        self.setAttribute(Qt.WidgetAttribute.WA_TabletTracking, True)
        self.setMinimumSize(480, 640)

    def set_tool(self, tool: str) -> None:
        self.tool = tool

    def set_page(self, page: Page | None, lines: list[StoryLine] | None = None) -> None:
        self.page = page
        self.lines = list(lines or (page.texts if page else []))
        self._stroke = []
        self.update()

    def _to_mm(self, pos: QPointF) -> tuple[float, float]:
        return (pos.x() - self._pan_x) / self._scale, (pos.y() - self._pan_y) / self._scale

    def _pt(self, x: float, y: float) -> QPointF:
        return QPointF(x * self._scale + self._pan_x, y * self._scale + self._pan_y)

    def _xy(self, point) -> tuple[float, float]:
        return float(point[0]), float(point[1])

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
        for line in self.lines:
            self._draw_balloon(painter, line)
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
            start = self._xy(stroke[0])
            path = QPainterPath(self._pt(*start))
            for point in stroke[1:]:
                path.lineTo(self._pt(*self._xy(point)))
            painter.drawPath(path)

    def _draw_balloon(self, painter: QPainter, line: StoryLine) -> None:
        p = self._pt(line.x_mm, line.y_mm)
        w = max(12, line.w_mm * self._scale)
        h = max(10, line.h_mm * self._scale)
        painter.setPen(QPen(QColor("#111111"), 2))
        painter.setBrush(QColor("#ffffff"))
        painter.drawEllipse(int(p.x()), int(p.y()), int(w), int(h))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawText(int(p.x()) + 4, int(p.y()) + int(h / 2), line.text)

    def _hit_line(self, x_mm: float, y_mm: float) -> StoryLine | None:
        for line in reversed(self.lines):
            if line.x_mm <= x_mm <= line.x_mm + line.w_mm and line.y_mm <= y_mm <= line.y_mm + line.h_mm:
                return line
        return None

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.MiddleButton:
            self._panning = True
            self._last_pos = event.position()
            return
        if self.page is None or event.button() != Qt.MouseButton.LeftButton:
            return
        x_mm, y_mm = self._to_mm(event.position())
        hit = self._hit_line(x_mm, y_mm)
        if hit is not None:
            self._drag_line = hit
            return
        self._stroke = [(x_mm, y_mm)]
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if self._panning:
            delta = event.position() - self._last_pos
            self._pan_x += delta.x()
            self._pan_y += delta.y()
            self._last_pos = event.position()
            self.update()
            return
        if self._drag_line is not None:
            x_mm, y_mm = self._to_mm(event.position())
            self._drag_line.x_mm = x_mm
            self._drag_line.y_mm = y_mm
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
        if self._drag_line is not None:
            self.textMoved.emit(self._drag_line.id, self._drag_line.x_mm, self._drag_line.y_mm)
            self._drag_line = None
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
        packed = [pack_point(float(pt[0]), float(pt[1]), float(pt[2]) if len(pt) > 2 else None) for pt in self._stroke]
        self.strokeCommitted.emit(packed)
        self._stroke = []
        self.changed.emit()
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        delta = event.angleDelta().y()
        factor = 1.1 if delta > 0 else 0.9
        self._scale = min(8.0, max(0.8, self._scale * factor))
        self.update()

    def tabletEvent(self, event) -> None:  # noqa: N802
        if self.page is None:
            return
        x_mm, y_mm = self._to_mm(event.position())
        pressure = float(event.pressure())
        etype = event.type()
        from PySide6.QtCore import QEvent

        if etype == QEvent.Type.TabletPress:
            self._stroke = [tuple(pack_point(x_mm, y_mm, pressure))]
            self.update()
            event.accept()
            return
        if etype == QEvent.Type.TabletMove and self._stroke:
            self._stroke.append(tuple(pack_point(x_mm, y_mm, pressure)))
            self.update()
            event.accept()
            return
        if etype == QEvent.Type.TabletRelease and self._stroke:
            if len(self._stroke) >= 2:
                self.strokeCommitted.emit(list(self._stroke))
            self._stroke = []
            self.changed.emit()
            self.update()
            event.accept()
