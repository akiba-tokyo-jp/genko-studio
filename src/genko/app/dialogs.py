"""Dialogs: start screen, new manuscript, export."""

from __future__ import annotations

import json
from pathlib import Path

from PySide6.QtCore import QUrl, Qt
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from genko.app import exporting
from genko.models import Binding, PageSpec

PAPERS = [
    ("B4 モノクロ（商業誌・投稿の原稿用紙、600 dpi）", "b4"),
    ("A4 モノクロ（同人誌・練習、600 dpi）", "a4"),
    ("縦読み・カラー（Webtoon、幅 80 mm）", "webtoon"),
]


def spec_for(key: str) -> PageSpec:
    return {"b4": PageSpec.b4_comic, "a4": PageSpec.a4_mono, "webtoon": PageSpec.webtoon}[key]()


def project_title(path: Path) -> str:
    try:
        return str(json.loads((Path(path) / "project.json").read_text(encoding="utf-8")).get("title") or Path(path).stem)
    except (OSError, ValueError):
        return Path(path).stem


# --- start screen -----------------------------------------------------------------------------------


class StartDialog(QDialog):
    """最近の原稿 / 開く / 新しく作る."""

    def __init__(self) -> None:
        from genko.app.main import recent_projects

        super().__init__()
        self.setWindowTitle("Genko Studio")
        self.chosen: Path | None = None
        self.resize(560, 420)
        head = QLabel("<h2>Genko Studio</h2>漫画原稿の編集と、エージェントが出した承認依頼の確認をします。")
        head.setWordWrap(True)
        self.list = QListWidget()
        self.list.setWordWrap(True)
        self.list.setSpacing(2)
        for path in recent_projects():
            item = QListWidgetItem(f"{project_title(path)}\n{path}")
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            self.list.addItem(item)
        if self.list.count():
            self.list.setCurrentRow(0)
        self.list.itemActivated.connect(lambda item: self._pick(Path(item.data(Qt.ItemDataRole.UserRole))))
        empty = QLabel("最近開いた原稿はまだありません。")
        empty.setVisible(self.list.count() == 0)
        open_recent = QPushButton("選んだ原稿を開く")
        open_recent.setDefault(True)
        open_recent.setEnabled(self.list.count() > 0)
        open_recent.clicked.connect(lambda: self.list.currentItem() and self._pick(Path(self.list.currentItem().data(Qt.ItemDataRole.UserRole))))
        browse = QPushButton("ほかの原稿を開く…")
        browse.clicked.connect(self._browse)
        new_button = QPushButton("新しい原稿を作る…")
        new_button.clicked.connect(self._new)
        buttons = QHBoxLayout()
        buttons.addWidget(new_button)
        buttons.addWidget(browse)
        buttons.addStretch(1)
        buttons.addWidget(open_recent)
        layout = QVBoxLayout(self)
        layout.addWidget(head)
        layout.addWidget(QLabel("最近の原稿"))
        layout.addWidget(self.list, 1)
        layout.addWidget(empty)
        layout.addLayout(buttons)

    def _pick(self, path: Path) -> None:
        self.chosen = path
        self.accept()

    def _browse(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "原稿（.genko のフォルダ）を開く")
        if path:
            self._pick(Path(path))

    def _new(self) -> None:
        dialog = NewProjectDialog(self)
        if dialog.exec() == QDialog.DialogCode.Accepted and dialog.created:
            self._pick(dialog.created)


# --- new manuscript -------------------------------------------------------------------------------


class NewProjectDialog(QDialog):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("新しい原稿")
        self.setMinimumWidth(520)
        self.created: Path | None = None
        self.title = QLineEdit()
        self.title.setPlaceholderText("例: 夏の午後の約束")
        self.episode = QSpinBox()
        self.episode.setRange(1, 999)
        self.pages = QSpinBox()
        self.pages.setRange(1, 400)
        self.pages.setValue(16)
        self.paper = QComboBox()
        for label, key in PAPERS:
            self.paper.addItem(label, key)
        self.binding = QComboBox()
        self.binding.addItem("右綴じ（縦書きの漫画）", "right")
        self.binding.addItem("左綴じ", "left")
        self.folder = QLineEdit(str(Path.home()))
        pick = QPushButton("選ぶ…")
        pick.clicked.connect(self._pick_folder)
        where = QHBoxLayout()
        where.addWidget(self.folder, 1)
        where.addWidget(pick)
        self.where_note = QLabel()
        self.where_note.setStyleSheet("color:#666")
        self.title.textChanged.connect(self._note)
        self.folder.textChanged.connect(self._note)
        form = QFormLayout()
        form.addRow("作品名", self.title)
        form.addRow("話数", self.episode)
        form.addRow("ページ数", self.pages)
        form.addRow("原稿用紙", self.paper)
        form.addRow("綴じ", self.binding)
        form.addRow("保存する場所", where)
        form.addRow("", self.where_note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("作る")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.create)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
        self._note()

    def target(self) -> Path:
        from genko.export import safe_name

        name = safe_name(self.title.text().strip() or "無題", "manga")
        if self.episode.value() > 1:
            name += f"_{self.episode.value():02d}"
        return Path(self.folder.text()).expanduser() / f"{name}.genko"

    def _note(self) -> None:
        self.where_note.setText(f"作られるフォルダ: {self.target()}")

    def _pick_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "保存する場所", self.folder.text())
        if path:
            self.folder.setText(path)

    def create(self) -> None:
        from genko.io import save_episode
        from genko.models import new_episode

        target = self.target()
        if (target / "project.json").exists():
            QMessageBox.warning(self, "Genko", f"同じ名前の原稿がすでにあります:\n{target}")
            return
        episode = new_episode(self.title.text().strip() or "無題", self.episode.value(), self.pages.value(),
                              spec_for(self.paper.currentData()), Binding(self.binding.currentData()))
        try:
            save_episode(episode, target)
        except OSError as exc:
            QMessageBox.warning(self, "Genko", f"保存できませんでした:\n{exc}")
            return
        self.created = target
        self.accept()


# --- export -------------------------------------------------------------------------------------------


class ExportDialog(QDialog):
    """Choose a format, its options and a folder. official=True locks the checked export (approval box)."""

    def __init__(self, parent, episode, project: Path | None, actor: str, official: bool = False) -> None:
        super().__init__(parent)
        self.setWindowTitle("正式な書き出し" if official else "書き出し")
        self.setMinimumWidth(560)
        self.episode, self.project, self.actor = episode, project, actor
        self.result_: dict | None = None
        self.format = QComboBox()
        for fmt in exporting.FORMATS:
            if official and not fmt.official:
                continue
            self.format.addItem(fmt.label, fmt.key)
        self.note = QLabel()
        self.note.setWordWrap(True)
        self.note.setStyleSheet("color:#555")
        self.dpi = QSpinBox()
        self.dpi.setRange(72, 1200)
        self.dpi.setSuffix(" dpi")
        self.width = QSpinBox()
        self.width.setRange(200, 4000)
        self.width.setValue(800)
        self.width.setSuffix(" px")
        self.max_height = QSpinBox()
        self.max_height.setRange(400, 20000)
        self.max_height.setValue(1280)
        self.max_height.setSuffix(" px")
        self.long_edge = QSpinBox()
        self.long_edge.setRange(400, 8000)
        self.long_edge.setValue(2048)
        self.long_edge.setSuffix(" px")
        self.jpeg = QCheckBox("JPEG にする（PNG より軽い）")
        self.spreads = QCheckBox("見開きも 1 枚ずつ出す")
        self.official = QCheckBox("正式な書き出し（点検して、書き出しの承認として記録する）")
        self.official.setChecked(official)
        self.official.setEnabled(not official)
        default_dir = (project.parent / f"{project.stem}_書き出し") if project else Path.home() / "genko_書き出し"
        self.folder = QLineEdit(str(default_dir))
        pick = QPushButton("選ぶ…")
        pick.clicked.connect(self._pick_folder)
        where = QHBoxLayout()
        where.addWidget(self.folder, 1)
        where.addWidget(pick)
        self.form = QFormLayout()
        self.form.addRow("形式", self.format)
        self.form.addRow("", self.note)
        self.rows: dict[str, QWidget] = {}
        for key, label, widget in (("dpi", "解像度", self.dpi), ("width", "幅", self.width), ("max_height", "1 枚の高さの上限", self.max_height),
                                   ("long_edge", "長辺", self.long_edge), ("jpeg", "", self.jpeg), ("spreads", "", self.spreads)):
            self.form.addRow(label, widget)
            self.rows[key] = widget
        self.form.addRow("", self.official)
        self.form.addRow("書き出し先", where)
        self.buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        self.buttons.button(QDialogButtonBox.StandardButton.Ok).setText("書き出す")
        self.buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        self.buttons.accepted.connect(self.run)
        self.buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addLayout(self.form)
        layout.addWidget(self.buttons)
        self.format.currentIndexChanged.connect(lambda _: self._format_changed())
        self._format_changed()

    def _format_changed(self) -> None:
        fmt = exporting.BY_KEY[self.format.currentData()]
        self.note.setText(fmt.note)
        for key, widget in self.rows.items():
            visible = key in fmt.options
            widget.setVisible(visible)
            label = self.form.labelForField(widget)
            if label is not None:
                label.setVisible(visible)
        self.dpi.setValue(exporting.default_dpi(self.episode, fmt.key))
        self.jpeg.setChecked(fmt.key == "sns")
        if not self.official.isEnabled() or not fmt.official:
            self.official.setChecked(self.official.isChecked() and fmt.official)
        self.official.setVisible(fmt.official)

    def _pick_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "書き出し先", self.folder.text())
        if path:
            self.folder.setText(path)

    def options(self) -> dict:
        return {"dpi": self.dpi.value(), "width": self.width.value(), "max_height": self.max_height.value(),
                "long_edge": self.long_edge.value(), "jpeg": self.jpeg.isChecked(), "spreads": self.spreads.isChecked()}

    def run(self) -> None:
        out = Path(self.folder.text()).expanduser()
        self.setCursor(Qt.CursorShape.WaitCursor)
        try:
            result = exporting.run(self.episode, self.project, self.format.currentData(), out,
                                   official=self.official.isChecked(), actor=self.actor, **self.options())
        finally:
            self.unsetCursor()
        self.result_ = result
        if not result.get("ok"):
            reasons = [e.get("message", "") for e in result.get("errors", [])[:12]]
            QMessageBox.warning(self, "Genko", "書き出せませんでした。\n" + ("\n".join(reasons) or result.get("error", "")))
            return
        box = QMessageBox(self)
        box.setWindowTitle("Genko")
        box.setText(f"{len(result['files'])} 個のファイルを書き出しました。\n{out}")
        open_button = box.addButton("フォルダを開く", QMessageBox.ButtonRole.ActionRole)
        box.addButton("閉じる", QMessageBox.ButtonRole.AcceptRole)
        box.exec()
        if box.clickedButton() is open_button:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(out)))
        self.accept()
