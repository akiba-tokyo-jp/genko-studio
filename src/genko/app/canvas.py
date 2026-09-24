"""The page canvas: the page as it will print (rendered by Genko) with light overlays on top.

Tools: 選択 (click a panel to select it, drag a balloon to move it, drag elsewhere to move the
view), ペン and 消しゴム. The view fits the page when it opens; Ctrl+wheel or pinch zooms,
the wheel or two fingers scroll, Space+drag or the middle button pans.
"""

from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QCursor, QPainter, QPainterPath, QPen, QPixmap, QWheelEvent
from PySide6.QtWidgets import QWidget

from genko.models import Page, Rect, StoryLine
from genko.stroke import pack_point

MIN_SCALE, MAX_SCALE = 0.3, 12.0  # screen px per mm


class PageCanvas(QWidget):
    changed = Signal()
    strokeCommitted = Signal(list)
    frameSelected = Signal(str)
    textMoved = Signal(str, float, float)
    contextMenuAt = Signal(str, QPointF)  # frame id ("" if none), global position
    zoomChanged = Signal(float)

    def __init__(self) -> None:
        super().__init__()
        self.page: Page | None = None
        self.lines: list[StoryLine] = []
        self._stroke: list[tuple] = []
        self._scale = 2.4
        self._pan_x = 20.0
        self._pan_y = 20.0
        self._fitted = True  # follow the window size until the person zooms or pans
        self._panning = False
        self._space = False
        self._last_pos = QPointF()
        self._press_pos: QPointF | None = None
        self._drag_line: StoryLine | None = None
        self._drag_pos: tuple[float, float] | None = None  # where the dragged balloon is shown; the model is untouched
        self._drag_grab = (0.0, 0.0)
        self.tool = "select"
        self._hover: tuple[float, float] | None = None
        self.brush_width_mm = 0.35
        self.overlay_name_strokes = False  # show the name strokes faintly over a proof render
        # renderer(dpi) -> QPixmap of the whole page; set by the window
        self.renderer: Callable[[int], QPixmap | None] | None = None
        self._rendered: tuple[int, QPixmap] | None = None
        self._rerender = QTimer(self)
        self._rerender.setSingleShot(True)
        self._rerender.setInterval(160)
        self._rerender.timeout.connect(self._render_now)
        self.setMouseTracking(True)
        self.setAttribute(Qt.WidgetAttribute.WA_TabletTracking, True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMinimumSize(300, 280)

    # --- state ---------------------------------------------------------------------------------

    @property
    def background(self) -> QPixmap | None:
        return self._rendered[1] if self._rendered else None

    def set_tool(self, tool: str) -> None:
        self.tool = tool
        self._stroke = []
        self._update_cursor()
        self.update()

    def set_page(self, page: Page | None, lines: list[StoryLine] | None = None) -> None:
        size_changed = self.page is None or page is None or (self.page.spec.width_mm, self.page.spec.height_mm) != (
            page.spec.width_mm, page.spec.height_mm)
        self.page = page
        self.lines = list(lines or (page.texts if page else []))
        self._stroke = []
        if size_changed or self._fitted:
            self.fit_page()
        self.invalidate()

    def invalidate(self) -> None:
        """The page changed: render it again (now, so a new stroke never blinks away)."""
        self._rendered = None
        self._render_now()

    def _wanted_dpi(self) -> int:
        ratio = self.devicePixelRatioF() or 1.0
        dpi = self._scale * 25.4 * ratio
        return int(max(48, min(220, round(dpi / 24) * 24)))

    def _render_now(self) -> None:
        if self.page is None or self.renderer is None:
            self._rendered = None
            self.update()
            return
        dpi = self._wanted_dpi()
        pix = self.renderer(dpi)
        self._rendered = (dpi, pix) if pix is not None else None
        self.update()

    # --- view --------------------------------------------------------------------------------------

    def _view_rect_mm(self) -> tuple[float, float, float, float]:
        spec = self.page.spec
        x0, w = 0.0, spec.width_mm
        if self.page.spread_with:
            if self.page.side() == "right":
                x0, w = -spec.width_mm, 2 * spec.width_mm
            else:
                w = 2 * spec.width_mm
        return x0, 0.0, w, spec.height_mm

    def fit_page(self) -> None:
        if self.page is None or self.width() <= 0 or self.height() <= 0:
            return
        x0, y0, w, h = self._view_rect_mm()
        margin = 16
        self._scale = max(MIN_SCALE, min(MAX_SCALE, min((self.width() - 2 * margin) / w, (self.height() - 2 * margin) / h)))
        self._pan_x = (self.width() - w * self._scale) / 2 - x0 * self._scale
        self._pan_y = (self.height() - h * self._scale) / 2 - y0 * self._scale
        self._fitted = True
        self._after_zoom()

    def zoom_by(self, factor: float, anchor: QPointF | None = None) -> None:
        anchor = anchor or QPointF(self.width() / 2, self.height() / 2)
        mx, my = self._to_mm(anchor)
        self._scale = max(MIN_SCALE, min(MAX_SCALE, self._scale * factor))
        self._pan_x = anchor.x() - mx * self._scale
        self._pan_y = anchor.y() - my * self._scale
        self._fitted = False
        self._after_zoom()

    def actual_size(self) -> None:
        """About the size of the paper on a typical screen (96 px per inch)."""
        self.zoom_by((96 / 25.4) / self._scale)

    def _after_zoom(self) -> None:
        if self._rendered and self._rendered[0] != self._wanted_dpi():
            self._rerender.start()
        self.zoomChanged.emit(self._scale)
        self.update()

    def zoom_percent(self) -> int:
        return round(self._scale / (96 / 25.4) * 100)

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        if self._fitted:
            self.fit_page()

    def _to_mm(self, pos: QPointF) -> tuple[float, float]:
        return (pos.x() - self._pan_x) / self._scale, (pos.y() - self._pan_y) / self._scale

    def _pt(self, x: float, y: float) -> QPointF:
        return QPointF(x * self._scale + self._pan_x, y * self._scale + self._pan_y)

    def _xy(self, point) -> tuple[float, float]:
        return float(point[0]), float(point[1])

    # --- painting ---------------------------------------------------------------------------------

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        painter.fillRect(self.rect(), QColor("#3a3a3a"))
        if self.page is None:
            painter.setPen(QColor("#bbbbbb"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "ページがありません")
            return
        spec = self.page.spec
        origin = self._pt(0, 0)
        page_rect = QRectF(origin.x(), origin.y(), spec.width_mm * self._scale, spec.height_mm * self._scale)
        if self.page.spread_with:
            # the partner sits on the other physical side; strokes drawn there go to the partner page
            offset = -spec.width_mm if self.page.side() == "right" else spec.width_mm
            painter.fillRect(page_rect.translated(offset * self._scale, 0), QColor("#e9e4d8"))
        painter.fillRect(page_rect, QColor("#ffffff"))
        if self.background is not None:
            painter.drawPixmap(page_rect, self.background, QRectF(self.background.rect()))
        else:
            self._draw_plain(painter)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self.overlay_name_strokes:
            self._draw_strokes(painter, self.page.name_strokes, QColor(58, 110, 165, 110), 1.2)
        self._draw_selection(painter)
        for line in self.lines:
            if line is self._drag_line:
                self._draw_balloon_box(painter, line, self._drag_pos, strong=True)
            elif self.tool == "select" and self._hover and self._hit_line(*self._hover) is line:
                self._draw_balloon_box(painter, line, None)
        if self._stroke:
            color = QColor("#e8590c") if self.tool == "pen" else QColor(200, 60, 60, 160)
            self._draw_strokes(painter, [self._stroke], color, max(1.5, self.brush_width_mm * self._scale))
        if self._hover and not self._stroke and self.tool in ("pen", "eraser"):
            hx, hy = self._pt(*self._hover).x(), self._pt(*self._hover).y()
            radius = max(2.0, (self.brush_width_mm if self.tool == "pen" else 1.5) * self._scale)
            painter.setPen(QPen(QColor("#e8590c"), 1))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(QPointF(hx, hy), radius, radius)

    def _draw_plain(self, painter: QPainter) -> None:
        """Without a renderer (tests, or before the first render): frames and balloon boxes."""
        painter.setPen(QPen(QColor("#111111"), 2))
        for frame in self.page.leaf_frames():
            self._draw_rect(painter, frame.rect)
        for line in self.lines:
            self._draw_balloon_box(painter, line, None)

    def _draw_selection(self, painter: QPainter) -> None:
        hover_frame = None
        if self.tool == "select" and self._hover and not self._drag_line:
            if self._hit_line(*self._hover) is None:
                hover_frame = self.page.frame_at(*self._hover)
        for frame in self.page.leaf_frames():
            if frame.id == self.page.selected_frame_id:
                painter.setPen(QPen(QColor("#1c7ed6"), 3))
                painter.setBrush(QColor(28, 126, 214, 16))
                self._draw_rect(painter, frame.rect)
            elif hover_frame is not None and frame.id == hover_frame.id:
                painter.setPen(QPen(QColor(28, 126, 214, 150), 1.5, Qt.PenStyle.DashLine))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                self._draw_rect(painter, frame.rect)
        painter.setBrush(Qt.BrushStyle.NoBrush)

    def _draw_rect(self, painter: QPainter, rect: Rect) -> None:
        p = self._pt(rect.x, rect.y)
        painter.drawRect(QRectF(p.x(), p.y(), rect.width * self._scale, rect.height * self._scale))

    def _draw_strokes(self, painter, strokes, color, width) -> None:
        pen = QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        for stroke in strokes:
            if len(stroke) < 2:
                continue
            path = QPainterPath(self._pt(*self._xy(stroke[0])))
            for point in stroke[1:]:
                path.lineTo(self._pt(*self._xy(point)))
            painter.drawPath(path)

    def _draw_balloon_box(self, painter: QPainter, line: StoryLine, at: tuple[float, float] | None, strong: bool = False) -> None:
        p = self._pt(*(at or (line.x_mm, line.y_mm)))
        rect = QRectF(p.x(), p.y(), max(10, line.w_mm * self._scale), max(10, line.h_mm * self._scale))
        painter.setPen(QPen(QColor("#e8590c"), 2 if strong else 1.5, Qt.PenStyle.DashLine))
        painter.setBrush(QColor(255, 255, 255, 170) if strong else Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)
        painter.setBrush(Qt.BrushStyle.NoBrush)

    # --- hit tests ----------------------------------------------------------------------------------

    def _hit_line(self, x_mm: float, y_mm: float) -> StoryLine | None:
        for line in reversed(self.lines):
            if line.x_mm <= x_mm <= line.x_mm + line.w_mm and line.y_mm <= y_mm <= line.y_mm + line.h_mm:
                return line
        return None

    def _update_cursor(self, pos: QPointF | None = None) -> None:
        if self._panning:
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
        elif self._space:
            self.setCursor(Qt.CursorShape.OpenHandCursor)
        elif self.tool in ("pen", "eraser"):
            self.setCursor(Qt.CursorShape.CrossCursor)
        elif pos is not None and self.page is not None and self._hit_line(*self._to_mm(pos)):
            self.setCursor(Qt.CursorShape.SizeAllCursor)
        else:
            self.setCursor(Qt.CursorShape.ArrowCursor)

    # --- mouse -----------------------------------------------------------------------------------------

    def _start_pan(self, pos: QPointF) -> None:
        self._panning = True
        self._last_pos = pos
        self._update_cursor()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        self.setFocus()
        pos = event.position()
        if event.button() == Qt.MouseButton.MiddleButton or (event.button() == Qt.MouseButton.LeftButton and self._space):
            self._start_pan(pos)
            return
        if self.page is None or event.button() != Qt.MouseButton.LeftButton:
            return
        x_mm, y_mm = self._to_mm(pos)
        self._press_pos = pos
        if self.tool == "select":
            hit = self._hit_line(x_mm, y_mm)
            if hit is not None:
                self._drag_line = hit
                self._drag_grab = (x_mm - hit.x_mm, y_mm - hit.y_mm)
                self._drag_pos = (hit.x_mm, hit.y_mm)
                return
            self._last_pos = pos  # a drag on empty paper moves the view (hand), a click selects
            return
        self._stroke = [(x_mm, y_mm)]
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        pos = event.position()
        if self._panning:
            delta = pos - self._last_pos
            self._pan_x += delta.x()
            self._pan_y += delta.y()
            self._last_pos = pos
            self._fitted = False
            self.update()
            return
        if self._drag_line is not None:
            x_mm, y_mm = self._to_mm(pos)
            gx, gy = self._drag_grab
            self._drag_pos = (x_mm - gx, y_mm - gy)
            self.update()
            return
        pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)
        if self.tool == "select" and pressed and self._press_pos is not None:
            if (pos - self._press_pos).manhattanLength() > 6:
                self._start_pan(self._last_pos)
                self.mouseMoveEvent(event)
            return
        if not self._stroke:
            self._hover = self._to_mm(pos)
            self._update_cursor(pos)
            self.update()
            return
        self._stroke.append(self._to_mm(pos))
        self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if self._panning:
            self._panning = False
            self._press_pos = None
            self._update_cursor(event.position())
            return
        if self._drag_line is not None:
            line, pos = self._drag_line, self._drag_pos
            self._drag_line = None
            self._drag_pos = None
            self._press_pos = None
            if pos is not None and (abs(pos[0] - line.x_mm) > 0.05 or abs(pos[1] - line.y_mm) > 0.05):
                self.textMoved.emit(line.id, round(pos[0], 2), round(pos[1], 2))  # becomes a move_line op
            self.update()
            return
        if self.page is None or event.button() != Qt.MouseButton.LeftButton:
            return
        if self.tool == "select":
            self._press_pos = None
            frame = self.page.frame_at(*self._to_mm(event.position()))
            if frame is not None:
                self.frameSelected.emit(frame.id)
            return
        if not self._stroke:
            return
        if len(self._stroke) < 4:
            self._stroke = []  # a tap with the pen draws nothing
            self.update()
            return
        packed = [pack_point(float(pt[0]), float(pt[1]), float(pt[2]) if len(pt) > 2 else None) for pt in self._stroke]
        self._stroke = []
        self.strokeCommitted.emit(packed)
        self.changed.emit()
        self.update()

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.MiddleButton:
            self.fit_page()

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._hover = None
        self.update()

    def contextMenuEvent(self, event) -> None:  # noqa: N802
        if self.page is None:
            return
        frame = self.page.frame_at(*self._to_mm(QPointF(event.pos())))
        if frame is not None and frame.id != self.page.selected_frame_id:
            self.frameSelected.emit(frame.id)
        self.contextMenuAt.emit(frame.id if frame else "", QPointF(event.globalPos()))

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            delta = event.angleDelta().y() or event.pixelDelta().y()
            if delta:
                self.zoom_by(1.0 + max(-0.5, min(0.5, delta / 600)), event.position())
            return
        pixel = event.pixelDelta()
        dx, dy = (pixel.x(), pixel.y()) if not pixel.isNull() else (event.angleDelta().x() / 3, event.angleDelta().y() / 3)
        if event.modifiers() & Qt.KeyboardModifier.ShiftModifier and not dx:
            dx, dy = dy, 0
        self._pan_x += dx
        self._pan_y += dy
        self._fitted = False
        self.update()

    def event(self, event) -> bool:  # noqa: A003
        from PySide6.QtCore import QEvent

        if event.type() == QEvent.Type.NativeGesture:  # trackpad pinch (macOS)
            from PySide6.QtCore import Qt as _Qt

            if event.gestureType() == _Qt.NativeGestureType.ZoomNativeGesture:
                self.zoom_by(1.0 + float(event.value()), event.position())
                return True
        return super().event(event)

    # --- keyboard --------------------------------------------------------------------------------------

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Space and not event.isAutoRepeat():
            self._space = True
            self._update_cursor()
            return
        if event.key() == Qt.Key.Key_Escape and self._drag_line is not None:
            self._drag_line = None
            self._drag_pos = None
            self.update()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Space and not event.isAutoRepeat():
            self._space = False
            self._panning = False
            self._update_cursor(QPointF(self.mapFromGlobal(QCursor.pos())))
            return
        super().keyReleaseEvent(event)

    # --- tablet ------------------------------------------------------------------------------------------

    def tabletEvent(self, event) -> None:  # noqa: N802
        if self.page is None or self.tool not in ("pen", "eraser"):
            event.ignore()  # the select tool works with the pen as a mouse
            return
        x_mm, y_mm = self._to_mm(event.position())
        pressure = float(event.pressure())
        tilt = abs(float(getattr(event, "xTilt", lambda: 0.0)())) / 60.0
        etype = event.type()
        from PySide6.QtCore import QEvent

        if etype == QEvent.Type.TabletPress:
            self._stroke = [tuple(pack_point(x_mm, y_mm, pressure, tilt=tilt))]
            self.update()
            event.accept()
            return
        if etype == QEvent.Type.TabletMove and self._stroke:
            self._stroke.append(tuple(pack_point(x_mm, y_mm, pressure, tilt=tilt)))
            self.update()
            event.accept()
            return
        if etype == QEvent.Type.TabletRelease and self._stroke:
            stroke = list(self._stroke)
            self._stroke = []
            if len(stroke) >= 2:
                self.strokeCommitted.emit(stroke)
            self.changed.emit()
            self.update()
            event.accept()
