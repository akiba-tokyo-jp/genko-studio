from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QTimer, Qt
from PySide6.QtGui import QAction, QColor, QImage, QKeySequence, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDockWidget,
    QFileDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSlider,
    QSplitter,
    QTextEdit,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from genko.app.canvas import PageCanvas
from genko.app.session import Session
from genko.app.studio_widgets import ApprovalBox, Library, PanelView, ProcessBar
from genko.export import export_print
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError

COMMIT_AFTER_MS = 1000  # changes reach the disk after a second without edits


class MainWindow(QMainWindow):
    def __init__(self, path: Path | None = None, actor: str | None = None) -> None:
        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.resize(1440, 900)
        self.session = Session.open(path, actor) if path else Session(new_episode("無題", 1, 8, PageSpec.a4_mono()), actor=actor)
        self._page_index = 0
        self._commit_timer = QTimer(self)
        self._commit_timer.setSingleShot(True)
        self._commit_timer.setInterval(COMMIT_AFTER_MS)
        self._commit_timer.timeout.connect(self.commit_now)
        self._watcher = QFileSystemWatcher(self)
        self._watcher.fileChanged.connect(self._on_disk_change)

        self.pages = QListWidget()
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.changed.connect(self._refresh_status)
        self.canvas.strokeCommitted.connect(self._on_stroke)
        self.canvas.frameSelected.connect(self._on_frame_selected)
        self.canvas.textMoved.connect(self._on_text_moved)
        self.layers = QListWidget()
        self.layers.itemClicked.connect(self._toggle_layer)
        self.blend = QComboBox()
        self.blend.addItems(["normal", "multiply", "screen", "add"])
        self.blend.currentTextChanged.connect(self._set_blend)
        self.clip = QCheckBox("下でクリップ")
        self.clip.toggled.connect(self._set_clip)
        self.hue = QSlider(Qt.Orientation.Horizontal)
        self.hue.setRange(0, 359)
        self.hue.sliderReleased.connect(self._hue_brush)
        self.filter_kind = QComboBox()
        self.filter_kind.addItems(["blur", "sharpen", "hue", "levels", "mosaic"])
        self.subview = QLabel()
        self.subview.setFixedHeight(160)
        self.tickets = QListWidget()
        self.speaker = QLineEdit()
        self.speaker.setPlaceholderText("話者")
        self.line = QLineEdit()
        self.line.setPlaceholderText("セリフ")
        self.story = QTextEdit()
        self.story.setReadOnly(True)
        self.status = QLabel()

        add_line = QPushButton("セリフ追加")
        add_line.clicked.connect(self._add_line)
        name_ok = QPushButton("ネームOK → ペン入れ")
        name_ok.clicked.connect(self._name_ok)
        split_h = QPushButton("選択コマを横に割る")
        split_h.clicked.connect(lambda: self._split("horizontal"))
        split_v = QPushButton("選択コマを縦に割る")
        split_v.clicked.connect(lambda: self._split("vertical"))
        merge = QPushButton("選択コマを結合")
        merge.clicked.connect(self._merge)
        add_page = QPushButton("ページ追加")
        add_page.clicked.connect(self._add_page)
        del_page = QPushButton("ページ削除")
        del_page.clicked.connect(self._del_page)
        pen = QPushButton("ペン")
        pen.clicked.connect(lambda: self.canvas.set_tool("pen"))
        eraser = QPushButton("消しゴム")
        eraser.clicked.connect(lambda: self.canvas.set_tool("eraser"))
        onion = QPushButton("オニオンスキン")
        onion.clicked.connect(self._onion)
        add_layer = QPushButton("レイヤー追加")
        add_layer.clicked.connect(self._add_layer)
        blur = QPushButton("ぼかし")
        blur.clicked.connect(lambda: self._filter(self.filter_kind.currentText()))

        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.addWidget(QLabel("ストーリーエディター"))
        right_layout.addWidget(self.speaker)
        right_layout.addWidget(self.line)
        right_layout.addWidget(add_line)
        right_layout.addWidget(self.story, 1)
        right_layout.addWidget(name_ok)
        right_layout.addWidget(split_h)
        right_layout.addWidget(split_v)
        right_layout.addWidget(merge)
        right_layout.addWidget(add_page)
        right_layout.addWidget(del_page)
        right_layout.addWidget(pen)
        right_layout.addWidget(eraser)
        right_layout.addWidget(onion)
        right_layout.addWidget(add_layer)
        right_layout.addWidget(blur)
        right_layout.addWidget(self.status)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.addWidget(QLabel("ページ管理"))
        left_layout.addWidget(self.pages)
        left_layout.addWidget(QLabel("レイヤー"))
        left_layout.addWidget(self.layers)
        left_layout.addWidget(self.blend)
        left_layout.addWidget(self.clip)
        left_layout.addWidget(QLabel("色相"))
        left_layout.addWidget(self.hue)
        left_layout.addWidget(self.filter_kind)
        left_layout.addWidget(QLabel("サブビュー"))
        left_layout.addWidget(self.subview)
        left_layout.addWidget(QLabel("助手チケット"))
        left_layout.addWidget(self.tickets)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.canvas)
        split.addWidget(right)
        split.setStretchFactor(1, 1)
        self.setCentralWidget(split)

        self._build_menu()
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
            QMessageBox.warning(self, "Genko", str(exc))
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
            lines = [f"・{c['ops'][0].get('op')}: {c['error']}" for c in result.conflicts[:8]]
            QMessageBox.information(self, "Genko", "エージェントの変更と重なったため、次の操作は入らなかった:\n" + "\n".join(lines))
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
            QMessageBox.information(self, "Genko", f"エージェントの変更と重なった操作が {len(result.conflicts)} 件あった")
        self._reload_pages()

    def closeEvent(self, event) -> None:  # noqa: N802
        self.commit_now()
        super().closeEvent(event)

    # --- studio panels ------------------------------------------------------------------------

    def _build_studio(self) -> None:
        self.process = ProcessBar()
        bar = QToolBar("工程")
        bar.addWidget(self.process)
        self.addToolBarBreak()
        self.addToolBar(bar)
        self.approvals = ApprovalBox(self)
        self.panel_view = PanelView(self)
        self.library = Library(self)
        for widget in (self.approvals, self.panel_view):
            widget.changed.connect(self._reload_pages)
        docks = []
        for title, widget in (("承認箱", self.approvals), ("コマ", self.panel_view), ("ライブラリ", self.library)):
            dock = QDockWidget(title, self)
            dock.setWidget(widget)
            dock.setObjectName(title)
            self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
            docks.append(dock)
        self.tabifyDockWidget(docks[0], docks[1])
        self.tabifyDockWidget(docks[1], docks[2])
        docks[0].raise_()
        self.studio_docks = docks

    def _refresh_studio(self) -> None:
        self.process.refresh(self.episode)
        self.approvals.refresh()
        page = self._current()
        if self.panel_view.frame_id and (page is None or not self._has_frame(page, self.panel_view.frame_id)):
            self.panel_view.frame_id = None
        if self.panel_view.frame_id is None and page is not None and page.selected_frame_id and self._has_frame(page, page.selected_frame_id):
            self.panel_view.frame_id = page.selected_frame_id
        self.panel_view.refresh()
        self.library.refresh()

    @staticmethod
    def _has_frame(page, frame_id: str) -> bool:
        try:
            page._find(frame_id)
            return True
        except (KeyError, IndexError):
            return False

    def _build_menu(self) -> None:
        bar = QToolBar()
        self.addToolBar(bar)
        actions = [
            ("新規", QKeySequence.StandardKey.New, self._new),
            ("開く", QKeySequence.StandardKey.Open, self._open),
            ("保存", QKeySequence.StandardKey.Save, self._save),
            ("書き出し", QKeySequence.StandardKey.SaveAs, self._export),
            ("元に戻す", QKeySequence.StandardKey.Undo, self._undo),
        ]
        for title, shortcut, slot in actions:
            action = QAction(title, self)
            action.setShortcut(shortcut)
            action.triggered.connect(slot)
            bar.addAction(action)
        extras = [
            ("ペン", QKeySequence("B"), lambda: self.canvas.set_tool("pen")),
            ("消しゴム", QKeySequence("E"), lambda: self.canvas.set_tool("eraser")),
            ("色", QKeySequence("C"), self._pick_color),
            ("太+", QKeySequence("]"), lambda: self._nudge_brush(0.15)),
            ("太-", QKeySequence("["), lambda: self._nudge_brush(-0.15)),
            ("前頁", QKeySequence(QKeySequence.StandardKey.MoveToPreviousPage), lambda: self._jump(-1)),
            ("次頁", QKeySequence(QKeySequence.StandardKey.MoveToNextPage), lambda: self._jump(1)),
        ]
        for title, shortcut, slot in extras:
            action = QAction(title, self)
            action.setShortcut(shortcut)
            action.triggered.connect(slot)
            bar.addAction(action)

    def _apply(self, ops: list[dict]) -> bool:
        return self.apply_ops(ops)

    def _current(self):
        if not self.episode.pages:
            return None
        self._page_index = min(self._page_index, len(self.episode.pages) - 1)
        return self.episode.pages[self._page_index]

    def _reload_pages(self) -> None:
        self.pages.blockSignals(True)
        self.pages.clear()
        for page in self.episode.pages:
            name = "✓" if page.name_ok else ("…" if page.plan and page.plan.get("name") else "·")
            art = "✓" if page.art_ok else "·"
            self.pages.addItem(f"p{page.index:02d}  ネーム{name} 作画{art}  {page.stage}")
        self.pages.blockSignals(False)
        self.pages.setCurrentRow(self._page_index)
        self._show_page()

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
        self.canvas.set_page(page, lines)
        self.canvas.background = self._page_background(page)
        self.canvas.brush_width_mm = float(self.episode.brush_width_mm)
        self._refresh_story()
        self._refresh_layers()
        self._refresh_tickets()
        self._refresh_status()
        self._refresh_subview()
        if hasattr(self, "process"):
            self._refresh_studio()

    def _page_background(self, page):
        """The page as it prints, under the editing canvas (placed art, tones, finished art)."""
        if page is None or not any(layer.kind.value == "placed" for layer in page.layers):
            return None
        from io import BytesIO

        from genko.render import render_page

        image = render_page(page, 100, mode="proof", episode=self.episode)
        buf = BytesIO()
        image.save(buf, format="PNG")
        return QPixmap.fromImage(QImage.fromData(buf.getvalue()))

    def _refresh_story(self) -> None:
        page = self._current()
        if page is None:
            self.story.clear()
            return
        lines = []
        for line in self.episode.story_for_page(page.index):
            who = f"{line.speaker}: " if line.speaker else ""
            lines.append(who + line.text)
        self.story.setPlainText("\n".join(lines))

    def _refresh_layers(self) -> None:
        self.layers.clear()
        page = self._current()
        if page is None:
            return
        for layer in page.layers:
            mark = "●" if layer.visible else "○"
            label = layer.title or layer.role.value
            indent = "　" if layer.parent_id else ""
            clip = " clip" if layer.clip else ""
            self.layers.addItem(f"{indent}{mark} {label} {layer.blend}{clip}")

    def _toggle_layer(self, _item) -> None:
        page = self._current()
        if page is None:
            return
        row = self.layers.currentRow()
        if row < 0 or row >= len(page.layers):
            return
        layer = page.layers[row]
        self._apply([{"op": "set_layer", "page": page.index, "id": layer.id, "visible": not layer.visible}])

    def _refresh_tickets(self) -> None:
        self.tickets.clear()
        for ticket in self.episode.tickets:
            if ticket.get("status") != "open":
                continue
            what = ticket.get("text") or ticket.get("gate") or ticket.get("role") or ""
            self.tickets.addItem(f"{ticket.get('kind') or ''} p{ticket.get('page_index') or '-'} → {ticket.get('assignee')}: {what}"[:80])

    def _refresh_status(self) -> None:
        page = self._current()
        if page is None:
            self.status.setText("")
            return
        pending = "未保存の変更あり" if self.session.dirty else "保存済み"
        self.status.setText(
            f"{self.episode.title}  EP{self.episode.episode}  "
            f"p{page.index}  stage={page.stage}  name_ok={page.name_ok}  "
            f"frames={len(page.leaf_frames())}  sel={page.selected_frame_id or '-'}  "
            f"r{self.session.base_revision} {pending}  {self.session.actor}"
        )
        self.setWindowTitle(f"Genko Studio — {self.episode.title} #{self.episode.episode}")

    def _on_stroke(self, points: list) -> None:
        page = self._current()
        if page is None:
            return
        layer = "ink" if page.stage == "ink" else "name"
        if self.canvas.tool == "eraser":
            self._apply(
                [
                    {
                        "op": "erase_raster",
                        "page": page.index,
                        "layer": layer,
                        "points": [[p[0], p[1]] for p in points],
                        "width_mm": 3,
                    }
                ]
            )
            return
        self._apply([{"op": "add_stroke", "page": page.index, "layer": layer, "points": points}])

    def _onion(self) -> None:
        page = self._current()
        if page is None or page.index < 2:
            return
        self._apply([{"op": "step_onion", "page": page.index, "delta": -1}])

    def _set_blend(self, mode: str) -> None:
        page = self._current()
        if page is None or not page.layers:
            return
        row = max(0, self.layers.currentRow())
        layer = page.layers[min(row, len(page.layers) - 1)]
        if (layer.blend or "normal") == mode:
            return
        self._apply([{"op": "set_layer", "page": page.index, "id": layer.id, "blend": mode}])

    def _pick_color(self) -> None:
        color = QColorDialog.getColor(QColor(*self.episode.brush_rgb), self)
        if color.isValid():
            self._apply([{"op": "set_brush", "rgb": [color.red(), color.green(), color.blue()]}])

    def _nudge_brush(self, delta: float) -> None:
        width = max(0.15, float(self.episode.brush_width_mm) + delta)
        self.canvas.brush_width_mm = width
        self._apply([{"op": "set_brush", "width_mm": width}])

    def _set_clip(self, on: bool) -> None:
        page = self._current()
        if page is None or not page.layers:
            return
        row = max(0, self.layers.currentRow())
        layer = page.layers[min(row, len(page.layers) - 1)]
        if layer.clip == on:
            return
        self._apply([{"op": "set_layer", "page": page.index, "id": layer.id, "clip": on}])

    def _hue_brush(self) -> None:
        color = QColor.fromHsv(int(self.hue.value()), 220, 220)
        self._apply([{"op": "set_brush", "rgb": [color.red(), color.green(), color.blue()]}])

    def _jump(self, delta: int) -> None:
        nxt = self._page_index + delta
        if 0 <= nxt < len(self.episode.pages):
            self.pages.setCurrentRow(nxt)

    def _refresh_subview(self) -> None:
        page = self._current()
        if page is None:
            self.subview.clear()
            return
        from io import BytesIO

        from genko.render import render_page

        image = render_page(page, 48, mode="name", episode=self.episode)
        buf = BytesIO()
        image.save(buf, format="PNG")
        pix = QPixmap.fromImage(QImage.fromData(buf.getvalue()))
        width = max(80, int(120 * getattr(self.canvas, "_scale", 1.0)))
        self.subview.setPixmap(pix.scaled(width, 150, Qt.AspectRatioMode.KeepAspectRatio))

    def _add_layer(self) -> None:
        page = self._current()
        if page is None:
            return
        self._apply([{"op": "add_layer", "page": page.index, "name": "レイヤー", "blend": "multiply"}])

    def _filter(self, kind: str) -> None:
        page = self._current()
        if page is None:
            return
        layer = "ink" if page.stage == "ink" else "name"
        kind = kind or self.filter_kind.currentText()
        self._apply([{"op": "filter_raster", "page": page.index, "layer": layer, "kind": kind, "radius": 2}])

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

    def _on_text_moved(self, line_id: str, x_mm: float, y_mm: float) -> None:
        self._apply([{"op": "move_line", "id": line_id, "x_mm": x_mm, "y_mm": y_mm}])

    def _add_line(self) -> None:
        page = self._current()
        if page is None or not self.line.text().strip():
            return
        self._apply(
            [
                {
                    "op": "add_line",
                    "page": page.index,
                    "text": self.line.text().strip(),
                    "speaker": self.speaker.text().strip(),
                    "frame_id": page.selected_frame_id,
                }
            ]
        )
        self.line.clear()

    def _name_ok(self) -> None:
        page = self._current()
        if page is None:
            return
        self._apply([{"op": "name_ok", "page": page.index}])

    def _split(self, axis: str) -> None:
        page = self._current()
        if page is None:
            return
        if not page.selected_frame_id:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選んでください")
            return
        self._apply(
            [{"op": "split_frame", "page": page.index, "axis": axis, "frame_id": page.selected_frame_id}]
        )

    def _merge(self) -> None:
        page = self._current()
        if page is None or not page.selected_frame_id:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選んでください")
            return
        self._apply([{"op": "merge_frame", "page": page.index, "frame_id": page.selected_frame_id}])

    def _add_page(self) -> None:
        self._apply([{"op": "add_page"}])
        self._page_index = len(self.episode.pages) - 1
        self._reload_pages()

    def _del_page(self) -> None:
        page = self._current()
        if page is None:
            return
        self._apply([{"op": "delete_page", "page": page.index}])

    def _undo(self) -> None:
        try:
            self.session.undo()
        except ApplyError as exc:
            QMessageBox.information(self, "Genko", str(exc))
        self._watch()
        self._reload_pages()

    def _new(self) -> None:
        self.commit_now()
        self.session = Session(new_episode("無題", 1, 8, PageSpec.a4_mono()), actor=self.session.actor)
        self._page_index = 0
        self._watch()
        self._reload_pages()

    def open_project(self, path: Path) -> None:
        self.commit_now()
        self.session = Session.open(Path(path), self.session.actor)
        remember_project(Path(path))
        self._page_index = 0
        self._watch()
        self._reload_pages()

    def _open(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Open .genko folder")
        if path:
            self.open_project(Path(path))

    def _save(self) -> None:
        if self.path is None:
            path, _ = QFileDialog.getSaveFileName(self, "Save .genko folder", "untitled.genko")
            if not path:
                return
            self.session.save_as(Path(path))
            remember_project(Path(path))
            self._watch()
        else:
            self.commit_now()
        self.status.setText(f"saved {self.path}")

    def _export(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Export PNG sequence")
        if not path:
            return
        files = export_print(self.episode, Path(path), fmt="png", dpi=150)
        QMessageBox.information(self, "Genko", f"{len(files)} pages exported")



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


class StartDialog(QDialog):
    """最近のプロジェクト / 開く / 新規。"""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.chosen: Path | None = None
        self.list = QListWidget()
        for path in recent_projects():
            self.list.addItem(str(path))
        self.list.itemDoubleClicked.connect(lambda item: self._pick(Path(item.text())))
        open_button = QPushButton("開く…")
        open_button.clicked.connect(self._browse)
        new_button = QPushButton("新規")
        new_button.clicked.connect(self.accept)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("最近のプロジェクト"))
        layout.addWidget(self.list)
        layout.addWidget(open_button)
        layout.addWidget(new_button)

    def _pick(self, path: Path) -> None:
        self.chosen = path
        self.accept()

    def _browse(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Open .genko folder")
        if path:
            self._pick(Path(path))


def run_app(path: Path | None = None) -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("Genko Studio")
    if path is None and len(sys.argv) > 1 and Path(sys.argv[1]).is_dir():
        path = Path(sys.argv[1])
    if path is None:
        start = StartDialog()
        if start.exec() != QDialog.DialogCode.Accepted:
            return 0
        path = start.chosen
    window = MainWindow(path)
    if path is not None:
        remember_project(path)
    window.show()
    return app.exec()


def main() -> None:
    raise SystemExit(run_app())


if __name__ == "__main__":
    main()
