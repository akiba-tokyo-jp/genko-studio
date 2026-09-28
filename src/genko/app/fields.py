"""Fields as painting tools have them (V2): a slider beside the number (drag for a feel, type for an exact value),
and a sample line drawn with the pen as it is set now, so a change is seen before the page is touched."""

from __future__ import annotations

import math

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QHBoxLayout, QLabel, QSlider, QWidget

STEPS = 1000


def slider_for(spin, log: bool = False) -> QWidget:
    """A row: a slider and the number field it moves (and follows). `log`: even steps over a wide range (a pen from
    0.05 to 50 mm moves as finely at the thin end as at the thick)."""
    row = QWidget()
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(8)
    slider = QSlider(Qt.Orientation.Horizontal)
    slider.setRange(0, STEPS)
    slider.setToolTip(spin.toolTip())
    low, high = float(spin.minimum()), float(spin.maximum())
    if log:
        low = max(low, 1e-3)

    def to_slider(value: float) -> int:
        if log:
            return round(STEPS * math.log(max(value, low) / low) / math.log(high / low))
        return round(STEPS * (value - low) / ((high - low) or 1))

    def from_slider(pos: int) -> float:
        t = pos / STEPS
        return low * (high / low) ** t if log else low + (high - low) * t

    def moved(pos: int) -> None:
        if not getattr(slider, "_following", False):
            value = from_slider(pos)
            spin.setValue(round(value, spin.decimals()) if hasattr(spin, "decimals") else round(value))

    def follow(value) -> None:
        slider._following = True
        slider.setValue(to_slider(float(value)))
        slider._following = False

    slider.valueChanged.connect(moved)
    spin.valueChanged.connect(follow)
    follow(spin.value())
    spin.setMaximumWidth(96)
    layout.addWidget(slider, 1)
    layout.addWidget(spin)
    row.slider = slider
    return row


def with_value(slider: QSlider, suffix: str = "%") -> QWidget:
    """A slider with its value written beside it."""
    row = QWidget()
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(8)
    value = QLabel()
    value.setMinimumWidth(value.fontMetrics().horizontalAdvance("100%") + 4)
    value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
    slider.valueChanged.connect(lambda v: value.setText(f"{v}{suffix}"))
    value.setText(f"{slider.value()}{suffix}")
    layout.addWidget(slider, 1)
    layout.addWidget(value)
    row.value_label = value
    return row


class LineSample(QLabel):
    """The pen as it is set now, drawn: an S of one line pressed lightly, hard, then lightly, at the real width
    (to a limit), with its taper, pressure, opacity and colour. Redrawn a moment after a setting changes."""

    def __init__(self, fields) -> None:
        super().__init__()
        self.fields = fields  # () -> {"kind", "width_mm", "taper", "pressure_gamma"?, "opacity"?, "rgb"?}
        self.setMinimumHeight(56)
        self.setMinimumWidth(80)
        # (as wide as the panel gives it: its picture follows the width, never the other way round)
        from PySide6.QtWidgets import QSizePolicy

        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.setObjectName("lineSample")
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setToolTip("今の設定で引いた線の見本（弱く → 強く → 弱く）")
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setInterval(60)
        self._timer.timeout.connect(self._draw)

    def sizeHint(self):  # noqa: N802
        from PySide6.QtCore import QSize

        return QSize(160, 56)

    def minimumSizeHint(self):  # noqa: N802
        from PySide6.QtCore import QSize

        return QSize(80, 56)

    def refresh(self) -> None:
        self._timer.start()

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        self.refresh()

    def _draw(self) -> None:
        from PIL import Image

        from genko import brushes
        from genko.app import theme
        from genko.stroke import taper_points

        try:
            f = self.fields()
        except RuntimeError:
            return
        scale = 2
        w, h = max(80, self.width() - 10) * scale, 50 * scale
        dpi = round(96 * scale)
        mm = 25.4 / dpi
        gamma = float(f.get("pressure_gamma") or 1.0)
        pts = [[(10 + i * (w - 20) / 80) * mm, (h / 2 + h * 0.22 * math.sin(i / 12.7)) * mm,
                max(0.02, math.sin(math.pi * i / 80)) ** gamma] for i in range(81)]
        if f.get("taper"):
            pts = taper_points(pts)
        width = max(0.1, min(float(f.get("width_mm") or 1.0), h * 0.3 * mm))
        image = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        try:
            drawn = brushes.draw((w, h), pts, dpi, width, f.get("kind") or "g", seed="sample")
        except Exception:  # (a broken brush of one's own: no sample rather than an error)
            drawn = None
        if drawn is not None:
            cover, origin = drawn
            rgb = tuple(f.get("rgb") or (20, 20, 20))
            if rgb == (20, 20, 20) and theme.tokens().dark:
                rgb = (225, 225, 228)  # (black ink on a dark panel: shown light, so it reads)
            ink = Image.new("RGBA", cover.size, (*rgb, 255))
            ink.putalpha(cover.point(lambda v: int(v * max(0.08, float(f.get("opacity", 1.0))))))
            image.alpha_composite(ink, origin)
        pixmap = QPixmap.fromImage(QImage(image.tobytes(), w, h, w * 4, QImage.Format.Format_RGBA8888).copy())
        pixmap.setDevicePixelRatio(scale)
        self.setPixmap(pixmap)


def effect_picture(kind: str, size: tuple[int, int] = (60, 44)) -> QPixmap:
    """A small picture of an effect (the same drawing the page gets), for the effect tool's choices."""
    from PIL import Image

    from genko import effects
    from genko.app import theme
    from genko.models import PageSpec, new_episode

    key = (kind, size, theme.tokens().dark)
    if key in _PICTURES:
        return _PICTURES[key]
    page = new_episode("t", 1, 1, PageSpec(width_mm=72, height_mm=52, dpi=600, bleed_mm=0, inner_margin_mm=0)).pages[0]
    dpi = 2 * 25.4 * size[0] / page.spec.width_mm
    b = page.bleed_rect_mm()
    params = {"center": [b.x + b.width / 2, b.y + b.height / 2], "count": 60 if kind != "speed" else 26,
              "inner": [b.width * 0.16, b.height * 0.16]}
    w, h = round(b.width * dpi / 25.4) + 2, round(b.height * dpi / 25.4) + 2
    image = effects.draw(Image.new("RGB", (w, h), (255, 255, 255)), {"id": f"sample-{kind}", "kind": kind, "params": params}, page, dpi)
    image = image.resize((size[0] * 2, size[1] * 2) if abs(w / h - size[0] / size[1]) < 0.05 else
                         (size[0] * 2, round(size[0] * 2 * h / w)), Image.Resampling.LANCZOS).crop((0, 0, size[0] * 2, size[1] * 2))
    pixmap = QPixmap.fromImage(QImage(image.convert("RGBA").tobytes(), image.width, image.height, image.width * 4,
                                      QImage.Format.Format_RGBA8888).copy())
    pixmap.setDevicePixelRatio(2)
    _PICTURES[key] = pixmap
    return pixmap


_PICTURES: dict = {}


def effect_tiles(actions, kinds) -> QWidget:
    """The effect kinds as pictures (two to a row) with their names under them: chosen by how they look."""
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QIcon
    from PySide6.QtWidgets import QGridLayout, QToolButton

    box = QWidget()
    grid = QGridLayout(box)
    grid.setContentsMargins(0, 0, 0, 0)
    grid.setSpacing(6)
    for i, (action, kind) in enumerate(zip(actions, kinds)):
        tile = QToolButton()
        tile.setObjectName("effectTile")
        try:
            action.setIcon(QIcon(effect_picture(kind)))
        except Exception:  # (no picture: the name alone still works)
            pass
        tile.setDefaultAction(action)
        tile.setIconSize(QSize(60, 44))
        tile.setMinimumWidth(40)
        tile.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextUnderIcon)
        tile.setSizePolicy(tile.sizePolicy().Policy.Expanding, tile.sizePolicy().Policy.Fixed)
        grid.addWidget(tile, i // 2, i % 2)
    return box
