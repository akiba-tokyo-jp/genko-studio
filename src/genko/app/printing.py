"""ファイル → 印刷…: pages on paper, from any printer the computer has.

The pages are drawn as they print (the print render: no name lines, tones as dots), cut to the
finished size or left as the whole sheet, and fitted to the printer's paper keeping their shape.
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QImage, QPainter
from PySide6.QtPrintSupport import QPrintDialog, QPrinter
from PySide6.QtWidgets import (
    QCheckBox,
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


def sheets(episode, pages: list[int], spreads: bool) -> list[list]:
    """The pages grouped by sheet: one page each, or a spread's two pages side by side (left first)."""
    by_index = {page.index: page for page in episode.pages}
    out, done = [], set()
    for index in pages:
        if index in done:
            continue
        page = by_index[index]
        partner = by_index.get(page.spread_with) if spreads and page.spread_with in pages else None
        if partner is None:
            out.append([page])
            done.add(index)
            continue
        pair = [page, partner] if page.side() == "left" else [partner, page]
        out.append(pair)
        done.update({page.index, partner.index})
    return out


def print_pages(episode, printer: QPrinter, pages: list[int], area: str = "trim", progress=None,
                scale: str = "fit", spreads: bool = False) -> int:
    """Draw these pages on the printer. scale "fit": as large as the paper allows; "actual": the page's
    real size (100 %). spreads: a spread's two pages on one sheet. Returns how many sheets were printed."""
    from PIL import Image

    from genko.export import crop_to
    from genko.render import render_page

    dpi = max(150, min(MAX_DPI, printer.resolution() or 300))
    painter = QPainter()
    if not painter.begin(printer):
        raise RuntimeError("プリンターを開けませんでした")
    done = 0
    groups = sheets(episode, pages, spreads)
    try:
        for n, group in enumerate(groups):
            if n:
                printer.newPage()
            images = [crop_to(render_page(page, dpi, mode="print", episode=episode, crop_marks=area == "paper"), page, area, dpi)
                      for page in group]
            if len(images) > 1:
                joined = Image.new("RGB", (sum(i.width for i in images), max(i.height for i in images)), "white")
                x = 0
                for image in images:
                    joined.paste(image.convert("RGB"), (x, 0))
                    x += image.width
                images = [joined]
            picture = _qimage(images[0])
            room = QRectF(painter.viewport())
            if scale == "actual":
                # the page's own size: its pixels at `dpi`, in the printer's device pixels
                factor = (printer.resolution() or dpi) / dpi
                w, h = picture.width() * factor, picture.height() * factor
            else:
                fit = min(room.width() / picture.width(), room.height() / picture.height())
                w, h = picture.width() * fit, picture.height() * fit
            target = QRectF(room.x() + (room.width() - w) / 2, room.y() + (room.height() - h) / 2, w, h)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(target, picture)
            done += 1
            if progress is not None:
                progress(done, len(groups))
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
        self.scale = QComboBox()
        self.scale.addItem("用紙に合わせる", "fit")
        self.scale.addItem("原寸（100%）", "actual")
        self.scale.setToolTip("原寸: 原稿の実際の大きさで印刷します（用紙より大きい所は切れます）")
        self.spreads = QCheckBox("見開きは 2 ページを 1 枚に")
        form = QFormLayout()
        form.addRow("ページ", self.pages)
        form.addRow("範囲", self.area)
        form.addRow("大きさ", self.scale)
        form.addRow("", self.spreads)
        note = QLabel("印刷と同じ見え方（ネームは出ません）で、用紙に合わせて縮めて印刷します。"
                      "入稿用のデータは「書き出し…」で作ります。")
        note.setWordWrap(True)
        note.setStyleSheet("color:#666")
        buttons = QDialogButtonBox()
        self.print_button = buttons.addButton("プリンターを選んで印刷…", QDialogButtonBox.ButtonRole.AcceptRole)
        preview = buttons.addButton("プレビュー…", QDialogButtonBox.ButtonRole.ActionRole)
        preview.clicked.connect(self.preview)
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
            printed = print_pages(self.window.episode, printer, pages, self.area.currentData(),
                                  scale=self.scale.currentData(), spreads=self.spreads.isChecked())
        except RuntimeError as exc:
            QMessageBox.warning(self, "印刷", str(exc))
            return
        finally:
            self.unsetCursor()
        self.window.statusBar().showMessage(f"{printed} 枚を印刷に送りました", 5000)
        self.accept()

    def preview(self) -> QDialog | None:
        """How the sheets will come out, before any paper is used."""
        from PySide6.QtPrintSupport import QPrintPreviewDialog

        try:
            pages = self.chosen()
        except ValueError as exc:
            QMessageBox.warning(self, "印刷", str(exc).replace("書き出す", "印刷する"))
            return None
        printer = QPrinter(QPrinter.PrinterMode.HighResolution)
        dialog = QPrintPreviewDialog(printer, self)
        dialog.setWindowTitle("印刷のプレビュー")
        dialog.paintRequested.connect(lambda p: print_pages(self.window.episode, p, pages, self.area.currentData(),
                                                            scale=self.scale.currentData(), spreads=self.spreads.isChecked()))
        dialog.open()
        return dialog
