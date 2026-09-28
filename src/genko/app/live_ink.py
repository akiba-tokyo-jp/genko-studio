"""The line being drawn, drawn the way it will print: the same brush (genko.brushes.draw), pressure,
colour and opacity, at the screen's resolution, added to piece by piece as the pen moves — only the new
part is drawn, so a long line costs no more per move than a short one. The finished line (with its
smoothing and tapered ends) replaces it when the pen lifts."""

from __future__ import annotations

from PIL import Image, ImageChops
from PySide6.QtCore import Qt
from PySide6.QtGui import QImage, QPainter

from genko import brushes


class LiveInk:
    def __init__(self, size: tuple[int, int], scale: float, pan: tuple[float, float], brush: dict) -> None:
        """size: the canvas (px); scale: px per mm; pan: where the page's corner is (px); brush: the
        add_stroke fields of the pen in hand (kind, width_mm, rgb, opacity, pressure_gamma)."""
        self.size = (max(1, int(size[0])), max(1, int(size[1])))
        self.scale = float(scale)
        self.pan = (float(pan[0]), float(pan[1]))
        self.kind = brush.get("kind") or brushes.DEFAULT
        b = brushes.brush(self.kind)
        self.width_mm = float(brush.get("width_mm") or b.width_mm)
        self.rgb = tuple(brush.get("rgb") or b.rgb or (20, 20, 20))
        self.opacity = max(0.0, min(1.0, float(brush.get("opacity", 1.0)))) * b.opacity
        self.gamma = float(brush.get("pressure_gamma") or 1.0)
        self.stabilize = int(brush.get("stabilize") or 0)  # the same steadying as the finished line
        self.tail: tuple[int, int, QImage] | None = None  # the last few points, which the steadying still moves
        self.image = QImage(self.size[0], self.size[1], QImage.Format.Format_ARGB32_Premultiplied)
        self.image.fill(Qt.GlobalColor.transparent)
        self._cover = Image.new("L", self.size, 0)  # how much of each pixel the line covers so far
        self._drawn = 0  # points already drawn

    def matches(self, scale: float, pan: tuple[float, float], size: tuple[int, int]) -> bool:
        return (abs(scale - self.scale) < 1e-9 and abs(pan[0] - self.pan[0]) < 1e-6 and abs(pan[1] - self.pan[1]) < 1e-6
                and (max(1, int(size[0])), max(1, int(size[1]))) == self.size)

    def _screen(self, point) -> tuple:
        """A page point (mm) moved so the brush's page pixels land on the canvas's pixels."""
        x = float(point[0]) + self.pan[0] / self.scale
        y = float(point[1]) + self.pan[1] / self.scale
        # a point without pressure (the mouse) is kept at 0.7, as add_stroke keeps it
        pressure = max(0.0, min(1.0, float(point[2]))) if len(point) > 2 else 0.7
        return (x, y, pressure ** self.gamma if self.gamma != 1.0 else pressure)

    def extend(self, points: list) -> None:
        """Draw the part of the line added since the last call (with one point of overlap for the join)."""
        if not points:
            return
        start = max(0, self._drawn - 2)
        part = [self._screen(p) for p in points[start:]]
        if len(part) == 1:
            part = [part[0], (part[0][0] + 0.01 / self.scale, part[0][1] + 0.01 / self.scale, *part[0][2:])]
        self._paint(part)
        self._drawn = len(points)

    def follow(self, points: list) -> None:
        """The line so far, steadied as it will be when the pen lifts: the settled part is drawn once, and
        only the last few points (which the steadying still moves) are drawn again each time."""
        from genko.stroke import stabilize_points

        if self.stabilize < 3 or len(points) < 3:
            self.tail = None
            self.extend(points)
            return
        steady = stabilize_points([list(p) for p in points], self.stabilize)
        settled = steady[:max(1, len(steady) - self.stabilize // 2)]
        self.extend(settled)
        rest = [self._screen(p) for p in steady[max(0, len(settled) - 2):]]
        drawn = brushes.draw(self.size, rest, round(self.scale * 25.4, 4), self.width_mm, self.kind, seed="live") if len(rest) > 1 else None
        if drawn is None:
            self.tail = None
            return
        mask, (x0, y0) = drawn
        alpha = mask if self.opacity >= 1 else mask.point(lambda v, o=self.opacity: int(v * o))
        patch = Image.new("RGBA", mask.size, (*self.rgb, 0))
        patch.putalpha(alpha)
        data = patch.tobytes()
        self.tail = (x0, y0, QImage(data, patch.width, patch.height, patch.width * 4, QImage.Format.Format_RGBA8888).copy())

    def redraw(self, points: list, *copies: list) -> None:
        """Start the picture again (the line snapped to a ruler, became a straight line, or has symmetry
        copies)."""
        self.image.fill(Qt.GlobalColor.transparent)
        self._cover = Image.new("L", self.size, 0)
        self.tail = None
        for copy in copies:
            self._drawn = 0
            self.extend(copy)
        self._drawn = 0
        self.extend(points)

    def _paint(self, part: list) -> None:
        drawn = brushes.draw(self.size, part, round(self.scale * 25.4, 4), self.width_mm, self.kind, seed="live")
        if drawn is None:
            return
        mask, (x0, y0) = drawn
        box = (x0, y0, x0 + mask.width, y0 + mask.height)
        cover = ImageChops.lighter(self._cover.crop(box), mask)  # overlaps never darken twice
        self._cover.paste(cover, box[:2])
        alpha = cover if self.opacity >= 1 else cover.point(lambda v, o=self.opacity: int(v * o))
        patch = Image.new("RGBA", cover.size, (*self.rgb, 0))
        patch.putalpha(alpha)
        data = patch.tobytes()
        piece = QImage(data, patch.width, patch.height, patch.width * 4, QImage.Format.Format_RGBA8888)
        painter = QPainter(self.image)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        painter.drawImage(x0, y0, piece)
        painter.end()

    def coverage(self) -> Image.Image:
        return self._cover.copy()
