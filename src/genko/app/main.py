from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QTimer
from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSplitter,
    QTextEdit,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from genko.app.canvas import PageCanvas
from genko.export import export_print
from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.resize(1280, 840)
        self.episode = new_episode("無題", 1, 8, PageSpec.a4_mono())
        self.path: Path | None = None
        self._page_index = 0

        self.pages = QListWidget()
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.changed.connect(self._refresh_status)
        self.canvas.strokeCommitted.connect(self._on_stroke)
        self.canvas.frameSelected.connect(self._on_frame_selected)
        self.canvas.textMoved.connect(self._on_text_moved)
        self.layers = QListWidget()
        self.layers.itemClicked.connect(self._toggle_layer)
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
        blur.clicked.connect(lambda: self._filter("blur"))

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
        left_layout.addWidget(QLabel("助手チケット"))
        left_layout.addWidget(self.tickets)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.canvas)
        split.addWidget(right)
        split.setStretchFactor(1, 1)
        self.setCentralWidget(split)

        self._build_menu()
        self._reload_pages()
        self.pages.setCurrentRow(0)
        self._autosave = QTimer(self)
        self._autosave.setInterval(60_000)
        self._autosave.timeout.connect(self._maybe_autosave)
        self._autosave.start()

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

    def _apply(self, ops: list[dict]) -> bool:
        try:
            apply_ops(self.episode, ops)
        except ApplyError as exc:
            QMessageBox.warning(self, "Genko", str(exc))
            return False
        self._reload_pages()
        return True

    def _current(self):
        if not self.episode.pages:
            return None
        self._page_index = min(self._page_index, len(self.episode.pages) - 1)
        return self.episode.pages[self._page_index]

    def _reload_pages(self) -> None:
        self.pages.blockSignals(True)
        self.pages.clear()
        for page in self.episode.pages:
            mark = "✓" if page.name_ok else "·"
            self.pages.addItem(f"p{page.index:02d}  {mark}  {page.stage}")
        self.pages.blockSignals(False)
        self.pages.setCurrentRow(self._page_index)
        self._show_page()

    def _select_page(self, row: int) -> None:
        if row < 0:
            return
        self._page_index = row
        self._show_page()

    def _show_page(self) -> None:
        page = self._current()
        lines = self.episode.story_for_page(page.index) if page else []
        self.canvas.set_page(page, lines)
        self._refresh_story()
        self._refresh_layers()
        self._refresh_tickets()
        self._refresh_status()

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
            self.layers.addItem(f"{mark} {label} {layer.blend}")

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
            self.tickets.addItem(f"{ticket.get('status')} p{ticket.get('page_index')} {ticket.get('role')} {ticket.get('assignee')}")

    def _refresh_status(self) -> None:
        page = self._current()
        if page is None:
            self.status.setText("")
            return
        self.status.setText(
            f"{self.episode.title}  EP{self.episode.episode}  "
            f"p{page.index}  stage={page.stage}  name_ok={page.name_ok}  "
            f"frames={len(page.leaf_frames())}  sel={page.selected_frame_id or '-'}"
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
        self._apply([{"op": "set_onion", "page": page.index, "from": page.index - 1}])

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
        self._apply([{"op": "filter_raster", "page": page.index, "layer": layer, "kind": kind, "radius": 2}])

    def _on_frame_selected(self, frame_id: str) -> None:
        page = self._current()
        if page is None:
            return
        page.selected_frame_id = frame_id
        self.canvas.update()
        self._refresh_status()

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
        self._apply([{"op": "undo"}])

    def _new(self) -> None:
        self.episode = new_episode("無題", 1, 8, PageSpec.a4_mono())
        self.path = None
        self._page_index = 0
        self._reload_pages()

    def _open(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Open .genko folder")
        if not path:
            return
        self.episode = load_episode(Path(path))
        self.path = Path(path)
        self._page_index = 0
        self._reload_pages()

    def _save(self) -> None:
        if self.path is None:
            path, _ = QFileDialog.getSaveFileName(self, "Save .genko folder", "untitled.genko")
            if not path:
                return
            self.path = Path(path)
        save_episode(self.episode, self.path)
        self.status.setText(f"saved {self.path}")

    def _export(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Export PNG sequence")
        if not path:
            return
        files = export_print(self.episode, Path(path), fmt="png", dpi=150)
        QMessageBox.information(self, "Genko", f"{len(files)} pages exported")

    def _maybe_autosave(self) -> None:
        if self.path is None or not self.episode.autosave:
            return
        save_episode(self.episode, self.path)


def run_app() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("Genko Studio")
    window = MainWindow()
    window.show()
    return app.exec()


def main() -> None:
    raise SystemExit(run_app())


if __name__ == "__main__":
    main()
