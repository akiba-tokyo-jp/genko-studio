"""Tool and command icons, drawn by Genko itself (no image files, nothing borrowed): simple line pictures
on a 32×32 grid, in the window's text colour so they read on light and dark themes."""

from __future__ import annotations

import math
from functools import lru_cache

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPainterPath, QPen, QPixmap, QPolygonF

INK = QColor(40, 44, 52)
ACCENT = QColor(232, 89, 12)


def _pen(width: float = 2.2, colour: QColor = INK, style=Qt.PenStyle.SolidLine) -> QPen:
    pen = QPen(colour, width, style, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    return pen


def _poly(points) -> QPolygonF:
    return QPolygonF([QPointF(x, y) for x, y in points])


def _draw(name: str, p: QPainter) -> None:
    p.setPen(_pen())
    p.setBrush(Qt.BrushStyle.NoBrush)
    if name == "select":
        p.setBrush(INK)
        p.drawPolygon(_poly([(9, 5), (9, 25), (14, 20), (18, 28), (21, 27), (17, 19), (24, 19)]))
    elif name == "pen":
        p.drawPolygon(_poly([(22, 4), (28, 10), (13, 25), (6, 27), (8, 20)]))
        p.drawLine(QPointF(8, 20), QPointF(13, 25))
        p.setPen(_pen(1.6, ACCENT))
        p.drawLine(QPointF(19, 7), QPointF(25, 13))
    elif name == "eraser":
        p.drawPolygon(_poly([(14, 6), (27, 16), (18, 27), (5, 17)]))
        p.drawLine(QPointF(10, 12), QPointF(22, 21))
        p.drawLine(QPointF(5, 29), QPointF(28, 29))
    elif name == "text":
        font = QFont()
        font.setPixelSize(22)
        font.setBold(True)
        p.setFont(font)
        p.setPen(INK)
        p.drawText(QRectF(0, 0, 32, 32), Qt.AlignmentFlag.AlignCenter, "あ")
    elif name == "frame":
        p.drawRect(QRectF(4, 4, 24, 24))
        p.drawLine(QPointF(4, 14), QPointF(28, 14))
        p.drawLine(QPointF(17, 14), QPointF(13, 28))
    elif name in ("fill", "lassofill"):
        p.drawPolygon(_poly([(6, 14), (15, 5), (25, 15), (16, 24)]))
        p.setBrush(ACCENT)
        p.setPen(_pen(1.2, ACCENT))
        p.drawEllipse(QPointF(26, 24), 3, 4)
        if name == "lassofill":
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(_pen(1.6, INK, Qt.PenStyle.DashLine))
            p.drawEllipse(QRectF(2, 2, 26, 26))
    elif name == "picker":
        p.drawLine(QPointF(7, 25), QPointF(19, 13))
        p.drawPolygon(_poly([(18, 8), (24, 14), (21, 17), (15, 11)]))
        p.drawLine(QPointF(21, 7), QPointF(25, 11))
        p.setBrush(ACCENT)
        p.drawEllipse(QPointF(6, 26), 2.5, 2.5)
    elif name == "rect":
        p.setPen(_pen(2, INK, Qt.PenStyle.DashLine))
        p.drawRect(QRectF(5, 7, 22, 18))
    elif name == "lasso":
        p.setPen(_pen(2, INK, Qt.PenStyle.DashLine))
        path = QPainterPath(QPointF(8, 20))
        path.cubicTo(QPointF(0, 8), QPointF(18, 0), QPointF(26, 8))
        path.cubicTo(QPointF(32, 16), QPointF(18, 24), QPointF(12, 20))
        p.drawPath(path)
        p.setPen(_pen())
        p.drawLine(QPointF(12, 20), QPointF(10, 28))
    elif name == "wand":
        p.drawLine(QPointF(6, 27), QPointF(20, 13))
        p.setPen(_pen(1.8, ACCENT))
        for a in range(0, 360, 60):
            r = math.radians(a)
            p.drawLine(QPointF(23 + 3 * math.cos(r), 9 + 3 * math.sin(r)), QPointF(23 + 6 * math.cos(r), 9 + 6 * math.sin(r)))
    elif name == "reshape":
        path = QPainterPath(QPointF(4, 24))
        path.cubicTo(QPointF(10, 24), QPointF(12, 8), QPointF(18, 8))
        path.cubicTo(QPointF(24, 8), QPointF(24, 22), QPointF(29, 22))
        p.drawPath(path)
        p.setBrush(ACCENT)
        p.setPen(_pen(1.2, ACCENT))
        p.drawEllipse(QPointF(18, 8), 3, 3)
    elif name == "ruler":
        p.drawPolygon(_poly([(4, 22), (22, 4), (28, 10), (10, 28)]))
        for k in range(4):
            x, y = 9 + k * 4, 17 - k * 4
            p.drawLine(QPointF(x, y), QPointF(x + 3, y + 3))
    elif name == "3d":
        p.drawPolygon(_poly([(16, 4), (27, 10), (16, 16), (5, 10)]))
        p.drawPolyline(_poly([(5, 10), (5, 22), (16, 28), (27, 22), (27, 10)]))
        p.drawLine(QPointF(16, 16), QPointF(16, 28))
    elif name == "effect":
        for a in range(0, 360, 30):
            r = math.radians(a)
            p.drawLine(QPointF(16 + 6 * math.cos(r), 16 + 6 * math.sin(r)), QPointF(16 + 14 * math.cos(r), 16 + 14 * math.sin(r)))
    elif name == "stamp":
        p.drawRect(QRectF(7, 18, 18, 6))
        p.drawRect(QRectF(12, 6, 8, 12))
        p.drawLine(QPointF(5, 28), QPointF(27, 28))
    elif name in ("undo", "redo"):
        path = QPainterPath(QPointF(8, 14))
        path.cubicTo(QPointF(14, 6), QPointF(28, 10), QPointF(24, 24))
        tip = [(8, 14), (7, 6), (15, 12)]
        if name == "redo":
            p.translate(32, 0)
            p.scale(-1, 1)
        p.drawPath(path)
        p.drawPolyline(_poly(tip))
    elif name in ("zoom_in", "zoom_out", "fit"):
        if name == "fit":
            p.drawRect(QRectF(5, 5, 22, 22))
            p.drawRect(QRectF(11, 9, 10, 14))
        else:
            p.drawEllipse(QRectF(4, 4, 18, 18))
            p.drawLine(QPointF(19, 19), QPointF(28, 28))
            p.drawLine(QPointF(9, 13), QPointF(17, 13))
            if name == "zoom_in":
                p.drawLine(QPointF(13, 9), QPointF(13, 17))
    elif name in ("prev", "next"):
        if name == "next":
            p.translate(32, 0)
            p.scale(-1, 1)
        p.drawPolyline(_poly([(20, 6), (10, 16), (20, 26)]))
    elif name == "export":
        p.drawPolyline(_poly([(6, 18), (6, 28), (26, 28), (26, 18)]))
        p.drawLine(QPointF(16, 4), QPointF(16, 20))
        p.drawPolyline(_poly([(10, 10), (16, 4), (22, 10)]))
    else:
        p.drawRect(QRectF(6, 6, 20, 20))


@lru_cache(maxsize=64)
def icon(name: str) -> QIcon:
    pixmap = QPixmap(64, 64)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.scale(2, 2)
    _draw(name, painter)
    painter.end()
    return QIcon(pixmap)
