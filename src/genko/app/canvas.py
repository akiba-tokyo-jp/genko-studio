"""The page canvas: the page as it will print (rendered by Genko) with light overlays on top.

Tools: 選択 (click a panel to select it, drag a balloon to move it, drag elsewhere to move the
view), ペン and 消しゴム. The view fits the page when it opens; Ctrl+wheel or pinch zooms,
the wheel or two fingers scroll, Space+drag or the middle button pans.
"""

from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QCursor, QPainter, QPainterPath, QPen, QPixmap, QWheelEvent
from PySide6.QtWidgets import QLabel, QPlainTextEdit, QWidget

from genko.models import Page, Rect, StoryLine
from genko.stroke import pack_point

HANDLE_PX = 7


class InlineEditor(QPlainTextEdit):
    """Typing a line where it goes: Ctrl+Enter (or clicking elsewhere) keeps it, Esc drops it."""

    def __init__(self, parent, text: str, on_done) -> None:
        super().__init__(parent)
        self.on_done = on_done
        self.finished = False
        self.setPlainText(text)
        self.setStyleSheet("QPlainTextEdit{background:#fffbe6;border:2px solid #e8590c;font-size:15px}")
        self.setPlaceholderText("台詞を入力（改行で次の列、ルビは ｜約束《やくそく》）")
        self.hint = QLabel("Ctrl+Enter で決定・Esc でやめる", parent)
        self.hint.setStyleSheet("background:#e8590c;color:white;padding:1px 4px")
        self.hint.adjustSize()

    def place(self, x: float, y: float) -> None:
        self.setGeometry(int(x), int(y), 260, 110)
        parent = self.parentWidget()
        hx = max(0, min(int(x), (parent.width() if parent else 10000) - self.hint.width()))
        self.hint.move(hx, max(0, int(y) - self.hint.height()))
        self.show()
        self.hint.show()
        self.setFocus()
        self.moveCursor(self.textCursor().MoveOperation.End)

    def finish(self, keep: bool) -> None:
        if self.finished:
            return
        self.finished = True
        text = self.toPlainText().strip()
        self.hide()
        self.hint.hide()
        self.hint.deleteLater()
        self.deleteLater()
        self.on_done(text if keep else None)

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Escape:
            self.finish(False)
            return
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter) and event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            self.finish(True)
            return
        super().keyPressEvent(event)

    def focusOutEvent(self, event) -> None:  # noqa: N802
        super().focusOutEvent(event)
        self.finish(True)


MIN_SCALE, MAX_SCALE = 0.3, 12.0  # screen px per mm


class PageCanvas(QWidget):
    changed = Signal()
    strokeCommitted = Signal(list)
    frameSelected = Signal(str)
    textMoved = Signal(str, float, float)
    contextMenuAt = Signal(str, QPointF)  # frame id ("" if none), global position
    zoomChanged = Signal(float)
    lineSelected = Signal(str, bool)  # line id, open the lines panel
    lineGeometry = Signal(str, object)  # line id, {x_mm, y_mm, w_mm, h_mm} or {tails}: a move_line op
    lineEditRequested = Signal(str)  # double-click on a balloon: type over it
    lineContextMenu = Signal(str, QPointF)
    textRequested = Signal(float, float)  # the text tool clicked here (mm)
    gutterMoved = Signal(str, int, float)  # split node id, gutter index, delta mm (a move_gutter op)
    cutRequested = Signal(str, QPointF, QPointF)  # panel id, cut line ends (mm): a cut_frame op
    frameShaped = Signal(str, object)  # panel id, [[x, y], …]: a free-form panel (set_frame poly)
    colourPicked = Signal(object)  # (r, g, b) under the eyedropper
    fillRequested = Signal(float, float)  # the fill tool clicked here (mm)
    areaFilled = Signal(object)  # a drawn area to fill: [[x, y], …]
    areaSelected = Signal(object)  # a new selection area {"poly": …} (the window may replace it, e.g. auto-select)
    wandRequested = Signal(float, float)
    selectionTransformed = Signal(object)  # [a, b, c, d, e, f] applied to the selection
    strokeReshaped = Signal(str, object)  # stroke id, new points

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
        self.eraser_mm = 2.0
        self.show_guides = True  # bleed, trim line and the basic frame
        self.selected_line_id: str | None = None
        self._handle_drag: dict | None = None
        self._frame_drag: dict | None = None  # the panel tool: {"kind": gutter|cut|vertex, ...}
        self._modifiers = Qt.KeyboardModifier.NoModifier
        self.selection: dict | None = None  # {"area": {...}, "outline": [[x, y], …]} (mm)
        self.marquee = "rect"  # rect | lasso | wand
        self._sel_drag: dict | None = None
        self.strokes_for_reshape = None  # callable → the target layer's strokes
        self.reshape_radius_mm = 6.0
        self._reshape: dict | None = None
        self.editor: InlineEditor | None = None
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

    def open_editor(self, x_mm: float, y_mm: float, text: str, on_done) -> InlineEditor:
        """Type a line at this point of the page; on_done(text or None) when finished."""
        if self.editor is not None and not self.editor.finished:
            self.editor.finish(True)
        p = self._pt(x_mm, y_mm)
        self.editor = InlineEditor(self, text, on_done)
        self.editor.place(max(0.0, min(self.width() - 260.0, p.x())), max(20.0, min(self.height() - 110.0, p.y())))
        return self.editor

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
        if self.show_guides:
            self._draw_guides(painter, page_rect)
        if self.overlay_name_strokes:
            self._draw_strokes(painter, self.page.name_strokes, QColor(58, 110, 165, 110), 1.2)
        self._draw_selection(painter)
        for line in self.lines:
            if line is self._drag_line:
                self._draw_balloon_box(painter, line, self._drag_pos, strong=True)
            elif line.id == self.selected_line_id:
                self._draw_balloon_box(painter, line, None, strong=True, fill=False)
            elif self.tool == "select" and self._hover and self._hit_line(*self._hover) is line:
                self._draw_balloon_box(painter, line, None)
        self._draw_handles(painter)
        self._draw_selection_overlay(painter)
        if self._reshape is not None:
            painter.setPen(QPen(QColor("#e8590c"), 2))
            path = QPainterPath(self._pt(*self._reshape["points"][0][:2]))
            for pt in self._reshape["points"][1:]:
                path.lineTo(self._pt(*pt[:2]))
            painter.drawPath(path)
        if self._stroke and self.tool in ("lassofill", "marquee"):
            painter.setPen(QPen(QColor("#1c7ed6") if self.tool == "marquee" else QColor("#e8590c"), 1.5, Qt.PenStyle.DashLine))
            painter.setBrush(QColor(28, 126, 214, 30) if self.tool == "marquee" else QColor(232, 89, 12, 40))
            pts = self._marquee_points()
            painter.drawPolygon([self._pt(*p) for p in pts])
            painter.setBrush(Qt.BrushStyle.NoBrush)
        elif self._stroke:
            color = QColor("#e8590c") if self.tool == "pen" else QColor(200, 60, 60, 160)
            self._draw_strokes(painter, [self._stroke], color, max(1.5, self.brush_width_mm * self._scale))
        if self._hover and not self._stroke and self.tool in ("pen", "eraser"):
            hx, hy = self._pt(*self._hover).x(), self._pt(*self._hover).y()
            radius = max(2.0, (self.brush_width_mm if self.tool == "pen" else self.eraser_mm) / 2 * self._scale)
            painter.setPen(QPen(QColor("#e8590c"), 1))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(QPointF(hx, hy), radius, radius)

    def _draw_guides(self, painter: QPainter, page_rect: QRectF) -> None:
        """The bleed (cut off, shaded), the trim line (the finished size) and the basic frame."""
        spec = self.page.spec
        bleed = spec.bleed_mm * self._scale
        if bleed > 0:
            trim = page_rect.adjusted(bleed, bleed, -bleed, -bleed)
            shade = QColor(120, 120, 140, 40)
            painter.fillRect(QRectF(page_rect.left(), page_rect.top(), page_rect.width(), bleed), shade)
            painter.fillRect(QRectF(page_rect.left(), page_rect.bottom() - bleed, page_rect.width(), bleed), shade)
            painter.fillRect(QRectF(page_rect.left(), trim.top(), bleed, trim.height()), shade)
            painter.fillRect(QRectF(page_rect.right() - bleed, trim.top(), bleed, trim.height()), shade)
            painter.setPen(QPen(QColor(200, 40, 120, 200), 1))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(trim)
        inner = self.page.inner_rect_mm()
        painter.setPen(QPen(QColor(28, 126, 214, 150), 1, Qt.PenStyle.DashLine))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        self._draw_rect(painter, inner)

    def _draw_plain(self, painter: QPainter) -> None:
        """Without a renderer (tests, or before the first render): frames and balloon boxes."""
        painter.setPen(QPen(QColor("#111111"), 2))
        for frame in self.page.leaf_frames():
            self._draw_frame(painter, frame)
        for line in self.lines:
            self._draw_balloon_box(painter, line, None)

    def _draw_selection(self, painter: QPainter) -> None:
        hover_frame = None
        if self.tool in ("select", "frame") and self._hover and not self._drag_line:
            if self._hit_line(*self._hover) is None:
                hover_frame = self.page.frame_at(*self._hover)
        for frame in self.page.leaf_frames():
            if frame.id == self.page.selected_frame_id:
                painter.setPen(QPen(QColor("#1c7ed6"), 3))
                painter.setBrush(QColor(28, 126, 214, 16))
                self._draw_frame(painter, frame)
            elif hover_frame is not None and frame.id == hover_frame.id:
                painter.setPen(QPen(QColor(28, 126, 214, 150), 1.5, Qt.PenStyle.DashLine))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                self._draw_frame(painter, frame)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        if self.tool == "frame":
            self._draw_frame_tool(painter)

    def _draw_frame(self, painter: QPainter, frame) -> None:
        from genko.frames import shape

        painter.drawPolygon([self._pt(x, y) for x, y in shape(frame)])

    # --- the panel tool: drag gutters, cut panels (any angle), move corners -------------------------------

    def _gutters(self) -> list[dict]:
        from genko.frames import gutters

        return gutters(self.page.frames[0]) if self.page and self.page.frames else []

    def _hit_gutter(self, x_mm: float, y_mm: float):
        from genko.frames import distance_to_segment

        tolerance = 6 / self._scale
        best = None
        for gutter in self._gutters():
            d = distance_to_segment((x_mm, y_mm), gutter["p0"], gutter["p1"])
            if d <= gutter["width"] / 2 + tolerance and (best is None or d < best[0]):
                best = (d, gutter)
        return best[1] if best else None

    def _vertex_handles(self) -> list[tuple[int, tuple[float, float]]]:
        from genko.frames import shape

        if self.tool != "frame" or not self.page or not self.page.selected_frame_id:
            return []
        try:
            frame = self.page._find(self.page.selected_frame_id)
        except (KeyError, IndexError):
            return []
        drag = self._frame_drag
        pts = drag["poly"] if drag and drag["kind"] == "vertex" else shape(frame)
        return list(enumerate(pts))

    def _draw_frame_tool(self, painter: QPainter) -> None:
        drag = self._frame_drag
        hover = self._hit_gutter(*self._hover) if self._hover and not drag else None
        for gutter in self._gutters():
            strong = hover is gutter or (drag and drag["kind"] == "gutter" and drag["gutter"]["node"] == gutter["node"]
                                        and drag["gutter"]["index"] == gutter["index"])
            if not strong:
                continue
            p0, p1 = gutter["p0"], gutter["p1"]
            if drag and drag["kind"] == "gutter":
                dx, dy = drag["offset"]
                p0, p1 = (p0[0] + dx, p0[1] + dy), (p1[0] + dx, p1[1] + dy)
            painter.setPen(QPen(QColor(232, 89, 12, 160), max(3.0, gutter["width"] * self._scale)))
            painter.drawLine(self._pt(*p0), self._pt(*p1))
        if drag and drag["kind"] == "cut":
            painter.setPen(QPen(QColor("#e03131"), 2, Qt.PenStyle.DashLine))
            painter.drawLine(self._pt(*drag["p0"]), self._pt(*drag["p1"]))
        if drag and drag["kind"] == "vertex":
            painter.setPen(QPen(QColor("#e8590c"), 2, Qt.PenStyle.DashLine))
            painter.drawPolygon([self._pt(x, y) for x, y in drag["poly"]])
        for _i, (x, y) in self._vertex_handles():
            p = self._pt(x, y)
            painter.setPen(QPen(QColor("#1c7ed6"), 1.5))
            painter.setBrush(QColor("white"))
            painter.drawEllipse(p, HANDLE_PX / 2 + 1, HANDLE_PX / 2 + 1)
        painter.setBrush(Qt.BrushStyle.NoBrush)

    def _frame_press(self, pos: QPointF) -> None:
        from genko.frames import shape

        x_mm, y_mm = self._to_mm(pos)
        for i, (vx, vy) in self._vertex_handles():
            p = self._pt(vx, vy)
            if abs(p.x() - pos.x()) <= HANDLE_PX + 2 and abs(p.y() - pos.y()) <= HANDLE_PX + 2:
                frame = self.page._find(self.page.selected_frame_id)
                self._frame_drag = {"kind": "vertex", "index": i, "frame": frame.id, "poly": [list(p) for p in shape(frame)]}
                return
        gutter = self._hit_gutter(x_mm, y_mm)
        if gutter is not None:
            self._frame_drag = {"kind": "gutter", "gutter": gutter, "start": (x_mm, y_mm), "offset": (0.0, 0.0), "delta": 0.0}
            return
        frame = self.page.frame_at(x_mm, y_mm)
        # a cut may start outside the panels (from the margin across): the panel is found on release
        self._frame_drag = {"kind": "cut", "frame": frame.id if frame else None, "p0": (x_mm, y_mm), "p1": (x_mm, y_mm)}

    def _frame_move(self, pos: QPointF) -> None:
        import math

        drag = self._frame_drag
        x_mm, y_mm = self._to_mm(pos)
        if drag["kind"] == "vertex":
            drag["poly"][drag["index"]] = [round(x_mm, 2), round(y_mm, 2)]
        elif drag["kind"] == "gutter":
            g = drag["gutter"]
            (ax, ay), (bx, by) = g["p0"], g["p1"]
            length = math.hypot(bx - ax, by - ay) or 1.0
            nx, ny = -(by - ay) / length, (bx - ax) / length
            if (g["horizontal"] and ny < 0) or (not g["horizontal"] and nx < 0):
                nx, ny = -nx, -ny
            delta = (x_mm - drag["start"][0]) * nx + (y_mm - drag["start"][1]) * ny
            drag["delta"], drag["offset"] = delta, (nx * delta, ny * delta)
        else:
            x0, y0 = drag["p0"]
            dx, dy = x_mm - x0, y_mm - y0
            # nearly level or upright cuts snap straight (hold Alt for a free angle)
            free = bool(self._modifiers & Qt.KeyboardModifier.AltModifier)
            if not free and abs(dy) <= abs(dx) * 0.07:
                y_mm = y0
            elif not free and abs(dx) <= abs(dy) * 0.07:
                x_mm = x0
            drag["p1"] = (x_mm, y_mm)
        self.update()

    def _frame_release(self) -> None:
        import math

        drag, self._frame_drag = self._frame_drag, None
        if drag["kind"] == "vertex":
            self.frameShaped.emit(drag["frame"], drag["poly"])
        elif drag["kind"] == "gutter":
            if abs(drag["delta"]) > 0.2:
                self.gutterMoved.emit(drag["gutter"]["node"], drag["gutter"]["index"], round(drag["delta"], 2))
        elif math.dist(drag["p0"], drag["p1"]) >= 5:
            mid = ((drag["p0"][0] + drag["p1"][0]) / 2, (drag["p0"][1] + drag["p1"][1]) / 2)
            target = self.page.frame_at(*mid)
            frame_id = target.id if target is not None else drag["frame"]
            if frame_id:
                self.cutRequested.emit(frame_id, QPointF(*drag["p0"]), QPointF(*drag["p1"]))
        elif drag["frame"]:
            self.frameSelected.emit(drag["frame"])
        self.update()

    # --- balloon handles -----------------------------------------------------------------------------

    def _selected_line(self) -> StoryLine | None:
        return next((ln for ln in self.lines if ln.id == self.selected_line_id), None)

    @staticmethod
    def _tails(line) -> list[dict]:
        tails = [dict(t) for t in (getattr(line, "tails", None) or []) if t.get("to")]
        if not tails and line.tail:
            tails = [{"to": list(line.tail)}]
        return tails

    def _handles(self) -> list[tuple[str, object, tuple[float, float]]]:
        """(kind, key, point in mm) of the selected balloon: 8 resize handles, and per tail its tip and bend."""
        line = self._selected_line()
        if line is None or self.tool != "select":
            return []
        box = self._handle_drag["cur"] if self._handle_drag and self._handle_drag["kind"] == "resize" else (
            line.x_mm, line.y_mm, line.w_mm, line.h_mm)
        x, y, w, h = box
        out: list = []
        for key, (fx, fy) in {"nw": (0, 0), "n": (0.5, 0), "ne": (1, 0), "e": (1, 0.5), "se": (1, 1), "s": (0.5, 1),
                              "sw": (0, 1), "w": (0, 0.5)}.items():
            out.append(("resize", key, (x + w * fx, y + h * fy)))
        tails = self._handle_drag["tails"] if self._handle_drag and self._handle_drag["kind"] == "tail" else self._tails(line)
        cx, cy = x + w / 2, y + h / 2
        for i, tail in enumerate(tails):
            tip = tail["to"]
            via = tail.get("via") or [(cx + tip[0]) / 2, (cy + tip[1]) / 2]
            out.append(("tail", (i, "to"), (tip[0], tip[1])))
            out.append(("tail", (i, "via"), (via[0], via[1])))
        return out

    def _hit_handle(self, pos: QPointF):
        for kind, key, (hx, hy) in reversed(self._handles()):
            p = self._pt(hx, hy)
            if abs(p.x() - pos.x()) <= HANDLE_PX + 2 and abs(p.y() - pos.y()) <= HANDLE_PX + 2:
                return kind, key
        return None

    def _draw_handles(self, painter: QPainter) -> None:
        line = self._selected_line()
        if line is None or self.tool != "select":
            return
        drag = self._handle_drag
        if drag and drag["kind"] == "resize":
            x, y, w, h = drag["cur"]
            p = self._pt(x, y)
            painter.setPen(QPen(QColor("#e8590c"), 2, Qt.PenStyle.DashLine))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(QRectF(p.x(), p.y(), w * self._scale, h * self._scale))
        if drag and drag["kind"] == "tail":
            painter.setPen(QPen(QColor("#e8590c"), 2, Qt.PenStyle.DashLine))
            cx, cy = line.x_mm + line.w_mm / 2, line.y_mm + line.h_mm / 2
            for tail in drag["tails"]:
                path = QPainterPath(self._pt(cx, cy))
                via = tail.get("via") or [(cx + tail["to"][0]) / 2, (cy + tail["to"][1]) / 2]
                path.quadTo(self._pt(*via), self._pt(*tail["to"]))
                painter.drawPath(path)
        for kind, key, (hx, hy) in self._handles():
            p = self._pt(hx, hy)
            painter.setPen(QPen(QColor("#e8590c"), 1.5))
            painter.setBrush(QColor("white"))
            if kind == "resize":
                painter.drawRect(QRectF(p.x() - HANDLE_PX / 2, p.y() - HANDLE_PX / 2, HANDLE_PX, HANDLE_PX))
            elif key[1] == "to":
                painter.setBrush(QColor("#e8590c"))
                painter.drawEllipse(p, HANDLE_PX / 2 + 1, HANDLE_PX / 2 + 1)
            else:
                painter.drawPolygon([p + QPointF(0, -5), p + QPointF(5, 0), p + QPointF(0, 5), p + QPointF(-5, 0)])
        painter.setBrush(Qt.BrushStyle.NoBrush)

    def _drag_handle(self, pos: QPointF) -> None:
        drag = self._handle_drag
        x_mm, y_mm = self._to_mm(pos)
        if drag["kind"] == "resize":
            x, y, w, h = drag["orig"]
            key = drag["key"]
            left, top, right, bottom = x, y, x + w, y + h
            if "w" in key:
                left = min(x_mm, right - 4)
            if "e" in key:
                right = max(x_mm, left + 4)
            if key.startswith("n"):
                top = min(y_mm, bottom - 4)
            if key.startswith("s"):
                bottom = max(y_mm, top + 4)
            drag["cur"] = (round(left, 2), round(top, 2), round(right - left, 2), round(bottom - top, 2))
        else:
            index, part = drag["key"]
            drag["tails"][index][part] = [round(x_mm, 2), round(y_mm, 2)]
        self.update()

    # --- selections: rectangle, lasso, auto; move / scale / rotate by handles -------------------------------

    def _marquee_points(self) -> list:
        if self.tool == "marquee" and self.marquee == "rect" and len(self._stroke) >= 2:
            (x0, y0), (x1, y1) = self._stroke[0][:2], self._stroke[-1][:2]
            return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
        return [p[:2] for p in self._stroke]

    def set_selection(self, area: dict | None, outline: list | None = None) -> None:
        if area is None:
            self.selection = None
        else:
            if outline is None:
                if area.get("poly"):
                    outline = [list(p) for p in area["poly"]]
                else:
                    x, y, w, h = area["mask"]["box"]
                    outline = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]
            self.selection = {"area": area, "outline": outline}
        self.update()

    def _sel_box(self, outline=None):
        pts = outline or self.selection["outline"]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return min(xs), min(ys), max(xs), max(ys)

    def _sel_handles(self) -> list:
        if not self.selection or self.tool != "marquee":
            return []
        x0, y0, x1, y1 = self._sel_box()
        cx = (x0 + x1) / 2
        out = [("scale", key, (x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy)) for key, (fx, fy) in
               {"nw": (0, 0), "n": (0.5, 0), "ne": (1, 0), "e": (1, 0.5), "se": (1, 1), "s": (0.5, 1), "sw": (0, 1), "w": (0, 0.5)}.items()]
        out.append(("rotate", "r", (cx, y0 - 18 / self._scale)))
        return out

    def _sel_matrix(self, pos_mm) -> list:
        import math

        drag = self._sel_drag
        x0, y0, x1, y1 = drag["box"]
        px, py = pos_mm
        sx0, sy0 = drag["start"]
        if drag["kind"] == "move":
            return [1, 0, 0, 1, px - sx0, py - sy0]
        if drag["kind"] == "rotate":
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            angle = math.atan2(py - cy, px - cx) - math.atan2(sy0 - cy, sx0 - cx)
            if self._modifiers & Qt.KeyboardModifier.ShiftModifier:
                angle = round(angle / (math.pi / 12)) * (math.pi / 12)
            c, s = math.cos(angle), math.sin(angle)
            return [c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy]
        key = drag["key"]
        ax = x1 if "w" in key else x0 if "e" in key else (x0 + x1) / 2
        ay = y1 if key.startswith("n") else y0 if key.startswith("s") else (y0 + y1) / 2
        sx = (px - ax) / ((sx0 - ax) or 1e-6) if ("w" in key or "e" in key) else 1.0
        sy = (py - ay) / ((sy0 - ay) or 1e-6) if (key.startswith("n") or key.startswith("s")) else 1.0
        if self._modifiers & Qt.KeyboardModifier.ShiftModifier and key in ("nw", "ne", "se", "sw"):
            sx = sy = (abs(sx) + abs(sy)) / 2 * (1 if sx * sy > 0 else -1)
        return [sx, 0, 0, sy, ax - sx * ax, ay - sy * ay]

    @staticmethod
    def _apply(m, pts):
        a, b, c, d, e, f = m
        return [[a * x + c * y + e, b * x + d * y + f] for x, y in pts]

    def _draw_selection_overlay(self, painter: QPainter) -> None:
        if not self.selection:
            return
        outline = self.selection["outline"]
        if self._sel_drag and self._sel_drag.get("matrix"):
            outline = self._apply(self._sel_drag["matrix"], outline)
        pts = [self._pt(*p) for p in outline]
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.setPen(QPen(QColor("white"), 1.5))
        painter.drawPolygon(pts)
        painter.setPen(QPen(QColor("#1c7ed6"), 1.5, Qt.PenStyle.DashLine))
        painter.drawPolygon(pts)
        for kind, _key, (hx, hy) in self._sel_handles():
            p = self._pt(hx, hy)
            painter.setPen(QPen(QColor("#1c7ed6"), 1.5))
            painter.setBrush(QColor("white"))
            if kind == "rotate":
                painter.drawEllipse(p, 5, 5)
            else:
                painter.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8))
        painter.setBrush(Qt.BrushStyle.NoBrush)

    def _marquee_press(self, pos: QPointF) -> bool:
        """Start moving / scaling / turning the selection; False when the press starts a new one."""
        if not self.selection:
            return False
        x_mm, y_mm = self._to_mm(pos)
        for kind, key, (hx, hy) in self._sel_handles():
            p = self._pt(hx, hy)
            if abs(p.x() - pos.x()) <= 7 and abs(p.y() - pos.y()) <= 7:
                self._sel_drag = {"kind": kind, "key": key, "start": (x_mm, y_mm), "box": self._sel_box()}
                return True
        from genko.selection import contains

        if contains({"poly": self.selection["outline"]}, x_mm, y_mm):
            self._sel_drag = {"kind": "move", "key": "", "start": (x_mm, y_mm), "box": self._sel_box()}
            return True
        return False

    # --- reshaping a line (つまむ) ----------------------------------------------------------------------

    def _reshape_press(self, x_mm: float, y_mm: float) -> None:
        """Grab the nearest line (anywhere along it); it is walked in 1 mm steps so the pinch bends smoothly."""
        import math

        strokes = self.strokes_for_reshape() if self.strokes_for_reshape else []
        reach = max(1.0, 10 / self._scale)  # about 10 px on screen, at least 1 mm
        best = None
        for stroke in strokes:
            pts = stroke.points
            for a, b in zip(pts, pts[1:] or pts):
                dx, dy = b[0] - a[0], b[1] - a[1]
                seg = dx * dx + dy * dy
                t = 0.0 if seg == 0 else max(0.0, min(1.0, ((x_mm - a[0]) * dx + (y_mm - a[1]) * dy) / seg))
                d = math.hypot(x_mm - (a[0] + t * dx), y_mm - (a[1] + t * dy))
                if d <= reach and (best is None or d < best[0]):
                    best = (d, stroke)
        if best is None:
            return
        stroke = best[1]
        pressure = stroke.pressure if len(stroke.pressure) == len(stroke.points) else [None] * len(stroke.points)
        src = [[x, y] + ([p] if p is not None else []) for (x, y), p in zip(stroke.points, pressure)]
        points = [src[0]]
        for a, b in zip(src, src[1:]):
            n = max(1, math.ceil(math.dist(a[:2], b[:2]) / 1.0))
            for k in range(1, n + 1):
                t = k / n
                pt = [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]
                if len(a) > 2 and len(b) > 2:
                    pt.append(a[2] + (b[2] - a[2]) * t)
                points.append(pt)
        self._reshape = {"id": stroke.id, "orig": [list(p) for p in points], "points": points, "grab": (x_mm, y_mm)}

    def _reshape_move(self, x_mm: float, y_mm: float) -> None:
        import math

        gx, gy = self._reshape["grab"]
        dx, dy = x_mm - gx, y_mm - gy
        radius = max(0.5, self.reshape_radius_mm)
        moved = []
        for pt in self._reshape["orig"]:
            w = max(0.0, 1 - math.hypot(pt[0] - gx, pt[1] - gy) / radius) ** 2
            moved.append([pt[0] + dx * w, pt[1] + dy * w] + pt[2:])
        self._reshape["points"] = moved
        self.update()

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

    def _draw_balloon_box(self, painter: QPainter, line: StoryLine, at: tuple[float, float] | None, strong: bool = False,
                          fill: bool = True) -> None:
        p = self._pt(*(at or (line.x_mm, line.y_mm)))
        rect = QRectF(p.x(), p.y(), max(10, line.w_mm * self._scale), max(10, line.h_mm * self._scale))
        painter.setPen(QPen(QColor("#e8590c"), 2 if strong else 1.5, Qt.PenStyle.DashLine))
        painter.setBrush(QColor(255, 255, 255, 170) if strong and fill else Qt.BrushStyle.NoBrush)
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
        elif self.tool == "text":
            self.setCursor(Qt.CursorShape.IBeamCursor)
        elif self.tool in ("picker", "fill", "lassofill", "marquee", "reshape"):
            self.setCursor(Qt.CursorShape.PointingHandCursor if self.tool in ("picker", "fill") else Qt.CursorShape.CrossCursor)
        elif self.tool == "frame" and pos is not None and self.page is not None:
            gutter = self._hit_gutter(*self._to_mm(pos))
            if gutter is not None:
                self.setCursor(Qt.CursorShape.SplitVCursor if gutter["horizontal"] else Qt.CursorShape.SplitHCursor)
            else:
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
        if self.tool == "text":
            self.textRequested.emit(x_mm, y_mm)
            return
        if self.tool == "frame":
            self._modifiers = event.modifiers()
            self._frame_press(pos)
            self.update()
            return
        if self.tool == "picker":
            self._pick_colour(pos)
            return
        if self.tool == "fill":
            self.fillRequested.emit(x_mm, y_mm)
            return
        if self.tool == "marquee":
            self._modifiers = event.modifiers()
            if self._marquee_press(pos):
                return
            if self.marquee == "wand":
                self.wandRequested.emit(x_mm, y_mm)
                return
            self.set_selection(None)
        if self.tool == "reshape":
            self._reshape_press(x_mm, y_mm)
            return
        if self.tool == "select":
            handle = self._hit_handle(pos)
            if handle is not None:
                line = self._selected_line()
                kind, key = handle
                self._handle_drag = {"kind": kind, "key": key, "line": line.id,
                                     "orig": (line.x_mm, line.y_mm, line.w_mm, line.h_mm),
                                     "cur": (line.x_mm, line.y_mm, line.w_mm, line.h_mm),
                                     "tails": [dict(t, to=list(t["to"])) for t in self._tails(line)]}
                return
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
        if self._handle_drag is not None:
            self._drag_handle(pos)
            return
        if self._frame_drag is not None:
            self._modifiers = event.modifiers()
            self._frame_move(pos)
            return
        if self._sel_drag is not None:
            self._modifiers = event.modifiers()
            self._sel_drag["matrix"] = self._sel_matrix(self._to_mm(pos))
            self.update()
            return
        if self._reshape is not None:
            self._reshape_move(*self._to_mm(pos))
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
        if self._frame_drag is not None:
            self._press_pos = None
            self._frame_release()
            return
        if self._sel_drag is not None:
            drag, self._sel_drag = self._sel_drag, None
            matrix = drag.get("matrix")
            if matrix and any(abs(v - w) > 1e-4 for v, w in zip(matrix, [1, 0, 0, 1, 0, 0])):
                self.selectionTransformed.emit(matrix)
            self.update()
            return
        if self._reshape is not None:
            shape, self._reshape = self._reshape, None
            if shape["points"] != shape["orig"]:
                self.strokeReshaped.emit(shape["id"], shape["points"])
            self.update()
            return
        if self.tool in ("lassofill", "marquee") and self._stroke:
            pts = [list(p) for p in self._marquee_points()]
            self._stroke = []
            import math

            if len(pts) >= 3 and max(math.dist(pts[0], p) for p in pts) > 1.0:
                if self.tool == "lassofill":
                    self.areaFilled.emit(pts)
                else:
                    self.set_selection({"poly": pts})
                    self.areaSelected.emit({"poly": pts})
            self.update()
            return
        if self._handle_drag is not None:
            drag, self._handle_drag = self._handle_drag, None
            self._press_pos = None
            if drag["kind"] == "resize" and drag["cur"] != drag["orig"]:
                x, y, w, h = drag["cur"]
                self.lineGeometry.emit(drag["line"], {"x_mm": x, "y_mm": y, "w_mm": w, "h_mm": h})
            elif drag["kind"] == "tail":
                self.lineGeometry.emit(drag["line"], {"tails": drag["tails"]})
            self.update()
            return
        if self._drag_line is not None:
            line, pos = self._drag_line, self._drag_pos
            self._drag_line = None
            self._drag_pos = None
            self._press_pos = None
            self.selected_line_id = line.id
            if pos is not None and (abs(pos[0] - line.x_mm) > 0.05 or abs(pos[1] - line.y_mm) > 0.05):
                self.textMoved.emit(line.id, round(pos[0], 2), round(pos[1], 2))  # becomes a move_line op
            self.lineSelected.emit(line.id, False)
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
        if len(self._stroke) == 1:
            x, y = self._stroke[0][:2]
            self._stroke.append((x + 0.01, y + 0.01))  # a tap: a dot with the pen, a spot with the eraser
        packed = [pack_point(float(pt[0]), float(pt[1]), float(pt[2]) if len(pt) > 2 else None) for pt in self._stroke]
        self._stroke = []
        self.strokeCommitted.emit(packed)
        self.changed.emit()
        self.update()

    def _pick_colour(self, pos: QPointF) -> None:
        if self.background is None or self.page is None:
            return
        x_mm, y_mm = self._to_mm(pos)
        image = self.background.toImage()
        px = int(x_mm / self.page.spec.width_mm * image.width())
        py = int(y_mm / self.page.spec.height_mm * image.height())
        if 0 <= px < image.width() and 0 <= py < image.height():
            colour = image.pixelColor(px, py)
            self.colourPicked.emit((colour.red(), colour.green(), colour.blue()))

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.MiddleButton:
            self.fit_page()
            return
        if self.page is not None and event.button() == Qt.MouseButton.LeftButton and self.tool == "select":
            hit = self._hit_line(*self._to_mm(event.position()))
            if hit is not None:
                self.selected_line_id = hit.id
                self.lineSelected.emit(hit.id, False)
                self.lineEditRequested.emit(hit.id)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._hover = None
        self.update()

    def contextMenuEvent(self, event) -> None:  # noqa: N802
        if self.page is None:
            return
        hit = self._hit_line(*self._to_mm(QPointF(event.pos())))
        if hit is not None:
            self.selected_line_id = hit.id
            self.update()
            self.lineContextMenu.emit(hit.id, QPointF(event.globalPos()))
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
        if event.key() == Qt.Key.Key_Escape and self.selection is not None:
            self.set_selection(None)
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
            if len(stroke) == 1:
                x, y, *rest = stroke[0]
                stroke.append((x + 0.01, y + 0.01, *rest))  # a tap with the pen is a dot
            self.strokeCommitted.emit(stroke)
            self.changed.emit()
            self.update()
            event.accept()
