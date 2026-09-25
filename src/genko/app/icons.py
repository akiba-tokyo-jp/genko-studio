"""Tool and command icons, drawn by Genko itself (no image files, nothing borrowed): simple line pictures
on a 32×32 grid, in the window's text colour so they read on light and dark themes."""

from __future__ import annotations

import math
from functools import lru_cache

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPainterPath, QPen, QPixmap, QPolygonF

INK = QColor(40, 44, 52)
ACCENT = QColor(232, 89, 12)


LIGHT_INK = INK
DARK_INK = QColor(222, 225, 230)  # (on the dark screen the pictures are drawn light)


def _pen(width: float = 2.2, colour: QColor | None = None, style=Qt.PenStyle.SolidLine) -> QPen:
    colour = INK if colour is None else colour
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
    elif name == "fill":
        # a paint bucket, tipped, pouring
        p.save()
        p.translate(14, 16)
        p.rotate(-35)
        p.drawPolygon(_poly([(-8, -6), (8, -6), (6, 9), (-6, 9)]))
        p.drawArc(QRectF(-7, -12, 14, 12), 0, 180 * 16)
        p.restore()
        p.setBrush(ACCENT)
        p.setPen(_pen(1.2, ACCENT))
        p.drawPolygon(_poly([(23, 14), (27, 22), (27, 26), (23, 28), (20, 25), (21, 20)]))
    elif name == "lassofill":
        # a loop, dashed, with its inside filled
        path = QPainterPath(QPointF(6, 18))
        path.cubicTo(QPointF(2, 6), QPointF(20, 0), QPointF(27, 8))
        path.cubicTo(QPointF(32, 16), QPointF(22, 28), QPointF(12, 26))
        path.cubicTo(QPointF(8, 25), QPointF(7, 22), QPointF(6, 18))
        p.setBrush(QColor(ACCENT.red(), ACCENT.green(), ACCENT.blue(), 150))
        p.setPen(_pen(1.8, INK, Qt.PenStyle.DashLine))
        p.drawPath(path)
    elif name == "picker":
        # an eyedropper: bulb, tube, a drop at the tip
        p.setBrush(INK)
        p.drawEllipse(QPointF(24, 8), 4.5, 4.5)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(QPointF(17, 12), QPointF(20, 15))
        p.drawPolygon(_poly([(19, 11), (21, 13), (9, 25), (7, 23)]))
        p.setBrush(ACCENT)
        p.setPen(_pen(1.2, ACCENT))
        p.drawEllipse(QPointF(5.5, 27.5), 2.5, 2.5)
    elif name == "shape":
        p.drawRect(QRectF(4, 12, 15, 15))
        p.drawEllipse(QRectF(13, 4, 15, 15))
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
    elif name == "move":
        for (x0, y0, x1, y1) in ((16, 3, 16, 29), (3, 16, 29, 16)):
            p.drawLine(QPointF(x0, y0), QPointF(x1, y1))
        for tip in (((12, 7), (16, 3), (20, 7)), ((12, 25), (16, 29), (20, 25)), ((7, 12), (3, 16), (7, 20)), ((25, 12), (29, 16), (25, 20))):
            p.drawPolyline(_poly(tip))
    elif name == "gradient":
        from PySide6.QtGui import QLinearGradient

        shade = QLinearGradient(4, 16, 28, 16)
        shade.setColorAt(0, INK)
        shade.setColorAt(1, QColor(255, 255, 255, 0))
        p.setBrush(shade)
        p.drawRect(QRectF(4, 8, 24, 16))
    elif name == "export":
        p.drawPolyline(_poly([(6, 18), (6, 28), (26, 28), (26, 18)]))
        p.drawLine(QPointF(16, 4), QPointF(16, 20))
        p.drawPolyline(_poly([(10, 10), (16, 4), (22, 10)]))
    else:
        p.drawRect(QRectF(6, 6, 20, 20))


def dark_screen() -> bool:
    from PySide6.QtGui import QGuiApplication, QPalette

    app = QGuiApplication.instance()
    return app is not None and app.palette().color(QPalette.ColorRole.Window).lightness() < 128


def icon(name: str) -> QIcon:
    """A tool's picture, drawn in the screen's text colour (dark on a light screen, light on a dark one)."""
    return _icon(name, dark_screen())


@lru_cache(maxsize=128)
def _icon(name: str, dark: bool) -> QIcon:
    global INK
    INK = DARK_INK if dark else LIGHT_INK
    pixmap = QPixmap(64, 64)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.scale(2, 2)
    _draw(name, painter)
    painter.end()
    INK = LIGHT_INK
    return QIcon(pixmap)
