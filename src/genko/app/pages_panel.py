"""The page list: small pictures of every page, drag to reorder, and a menu for copies, spreads and
ノンブル. Pictures are drawn one at a time in the background so the window stays quick."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, QTimer
from PySide6.QtGui import QColor, QIcon, QImage, QPixmap
from PySide6.QtWidgets import QAbstractItemView, QListWidget, QListWidgetItem, QMenu, QMessageBox

THUMB_H = 100


def _icon(image) -> QIcon:
    rgb = image.convert("RGB")
    qimage = QImage(rgb.tobytes("raw", "RGB"), rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888).copy()
    return QIcon(QPixmap.fromImage(qimage))


class PageList(QListWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.setIconSize(QSize(round(THUMB_H * 0.72), THUMB_H))
        self.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.setDefaultDropAction(Qt.DropAction.MoveAction)
        self.setSpacing(2)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._menu)
        self._thumbs: dict[str, QIcon] = {}
        self._queue: list[str] = []
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setInterval(0)
        self._timer.timeout.connect(self._next_thumb)

    # --- pictures ----------------------------------------------------------------------------------------

    def fill(self, pages, text_of, dirty: str | None = "all") -> None:
        """The list from the book's pages; `dirty` = "all", a page id, or None (keep the pictures)."""
        if dirty == "all":
            self._thumbs.clear()
        elif dirty:
            self._thumbs.pop(dirty, None)
        self.blockSignals(True)
        self.clear()
        for page in pages:
            item = QListWidgetItem(text_of(page))
            item.setData(Qt.ItemDataRole.UserRole, page.index)
            item.setData(Qt.ItemDataRole.UserRole + 1, page.id)
            item.setIcon(self._thumbs.get(page.id) or self._blank())
            self.addItem(item)
        self.blockSignals(False)
        self._queue = [p.id for p in pages if p.id not in self._thumbs]
        if self._queue:
            self._timer.start()

    def update_page(self, page, text: str) -> None:
        """One page changed: its label now, its picture a moment later."""
        self._thumbs.pop(page.id, None)
        for row in range(self.count()):
            item = self.item(row)
            if item.data(Qt.ItemDataRole.UserRole + 1) == page.id:
                item.setText(text)
        if page.id not in self._queue:
            self._queue.append(page.id)
        self._timer.start(400)

    def _blank(self) -> QIcon:
        pix = QPixmap(self.iconSize())
        pix.fill(QColor("#f4f4f4"))
        return QIcon(pix)

    def _next_thumb(self) -> None:
        if not self._queue:
            return
        page_id = self._queue.pop(0)
        self._make(page_id)
        if self._queue:
            self._timer.start(0)

    def _make(self, page_id: str) -> None:
        from genko.render import render_page

        episode = self.window.episode
        page = next((p for p in episode.pages if p.id == page_id), None)
        if page is None:
            return
        dpi = max(6, round(THUMB_H / (page.spec.height_mm / 25.4)))
        try:
            image = render_page(page, dpi, mode="proof" if page.name_ok else "name", episode=episode)
        except Exception:
            return
        icon = _icon(image)
        self._thumbs[page_id] = icon
        for row in range(self.count()):
            item = self.item(row)
            if item.data(Qt.ItemDataRole.UserRole + 1) == page_id:
                item.setIcon(icon)

    def finish_pictures(self) -> None:
        """Draw every waiting picture now (tests, and before a screenshot)."""
        while self._queue:
            self._make(self._queue.pop(0))

    # --- order -------------------------------------------------------------------------------------------

    def dropEvent(self, event) -> None:  # noqa: N802
        before = [self.item(r).data(Qt.ItemDataRole.UserRole) for r in range(self.count())]
        moving = self.currentItem().data(Qt.ItemDataRole.UserRole) if self.currentItem() else None
        super().dropEvent(event)
        order = [self.item(r).data(Qt.ItemDataRole.UserRole) for r in range(self.count())]
        if order != before and sorted(order) == sorted(before):
            self.window.reorder_pages(order, moving)
        else:
            self.window._reload_pages()

    def move_page(self, index: int, delta: int) -> None:
        order = [p.index for p in self.window.episode.pages]
        i = order.index(index)
        j = max(0, min(len(order) - 1, i + delta))
        if i == j:
            return
        order.insert(j, order.pop(i))
        self.window.reorder_pages(order, index)

    # --- the menu ---------------------------------------------------------------------------------------

    def _menu(self, pos) -> None:
        item = self.itemAt(pos)
        if item is None:
            return
        index = item.data(Qt.ItemDataRole.UserRole)
        page = next((p for p in self.window.episode.pages if p.index == index), None)
        if page is None:
            return
        menu = QMenu(self)
        w = self.window
        menu.addAction("この後ろにページを追加", lambda: w.add_page_after(index))
        menu.addAction("このページを複製", lambda: w.duplicate_page(index))
        menu.addSeparator()
        menu.addAction("前へ移す", lambda: self.move_page(index, -1))
        menu.addAction("後ろへ移す", lambda: self.move_page(index, 1))
        menu.addSeparator()
        if page.spread_with:
            menu.addAction("見開きを解除", lambda: w.set_spread(index, None))
        else:
            if index < len(w.episode.pages):
                menu.addAction(f"{index + 1} ページと見開きにする", lambda: w.set_spread(index, index + 1))
            if index > 1:
                menu.addAction(f"{index - 1} ページと見開きにする", lambda: w.set_spread(index, index - 1))
        menu.addAction("このページのノンブルを隠す" if page.numero else "このページのノンブルを出す",
                       lambda: w.apply_ops([{"op": "set_nombre", "page": index, "numero": not page.numero}]))
        menu.addSeparator()
        menu.addAction("このページを削除…", lambda: self._delete(index))
        menu.exec(self.mapToGlobal(pos))

    def _delete(self, index: int) -> None:
        w = self.window
        answer = QMessageBox.question(self, "Genko", f"{index} ページを削除しますか？（元に戻す で取り消せます）")
        if answer == QMessageBox.StandardButton.Yes:
            w.apply_ops([{"op": "delete_page", "page": index}])


def nombre_dialog(window) -> None:
    """ノンブルの設定: where, face, size, the first number, the hidden nombre."""
    from PySide6.QtWidgets import QCheckBox, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox, QFormLayout, QSpinBox

    from genko import fonts, nombre

    cfg = nombre.settings(window.episode)
    dialog = QDialog(window)
    dialog.setWindowTitle("ノンブルの設定")
    show = QCheckBox("ノンブルを入れる")
    show.setChecked(bool(cfg["show"]))
    position = QComboBox()
    for key in nombre.POSITIONS:
        position.addItem(nombre.LABELS[key], key)
    position.setCurrentIndex(max(0, position.findData(cfg["position"])))
    face = QComboBox()
    for key, (label, *_rest) in fonts.BUNDLED.items():
        face.addItem(label, key)
    face.setCurrentIndex(max(0, face.findData(cfg["font"])))
    size = QDoubleSpinBox()
    size.setRange(1, 20)
    size.setSingleStep(0.5)
    size.setSuffix(" mm")
    size.setValue(float(cfg["size_mm"]))
    start = QSpinBox()
    start.setRange(0, 9999)
    start.setValue(int(cfg["start"]))
    start.setToolTip("1 ページ目の番号（前の話から続けるとき）")
    hidden = QCheckBox("隠しノンブルを入れる（のど側の下、製本で見えなくなる所）")
    hidden.setChecked(bool(cfg["hidden"]))
    hidden_size = QDoubleSpinBox()
    hidden_size.setRange(1, 10)
    hidden_size.setSuffix(" mm")
    hidden_size.setValue(float(cfg["hidden_size_mm"]))
    form = QFormLayout(dialog)
    form.addRow("", show)
    form.addRow("位置", position)
    form.addRow("書体", face)
    form.addRow("大きさ", size)
    form.addRow("始まりの番号", start)
    form.addRow("", hidden)
    form.addRow("隠しノンブルの大きさ", hidden_size)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
    buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    if dialog.exec() == QDialog.DialogCode.Accepted:
        window.apply_ops([{"op": "set_nombre", "show": show.isChecked(), "position": position.currentData(), "font": face.currentData(),
                           "size_mm": size.value(), "start": start.value(), "hidden": hidden.isChecked(),
                           "hidden_size_mm": hidden_size.value()}])
