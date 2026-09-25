"""The page canvas's guides: rulers (placing, editing, showing, snapping the pen), the grid, and the 3D
figures and boxes (dragging joints, moving and turning boxes). Mixed into PageCanvas.

Everything here changes the book only through the canvas's signals (the window turns them into ops).
"""

from __future__ import annotations

import copy
import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen

from genko import mannequin, prim3d, rulers

ACTIVE = QColor("#2b8a3e")
IDLE = QColor(130, 130, 130, 170)
PRIM = QColor(70, 90, 170)


class GuideMixin:
    """Needs the canvas's _pt, _to_mm, _scale, page, tool, update() and the guide signals."""

    def _init_guides(self) -> None:
        self.rulers_visible = True
        self.snap_rulers = True
        self.grid_visible = False
        self.grid_snap = False
        self.grid_mm = 5.0
        self.ruler_kind = "line"
        self.ruler_vps = 1  # perspective: how many vanishing points
        self.ruler_copies = 2  # symmetry
        self.selected_ruler_id: str | None = None
        self._ruler_draft: list | None = None  # points of a ruler being placed
        self._ruler_drag: dict | None = None
        self.selected_prim_id: str | None = None
        self._prim_drag: dict | None = None
        self.selected_effect_id: str | None = None
        self.highlight_box: list | None = None  # a problem from the checks, shown on the page (mm)
        self._effect_drag: dict | None = None

    # --- helpers -------------------------------------------------------------------------------------------

    def _page_rulers(self) -> list[dict]:
        return list(getattr(self.page, "rulers", None) or []) if self.page is not None else []

    def _frame_contains(self):
        from genko.ops import _frame_contains

        return _frame_contains(self.page)

    def grid_point(self, x: float, y: float) -> tuple[float, float]:
        if self.grid_snap and self.grid_mm > 0:
            return rulers.snap_to_grid((x, y), self.grid_mm)
        return x, y

    def snapped_preview(self, points: list) -> list[list]:
        """What the pen line will become (and its symmetry copies), for drawing while the pen is down."""
        rs = self._page_rulers()
        if not (self.snap_rulers and rs and len(points) >= 2):
            return [points]
        inside = self._frame_contains()
        main = rulers.snap(points, rs, inside)
        return [main, *rulers.symmetry_copies(main, rs, inside)]

    def _ruler_handles(self) -> list[tuple[str, int, tuple[float, float]]]:
        out = []
        for ruler in self._page_rulers():
            if not ruler.get("visible", True):
                continue
            for i, p in enumerate(ruler.get("points") or []):
                out.append((ruler["id"], i, (float(p[0]), float(p[1]))))
        return out

    def _near(self, pos: QPointF, point, px: float = 8) -> bool:
        q = self._pt(*point)
        return abs(q.x() - pos.x()) <= px and abs(q.y() - pos.y()) <= px

    # --- painting ------------------------------------------------------------------------------------------

    def _draw_grid(self, painter: QPainter) -> None:
        if not self.grid_visible or self.page is None or self.grid_mm <= 0:
            return
        spec = self.page.spec
        step = self.grid_mm
        if step * self._scale < 4:  # too dense to see: every fifth line only
            step *= 5
        for k in range(int(spec.width_mm / step) + 1):
            x = k * step
            major = abs((x / self.grid_mm) % 5) < 1e-6
            painter.setPen(QPen(QColor(80, 140, 220, 90 if major else 45), 1))
            painter.drawLine(self._pt(x, 0), self._pt(x, spec.height_mm))
        for k in range(int(spec.height_mm / step) + 1):
            y = k * step
            major = abs((y / self.grid_mm) % 5) < 1e-6
            painter.setPen(QPen(QColor(80, 140, 220, 90 if major else 45), 1))
            painter.drawLine(self._pt(0, y), self._pt(spec.width_mm, y))

    def _line_across(self, painter: QPainter, a, d, length: float = 2000.0) -> None:
        painter.drawLine(self._pt(a[0] - d[0] * length, a[1] - d[1] * length), self._pt(a[0] + d[0] * length, a[1] + d[1] * length))

    def _draw_one_ruler(self, painter: QPainter, ruler: dict, selected: bool) -> None:
        colour = ACTIVE if ruler.get("active", True) else IDLE
        painter.setPen(QPen(colour, 2.2 if selected else 1.3, Qt.PenStyle.SolidLine if ruler.get("active", True) else Qt.PenStyle.DashLine))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        pts = [(float(p[0]), float(p[1])) for p in ruler.get("points") or []]
        kind = ruler.get("kind")
        hover = self._hover
        if kind == "line" and len(pts) >= 2:
            d = rulers._unit(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1])
            if d:
                self._line_across(painter, pts[0], d)
        elif kind == "curve" and len(pts) >= 2:
            curve = rulers.smooth_curve(pts)
            path = QPainterPath(self._pt(*curve[0]))
            for p in curve[1:]:
                path.lineTo(self._pt(*p))
            painter.drawPath(path)
        elif kind == "parallel":
            a = math.radians(float(ruler.get("angle", 0) or 0))
            d = (math.cos(a), math.sin(a))
            anchors = pts[:1] + ([hover] if hover else [])
            spec = self.page.spec
            if not anchors:
                anchors = [(spec.width_mm / 2, spec.height_mm / 2)]
            for p in anchors:
                self._line_across(painter, p, d)
        elif kind == "concentric" and pts:
            c = pts[0]
            ratio = float(ruler.get("ratio", 1) or 1)
            angle = float(ruler.get("angle", 0) or 0)
            radii = [r * 15 for r in range(1, 6)]
            if hover:
                dx, dy = hover[0] - c[0], hover[1] - c[1]
                rot = math.radians(angle)
                lx, ly = dx * math.cos(rot) + dy * math.sin(rot), (-dx * math.sin(rot) + dy * math.cos(rot)) / ratio
                radii.append(math.hypot(lx, ly))
            painter.save()
            centre = self._pt(*c)
            painter.translate(centre)
            painter.rotate(angle)
            for r in radii:
                painter.drawEllipse(QPointF(0, 0), r * self._scale, r * ratio * self._scale)
            painter.restore()
        elif kind == "radial" and pts:
            c = pts[0]
            for k in range(24):
                a = k * math.pi / 12
                painter.drawLine(self._pt(*c), self._pt(c[0] + math.cos(a) * 2000, c[1] + math.sin(a) * 2000))
        elif kind == "perspective" and pts:
            h = rulers.horizon(ruler)
            if h:
                painter.setPen(QPen(QColor("#e8590c"), 1.3))
                self._line_across(painter, h[0], h[1])
                painter.setPen(QPen(colour, 1))
            for v in pts:
                for k in range(16):
                    a = k * math.pi / 8
                    painter.drawLine(self._pt(*v), self._pt(v[0] + math.cos(a) * 2000, v[1] + math.sin(a) * 2000))
            if hover:
                painter.setPen(QPen(colour, 1.6, Qt.PenStyle.DotLine))
                for d in rulers.directions(ruler, hover):
                    self._line_across(painter, hover, d)
        elif kind == "symmetry" and len(pts) >= 2:
            painter.setPen(QPen(QColor("#ae3ec9"), 1.5, Qt.PenStyle.DashDotLine))
            a = pts[0]
            base = math.atan2(pts[1][1] - a[1], pts[1][0] - a[0])
            copies = int(ruler.get("copies", 2) or 2)
            count = 1 if copies == 2 else copies
            for k in range(count):
                ang = base + k * 2 * math.pi / copies if copies > 2 else base
                self._line_across(painter, a, (math.cos(ang), math.sin(ang)))
        # control points
        painter.setPen(QPen(colour, 1.3))
        painter.setBrush(QColor("white"))
        for p in pts:
            q = self._pt(*p)
            painter.drawRect(QRectF(q.x() - 4, q.y() - 4, 8, 8))
        painter.setBrush(Qt.BrushStyle.NoBrush)

    def _draw_rulers(self, painter: QPainter) -> None:
        if self.page is None:
            return
        painter.save()
        if self.rulers_visible or self.tool == "ruler":
            for ruler in self._page_rulers():
                if ruler.get("visible", True) or self.tool == "ruler":
                    if self._ruler_drag and self._ruler_drag["id"] == ruler["id"]:
                        ruler = self._ruler_drag["ruler"]
                    self._draw_one_ruler(painter, ruler, ruler.get("id") == self.selected_ruler_id)
        if self._ruler_draft:
            draft = self._draft_ruler(self._ruler_draft + ([self._hover] if self._hover and self.ruler_kind in ("curve", "perspective") else []))
            if draft:
                self._draw_one_ruler(painter, draft, True)
        painter.restore()

    def _draw_prims_overlay(self, painter: QPainter) -> None:
        if self.page is None or self.tool != "3d":
            return
        painter.save()
        for prim in self.page.prims:
            shown = self._prim_drag["prim"] if self._prim_drag and self._prim_drag["id"] == prim.get("id") else prim
            selected = prim.get("id") == self.selected_prim_id
            if shown is not prim:  # the part being dragged, drawn live
                painter.setPen(QPen(QColor("#e8590c"), 2))
                for a, b in self._prim_lines(shown):
                    painter.drawLine(self._pt(*a), self._pt(*b))
            for name, point in self._prim_handles(shown):
                q = self._pt(*point)
                painter.setPen(QPen(PRIM, 1.2))
                painter.setBrush(QColor("#e8590c") if selected and name in ("pelvis", "move") else QColor("white"))
                if name == "turn":
                    painter.drawEllipse(q, 6, 6)
                else:
                    painter.drawEllipse(q, 4.5, 4.5)
            if selected:
                x, y, w, h = prim3d.prim_bbox(shown)
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.setPen(QPen(QColor("#1c7ed6"), 1, Qt.PenStyle.DashLine))
                a, b = self._pt(x, y), self._pt(x + w, y + h)
                painter.drawRect(QRectF(a, b))
        painter.restore()

    @staticmethod
    def _prim_lines(prim: dict) -> list:
        if prim.get("kind") == "mannequin":
            bone = mannequin.skeleton(prim)
            lines = [(a, b) for a, b, _ in bone["segments"]]
            (hx, hy), r = bone["head"]
            circle = [(hx + r * math.cos(k * math.pi / 8), hy + r * math.sin(k * math.pi / 8)) for k in range(17)]
            return lines + list(zip(circle, circle[1:]))
        return [(a, b) for a, b, _ in prim3d.edges(prim)]

    def _prim_handles(self, prim: dict) -> list[tuple[str, tuple[float, float]]]:
        if prim.get("kind") == "mannequin":
            points = mannequin.skeleton(prim)["points"]
            return [("pelvis", points["pelvis"])] + [(name, points[name]) for name in mannequin.HANDLES]
        x, y, w, h = prim3d.bbox(prim)
        pos = prim.get("pos") or [0, 0, 0]
        return [("move", (float(pos[0]), float(pos[1]))), ("turn", (x + w / 2, y - 8))]

    # --- placing rulers ------------------------------------------------------------------------------------

    def _draft_ruler(self, points: list) -> dict | None:
        """The ruler the points placed so far make (None while it is not one yet)."""
        pts = [list(p) for p in points if p is not None]
        kind = self.ruler_kind
        if not pts:
            return None
        if kind == "curve":  # a double-click adds the same point twice
            kept = [pts[0]]
            for pt in pts[1:]:
                if math.dist(pt, kept[-1]) > 0.5:
                    kept.append(pt)
            pts = kept
            if len(pts) < 2:
                return None
        ruler = {"id": "_draft", "kind": kind, "points": pts, "active": True}
        if kind in ("line", "symmetry"):
            if len(pts) < 2:
                return None
            ruler["points"] = [pts[0], pts[-1]]
            if kind == "symmetry":
                ruler["copies"] = self.ruler_copies
        elif kind == "parallel":
            if len(pts) < 2:
                return None
            ruler["points"] = [pts[0], pts[-1]]
            ruler["angle"] = round(math.degrees(math.atan2(pts[-1][1] - pts[0][1], pts[-1][0] - pts[0][0])), 2)
        elif kind == "concentric":
            ruler["points"] = [pts[0]]
            if len(pts) >= 2:
                dx, dy = abs(pts[-1][0] - pts[0][0]), abs(pts[-1][1] - pts[0][1])
                if self._modifiers & Qt.KeyboardModifier.AltModifier and dx > 0.5:
                    ruler["ratio"] = round(max(0.05, dy / dx), 3)
        elif kind == "radial":
            ruler["points"] = [pts[0]]
        elif kind == "perspective":
            ruler["points"] = pts[: max(1, self.ruler_vps)]
        return ruler

    def _ruler_press(self, pos: QPointF) -> None:
        x, y = self.grid_point(*self._to_mm(pos))
        if self._ruler_draft is None:
            for ruler_id, index, point in self._ruler_handles():
                if self._near(pos, point):
                    ruler = copy.deepcopy(next(r for r in self._page_rulers() if r["id"] == ruler_id))
                    self.selected_ruler_id = ruler_id
                    self._ruler_drag = {"id": ruler_id, "index": index, "ruler": ruler, "moved": False}
                    self.update()
                    return
        if self.ruler_kind in ("curve", "perspective"):  # click by click
            self._ruler_draft = (self._ruler_draft or []) + [[round(x, 2), round(y, 2)]]
            if self.ruler_kind == "perspective" and len(self._ruler_draft) >= self.ruler_vps:
                self._finish_ruler()
            self.update()
            return
        if self.ruler_kind == "radial":
            self._ruler_draft = [[round(x, 2), round(y, 2)]]
            self._finish_ruler()
            return
        self._ruler_draft = [[round(x, 2), round(y, 2)]]  # dragged kinds: line, parallel, concentric, symmetry
        self.update()

    def _ruler_move(self, pos: QPointF) -> bool:
        x, y = self.grid_point(*self._to_mm(pos))
        if self._ruler_drag is not None:
            drag = self._ruler_drag
            drag["ruler"]["points"][drag["index"]] = [round(x, 2), round(y, 2)]
            if drag["ruler"]["kind"] == "parallel" and len(drag["ruler"]["points"]) >= 2:
                (ax, ay), (bx, by) = drag["ruler"]["points"][:2]
                drag["ruler"]["angle"] = round(math.degrees(math.atan2(by - ay, bx - ax)), 2)
            drag["moved"] = True
            self.update()
            return True
        if self._ruler_draft and self.ruler_kind in ("line", "parallel", "concentric", "symmetry"):
            self._ruler_draft = [self._ruler_draft[0], [round(x, 2), round(y, 2)]]
            self.update()
            return True
        return False

    def _ruler_release(self) -> None:
        if self._ruler_drag is not None:
            drag, self._ruler_drag = self._ruler_drag, None
            if drag["moved"]:
                change = {"points": drag["ruler"]["points"]}
                if "angle" in drag["ruler"]:
                    change["angle"] = drag["ruler"]["angle"]
                self.rulerEdited.emit(drag["id"], change)
            self.update()
            return
        if self._ruler_draft and self.ruler_kind in ("line", "parallel", "concentric", "symmetry"):
            if len(self._ruler_draft) >= 2 and math.dist(self._ruler_draft[0], self._ruler_draft[-1]) > 1.0:
                self._finish_ruler()
            elif self.ruler_kind == "concentric":
                self._finish_ruler()  # a click: circles around that point
            else:
                self._ruler_draft = None
            self.update()

    def finish_curve(self) -> None:
        """Enter or a double-click ends a curve ruler."""
        if self._ruler_draft and self.ruler_kind == "curve" and len(self._ruler_draft) >= 2:
            self._finish_ruler()

    def cancel_ruler(self) -> None:
        self._ruler_draft = None
        self._ruler_drag = None
        self.update()

    def _finish_ruler(self) -> None:
        ruler = self._draft_ruler(self._ruler_draft or [])
        self._ruler_draft = None
        if ruler:
            ruler.pop("id", None)
            ruler.pop("active", None)
            self.rulerPlaced.emit(ruler)
        self.update()

    def _draw_highlight(self, painter: QPainter) -> None:
        if not self.highlight_box or self.page is None:
            return
        x, y, w, h = self.highlight_box
        pad = 2.0
        a, b = self._pt(x - pad, y - pad), self._pt(x + w + pad, y + h + pad)
        painter.save()
        painter.setPen(QPen(QColor("#e03131"), 2.5, Qt.PenStyle.DashLine))
        painter.setBrush(QColor(224, 49, 49, 40))
        painter.drawRect(QRectF(a, b))
        painter.restore()

    # --- effect lines: click to put one, drag its centre ---------------------------------------------------

    def _effect_handles(self) -> list[tuple[str, tuple[float, float]]]:
        from genko import effects

        out = []
        for effect in (self.page.effects if self.page is not None else []):
            if effect.get("kind") in ("focus", "uni_flash", "beta_flash"):
                _, box = effects.area(effect, self.page)
                params = effect.get("params") or {}
                c = params.get("center") or [box[0] + box[2] / 2, box[1] + box[3] / 2]
                out.append((effect["id"], (float(c[0]), float(c[1]))))
        return out

    def _draw_effect_handles(self, painter: QPainter) -> None:
        if self.page is None or self.tool != "effect":
            return
        painter.save()
        for effect_id, point in self._effect_handles():
            if self._effect_drag and self._effect_drag["id"] == effect_id:
                point = self._effect_drag["to"]
            q = self._pt(*point)
            painter.setPen(QPen(QColor("#e8590c"), 2))
            painter.setBrush(QColor(255, 255, 255, 220) if effect_id != self.selected_effect_id else QColor("#e8590c"))
            painter.drawEllipse(q, 7, 7)
            painter.drawLine(QPointF(q.x() - 11, q.y()), QPointF(q.x() + 11, q.y()))
            painter.drawLine(QPointF(q.x(), q.y() - 11), QPointF(q.x(), q.y() + 11))
        painter.restore()

    def _effect_press(self, pos: QPointF) -> None:
        for effect_id, point in self._effect_handles():
            if self._near(pos, point, 10):
                self.selected_effect_id = effect_id
                self._effect_drag = {"id": effect_id, "to": point, "moved": False}
                self.effectSelected.emit(effect_id)
                self.update()
                return
        x, y = self._to_mm(pos)
        self.effectRequested.emit(x, y)

    def _effect_move(self, pos: QPointF) -> bool:
        if self._effect_drag is None:
            return False
        x, y = self._to_mm(pos)
        self._effect_drag["to"] = (round(x, 2), round(y, 2))
        self._effect_drag["moved"] = True
        self.update()
        return True

    def _effect_release(self) -> None:
        drag, self._effect_drag = self._effect_drag, None
        if drag and drag["moved"]:
            self.effectMoved.emit(drag["id"], list(drag["to"]))
        self.update()

    # --- 3D --------------------------------------------------------------------------------------------

    def _prim_press(self, pos: QPointF) -> None:
        x, y = self._to_mm(pos)
        prims = list(self.page.prims) if self.page is not None else []
        # the selected one first, so its handles win where figures overlap
        prims.sort(key=lambda p: p.get("id") != self.selected_prim_id)
        for prim in prims:
            for name, point in self._prim_handles(prim):
                if self._near(pos, point):
                    self._select_prim(prim["id"])
                    self._prim_drag = {"id": prim["id"], "handle": name, "prim": copy.deepcopy(prim), "orig": copy.deepcopy(prim),
                                       "start": (x, y), "moved": False}
                    return
        for prim in reversed(prims):
            bx, by, bw, bh = prim3d.prim_bbox(prim)
            if bx <= x <= bx + bw and by <= y <= by + bh:
                self._select_prim(prim["id"])
                handle = "pelvis" if prim.get("kind") == "mannequin" else "move"
                self._prim_drag = {"id": prim["id"], "handle": handle, "prim": copy.deepcopy(prim), "orig": copy.deepcopy(prim),
                                   "start": (x, y), "moved": False, "grab": True}
                return
        self._select_prim(None)

    def _select_prim(self, prim_id: str | None) -> None:
        if prim_id != self.selected_prim_id:
            self.selected_prim_id = prim_id
            self.primSelected.emit(prim_id or "")
        self.update()

    def _prim_move(self, pos: QPointF) -> bool:
        drag = self._prim_drag
        if drag is None:
            return False
        x, y = self._to_mm(pos)
        orig, prim = drag["orig"], drag["prim"]
        sx, sy = drag["start"]
        handle = drag["handle"]
        if handle in ("pelvis", "move"):
            ox, oy, *rest = list(orig.get("pos") or [0, 0, 0]) + [0]
            prim["pos"] = [round(ox + x - sx, 3), round(oy + y - sy, 3), (rest or [0])[0]]
        elif handle == "turn":
            tip, turn, lean = (list(orig.get("rot") or [0, 0, 0]) + [0, 0, 0])[:3]
            prim["rot"] = [round(tip - (y - sy) / 40, 4), round(turn + (x - sx) / 40, 4), lean]
        else:
            change = mannequin.pose_to(prim, handle, (x, y))
            for joint, values in change.get("joints", {}).items():
                prim.setdefault("joints", {}).setdefault(joint, {}).update(values)
        drag["to"] = [round(x, 2), round(y, 2)]
        drag["moved"] = True
        self.update()
        return True

    def _prim_release(self) -> None:
        drag, self._prim_drag = self._prim_drag, None
        if drag and drag["moved"]:
            prim = drag["prim"]
            if drag["handle"] in ("move", "turn"):
                key = "pos" if drag["handle"] == "move" else "rot"
                self.primEdited.emit(drag["id"], {key: prim[key]})
            elif drag["handle"] == "pelvis":
                self.primEdited.emit(drag["id"], {"pos": prim["pos"]})
            else:
                self.primPosed.emit(drag["id"], drag["handle"], drag["to"])
        self.update()
