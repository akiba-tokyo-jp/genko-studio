from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QPointF, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QColor, QImage, QKeySequence, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDockWidget,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSplitter,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from genko.app import wording
from genko.app.canvas import PageCanvas
from genko.app.dialogs import ExportDialog, NewProjectDialog, StartDialog  # noqa: F401  (StartDialog is re-exported)
from genko.app.session import Session
from genko.app.studio_widgets import ApprovalBox, Library, PanelView, ProcessBar
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError

COMMIT_AFTER_MS = 1000  # changes reach the disk after a second without edits


def _pixmap(image) -> QPixmap:
    from io import BytesIO

    buf = BytesIO()
    image.convert("RGB").save(buf, format="PNG")
    return QPixmap.fromImage(QImage.fromData(buf.getvalue()))


class StoryPanel(QWidget):
    """The page's lines, and adding one to the selected panel."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.list = QListWidget()
        self.speaker = QLineEdit()
        self.speaker.setPlaceholderText("話者（空でもよい）")
        self.line = QLineEdit()
        self.line.setPlaceholderText("台詞")
        self.line.returnPressed.connect(self.add)
        add = QPushButton("選んだコマに台詞を追加")
        add.clicked.connect(self.add)
        hint = QLabel("台詞の位置は、選択ツールで編集画面のフキダシをドラッグして動かします。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#666")
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("このページの台詞（読み順）"))
        layout.addWidget(self.list, 1)
        layout.addWidget(self.speaker)
        layout.addWidget(self.line)
        layout.addWidget(add)
        layout.addWidget(hint)

    def refresh(self) -> None:
        self.list.clear()
        page = self.window.current_page()
        if page is None:
            return
        for line in self.window.episode.story_for_page(page.index):
            who = f"{line.speaker}: " if line.speaker else ""
            self.list.addItem(who + line.text.replace("\n", " "))

    def add(self) -> None:
        page = self.window.current_page()
        text = self.line.text().strip()
        if page is None or not text:
            return
        if not page.selected_frame_id:
            QMessageBox.information(self, "Genko", "先に編集画面でコマをクリックして選びます")
            return
        if self.window.apply_ops([{"op": "add_line", "page": page.index, "text": text, "speaker": self.speaker.text().strip(),
                                   "frame_id": page.selected_frame_id}]):
            self.line.clear()


class LayerPanel(QWidget):
    """Layers of the page: visibility (check), blend, clip, a new layer, filters."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.list = QListWidget()
        self.list.itemChanged.connect(self._visibility)
        self.list.currentRowChanged.connect(lambda _: self._selected())
        self.blend = QComboBox()
        for key, label in wording.BLEND:
            self.blend.addItem(label, key)
        self.blend.activated.connect(lambda _: self._set_blend())
        self.clip = QCheckBox("下のレイヤーでクリップ")
        self.clip.clicked.connect(self._set_clip)
        add = QPushButton("レイヤーを追加")
        add.clicked.connect(self._add)
        self.filter = QComboBox()
        for key, label in wording.FILTERS:
            self.filter.addItem(label, key)
        apply_filter = QPushButton("フィルターをかける…")
        apply_filter.clicked.connect(self._filter)
        row = QHBoxLayout()
        row.addWidget(QLabel("合成"))
        row.addWidget(self.blend, 1)
        frow = QHBoxLayout()
        frow.addWidget(self.filter, 1)
        frow.addWidget(apply_filter)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("チェックで表示・非表示"))
        layout.addWidget(self.list, 1)
        layout.addLayout(row)
        layout.addWidget(self.clip)
        layout.addWidget(add)
        layout.addLayout(frow)
        self._loading = False

    def refresh(self) -> None:
        page = self.window.current_page()
        self._loading = True
        row = self.list.currentRow()
        self.list.clear()
        for layer in (page.layers if page else []):
            item = QListWidgetItem(("　" if layer.parent_id else "") + wording.layer_label(layer))
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked if layer.visible else Qt.CheckState.Unchecked)
            self.list.addItem(item)
        self._loading = False
        if page and page.layers:
            self.list.setCurrentRow(min(max(row, 0), len(page.layers) - 1))
        self._selected()

    def _layer(self):
        page = self.window.current_page()
        row = self.list.currentRow()
        if page is None or not 0 <= row < len(page.layers):
            return page, None
        return page, page.layers[row]

    def _selected(self) -> None:
        _, layer = self._layer()
        if layer is None:
            return
        index = self.blend.findData(layer.blend or "normal")
        self.blend.setCurrentIndex(max(0, index))
        self.clip.setChecked(bool(layer.clip))

    def _visibility(self, item: QListWidgetItem) -> None:
        if self._loading:
            return
        page = self.window.current_page()
        row = self.list.row(item)
        if page is None or not 0 <= row < len(page.layers):
            return
        visible = item.checkState() == Qt.CheckState.Checked
        if page.layers[row].visible != visible:
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": page.layers[row].id, "visible": visible}])

    def _set_blend(self) -> None:
        page, layer = self._layer()
        if layer is not None and (layer.blend or "normal") != self.blend.currentData():
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": layer.id, "blend": self.blend.currentData()}])

    def _set_clip(self, on: bool) -> None:
        page, layer = self._layer()
        if layer is not None and layer.clip != on:
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": layer.id, "clip": on}])

    def _add(self) -> None:
        page = self.window.current_page()
        if page is not None:
            self.window.apply_ops([{"op": "add_layer", "page": page.index, "name": "レイヤー", "blend": "multiply"}])

    def _filter(self) -> None:
        page = self.window.current_page()
        if page is None:
            return
        target = "ネーム" if page.stage == "name" else "ペン入れ"
        label = self.filter.currentText()
        if QMessageBox.question(self, "Genko", f"{page.index} ページの「{target}」レイヤー全体に「{label}」をかけます。\n"
                                "（元に戻す で取り消せます）") != QMessageBox.StandardButton.Yes:
            return
        layer = "name" if page.stage == "name" else "ink"
        self.window.apply_ops([{"op": "filter_raster", "page": page.index, "layer": layer, "kind": self.filter.currentData(), "radius": 2}])


class MainWindow(QMainWindow):
    def __init__(self, path: Path | None = None, actor: str | None = None) -> None:
        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.resize(1280, 800)
        self.session = Session.open(path, actor) if path else Session(new_episode("無題", 1, 8, PageSpec.a4_mono()), actor=actor)
        self._page_index = 0
        self._commit_timer = QTimer(self)
        self._commit_timer.setSingleShot(True)
        self._commit_timer.setInterval(COMMIT_AFTER_MS)
        self._commit_timer.timeout.connect(self.commit_now)
        self._watcher = QFileSystemWatcher(self)
        self._watcher.fileChanged.connect(self._on_disk_change)

        self.pages = QListWidget()
        self.pages.setMinimumWidth(110)
        self.pages.setMaximumWidth(220)
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.renderer = self._render_current
        self.canvas.changed.connect(self._refresh_status)
        self.canvas.strokeCommitted.connect(self._on_stroke)
        self.canvas.frameSelected.connect(self._on_frame_selected)
        self.canvas.textMoved.connect(self._on_text_moved)
        self.canvas.contextMenuAt.connect(self._context_menu)
        self.canvas.zoomChanged.connect(lambda _: self._refresh_zoom())

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(4, 4, 4, 4)
        left_layout.addWidget(QLabel("ページ"))
        left_layout.addWidget(self.pages, 1)
        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.canvas)
        split.setStretchFactor(1, 1)
        split.setChildrenCollapsible(False)
        self.setCentralWidget(split)

        self.status = QLabel()
        self.zoom_label = QLabel()
        self.status.setMinimumWidth(10)
        self.statusBar().addWidget(self.status, 1)

        self._build_actions()
        self._build_studio()
        self._watch()
        self._reload_pages()
        self.pages.setCurrentRow(0)

    # --- the session ------------------------------------------------------------------------

    @property
    def episode(self):
        return self.session.episode

    @property
    def path(self) -> Path | None:
        return self.session.path

    def current_page(self):
        return self._current()

    def apply_ops(self, ops: list[dict]) -> bool:
        """Apply in memory as the person, show it at once, and write it after a short pause."""
        try:
            self.session.apply(ops)
        except ApplyError as exc:
            QMessageBox.warning(self, "Genko", wording.error(str(exc)))
            return False
        if self.session.path is not None:
            self._commit_timer.start()
        self._reload_pages()
        return True

    def apply_and_commit(self, ops: list[dict]) -> bool:
        """For approvals: write at once, so the agent sees the decision immediately."""
        if not self.apply_ops(ops):
            return False
        self.commit_now()
        return True

    def commit_now(self) -> None:
        self._commit_timer.stop()
        if self.session.path is None or not (self.session.dirty or self.session.outside_change()):
            return
        result = self.session.commit()
        if result.conflicts:
            lines = [f"・{wording.error(c['error'])}" for c in result.conflicts[:8]]
            QMessageBox.information(self, "Genko", "エージェントの変更と重なったため、次の操作は入りませんでした:\n" + "\n".join(lines))
        self._watch()
        self._reload_pages()

    def _watch(self) -> None:
        if self._watcher.files():
            self._watcher.removePaths(self._watcher.files())
        if self.session.path is not None and (self.session.path / "project.json").exists():
            self._watcher.addPath(str(self.session.path / "project.json"))

    def _on_disk_change(self, _path: str) -> None:
        # project.json is replaced atomically, so the watch has to be set again
        self._watch()
        if not self.session.outside_change():
            return
        result = self.session.sync()
        if result.conflicts:
            QMessageBox.information(self, "Genko", f"エージェントの変更と重なった操作が {len(result.conflicts)} 件あり、入りませんでした")
        self._reload_pages()

    def closeEvent(self, event) -> None:  # noqa: N802
        self.commit_now()
        super().closeEvent(event)

    # --- actions, menus and toolbars ------------------------------------------------------------

    def _action(self, title: str, slot, shortcut=None, tip: str = "", checkable: bool = False) -> QAction:
        action = QAction(title, self)
        if shortcut is not None:
            keys = shortcut if isinstance(shortcut, list) else [shortcut]
            action.setShortcuts([QKeySequence(k) if isinstance(k, str) else QKeySequence(k) for k in keys])
        if tip:
            action.setStatusTip(tip)
            action.setToolTip(f"{title}  {action.shortcut().toString(QKeySequence.SequenceFormat.NativeText)}\n{tip}".strip())
        action.setCheckable(checkable)
        action.triggered.connect(slot)
        return action

    def _build_actions(self) -> None:
        std = QKeySequence.StandardKey
        a = self._action
        self.act_new = a("新しい原稿…", self._new, std.New)
        self.act_open = a("開く…", self._open, std.Open)
        self.act_save = a("保存", self._save, std.Save, "変更は自動で保存されます。今すぐ書き込むときに使います")
        self.act_save_as = a("別の場所に保存…", self._save_as, std.SaveAs)
        self.act_export = a("書き出し…", self._export, "Ctrl+E", "PDF・TIFF・PSD・縦読み・SNS 用などに書き出します")
        self.act_undo = a("元に戻す", self._undo, std.Undo)
        self.act_redo = a("やり直す", self._redo, [QKeySequence(std.Redo), QKeySequence("Ctrl+Y")])
        self.act_fit = a("全体を表示", self.canvas.fit_page, "Ctrl+0")
        self.act_zoom_in = a("拡大", lambda: self.canvas.zoom_by(1.25), [QKeySequence(std.ZoomIn), QKeySequence("Ctrl+=")])
        self.act_zoom_out = a("縮小", lambda: self.canvas.zoom_by(0.8), std.ZoomOut)
        self.act_actual = a("原寸（紙の大きさ）", self.canvas.actual_size, "Ctrl+1")
        self.act_prev = a("◀ 前のページ", lambda: self._jump(-1), [QKeySequence(std.MoveToPreviousPage), QKeySequence("Ctrl+Left")])
        self.act_next = a("次のページ ▶", lambda: self._jump(1), [QKeySequence(std.MoveToNextPage), QKeySequence("Ctrl+Right")])
        self.act_onion = a("前のページを透かす（オニオンスキン）", self._onion)
        self.act_select = a("選択", lambda: self._tool("select"), "V", "コマを選ぶ・フキダシを動かす・ドラッグで表示を動かす", True)
        self.act_pen = a("ペン", lambda: self._tool("pen"), "B", "ネームの段階はネームに、承認後はペン入れに描きます", True)
        self.act_eraser = a("消しゴム", lambda: self._tool("eraser"), "E", "", True)
        tools = QActionGroup(self)
        for act in (self.act_select, self.act_pen, self.act_eraser):
            tools.addAction(act)
        self.act_select.setChecked(True)
        self.act_color = a("ペンの色…", self._pick_color, "C")
        self.act_thicker = a("線を太く", lambda: self._nudge_brush(0.15), "]")
        self.act_thinner = a("線を細く", lambda: self._nudge_brush(-0.15), "[")
        self.act_split_h = a("コマを横に割る（上下に分ける）", lambda: self._split("horizontal"), "Ctrl+Shift+H")
        self.act_split_v = a("コマを縦に割る（左右に分ける）", lambda: self._split("vertical"), "Ctrl+Shift+V")
        self.act_merge = a("コマを結合（割る前に戻す）", self._merge, "Ctrl+Shift+M")
        self.act_add_page = a("ページを追加", self._add_page)
        self.act_del_page = a("このページを削除…", self._del_page)
        self.act_name_ok = a("ネーム完了 → 作画へ進む", self._name_ok, tip="承認の要らない原稿（エージェントを使わない原稿）で使います")

        bar = self.menuBar()
        menus = [
            ("ファイル", [self.act_new, self.act_open, None, self.act_save, self.act_save_as, None, self.act_export]),
            ("編集", [self.act_undo, self.act_redo]),
            ("表示", [self.act_fit, self.act_zoom_in, self.act_zoom_out, self.act_actual, None, self.act_prev, self.act_next,
                      None, self.act_onion]),
            ("ツール", [self.act_select, self.act_pen, self.act_eraser, None, self.act_color, self.act_thicker, self.act_thinner]),
            ("コマ", [self.act_split_h, self.act_split_v, self.act_merge]),
            ("ページ", [self.act_add_page, self.act_del_page, None, self.act_name_ok]),
        ]
        for title, actions in menus:
            menu = bar.addMenu(title)
            for act in actions:
                if act is None:
                    menu.addSeparator()
                else:
                    menu.addAction(act)
        self.view_menu = bar.addMenu("パネル")

        tools_bar = QToolBar("道具")
        tools_bar.setObjectName("tools")
        tools_bar.setMovable(False)
        tools_bar.setIconSize(QSize(16, 16))
        for act in (self.act_select, self.act_pen, self.act_eraser, None, self.act_undo, self.act_redo, None,
                    self.act_fit, self.act_zoom_out, self.act_zoom_in, None, self.act_prev, self.act_next, None, self.act_export):
            if act is None:
                tools_bar.addSeparator()
            else:
                tools_bar.addAction(act)
        self.addToolBar(tools_bar)

    def _build_studio(self) -> None:
        self.process = ProcessBar()
        self.statusBar().addPermanentWidget(self.process)
        self.statusBar().addPermanentWidget(self.zoom_label)
        self.approvals = ApprovalBox(self)
        self.panel_view = PanelView(self)
        self.story = StoryPanel(self)
        self.layers = LayerPanel(self)
        self.library = Library(self)
        for widget in (self.approvals, self.panel_view):
            widget.changed.connect(self._reload_pages)
        docks = []
        for title, widget in (("承認箱", self.approvals), ("コマ", self.panel_view), ("台詞", self.story),
                              ("レイヤー", self.layers), ("ライブラリ", self.library)):
            dock = QDockWidget(title, self)
            if widget is self.panel_view:
                # the panel view is tall; on a small screen it scrolls instead of making the window taller
                scroll = QScrollArea()
                scroll.setWidgetResizable(True)
                scroll.setFrameShape(QScrollArea.Shape.NoFrame)
                widget.setMinimumHeight(560)
                scroll.setWidget(widget)
                dock.setWidget(scroll)
            else:
                dock.setWidget(widget)
            dock.setObjectName(title)
            dock.setMinimumWidth(300)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable)
            self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
            self.view_menu.addAction(dock.toggleViewAction())
            docks.append(dock)
        for first, second in zip(docks, docks[1:]):
            self.tabifyDockWidget(first, second)
        docks[0].raise_()
        self.resizeDocks([docks[0]], [380], Qt.Orientation.Horizontal)
        self.studio_docks = docks

    def show_dock(self, title: str) -> None:
        for dock in self.studio_docks:
            if dock.windowTitle() == title:
                dock.show()
                dock.raise_()

    def _refresh_studio(self) -> None:
        self.process.refresh(self.episode)
        self.approvals.refresh()
        page = self._current()
        if self.panel_view.frame_id and (page is None or not self._has_frame(page, self.panel_view.frame_id)):
            self.panel_view.frame_id = None
        if self.panel_view.frame_id is None and page is not None and page.selected_frame_id and self._has_frame(page, page.selected_frame_id):
            self.panel_view.frame_id = page.selected_frame_id
        self.panel_view.refresh()
        self.story.refresh()
        self.layers.refresh()
        self.library.refresh()

    @staticmethod
    def _has_frame(page, frame_id: str) -> bool:
        try:
            page._find(frame_id)
            return True
        except (KeyError, IndexError):
            return False

    # --- pages ----------------------------------------------------------------------------------

    def _current(self):
        if not self.episode.pages:
            return None
        self._page_index = min(self._page_index, len(self.episode.pages) - 1)
        return self.episode.pages[self._page_index]

    @staticmethod
    def _page_text(page) -> str:
        if page.name_ok:
            name = "ネーム ✓"
        elif page.plan and page.plan.get("name"):
            name = "ネーム 確認待ち"
        else:
            name = "ネーム"
        art = " / 作画 ✓" if page.art_ok else (" / 作画中" if page.name_ok else "")
        done = " / 仕上げ ✓" if page.stage == "finish" else ""
        return f"{page.index} ページ\n{name}{art}{done}"

    def _reload_pages(self) -> None:
        self.pages.blockSignals(True)
        self.pages.clear()
        for page in self.episode.pages:
            self.pages.addItem(self._page_text(page))
        self.pages.blockSignals(False)
        self.pages.setCurrentRow(self._page_index)
        self._show_page()

    def go_to_page(self, index: int) -> None:
        row = next((i for i, p in enumerate(self.episode.pages) if p.index == index), None)
        if row is not None and row != self._page_index:
            self.pages.setCurrentRow(row)

    def _select_page(self, row: int) -> None:
        if row < 0:
            return
        if row != self._page_index:
            self.commit_now()  # a page switch writes what was done on the last page
            self.panel_view.frame_id = None
        self._page_index = row
        self._show_page()

    def _show_page(self) -> None:
        page = self._current()
        lines = self.episode.story_for_page(page.index) if page else []
        self.canvas.overlay_name_strokes = bool(page and page.name_ok and page.stage != "name")
        self.canvas.brush_width_mm = float(self.episode.brush_width_mm)
        self.canvas.set_page(page, lines)
        self._refresh_status()
        if hasattr(self, "process"):
            self._refresh_studio()

    def _render_current(self, dpi: int) -> QPixmap | None:
        page = self._current()
        if page is None:
            return None
        from genko.render import render_page

        mode = "name" if not page.name_ok else "proof"
        try:
            return _pixmap(render_page(page, dpi, mode=mode, episode=self.episode))
        except Exception:  # a broken asset must not take the editor down
            return None

    def _refresh_status(self) -> None:
        page = self._current()
        saved = "保存待ち…" if self.session.dirty else ("保存済み" if self.session.path else "未保存（ファイル → 別の場所に保存）")
        self.setWindowTitle(f"{self.episode.title} 第{self.episode.episode}話 — Genko Studio")
        if page is None:
            self.status.setText(saved)
            return
        selected = ""
        if page.selected_frame_id and self._has_frame(page, page.selected_frame_id):
            frames = page.leaf_frames()
            order = next((i + 1 for i, f in enumerate(frames) if f.id == page.selected_frame_id), None)
            selected = f" ・ 選択中: {order} コマ目" if order else ""
        self.status.setText(f"{page.index} ページ（{wording.STAGE.get(page.stage, page.stage)}） ・ コマ {len(page.leaf_frames())}"
                            f"{selected} ・ {saved} ・ {wording.actor(self.session.actor)}")
        self._refresh_zoom()

    def _refresh_zoom(self) -> None:
        self.zoom_label.setText(f"表示 {self.canvas.zoom_percent()}%")

    # --- editing -----------------------------------------------------------------------------

    def _tool(self, tool: str) -> None:
        self.canvas.set_tool(tool)
        {"select": self.act_select, "pen": self.act_pen, "eraser": self.act_eraser}[tool].setChecked(True)

    def _on_stroke(self, points: list) -> None:
        page = self._current()
        if page is None:
            return
        layer = "name" if page.stage == "name" else "ink"
        if self.canvas.tool == "eraser":
            self.apply_ops([{"op": "erase_raster", "page": page.index, "layer": layer,
                             "points": [[p[0], p[1]] for p in points], "width_mm": 3}])
            return
        self.apply_ops([{"op": "add_stroke", "page": page.index, "layer": layer, "points": points}])

    def _onion(self) -> None:
        page = self._current()
        if page is None or page.index < 2:
            return
        self.apply_ops([{"op": "step_onion", "page": page.index, "delta": -1}])

    def _pick_color(self) -> None:
        color = QColorDialog.getColor(QColor(*self.episode.brush_rgb), self, "ペンの色")
        if color.isValid():
            self.apply_ops([{"op": "set_brush", "rgb": [color.red(), color.green(), color.blue()]}])

    def _nudge_brush(self, delta: float) -> None:
        width = max(0.15, float(self.episode.brush_width_mm) + delta)
        self.canvas.brush_width_mm = width
        self.apply_ops([{"op": "set_brush", "width_mm": width}])
        self.statusBar().showMessage(f"線の太さ {width:.2f} mm", 2000)

    def _jump(self, delta: int) -> None:
        nxt = self._page_index + delta
        if 0 <= nxt < len(self.episode.pages):
            self.pages.setCurrentRow(nxt)

    def _on_frame_selected(self, frame_id: str) -> None:
        page = self._current()
        if page is None:
            return
        # selection goes through an op like every other change (no direct edits of the model)
        self.panel_view.frame_id = frame_id
        if page.selected_frame_id != frame_id:
            self.apply_ops([{"op": "select_frame", "page": page.index, "frame_id": frame_id}])
        else:
            self.panel_view.refresh()

    def _context_menu(self, frame_id: str, pos: QPointF) -> None:
        menu = QMenu(self)
        if frame_id:
            menu.addAction(self.act_split_h)
            menu.addAction(self.act_split_v)
            menu.addAction(self.act_merge)
            menu.addSeparator()
            show = menu.addAction("このコマの絵を見る（コマ パネル）")
            show.triggered.connect(lambda: self.show_dock("コマ"))
            menu.addSeparator()
        menu.addAction(self.act_fit)
        menu.exec(pos.toPoint())

    def _on_text_moved(self, line_id: str, x_mm: float, y_mm: float) -> None:
        self.apply_ops([{"op": "move_line", "id": line_id, "x_mm": x_mm, "y_mm": y_mm}])

    def _name_ok(self) -> None:
        page = self._current()
        if page is None:
            return
        self.apply_ops([{"op": "name_ok", "page": page.index}])

    def _split(self, axis: str) -> None:
        page = self._current()
        if page is None:
            return
        if not page.selected_frame_id:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選びます（選択ツール）")
            return
        self.apply_ops([{"op": "split_frame", "page": page.index, "axis": axis, "frame_id": page.selected_frame_id}])

    def _merge(self) -> None:
        page = self._current()
        if page is None or not page.selected_frame_id:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選びます（選択ツール）")
            return
        self.apply_ops([{"op": "merge_frame", "page": page.index, "frame_id": page.selected_frame_id}])

    def _add_page(self) -> None:
        self.apply_ops([{"op": "add_page"}])
        self._page_index = len(self.episode.pages) - 1
        self._reload_pages()

    def _del_page(self) -> None:
        page = self._current()
        if page is None:
            return
        extra = "\nこのページのネームは承認済みです。" if page.name_ok else ""
        answer = QMessageBox.question(self, "Genko", f"{page.index} ページを削除しますか？{extra}\n（元に戻す で取り消せます）")
        if answer == QMessageBox.StandardButton.Yes:
            self.apply_ops([{"op": "delete_page", "page": page.index}])

    def _undo(self) -> None:
        try:
            self.session.undo()
        except ApplyError as exc:
            self.statusBar().showMessage(wording.error(str(exc)), 3000)
        self._watch()
        self._reload_pages()

    def _redo(self) -> None:
        try:
            self.session.redo()
        except ApplyError as exc:
            self.statusBar().showMessage(wording.error(str(exc)), 3000)
        self._watch()
        self._reload_pages()

    # --- files ---------------------------------------------------------------------------------------

    def _new(self) -> None:
        dialog = NewProjectDialog(self)
        if dialog.exec() == QDialog.DialogCode.Accepted and dialog.created:
            self.open_project(dialog.created)

    def open_project(self, path: Path) -> None:
        self.commit_now()
        self.session = Session.open(Path(path), self.session.actor)
        remember_project(Path(path))
        self._page_index = 0
        self.panel_view.frame_id = None
        self._watch()
        self._reload_pages()
        self.canvas.fit_page()

    def _open(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "原稿（.genko のフォルダ）を開く")
        if path:
            if not (Path(path) / "project.json").is_file():
                QMessageBox.warning(self, "Genko", "Genko の原稿ではありません（.genko のフォルダを選びます）")
                return
            self.open_project(Path(path))

    def _save(self) -> None:
        if self.path is None:
            self._save_as()
            return
        self.commit_now()
        self.statusBar().showMessage(f"保存しました: {self.path}", 3000)

    def _save_as(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "原稿を保存する場所", f"{self.episode.title or '無題'}.genko")
        if not path:
            return
        target = Path(path) if path.endswith(".genko") else Path(path + ".genko")
        self.session.save_as(target)
        remember_project(target)
        self._watch()
        self._refresh_status()
        self.statusBar().showMessage(f"保存しました: {target}", 3000)

    def _export(self) -> None:
        self.commit_now()
        ExportDialog(self, self.episode, self.path, self.session.actor).exec()
        if self.session.outside_change():
            self.session.sync()
        self._reload_pages()


# --- recent projects and the start screen -----------------------------------------------------


def _recent_path() -> Path:
    from genko.tokens import config_dir

    return config_dir() / "recent.json"


def recent_projects() -> list[Path]:
    import json

    try:
        items = json.loads(_recent_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return [Path(p) for p in items if (Path(p) / "project.json").is_file()]


def remember_project(path: Path) -> None:
    import json

    items = [str(Path(path).resolve())] + [str(p) for p in recent_projects() if p.resolve() != Path(path).resolve()]
    target = _recent_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(items[:12], ensure_ascii=False, indent=2), encoding="utf-8")


def run_app(path: Path | None = None) -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("Genko Studio")
    if path is None and len(sys.argv) > 1 and Path(sys.argv[1]).is_dir():
        path = Path(sys.argv[1])
    if path is None:
        start = StartDialog()
        if start.exec() != QDialog.DialogCode.Accepted or start.chosen is None:
            return 0
        path = start.chosen
    window = MainWindow(path)
    remember_project(path)
    window.show()
    return app.exec()


def main() -> None:
    raise SystemExit(run_app())


if __name__ == "__main__":
    main()
