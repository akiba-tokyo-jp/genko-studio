"""ファイル → 印刷…: pages on paper, from any printer the computer has.

The pages are drawn as they print (the print render: no name lines, tones as dots), cut to the
finished size or left as the whole sheet, and fitted to the printer's paper keeping their shape.
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QImage, QPainter
from PySide6.QtPrintSupport import QPrintDialog, QPrinter
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QVBoxLayout,
)

AREAS = [("仕上がりで切る", "trim"), ("裁ち落としまで", "bleed"), ("用紙全体（トンボ付き）", "paper")]
MAX_DPI = 600


def _qimage(image) -> QImage:
    rgb = image.convert("RGB")
    data = rgb.tobytes()
    return QImage(data, rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888).copy()


def print_pages(episode, printer: QPrinter, pages: list[int], area: str = "trim", progress=None) -> int:
    """Draw these pages on the printer, one sheet each. Returns how many were printed."""
    from genko.export import crop_to
    from genko.render import render_page

    dpi = max(150, min(MAX_DPI, printer.resolution() or 300))
    painter = QPainter()
    if not painter.begin(printer):
        raise RuntimeError("プリンターを開けませんでした")
    done = 0
    try:
        by_index = {page.index: page for page in episode.pages}
        for n, index in enumerate(pages):
            page = by_index[index]
            if n:
                printer.newPage()
            image = render_page(page, dpi, mode="print", episode=episode, crop_marks=area == "paper")
            image = crop_to(image, page, area, dpi)
            picture = _qimage(image)
            room = QRectF(painter.viewport())
            scale = min(room.width() / picture.width(), room.height() / picture.height())
            w, h = picture.width() * scale, picture.height() * scale
            target = QRectF(room.x() + (room.width() - w) / 2, room.y() + (room.height() - h) / 2, w, h)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(target, picture)
            done += 1
            if progress is not None:
                progress(done, len(pages))
    finally:
        painter.end()
    return done


class PrintDialog(QDialog):
    """Which pages and how much of each; then the system's printer dialog."""

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.setWindowTitle("印刷")
        count = len(window.episode.pages)
        self.pages = QLineEdit(f"1-{count}" if count > 1 else "1")
        self.pages.setToolTip("例: 1-4, 7（全ページなら 1-%d）" % count)
        self.current = window.current_page().index if window.current_page() else 1
        self.area = QComboBox()
        for label, key in AREAS:
            self.area.addItem(label, key)
        form = QFormLayout()
        form.addRow("ページ", self.pages)
        form.addRow("範囲", self.area)
        note = QLabel("印刷と同じ見え方（ネームは出ません）で、用紙に合わせて縮めて印刷します。"
                      "入稿用のデータは「書き出し…」で作ります。")
        note.setWordWrap(True)
        note.setStyleSheet("color:#666")
        buttons = QDialogButtonBox()
        self.print_button = buttons.addButton("プリンターを選んで印刷…", QDialogButtonBox.ButtonRole.AcceptRole)
        this = buttons.addButton("このページだけ", QDialogButtonBox.ButtonRole.ActionRole)
        this.clicked.connect(lambda: self.pages.setText(str(self.current)))
        cancel = buttons.addButton("やめる", QDialogButtonBox.ButtonRole.RejectRole)
        cancel.clicked.connect(self.reject)
        self.print_button.clicked.connect(self._print)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(note)
        layout.addWidget(buttons)

    def chosen(self) -> list[int]:
        from genko.app.exporting import parse_pages

        return parse_pages(self.pages.text(), len(self.window.episode.pages))

    def _print(self, printer: QPrinter | None = None) -> None:
        try:
            pages = self.chosen()
        except ValueError as exc:
            QMessageBox.warning(self, "印刷", str(exc).replace("書き出す", "印刷する"))
            return
        if printer is None or isinstance(printer, bool):
            printer = QPrinter(QPrinter.PrinterMode.HighResolution)
            printer.setDocName(self.window.episode.title or "Genko")
            ask = QPrintDialog(printer, self)
            ask.setWindowTitle("プリンターを選ぶ")
            if ask.exec() != QDialog.DialogCode.Accepted:
                return
        self.setCursor(Qt.CursorShape.WaitCursor)
        try:
            printed = print_pages(self.window.episode, printer, pages, self.area.currentData())
        except RuntimeError as exc:
            QMessageBox.warning(self, "印刷", str(exc))
            return
        finally:
            self.unsetCursor()
        self.window.statusBar().showMessage(f"{printed} ページを印刷に送りました", 5000)
        self.accept()
