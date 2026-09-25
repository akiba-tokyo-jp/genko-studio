"""The navigator (全体図) and the page overview.

The navigator shows the whole page small, with a frame around the part seen in the canvas; clicking or
dragging in it moves the view there. The page overview lays all pages out large enough to recognise,
side by side as spreads, and opens the one double-clicked.
"""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QDialog, QLabel, QListWidget, QListWidgetItem, QVBoxLayout, QWidget


class Navigator(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.setMinimumHeight(150)
        self.setToolTip("クリック・ドラッグで、その場所を画面の真ん中に")
        window.canvas.changed.connect(self.update)
        window.canvas.zoomChanged.connect(lambda _: self.update())

    def refresh(self) -> None:
        self.update()

    def _page_rect(self) -> QRectF | None:
        page = self.window.canvas.page
        if page is None:
            return None
        w, h = page.spec.width_mm, page.spec.height_mm
        scale = min((self.width() - 8) / w, (self.height() - 8) / h)
        return QRectF((self.width() - w * scale) / 2, (self.height() - h * scale) / 2, w * scale, h * scale)

    def seen_mm(self) -> tuple[float, float, float, float]:
        """The part of the page seen in the canvas (x, y, w, h mm)."""
        canvas = self.window.canvas
        corners = [QPointF(0, 0), QPointF(canvas.width(), 0), QPointF(0, canvas.height()), QPointF(canvas.width(), canvas.height())]
        pts = [canvas._to_mm(canvas._ev(p)) for p in corners]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#3a3a3a"))
        rect = self._page_rect()
        if rect is None:
            return
        painter.fillRect(rect, QColor("white"))
        background = self.window.canvas.background
        if background is not None:
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawPixmap(rect, background, QRectF(background.rect()))
        page = self.window.canvas.page
        scale = rect.width() / page.spec.width_mm
        x, y, w, h = self.seen_mm()
        seen = QRectF(rect.x() + x * scale, rect.y() + y * scale, w * scale, h * scale).intersected(QRectF(self.rect()))
        painter.setPen(QPen(QColor("#e8590c"), 2))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(seen)

    def _go(self, pos: QPointF) -> None:
        rect = self._page_rect()
        if rect is None:
            return
        scale = rect.width() / self.window.canvas.page.spec.width_mm
        self.window.canvas.center_on((pos.x() - rect.x()) / scale, (pos.y() - rect.y()) / scale)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        self._go(event.position())

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if event.buttons() & Qt.MouseButton.LeftButton:
            self._go(event.position())


def _thumb(episode, page, dpi: int = 16) -> QPixmap:
    from genko.render import render_page

    image = render_page(page, dpi, mode="proof", episode=episode).convert("RGB")
    data = image.tobytes()
    return QPixmap.fromImage(QImage(data, image.width, image.height, image.width * 3, QImage.Format.Format_RGB888).copy())


class PageOverview(QDialog):
    """All pages at a glance; double-click opens one."""

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.setWindowTitle("ページを並べて見る")
        self.resize(900, 640)
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.ViewMode.IconMode)
        self.list.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.list.setMovement(QListWidget.Movement.Static)
        self.list.setIconSize(QSize(160, 226))
        self.list.setSpacing(6)
        self.list.itemDoubleClicked.connect(self._open)
        note = QLabel("ダブルクリックでそのページを開きます。見開きのページは「◀▶」で示します。")
        note.setStyleSheet("color:#666")
        layout = QVBoxLayout(self)
        layout.addWidget(note)
        layout.addWidget(self.list, 1)
        episode = window.episode
        for page in episode.pages:
            label = f"{page.index}"
            if page.spread_with:
                label += " ◀▶ " + str(page.spread_with)
            item = QListWidgetItem(QIcon(_thumb(episode, page)), label)
            item.setData(Qt.ItemDataRole.UserRole, page.index)
            if page.index == window.current_page().index:
                item.setSelected(True)
            self.list.addItem(item)

    def _open(self, item: QListWidgetItem) -> None:
        self.window.go_to_page(int(item.data(Qt.ItemDataRole.UserRole)))
        self.accept()
