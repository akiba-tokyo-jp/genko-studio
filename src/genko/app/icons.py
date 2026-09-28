"""Tool and command icons: Lucide (ISC licence, `lucide/LICENSE`) for everything a general icon set has, and a
few manga pictures Genko draws itself in the same line weight (あ for lettering, focus lines, a gradient).
All are drawn in the look's text colour; a chosen tool's picture turns the accent (theme.py)."""

from __future__ import annotations

import math
from functools import lru_cache

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPainterPath, QPen, QPixmap, QPolygonF

INK = QColor(40, 44, 52)
ACCENT = QColor(232, 89, 12)


LIGHT_INK = INK
DARK_INK = QColor(222, 225, 230)  # (on the dark screen the pictures are drawn light)


LUCIDE = {
    "select": "mouse-pointer-2", "pen": "pen-tool", "eraser": "eraser", "frame": "layout-dashboard", "fill": "paint-bucket",
    "lassofill": "lasso", "lasso": "lasso-select", "picker": "pipette", "blend": "blend", "shape": "shapes",
    "rect": "square-dashed", "wand": "wand-sparkles", "reshape": "spline", "ruler": "ruler", "3d": "box", "stamp": "stamp",
    "undo": "undo-2", "redo": "redo-2", "zoom_in": "zoom-in", "zoom_out": "zoom-out", "fit": "scan",
    "prev": "chevron-left", "next": "chevron-right", "move": "move", "export": "share", "add": "plus",
    "pen_layer": "pen-line", "paint_layer": "paintbrush", "folder": "folder-plus", "delete": "trash-2", "up": "arrow-up",
    "down": "arrow-down", "approve": "circle-check", "back": "corner-up-left", "expand": "maximize-2", "search": "search",
    "settings": "settings", "story": "file-text", "check": "clipboard-check", "page": "file", "open": "folder-open",
    "book": "book-open", "duplicate": "copy", "merge": "arrow-down-to-line", "more": "ellipsis", "info": "info",
    "eye": "eye", "eye_off": "eye-off", "close": "x", "caret_up": "chevron-up", "caret_down": "chevron-down",
    # a layer's kind, shown while it has nothing drawn yet
    "kind_strokes": "pen-line", "kind_raster": "paintbrush", "kind_folder": "folder", "kind_placed": "image",
    "kind_tone": "grid-3x3", "kind_fill": "square", "kind_adjust": "contrast", "kind_other": "layers",
}
LUCIDE_DIR = __import__("pathlib").Path(__file__).resolve().parent / "lucide"


@lru_cache(maxsize=128)
def _svg(file: str) -> str | None:
    try:
        return (LUCIDE_DIR / f"{file}.svg").read_text(encoding="utf-8")
    except OSError:
        return None


def _lucide(name: str, colour: str, size: int = 64):
    """A Lucide picture in this colour (its lines are drawn in currentColor)."""
    from PySide6.QtCore import QByteArray
    from PySide6.QtSvg import QSvgRenderer

    svg = _svg(LUCIDE[name])
    if svg is None:
        return None
    svg = svg.replace("currentColor", colour)
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    QSvgRenderer(QByteArray(svg.encode("utf-8"))).render(painter, QRectF(4, 4, size - 8, size - 8))
    painter.end()
    return pixmap


def _pen(width: float = 2.4, colour: QColor | None = None, style=Qt.PenStyle.SolidLine) -> QPen:
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
        font.setPixelSize(21)
        font.setWeight(QFont.Weight.DemiBold)
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
    elif name == "blend":
        # a drop, smeared: the colour pulled along
        path = QPainterPath(QPointF(12, 4))
        path.cubicTo(QPointF(20, 14), QPointF(20, 24), QPointF(12, 24))
        path.cubicTo(QPointF(4, 24), QPointF(4, 14), QPointF(12, 4))
        p.drawPath(path)
        p.setPen(_pen(1.6, ACCENT))
        for k, y in enumerate((14, 19, 24)):
            p.drawLine(QPointF(20 + k, y), QPointF(29, y))
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
    elif name in ("add", "pen_layer", "paint_layer", "folder"):
        if name == "folder":
            p.drawPolyline(_poly([(4, 10), (4, 26), (28, 26), (28, 12), (15, 12), (12, 8), (4, 8), (4, 10)]))
        elif name == "pen_layer":
            p.drawPolygon(_poly([(4, 20), (16, 26), (28, 20), (16, 14)]))
            p.drawLine(QPointF(14, 6), QPointF(20, 12))
        elif name == "paint_layer":
            p.drawPolygon(_poly([(4, 20), (16, 26), (28, 20), (16, 14)]))
            p.setBrush(ACCENT)
            p.setPen(_pen(1.2, ACCENT))
            p.drawEllipse(QPointF(17, 8), 4, 4)
        p.setPen(_pen(2.0, ACCENT))
        cx, cy = (25, 7) if name != "add" else (16, 16)
        size = 4 if name != "add" else 9
        p.drawLine(QPointF(cx - size, cy), QPointF(cx + size, cy))
        p.drawLine(QPointF(cx, cy - size), QPointF(cx, cy + size))
    elif name == "delete":
        p.drawLine(QPointF(6, 9), QPointF(26, 9))
        p.drawPolyline(_poly([(12, 9), (13, 5), (19, 5), (20, 9)]))
        p.drawPolygon(_poly([(8, 9), (10, 28), (22, 28), (24, 9)]))
        p.drawLine(QPointF(14, 14), QPointF(14, 23))
        p.drawLine(QPointF(18, 14), QPointF(18, 23))
    elif name in ("up", "down"):
        if name == "down":
            p.translate(0, 32)
            p.scale(1, -1)
        p.drawLine(QPointF(16, 27), QPointF(16, 6))
        p.drawPolyline(_poly([(8, 14), (16, 6), (24, 14)]))
    elif name == "approve":
        p.setPen(_pen(2.6, ACCENT))
        p.drawPolyline(_poly([(6, 17), (13, 24), (27, 9)]))
    elif name == "back":
        path = QPainterPath(QPointF(10, 12))
        path.lineTo(QPointF(22, 12))
        path.cubicTo(QPointF(30, 12), QPointF(30, 26), QPointF(22, 26))
        path.lineTo(QPointF(12, 26))
        p.drawPath(path)
        p.drawPolyline(_poly([(15, 7), (10, 12), (15, 17)]))
    elif name == "expand":
        for tip in (((5, 12), (5, 5), (12, 5)), ((20, 5), (27, 5), (27, 12)), ((27, 20), (27, 27), (20, 27)), ((12, 27), (5, 27), (5, 20))):
            p.drawPolyline(_poly(tip))
    elif name == "search":
        p.drawEllipse(QRectF(5, 5, 16, 16))
        p.drawLine(QPointF(18, 18), QPointF(27, 27))
    elif name == "settings":
        p.drawEllipse(QPointF(16, 16), 4.5, 4.5)
        for a in range(0, 360, 45):
            r = math.radians(a)
            p.drawLine(QPointF(16 + 8 * math.cos(r), 16 + 8 * math.sin(r)), QPointF(16 + 12 * math.cos(r), 16 + 12 * math.sin(r)))
        p.drawEllipse(QPointF(16, 16), 8.5, 8.5)
    elif name == "story":
        for x in (24, 18, 12):
            p.drawLine(QPointF(x, 5), QPointF(x, 22 if x != 12 else 16))
        p.setPen(_pen(2.0, ACCENT))
        p.drawLine(QPointF(6, 27), QPointF(26, 27))
    elif name == "check":
        for y in (8, 16, 24):
            p.drawLine(QPointF(14, y), QPointF(28, y))
        p.setPen(_pen(2.0, ACCENT))
        for y in (8, 16, 24):
            p.drawPolyline(_poly([(4, y), (6.5, y + 2.5), (10, y - 2.5)]))
    elif name == "page":
        p.drawPolygon(_poly([(7, 4), (20, 4), (26, 10), (26, 28), (7, 28)]))
        p.drawPolyline(_poly([(20, 4), (20, 10), (26, 10)]))
    elif name == "open":
        p.drawPolyline(_poly([(4, 10), (4, 26), (24, 26), (28, 14), (8, 14), (4, 26)]))
        p.drawPolyline(_poly([(4, 10), (4, 7), (12, 7), (14, 10), (24, 10), (24, 14)]))
    elif name == "book":
        p.drawPolyline(_poly([(16, 8), (16, 27)]))
        p.drawPolygon(_poly([(16, 8), (5, 5), (5, 24), (16, 27), (27, 24), (27, 5)]))
    elif name == "duplicate":
        p.drawRect(QRectF(5, 9, 16, 18))
        p.drawPolyline(_poly([(11, 9), (11, 4), (27, 4), (27, 22), (21, 22)]))
    elif name == "merge":
        p.drawPolygon(_poly([(4, 24), (16, 29), (28, 24), (16, 19)]))
        p.drawLine(QPointF(16, 3), QPointF(16, 15))
        p.drawPolyline(_poly([(11, 10), (16, 15), (21, 10)]))
    elif name == "more":
        p.setBrush(INK)
        for x in (8, 16, 24):
            p.drawEllipse(QPointF(x, 16), 1.8, 1.8)
    elif name == "info":
        p.drawEllipse(QRectF(5, 5, 22, 22))
        p.drawLine(QPointF(16, 14), QPointF(16, 22))
        p.setBrush(INK)
        p.drawEllipse(QPointF(16, 10), 1.2, 1.2)
    elif name in ("eye", "eye_off"):
        path = QPainterPath(QPointF(3, 16))
        path.quadTo(QPointF(16, 3), QPointF(29, 16))
        path.quadTo(QPointF(16, 29), QPointF(3, 16))
        p.drawPath(path)
        if name == "eye":
            p.setBrush(INK)
            p.drawEllipse(QPointF(16, 16), 4.2, 4.2)
        else:
            p.drawLine(QPointF(6, 27), QPointF(26, 5))
    else:
        p.drawRect(QRectF(6, 6, 20, 20))


def dark_screen() -> bool:
    from PySide6.QtGui import QGuiApplication, QPalette

    app = QGuiApplication.instance()
    return app is not None and app.palette().color(QPalette.ColorRole.Window).lightness() < 128


def colours() -> tuple[str, str]:
    """The ink and the accent the pictures are drawn in now (read once when many are drawn together)."""
    from genko.app import theme

    t = theme.tokens()
    return t.text, (t.muted if theme.mono_icons() else t.accent)


def icon(name: str, inks: tuple[str, str] | None = None) -> QIcon:
    """A tool's picture, drawn in the look's text colour and its one accent (grey only when chosen)."""
    return _icon(name, *(inks or colours()))


def _picture(name: str, ink: str, accent: str) -> QPixmap:
    if name in LUCIDE:
        pixmap = _lucide(name, ink)
        if pixmap is not None:
            return pixmap
    global INK, ACCENT
    INK, ACCENT = QColor(ink), QColor(accent)
    pixmap = QPixmap(64, 64)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.scale(2, 2)
    _draw(name, painter)
    painter.end()
    INK, ACCENT = LIGHT_INK, QColor(232, 89, 12)
    return pixmap


@lru_cache(maxsize=256)
def _icon(name: str, ink: str, accent: str) -> QIcon:
    """The picture in the ink; when its tool is the one in hand (a checked button) in the accent."""
    icon = QIcon(_picture(name, ink, ink))
    chosen = _picture(name, accent, accent)
    for mode in (QIcon.Mode.Normal, QIcon.Mode.Active, QIcon.Mode.Selected):
        icon.addPixmap(chosen, mode, QIcon.State.On)
    return icon
