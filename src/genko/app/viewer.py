"""A large, zoomable viewer for looking closely before deciding (pages, panels, candidates side by side).

Opens fitted to the window; the wheel zooms, dragging moves, 0 fits again, 1 shows 100%.
With several images they are laid out side by side with their captions; with a page list
the arrows (or ← →) move between pages.
"""

from __future__ import annotations

from typing import Callable

from PIL import Image
from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QFont, QKeySequence, QPainter, QShortcut
from PySide6.QtWidgets import (
    QDialog,
    QGraphicsScene,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
)

Item = tuple[str, Image.Image]


class ImageView(QGraphicsView):
    def __init__(self) -> None:
        super().__init__()
        self.setScene(QGraphicsScene(self))
        self.setRenderHints(QPainter.RenderHint.SmoothPixmapTransform | QPainter.RenderHint.Antialiasing)
        self.setDragMode(QGraphicsView.DragMode.ScrollHandDrag)
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self.setBackgroundBrush(QColor("#3a3a3a"))
        self._fitted = True

    def show_items(self, items: list[Item]) -> None:
        from genko.app.studio_widgets import to_pixmap

        scene = self.scene()
        scene.clear()
        x = 0.0
        gap = 24.0
        font = QFont()
        font.setPointSize(14)
        tallest = max((image.height for _, image in items), default=0)
        for caption, image in items:
            item = scene.addPixmap(to_pixmap(image))
            item.setPos(x, 0)
            item.setTransformationMode(Qt.TransformationMode.SmoothTransformation)
            if caption:
                text = scene.addSimpleText(caption, font)
                text.setBrush(QColor("#f1f3f5"))
                text.setPos(x, tallest + 8)
            x += image.width + gap
        scene.setSceneRect(scene.itemsBoundingRect().adjusted(-gap, -gap, gap, gap))
        self._fitted = True
        self.fit()

    def fit(self) -> None:
        self.fitInView(self.scene().sceneRect(), Qt.AspectRatioMode.KeepAspectRatio)
        self._fitted = True

    def actual(self) -> None:
        self.resetTransform()
        self._fitted = False

    def wheelEvent(self, event) -> None:  # noqa: N802
        delta = event.angleDelta().y() or event.pixelDelta().y()
        if delta:
            factor = 1.0 + max(-0.5, min(0.5, delta / 600))
            self.scale(factor, factor)
            self._fitted = False

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        if self._fitted:
            self.fit()


class ViewerDialog(QDialog):
    """items: what to show now. pages/load_page: optional page list and a loader for ← →."""

    def __init__(self, parent, title: str, items: list[Item], note: str = "",
                 pages: list[int] | None = None, load_page: Callable[[int], list[Item]] | None = None,
                 current: int | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle(title)
        self.pages = pages or []
        self.load_page = load_page
        self.index = self.pages.index(current) if current in self.pages else 0
        self.view = ImageView()
        self.caption = QLabel(note)
        self.caption.setWordWrap(True)
        prev_button = QPushButton("← 前のページ")
        next_button = QPushButton("次のページ →")
        prev_button.clicked.connect(lambda: self.step(-1))
        next_button.clicked.connect(lambda: self.step(1))
        fit = QPushButton("全体（0）")
        fit.clicked.connect(self.view.fit)
        actual = QPushButton("100%（1）")
        actual.clicked.connect(self.view.actual)
        close = QPushButton("閉じる")
        close.clicked.connect(self.accept)
        self.page_label = QLabel()
        bar = QHBoxLayout()
        if self.pages and load_page:
            bar.addWidget(prev_button)
            bar.addWidget(self.page_label)
            bar.addWidget(next_button)
        bar.addStretch(1)
        bar.addWidget(fit)
        bar.addWidget(actual)
        bar.addWidget(close)
        layout = QVBoxLayout(self)
        layout.addWidget(self.caption)
        layout.addWidget(self.view, 1)
        layout.addLayout(bar)
        for key, slot in (("0", self.view.fit), ("1", self.view.actual), ("Left", lambda: self.step(-1)),
                          ("Right", lambda: self.step(1))):
            QShortcut(QKeySequence(key), self, activated=slot)
        screen = parent.screen().availableGeometry() if parent is not None and parent.screen() else QRectF(0, 0, 1280, 800)
        self.resize(int(screen.width() * 0.85), int(screen.height() * 0.9))
        self.view.show_items(items)
        self._label()

    def _label(self) -> None:
        if self.pages:
            self.page_label.setText(f"{self.pages[self.index]} ページ（{self.index + 1} / {len(self.pages)}）")

    def step(self, delta: int) -> None:
        if not (self.pages and self.load_page):
            return
        nxt = self.index + delta
        if 0 <= nxt < len(self.pages):
            self.index = nxt
            self.view.show_items(self.load_page(self.pages[nxt]))
            self._label()
