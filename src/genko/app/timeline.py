"""The timeline (タイムライン) for an animation page: a row per animation folder, a column per frame; each cell
shows which cel is exposed there. Click a frame to see it (and draw on its cel), right-click to set the cel,
play it, show the frames around it faint (onion skin), move the camera, and write it out."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QCheckBox,
    QFileDialog,
    QGridLayout,
    QLabel,
    QMenu,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from genko import anim

CELL_W = 26


class TimelinePanel(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.frame = 1
        self.onion = QCheckBox("前後を透かす")
        self.onion.setToolTip("オニオンスキン: 前のフレームを赤、次を青で薄く出します")
        self.onion.setChecked(True)
        self.onion.toggled.connect(lambda _: self.window.canvas.invalidate())
        self.start = QPushButton("このページをアニメーションにする")
        self.start.clicked.connect(self._start)
        self.play_button = QPushButton("▶ 再生")
        self.play_button.setCheckable(True)
        self.play_button.toggled.connect(self.play)
        self.fps = QSpinBox()
        self.fps.setRange(1, 60)
        self.fps.setSuffix(" fps")
        self.fps.setToolTip("1 秒に何フレーム進むか")
        self.fps.editingFinished.connect(lambda: self._set({"fps": self.fps.value()}))
        self.frames = QSpinBox()
        self.frames.setRange(1, anim.MAX_FRAMES)
        self.frames.setSuffix(" フレーム")
        self.frames.editingFinished.connect(lambda: self._set({"frames": self.frames.value()}))
        self.loop = QCheckBox("くり返す")
        self.loop.toggled.connect(lambda on: self._set({"loop": bool(on)}))
        self.where = QLabel()
        self.table = QTableWidget()
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.horizontalHeader().setMinimumSectionSize(CELL_W)
        self.table.horizontalHeader().setDefaultSectionSize(CELL_W)
        self.table.verticalHeader().setDefaultSectionSize(22)
        self.table.cellClicked.connect(self._clicked)
        self.table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.table.customContextMenuRequested.connect(self._menu)
        self.table.cellDoubleClicked.connect(lambda row, col: self._menu(self.table.visualItemRect(self.table.item(row, col)).center()))
        self.add_folder = QPushButton("＋フォルダー")
        self.add_folder.setToolTip("アニメーションフォルダー: セルを入れ、タイムラインの 1 行になります")
        self.add_folder.clicked.connect(self._add_folder)
        self.add_cel = QPushButton("＋セル")
        self.add_cel.setToolTip("選んだ行のフォルダーに新しいセルを作り、このフレームから出します")
        self.add_cel.clicked.connect(self._add_cel)
        self.camera = QPushButton("カメラ")
        self.camera.setToolTip("カメラワーク: 今見えている範囲を、このフレームのカメラにします（フレームの間は動きます）")
        self.camera.clicked.connect(self._camera_here)
        self.export_button = QPushButton("書き出し…")
        self.export_button.clicked.connect(self._export)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)
        self._played: dict[int, object] = {}

        top = QGridLayout()
        top.addWidget(self.play_button, 0, 0)
        top.addWidget(self.fps, 0, 1)
        top.addWidget(self.frames, 1, 0)
        top.addWidget(self.loop, 1, 1)
        top.addWidget(self.onion, 2, 0)
        top.addWidget(self.where, 2, 1)
        buttons = QGridLayout()
        buttons.addWidget(self.add_folder, 0, 0)
        buttons.addWidget(self.add_cel, 0, 1)
        buttons.addWidget(self.camera, 1, 0)
        buttons.addWidget(self.export_button, 1, 1)
        layout = QVBoxLayout(self)
        layout.addWidget(self.start)
        layout.addLayout(top)
        layout.addWidget(self.table, 1)
        layout.addLayout(buttons)
        self.refresh()

    # --- state --------------------------------------------------------------------------------------------------

    def page(self):
        return self.window.current_page()

    def folder_ids(self) -> list[str]:
        page = self.page()
        return [t["folder"] for t in (anim.spec(page) or {}).get("tracks", [])] if page is not None else []

    def current_folder(self) -> str | None:
        ids = self.folder_ids()
        row = self.table.currentRow()
        return ids[row] if 0 <= row < len(ids) else (ids[0] if ids else None)

    def refresh(self) -> None:
        page = self.page()
        on = page is not None and anim.is_animation(page)
        self.start.setVisible(page is not None and not on)
        for widget in (self.play_button, self.fps, self.frames, self.loop, self.onion, self.where, self.table, self.add_folder,
                       self.add_cel, self.camera, self.export_button):
            widget.setEnabled(on)
        if not on:
            self.table.clear()
            self.table.setRowCount(0)
            self.table.setColumnCount(0)
            self.where.setText("")
            return
        data = anim.spec(page)
        count = anim.frames_of(page)
        self.frame = max(1, min(self.frame, count))
        for widget, value in ((self.fps, int(anim.fps_of(page))), (self.frames, count)):
            widget.blockSignals(True)
            widget.setValue(value)
            widget.blockSignals(False)
        self.loop.blockSignals(True)
        self.loop.setChecked(bool(data.get("loop", True)))
        self.loop.blockSignals(False)
        layers = {layer.id: layer for layer in page.layers}
        ids = self.folder_ids()
        row_now = max(0, self.table.currentRow())
        self.table.blockSignals(True)
        self.table.clear()
        self.table.setRowCount(len(ids))
        self.table.setColumnCount(count)
        self.table.setHorizontalHeaderLabels([str(f) for f in range(1, count + 1)])
        self.table.setVerticalHeaderLabels([layers[i].title if i in layers else "?" for i in ids])
        keys = {k["frame"] for k in data.get("camera") or []}
        for row, folder in enumerate(ids):
            starts = {at: cel for at, cel in anim.track(page, folder).get("cels", [])}
            for col in range(count):
                frame = col + 1
                cel = anim.cel_at(page, folder, frame)
                if frame in starts:
                    text = (layers[cel].title if cel in layers else "×") if cel else "×"
                else:
                    text = "│" if cel else ""
                item = QTableWidgetItem(text)
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                if frame == self.frame:
                    item.setBackground(QBrush(QColor(255, 225, 150)))
                elif frame in keys:
                    item.setBackground(QBrush(QColor(210, 230, 250)))
                self.table.setItem(row, col, item)
        if ids:
            self.table.setCurrentCell(min(row_now, len(ids) - 1), self.frame - 1)
        self.table.blockSignals(False)
        self.where.setText(f"{self.frame} / {count}")

    def set_frame(self, frame: int, follow: bool = True) -> None:
        """Show this frame; the drawing goes on the cel it shows in the chosen folder."""
        page = self.page()
        if page is None or not anim.is_animation(page):
            return
        self.frame = max(1, min(int(frame), anim.frames_of(page)))
        self.window.anim_frames[page.id] = self.frame
        folder = self.current_folder()
        cel = anim.cel_at(page, folder, self.frame) if folder else None
        if follow and cel and cel != self.window._target_layer_id and any(layer.id == cel for layer in page.layers):
            self.window.set_target_layer(cel)
            self.window.layers.refresh()
        self.refresh()
        self.window.canvas.invalidate()

    # --- edits --------------------------------------------------------------------------------------------------

    def _ops(self, ops: list[dict]) -> bool:
        ok = self.window.apply_ops(ops)
        self.refresh()
        self.window.layers.refresh()
        return ok

    def _set(self, change: dict) -> None:
        page = self.page()
        if page is not None and anim.is_animation(page):
            self._ops([{"op": "set_animation", "page": page.index, **change}])

    def _start(self) -> None:
        page = self.page()
        if page is None:
            return
        from genko.models import new_id

        folder, cel = new_id(), new_id()
        if self._ops([{"op": "set_animation", "page": page.index, "fps": 12, "frames": 24},
                      {"op": "add_anim_folder", "page": page.index, "id": folder},
                      {"op": "add_cel", "page": page.index, "folder": folder, "id": cel, "name": "1"}]):
            self.window.set_target_layer(cel)
            self.set_frame(1)

    def _add_folder(self) -> None:
        page = self.page()
        if page is not None:
            self._ops([{"op": "add_anim_folder", "page": page.index}])

    def _add_cel(self) -> None:
        page, folder = self.page(), self.current_folder()
        if page is None or folder is None:
            return
        from genko.models import new_id

        cel = new_id()
        if self._ops([{"op": "add_cel", "page": page.index, "folder": folder, "id": cel, "at": self.frame}]):
            self.window.set_target_layer(cel)
            self.set_frame(self.frame)

    def _clicked(self, row: int, col: int) -> None:
        self.set_frame(col + 1)

    def _menu(self, pos) -> None:
        page = self.page()
        item = self.table.itemAt(pos)
        if page is None or item is None:
            return
        row, frame = item.row(), item.column() + 1
        folder = self.folder_ids()[row]
        menu = QMenu(self)
        for layer in anim.cels_of(page, folder):
            menu.addAction(f"セル「{layer.title}」を出す", lambda c=layer.id: self._ops(
                [{"op": "set_exposure", "page": page.index, "folder": folder, "frame": frame, "cel": c}]))
        menu.addSeparator()
        menu.addAction("何も出さない（空セル）", lambda: self._ops(
            [{"op": "set_exposure", "page": page.index, "folder": folder, "frame": frame, "cel": None}]))
        menu.addAction("指定を消す（前のセルを続ける）", lambda: self._ops(
            [{"op": "set_exposure", "page": page.index, "folder": folder, "frame": frame, "clear": True}]))
        menu.addSeparator()
        menu.addAction("このフレームのカメラを消す", lambda: self._ops(
            [{"op": "set_camera_key", "page": page.index, "frame": frame, "rect": None}]))
        cel = anim.cel_at(page, folder, frame)
        if cel:
            table = list((anim.spec(page) or {}).get("light_table") or [])
            on = cel in table
            menu.addAction("ライトテーブルから外す" if on else "ライトテーブルに置く（いつも薄く見る）", lambda: self._ops(
                [{"op": "set_light_table", "page": page.index, "cels": [c for c in table if c != cel] if on else table + [cel]}]))
        menu.exec(self.table.viewport().mapToGlobal(pos))

    def _camera_here(self) -> None:
        """The part of the page the screen shows now becomes the camera at this frame."""
        page = self.page()
        if page is None:
            return
        canvas = self.window.canvas
        x0, y0 = canvas._to_mm(canvas.rect().topLeft())
        x1, y1 = canvas._to_mm(canvas.rect().bottomRight())
        x0, x1 = sorted((max(0.0, x0), min(page.spec.width_mm, x1)))
        y0, y1 = sorted((max(0.0, y0), min(page.spec.height_mm, y1)))
        self._ops([{"op": "set_camera_key", "page": page.index, "frame": self.frame, "rect": [x0, y0, x1 - x0, y1 - y0]}])

    # --- playing and writing out -----------------------------------------------------------------------------------

    def play(self, on: bool) -> None:
        page = self.page()
        if not on or page is None or not anim.is_animation(page):
            self.timer.stop()
            self.play_button.setText("▶ 再生")
            self.window.canvas.invalidate()
            return
        self.play_button.setText("■ 止める")
        self._played = {}
        self.timer.start(max(15, round(1000 / anim.fps_of(page))))

    def _tick(self) -> None:
        page = self.page()
        if page is None or not anim.is_animation(page):
            self.play_button.setChecked(False)
            return
        count = anim.frames_of(page)
        nxt = self.frame + 1
        if nxt > count:
            if not (anim.spec(page) or {}).get("loop", True):
                self.play_button.setChecked(False)
                return
            nxt = 1
        self.frame = nxt
        self.window.anim_frames[page.id] = nxt
        self.window.canvas.show_frame(self._frame_pixmap(page, nxt))
        self.where.setText(f"{nxt} / {count}")

    def _frame_pixmap(self, page, frame: int):
        """Frames are drawn once while playing (quick enough to keep up after the first time round)."""
        cached = self._played.get(frame)
        if cached is None:
            from genko.render import render_page

            dpi = min(100, self.window.canvas._wanted_dpi())
            image = render_page(anim.at_frame(page, frame), dpi, mode="proof", episode=self.window.episode)
            cached = (dpi, self.window._pixmap_of(image))
            self._played[frame] = cached
        return cached

    def _export(self) -> None:
        page = self.page()
        if page is None:
            return
        base = (self.window.path.parent if self.window.path else Path.home()) / f"p{page.index:03d}_animation.gif"
        path, chosen = QFileDialog.getSaveFileName(self, "アニメーションを書き出す", str(base),
                                                   "GIF (*.gif);;WebP (*.webp);;PNG（APNG） (*.png);;MP4 (*.mp4);;連番 PNG（フォルダー） (*)")
        if not path:
            return
        fmt = "frames" if "連番" in chosen else Path(path).suffix.lstrip(".") or "gif"
        self.setCursor(Qt.CursorShape.WaitCursor)
        try:
            anim.export(page, Path(path), episode=self.window.episode, fmt=fmt)
        except ValueError as exc:
            from genko.app import wording

            QMessageBox.warning(self, "Genko", wording.error(str(exc)))
            return
        finally:
            self.unsetCursor()
        self.window.flash(f"アニメーションを書き出しました: {path}", 5000)
