"""サブビュー: reference pictures beside the page — a photo, a character sheet, a colour scheme — to look
at while drawing and to take colours from (click: the pen's colour). The list is kept in the settings,
so it is there for every book.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QImage, QPainter, QPixmap
from PySide6.QtWidgets import QComboBox, QFileDialog, QHBoxLayout, QPushButton, QVBoxLayout, QWidget

from genko.app.preferences import settings

IMAGE_FILTER = "画像 (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)"


def kept() -> list[str]:
    value = settings().value("subview/images", "")
    return [p for p in str(value or "").split("\t") if p and Path(p).is_file()]


def keep(paths: list[str]) -> None:
    settings().setValue("subview/images", "\t".join(paths))


class Picture(QWidget):
    """The picture, fitted to the panel; a click reports the colour under it."""

    picked = Signal(object)  # (r, g, b)

    def __init__(self) -> None:
        super().__init__()
        self.image: QImage | None = None
        self.setMinimumHeight(140)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setToolTip("クリックした所の色をペンの色にします")

    def set_image(self, image: QImage | None) -> None:
        self.image = image
        self.update()

    def _target(self):
        if self.image is None or self.image.isNull():
            return None
        scale = min(self.width() / self.image.width(), self.height() / self.image.height())
        w, h = self.image.width() * scale, self.image.height() * scale
        return (self.width() - w) / 2, (self.height() - h) / 2, scale

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#3a3a3a"))
        target = self._target()
        if target is None:
            painter.setPen(QColor("#bbbbbb"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "「画像を足す」で資料を開きます")
            return
        x, y, scale = target
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        painter.drawPixmap(int(x), int(y), QPixmap.fromImage(self.image).scaled(
            int(self.image.width() * scale), int(self.image.height() * scale), Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation))

    def colour_at(self, pos: QPointF):
        target = self._target()
        if target is None:
            return None
        x, y, scale = target
        px, py = int((pos.x() - x) / scale), int((pos.y() - y) / scale)
        if not (0 <= px < self.image.width() and 0 <= py < self.image.height()):
            return None
        c = self.image.pixelColor(px, py)
        return c.red(), c.green(), c.blue()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        colour = self.colour_at(event.position())
        if colour is not None:
            self.picked.emit(colour)


class SubView(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.choice = QComboBox()
        self.choice.activated.connect(lambda _: self.show_current())
        add = QPushButton("画像を足す…")
        add.clicked.connect(self.add_dialog)
        remove = QPushButton("外す")
        remove.clicked.connect(self.remove_current)
        self.picture = Picture()
        self.picture.picked.connect(self._picked)
        row = QHBoxLayout()
        row.addWidget(add)
        row.addWidget(remove)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addWidget(self.choice)
        layout.addWidget(self.picture, 1)
        layout.addLayout(row)
        self.refresh()

    def refresh(self) -> None:
        current = self.choice.currentData()
        self.choice.clear()
        for path in kept():
            self.choice.addItem(Path(path).name, path)
        if current:
            self.choice.setCurrentIndex(max(0, self.choice.findData(current)))
        self.show_current()

    def show_current(self) -> None:
        path = self.choice.currentData()
        self.picture.set_image(QImage(path) if path else None)

    def add(self, path: str) -> bool:
        if QImage(path).isNull():
            return False
        paths = [p for p in kept() if p != path] + [path]
        keep(paths)
        self.refresh()
        self.choice.setCurrentIndex(self.choice.findData(path))
        self.show_current()
        return True

    def add_dialog(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "資料の画像", "", IMAGE_FILTER)
        if path and not self.add(path):
            self.window.flash("その画像は開けませんでした", 3000)

    def remove_current(self) -> None:
        path = self.choice.currentData()
        if path:
            keep([p for p in kept() if p != path])
            self.refresh()

    def _picked(self, rgb) -> None:
        self.window.brush.set_colour(rgb)
        self.window.flash(f"資料の色 {tuple(rgb)} をペンの色にしました", 2000)
