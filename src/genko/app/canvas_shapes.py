"""Canvas input for J2: figures (直線・折れ線・曲線・長方形・楕円・多角形), the selection's other shapes
(楕円・折れ線・選択ペン・選択消し・色域) with Shift to add, Alt to take away, both for the overlap, and
the scales along the top and left edges, from which guide lines are pulled out.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QRectF, Qt, Signal  # noqa: F401  (Signal: the canvas declares them)
from PySide6.QtGui import QColor, QFont, QPainter, QPen

SCALE_PX = 16  # the scales' width on screen


class ShapeSelectMixin:
    # --- set up (called from the canvas's __init__) ---------------------------------------------------

    def _init_shapes(self) -> None:
        self.shape_kind = "line"  # line | polyline | curve | rect | ellipse | polygon
        self.shape_sides = 5
        self._shape_drag: list | None = None  # [start, end] (mm) for figures drawn by dragging
        self._shape_pts: list = []  # clicked points (polyline, curve; the polyline selection)
        self.show_scale = False
        self._guide_drag: dict | None = None
        self.selection_pen_mm = 4.0
        self._sel_how = "replace"
        self.quick_mask = False
        self.launcher = None  # the small bar of what to do with a selection (set by the window)
        self._mask_picture = None  # (area id, QImage) of a mask selection shown on the page

    # --- how a new selection joins the old one ---------------------------------------------------------

    @staticmethod
    def how_from(modifiers) -> str:
        shift = bool(modifiers & Qt.KeyboardModifier.ShiftModifier)
        alt = bool(modifiers & Qt.KeyboardModifier.AltModifier)
        return "intersect" if shift and alt else "add" if shift else "subtract" if alt else "replace"

    def _selection_done(self, area: dict) -> None:
        how = self._sel_how
        if how == "replace" or self.selection is None:
            self.set_selection(area)
        self.selectionDrawn.emit(area, how)
        self.areaSelected.emit(area)

    # --- figures -----------------------------------------------------------------------------------------

    def _shape_press(self, x: float, y: float, modifiers) -> None:
        self._modifiers = modifiers
        if self.shape_kind in ("polyline", "curve"):
            self._shape_pts.append((x, y))
        else:
            self._shape_drag = [(x, y), (x, y)]
        self.update()

    def _shape_move(self, x: float, y: float, modifiers) -> None:
        if self._shape_drag is not None:
            self._modifiers = modifiers
            self._shape_drag[1] = self._constrained(self._shape_drag[0], (x, y), modifiers)
            self.update()

    def _constrained(self, a, b, modifiers):
        """Shift: lines at steps of 45°, figures square."""
        if not modifiers & Qt.KeyboardModifier.ShiftModifier:
            return b
        dx, dy = b[0] - a[0], b[1] - a[1]
        if self.shape_kind == "line" and self.tool != "marquee":
            angle = round(math.atan2(dy, dx) / (math.pi / 4)) * (math.pi / 4)
            length = math.hypot(dx, dy)
            return a[0] + length * math.cos(angle), a[1] + length * math.sin(angle)
        side = max(abs(dx), abs(dy))
        return a[0] + math.copysign(side, dx or 1), a[1] + math.copysign(side, dy or 1)

    def _shape_release(self) -> None:
        if self._shape_drag is None:
            return
        (x0, y0), (x1, y1) = self._shape_drag
        self._shape_drag = None
        if math.dist((x0, y0), (x1, y1)) < 0.5:
            self.update()
            return
        if self.shape_kind == "line":
            self.shapeDrawn.emit({"shape": "line", "points": [[round(x0, 3), round(y0, 3)], [round(x1, 3), round(y1, 3)]]})
        else:
            box = [round(min(x0, x1), 3), round(min(y0, y1), 3), round(abs(x1 - x0), 3), round(abs(y1 - y0), 3)]
            shape = {"shape": self.shape_kind, "box": box}
            if self.shape_kind == "polygon":
                shape["sides"] = self.shape_sides
            self.shapeDrawn.emit(shape)
        self.update()

    def finish_points(self, closed: bool = False) -> bool:
        """Enter or a double-click ends a polyline, a curve or a polyline selection."""
        pts, self._shape_pts = self._shape_pts, []
        self.update()
        if self.tool == "marquee":
            if len(pts) >= 3:
                self._selection_done({"poly": [[round(x, 3), round(y, 3)] for x, y in pts]})
                return True
            return False
        if len(pts) < 2:
            return False
        shape = {"shape": self.shape_kind, "points": [[round(x, 3), round(y, 3)] for x, y in pts]}
        if closed:
            shape["closed"] = True
        self.shapeDrawn.emit(shape)
        return True

    def cancel_points(self) -> bool:
        if not self._shape_pts and self._shape_drag is None:
            return False
        self._shape_pts, self._shape_drag = [], None
        self.update()
        return True

    def _shape_preview(self) -> list:
        """The figure being drawn, as the points of its outline (mm)."""
        from genko.ops import ApplyError, shape_points

        if self._shape_drag is not None:
            (x0, y0), (x1, y1) = self._shape_drag
            if self.shape_kind == "line" or self.tool == "marquee":
                if self.tool == "marquee" and self.marquee == "ellipse":
                    from genko.selops import ellipse_poly

                    return [tuple(p) for p in ellipse_poly([min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0)])] + [None]
                return [(x0, y0), (x1, y1)]
            try:
                pts, closed = shape_points(self.shape_kind, {"box": [min(x0, x1), min(y0, y1), abs(x1 - x0) or 0.01, abs(y1 - y0) or 0.01],
                                                             "sides": self.shape_sides})
            except ApplyError:
                return []
            return pts + ([pts[0]] if closed else [])
        if self._shape_pts:
            pts = list(self._shape_pts) + ([self._hover] if self._hover else [])
            if self.shape_kind == "curve" and self.tool != "marquee" and len(pts) >= 3:
                try:
                    return shape_points("curve", {"points": pts})[0]
                except ApplyError:
                    return pts
            return pts
        return []

    def _draw_shape_preview(self, painter: QPainter) -> None:
        pts = self._shape_preview()
        if not pts:
            return
        closed = pts[-1] is None
        pts = [p for p in pts if p is not None]
        painter.setPen(QPen(QColor("#1c7ed6") if self.tool == "marquee" else QColor("#e8590c"), 1.5,
                            Qt.PenStyle.DashLine if self.tool == "marquee" else Qt.PenStyle.SolidLine))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        q = [self._pt(*p) for p in pts]
        if closed:
            painter.drawPolygon(q)
        else:
            painter.drawPolyline(q)
        for p in self._shape_pts:
            c = self._pt(*p)
            painter.drawEllipse(c, 3, 3)

    # --- the scales and guide lines ----------------------------------------------------------------------

    def _scale_hit(self, pos: QPointF) -> str | None:
        """Which scale a press on the screen falls on: "h" (the top: a guide across) or "v" (the left)."""
        if not self.show_scale:
            return None
        if pos.y() < SCALE_PX and pos.x() >= SCALE_PX:
            return "h"
        if pos.x() < SCALE_PX and pos.y() >= SCALE_PX:
            return "v"
        return None

    def _guide_press(self, pos: QPointF) -> bool:
        axis = self._scale_hit(pos)
        if axis is None:
            return False
        self._guide_drag = {"axis": axis, "at": None}
        return True

    def _guide_move(self, pos: QPointF) -> bool:
        if self._guide_drag is None:
            return False
        x, y = self._to_mm(self._ev(pos))
        self._guide_drag["at"] = round(y if self._guide_drag["axis"] == "h" else x, 2)
        self.update()
        return True

    def _guide_release(self, pos: QPointF) -> bool:
        if self._guide_drag is None:
            return False
        drag, self._guide_drag = self._guide_drag, None
        if drag["at"] is not None and self._scale_hit(pos) is None and pos.x() >= SCALE_PX and pos.y() >= SCALE_PX:
            self.rulerPlaced.emit({"kind": "guide", "axis": drag["axis"], "at": drag["at"]})
        self.update()
        return True

    def _draw_guide_drag(self, painter: QPainter) -> None:
        if not self._guide_drag or self._guide_drag.get("at") is None or self.page is None:
            return
        at = self._guide_drag["at"]
        spec = self.page.spec
        a, b = ((-50, at), (spec.width_mm + 50, at)) if self._guide_drag["axis"] == "h" else ((at, -50), (at, spec.height_mm + 50))
        painter.setPen(QPen(QColor(0, 170, 200), 1, Qt.PenStyle.DashLine))
        painter.drawLine(self._pt(*a), self._pt(*b))

    def _draw_scale(self, painter: QPainter) -> None:
        """mm scales along the top and the left, following the view (zoom and pan; turned views show none)."""
        if not self.show_scale or self.page is None:
            return
        painter.save()
        painter.resetTransform()
        w, h = self.width(), self.height()
        painter.fillRect(QRectF(0, 0, w, SCALE_PX), QColor(235, 235, 238))
        painter.fillRect(QRectF(0, 0, SCALE_PX, h), QColor(235, 235, 238))
        painter.setPen(QColor(90, 90, 96))
        font = QFont(painter.font())
        font.setPixelSize(9)
        painter.setFont(font)
        if abs(self.rotation % 360) < 0.01:
            view = self._view()
            step = 1.0
            while step * self._scale < 5:
                step *= 5 if str(step)[0] == "1" else 2
            spec = self.page.spec
            k = 0
            while k * step <= spec.width_mm + 1e-6:
                x = view.map(self._pt(k * step, 0)).x()
                major = round(k * step) % (10 if step <= 2 else 50) == 0
                if SCALE_PX <= x <= w:
                    painter.drawLine(QPointF(x, SCALE_PX), QPointF(x, SCALE_PX - (8 if major else 4)))
                    if major:
                        painter.drawText(QPointF(x + 2, 9), f"{round(k * step)}")
                k += 1
            k = 0
            while k * step <= spec.height_mm + 1e-6:
                y = view.map(self._pt(0, k * step)).y()
                major = round(k * step) % (10 if step <= 2 else 50) == 0
                if SCALE_PX <= y <= h:
                    painter.drawLine(QPointF(SCALE_PX, y), QPointF(SCALE_PX - (8 if major else 4), y))
                    if major:
                        painter.drawText(QPointF(1, y - 2), f"{round(k * step)}")
                k += 1
        painter.fillRect(QRectF(0, 0, SCALE_PX, SCALE_PX), QColor(220, 220, 224))
        painter.restore()

    # --- a mask selection (or the quick mask) shown as a tint ----------------------------------------------

    def _draw_selection_mask(self, painter: QPainter) -> None:
        """A selection that is not a simple outline (auto-select, colour, pen, joined) shows as a tint of its
        own shape; the quick mask shows what is NOT selected in red, as in other drawing programs."""
        if not self.selection or self.page is None:
            return
        area = self.selection["area"]
        if not (area.get("mask") or self.quick_mask):
            return
        from PySide6.QtGui import QImage

        from genko import selops

        key = (id(area), self.quick_mask)
        if self._mask_picture is None or self._mask_picture[0] != key:
            dpi = 60
            mask = selops.to_mask(area, self.page, None, dpi)
            if self.quick_mask:
                from PIL import ImageChops

                mask = ImageChops.invert(mask)
            colour = (220, 40, 40, 110) if self.quick_mask else (28, 126, 214, 70)
            from PIL import Image

            tint = Image.new("RGBA", mask.size, colour[:3] + (0,))
            tint.putalpha(mask.point(lambda v, a=colour[3]: v * a // 255))
            data = tint.tobytes()
            picture = QImage(data, tint.width, tint.height, tint.width * 4, QImage.Format.Format_RGBA8888).copy()
            self._mask_picture = (key, picture)
        spec = self.page.spec
        painter.drawImage(QRectF(self._pt(0, 0), self._pt(spec.width_mm, spec.height_mm)), self._mask_picture[1])

    def _place_launcher(self) -> None:
        """Keep the selection launcher just under the selection (hidden while there is none)."""
        bar = self.launcher
        if bar is None:
            return
        if not self.selection or self._sel_drag or self.warp is not None or self.page is None:
            bar.hide()
            return
        outline = self.selection["outline"]
        view = self._view()
        pts = [view.map(self._pt(*p)) for p in outline]
        x = sum(p.x() for p in pts) / len(pts) - bar.sizeHint().width() / 2
        y = max(p.y() for p in pts) + 10
        x = max(4, min(self.width() - bar.sizeHint().width() - 4, x))
        y = max(4, min(self.height() - bar.sizeHint().height() - 4, y))
        bar.adjustSize()
        bar.move(int(x), int(y))
        bar.show()
        bar.raise_()
