"""The vector tool (J4, 線の編集): pick a line of the layer drawn on, see its control points, drag one,
Alt+click on the line to add one, Delete to take a point (or the chosen lines) away, cut a line where it is
clicked, join two lines (Shift+click the second), give the chosen lines the pen's colour.
"""

from __future__ import annotations

import math

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QPen

PICK_PX = 8.0


class VectorMixin:
    def _init_vector(self) -> None:
        self.vector_ids: list[str] = []
        self.vector_point: int | None = None  # the chosen control point of the first chosen line
        self.vector_cut = False
        self._vector_drag: dict | None = None

    def _vector_strokes(self) -> list:
        return list(self.strokes_for_reshape() or []) if self.strokes_for_reshape else []

    def _vector_hit(self, x: float, y: float):
        """The line under the pen (within a few screen pixels), or None."""
        from genko.ops import _nearest_segment

        reach = PICK_PX / max(0.01, self._scale)
        best, best_d = None, reach
        for stroke in self._vector_strokes():
            pts = list(stroke.points)
            if len(pts) < 2:
                continue
            _i, d, _p = _nearest_segment(pts, x, y)
            if d < best_d:
                best, best_d = stroke, d
        return best

    def _vector_point_hit(self, x: float, y: float) -> int | None:
        if not self.vector_ids:
            return None
        stroke = next((s for s in self._vector_strokes() if s.id == self.vector_ids[0]), None)
        if stroke is None:
            return None
        reach = PICK_PX / max(0.01, self._scale)
        for k, p in enumerate(stroke.points):
            if math.dist((p[0], p[1]), (x, y)) <= reach:
                return k
        return None

    def _vector_press(self, x: float, y: float, modifiers) -> None:
        point = self._vector_point_hit(x, y)
        if point is not None and not self.vector_cut:
            self.vector_point = point
            self._vector_drag = {"id": self.vector_ids[0], "index": point, "to": (x, y)}
            self.update()
            return
        hit = self._vector_hit(x, y)
        if hit is None:
            if not modifiers & Qt.KeyboardModifier.ShiftModifier:
                self.vector_ids, self.vector_point = [], None
            self.update()
            return
        if self.vector_cut:
            self.vectorEdited.emit({"action": "cut", "stroke_id": hit.id, "at": [round(x, 3), round(y, 3)]})
            self.vector_ids, self.vector_point = [], None
            return
        if modifiers & Qt.KeyboardModifier.AltModifier and self.vector_ids and hit.id == self.vector_ids[0]:
            self.vectorEdited.emit({"action": "add_point", "stroke_id": hit.id, "at": [round(x, 3), round(y, 3)]})
            return
        if modifiers & Qt.KeyboardModifier.ShiftModifier:
            if hit.id in self.vector_ids:
                self.vector_ids.remove(hit.id)
            else:
                self.vector_ids.append(hit.id)
        else:
            self.vector_ids = [hit.id]
        self.vector_point = None
        self.update()

    def _vector_move(self, x: float, y: float) -> bool:
        if self._vector_drag is None:
            return False
        self._vector_drag["to"] = (x, y)
        self.update()
        return True

    def _vector_release(self) -> bool:
        if self._vector_drag is None:
            return False
        drag, self._vector_drag = self._vector_drag, None
        x, y = drag["to"]
        self.vectorEdited.emit({"action": "move_point", "stroke_id": drag["id"], "index": drag["index"],
                                "to": [round(x, 3), round(y, 3)]})
        return True

    def vector_delete(self) -> bool:
        """Delete: the chosen point, else the chosen lines."""
        if not self.vector_ids:
            return False
        if self.vector_point is not None:
            self.vectorEdited.emit({"action": "delete_point", "stroke_id": self.vector_ids[0], "index": self.vector_point})
            self.vector_point = None
        else:
            self.vectorEdited.emit({"action": "delete", "ids": list(self.vector_ids)})
            self.vector_ids = []
        self.update()
        return True

    def _draw_vector(self, painter) -> None:
        if self.tool != "vector" or not self.vector_ids:
            return
        strokes = {s.id: s for s in self._vector_strokes()}
        for n, stroke_id in enumerate(self.vector_ids):
            stroke = strokes.get(stroke_id)
            if stroke is None:
                continue
            pts = [(p[0], p[1]) for p in stroke.points]
            if self._vector_drag is not None and self._vector_drag["id"] == stroke_id:
                pts[self._vector_drag["index"]] = self._vector_drag["to"]
            painter.setPen(QPen(QColor("#1c7ed6"), 1.5))
            painter.drawPolyline([self._pt(*p) for p in pts])
            if n == 0:  # the first chosen line shows its control points
                for k, p in enumerate(pts):
                    q = self._pt(*p)
                    painter.setBrush(QColor("#e8590c") if k == self.vector_point else QColor("white"))
                    painter.setPen(QPen(QColor("#1c7ed6"), 1))
                    painter.drawRect(int(q.x()) - 3, int(q.y()) - 3, 6, 6)
                painter.setBrush(Qt.BrushStyle.NoBrush)
