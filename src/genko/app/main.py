from __future__ import annotations

import sys
from pathlib import Path

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
from genko.export import export_png_sequence
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
        add_page = QPushButton("ページ追加")
        add_page.clicked.connect(self._add_page)
        del_page = QPushButton("ページ削除")
        del_page.clicked.connect(self._del_page)

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
        right_layout.addWidget(add_page)
        right_layout.addWidget(del_page)
        right_layout.addWidget(self.status)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.addWidget(QLabel("ページ管理"))
        left_layout.addWidget(self.pages)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.canvas)
        split.addWidget(right)
        split.setStretchFactor(1, 1)
        self.setCentralWidget(split)

        self._build_menu()
        self._reload_pages()
        self.pages.setCurrentRow(0)

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
        self.canvas.set_page(page)
        self._refresh_story()
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
        self._apply([{"op": "add_stroke", "page": page.index, "layer": layer, "points": points}])

    def _on_frame_selected(self, frame_id: str) -> None:
        page = self._current()
        if page is None:
            return
        page.selected_frame_id = frame_id
        self.canvas.update()
        self._refresh_status()

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
        files = export_png_sequence(self.episode, Path(path), working_dpi=150, mode="print")
        QMessageBox.information(self, "Genko", f"{len(files)} pages exported")


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
