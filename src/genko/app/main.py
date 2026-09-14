from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
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
from genko.models import Episode, PageSpec, new_episode
from genko.pipeline import InkBlockedError, advance


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.resize(1280, 840)
        self.episode: Episode = new_episode("無題", 1, 8, PageSpec.a4_mono())
        self.path: Path | None = None
        self._page_index = 0

        self.pages = QListWidget()
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.changed.connect(self._refresh_status)
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
        split_h = QPushButton("横に割る")
        split_h.clicked.connect(lambda: self._split("horizontal"))
        split_v = QPushButton("縦に割る")
        split_v.clicked.connect(lambda: self._split("vertical"))

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
        ]
        for title, shortcut, slot in actions:
            action = QAction(title, self)
            action.setShortcut(shortcut)
            action.triggered.connect(slot)
            bar.addAction(action)

    def _current(self):
        if not self.episode.pages:
            return None
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
            f"frames={len(page.leaf_frames())}"
        )
        self.setWindowTitle(f"Genko Studio — {self.episode.title} #{self.episode.episode}")

    def _add_line(self) -> None:
        page = self._current()
        if page is None or not self.line.text().strip():
            return
        self.episode.add_line(page.index, self.line.text().strip(), self.speaker.text().strip())
        self.line.clear()
        self._refresh_story()

    def _name_ok(self) -> None:
        page = self._current()
        if page is None:
            return
        page.name_ok = True
        try:
            advance(page, to="ink")
        except InkBlockedError as exc:
            QMessageBox.warning(self, "Genko", str(exc))
            return
        self._reload_pages()

    def _split(self, axis: str) -> None:
        page = self._current()
        if page is None:
            return
        leaves = page.leaf_frames()
        if not leaves:
            return
        page.split_frame(leaves[0].id, axis=axis, ratio=0.5, gutter_mm=4)
        self.canvas.update()
        self._refresh_status()

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
            path = QFileDialog.getExistingDirectory(self, "Save .genko folder")
            if not path:
                return
            self.path = Path(path)
        save_episode(self.episode, self.path)
        self.status.setText(f"saved {self.path}")

    def _export(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Export PNG sequence")
        if not path:
            return
        files = export_png_sequence(self.episode, Path(path), working_dpi=150)
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
