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
from genko.models import PAPER_PRESETS, Binding, PageSpec

PAPERS = [(label, key) for key, (label, _make) in PAPER_PRESETS.items()]


def spec_for(key: str) -> PageSpec:
    return PAPER_PRESETS[key][1]()


class PaperDialog(QDialog):
    """用紙の設定: a preset, or every number — paper, finished size, bleed and the basic frame's margins."""

    def __init__(self, parent, spec: PageSpec, changing: bool = False) -> None:
        from PySide6.QtWidgets import QDoubleSpinBox, QGridLayout

        super().__init__(parent)
        self.setWindowTitle("原稿用紙の設定")
        self.preset = QComboBox()
        self.preset.addItem("（数値で決める）", "")
        for key, (label, _make) in PAPER_PRESETS.items():
            self.preset.addItem(label, key)
        self.preset.currentIndexChanged.connect(lambda _: self._from_preset())

        def spin(lo, hi, value):
            box = QDoubleSpinBox()
            box.setRange(lo, hi)
            box.setDecimals(1)
            box.setSingleStep(0.5)
            box.setSuffix(" mm")
            box.setValue(float(value))
            box.valueChanged.connect(lambda _: self._changed())
            return box

        tw, th = spec.trim_size()
        m = spec.margins()
        self.paper_w, self.paper_h = spin(20, 1000, spec.width_mm), spin(20, 2000, spec.height_mm)
        self.trim_w, self.trim_h = spin(10, 1000, tw), spin(10, 2000, th)
        self.bleed = spin(0, 20, spec.bleed_mm)
        self.top, self.bottom = spin(0, 200, m["top"]), spin(0, 200, m["bottom"])
        self.inner, self.outer = spin(0, 200, m["inner"]), spin(0, 200, m["outer"])
        self.dpi = QSpinBox()
        self.dpi.setRange(72, 1200)
        self.dpi.setValue(int(spec.dpi))
        self.dpi.setSuffix(" dpi")
        grid = QGridLayout()
        rows = [("用紙（キャンバス）", self.paper_w, self.paper_h), ("仕上がり（トンボの内側）", self.trim_w, self.trim_h)]
        grid.addWidget(QLabel("幅"), 0, 1)
        grid.addWidget(QLabel("高さ"), 0, 2)
        for i, (label, a, b) in enumerate(rows, start=1):
            grid.addWidget(QLabel(label), i, 0)
            grid.addWidget(a, i, 1)
            grid.addWidget(b, i, 2)
        grid.addWidget(QLabel("裁ち落とし（仕上がりの外）"), 3, 0)
        grid.addWidget(self.bleed, 3, 1)
        grid.addWidget(QLabel("基本枠までの余白　上 / 下"), 4, 0)
        grid.addWidget(self.top, 4, 1)
        grid.addWidget(self.bottom, 4, 2)
        grid.addWidget(QLabel("　　　　　　　　のど / 小口"), 5, 0)
        grid.addWidget(self.inner, 5, 1)
        grid.addWidget(self.outer, 5, 2)
        grid.addWidget(QLabel("解像度"), 6, 0)
        grid.addWidget(self.dpi, 6, 1)
        self.summary = QLabel()
        self.summary.setWordWrap(True)
        self.move = QCheckBox("コマ・台詞・絵を新しい基本枠に合わせて動かす")
        self.move.setChecked(True)
        self.move.setVisible(changing)
        hint = QLabel("数値は出版社・印刷所で違います。投稿・入稿の前に、先方の原稿用紙の指定を確かめてください。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#666")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("変える" if changing else "決める")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        self.ok_button = buttons.button(QDialogButtonBox.StandardButton.Ok)
        layout = QVBoxLayout(self)
        row = QHBoxLayout()
        row.addWidget(QLabel("見本"))
        row.addWidget(self.preset, 1)
        layout.addLayout(row)
        layout.addLayout(grid)
        layout.addWidget(self.summary)
        layout.addWidget(self.move)
        layout.addWidget(hint)
        layout.addWidget(buttons)
        self._preset_key = ""
        same = next((key for key, (_label, make) in PAPER_PRESETS.items() if make() == spec), "")
        if same:  # the book is on a preset: show it as that
            self.preset.blockSignals(True)
            self.preset.setCurrentIndex(self.preset.findData(same))
            self.preset.blockSignals(False)
            self._preset_key = same
        self._changed(keep_preset=True)

    def _from_preset(self) -> None:
        key = self.preset.currentData()
        if not key:
            return
        spec = PAPER_PRESETS[key][1]()
        tw, th = spec.trim_size()
        m = spec.margins()
        widgets = (self.paper_w, self.paper_h, self.trim_w, self.trim_h, self.bleed, self.top, self.bottom, self.inner, self.outer)
        values = (spec.width_mm, spec.height_mm, tw, th, spec.bleed_mm, m["top"], m["bottom"], m["inner"], m["outer"])
        for widget, value in zip(widgets, values):
            widget.blockSignals(True)
            widget.setValue(float(value))
            widget.blockSignals(False)
        self.dpi.setValue(int(spec.dpi))
        self._preset_key = key
        self._changed(keep_preset=True)

    def spec(self) -> PageSpec:
        if self._preset_key:
            return PAPER_PRESETS[self._preset_key][1]()
        return PageSpec.custom(self.paper_w.value(), self.paper_h.value(), self.trim_w.value(), self.trim_h.value(), self.bleed.value(),
                               self.top.value(), self.bottom.value(), self.inner.value(), self.outer.value(), self.dpi.value())

    def _changed(self, keep_preset: bool = False) -> None:
        if not keep_preset and getattr(self, "_preset_key", ""):
            self._preset_key = ""
            self.preset.blockSignals(True)
            self.preset.setCurrentIndex(0)
            self.preset.blockSignals(False)
        try:
            text = self.spec().describe()
            ok = True
        except ValueError as exc:
            text = {"the paper must hold the finished size and its bleed": "用紙が、仕上がりと裁ち落としより小さくなっています",
                    "the basic frame must fit inside the finished size": "基本枠が仕上がりに収まりません"}.get(str(exc), str(exc))
            ok = False
        self.summary.setText(text)
        self.summary.setStyleSheet("" if ok else "color:#c92a2a")
        if hasattr(self, "ok_button"):
            self.ok_button.setEnabled(ok)

    def _accept(self) -> None:
        try:
            self.spec()
        except ValueError:
            return
        self.accept()

    def op(self) -> dict:
        """The set_page_spec op for this choice."""
        if self._preset_key:
            return {"op": "set_page_spec", "preset": self._preset_key, "move": self.move.isChecked()}
        return {"op": "set_page_spec", "paper": [self.paper_w.value(), self.paper_h.value()],
                "trim": [self.trim_w.value(), self.trim_h.value()], "bleed_mm": self.bleed.value(),
                "margins": [self.top.value(), self.bottom.value(), self.inner.value(), self.outer.value()], "dpi": self.dpi.value(),
                "move": self.move.isChecked()}


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
        self.paper.addItem("自分で決める…", "custom")
        self.custom_spec: PageSpec | None = None
        self.paper_note = QLabel()
        self.paper_note.setWordWrap(True)
        self.paper_note.setStyleSheet("color:#666")
        self.paper.currentIndexChanged.connect(lambda _: self._paper_changed())
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
        form.addRow("", self.paper_note)
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
        self._paper_changed()

    def target(self) -> Path:
        from genko.export import safe_name

        name = safe_name(self.title.text().strip() or "無題", "manga")
        if self.episode.value() > 1:
            name += f"_{self.episode.value():02d}"
        return Path(self.folder.text()).expanduser() / f"{name}.genko"

    def _note(self) -> None:
        self.where_note.setText(f"作られるフォルダ: {self.target()}")

    def chosen_spec(self) -> PageSpec:
        if self.paper.currentData() == "custom":
            return self.custom_spec or PageSpec.b4_comic()
        return spec_for(self.paper.currentData())

    def _paper_changed(self) -> None:
        if self.paper.currentData() == "custom":
            dialog = PaperDialog(self, self.custom_spec or PageSpec.b4_comic())
            if dialog.exec() == QDialog.DialogCode.Accepted:
                self.custom_spec = dialog.spec()
            elif self.custom_spec is None:
                self.paper.setCurrentIndex(0)
                return
        self.paper_note.setText(self.chosen_spec().describe())

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
                              self.chosen_spec(), Binding(self.binding.currentData()))
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
        from genko.export import AREA_LABELS, AREAS

        self.area = QComboBox()
        for key in AREAS:
            self.area.addItem(AREA_LABELS[key], key)
        self.area.setCurrentIndex(self.area.findData("bleed"))
        self.area.setToolTip("印刷所の指定に合わせます。多くは「裁ち落としまで」。トンボ付きは用紙全体")
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
        for key, label, widget in (("dpi", "解像度", self.dpi), ("area", "書き出す範囲", self.area), ("width", "幅", self.width), ("max_height", "1 枚の高さの上限", self.max_height),
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
                "long_edge": self.long_edge.value(), "jpeg": self.jpeg.isChecked(), "spreads": self.spreads.isChecked(),
                "area": self.area.currentData()}

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


# --- panel layout templates --------------------------------------------------------------------------------


def _tree(frame) -> dict:
    r = frame.rect
    node = {"rect_mm": [r.x, r.y, r.width, r.height]}
    if frame.children:
        node["axis"] = frame.split_axis
        node["children"] = [_tree(child) for child in frame.children]
    return node


def _plan(key: str) -> dict:
    """A template as a layout plan (a person's layout has no panel briefs yet)."""
    from genko.studio import layout

    slots = layout.slots_in_order(layout.resolve_tiers({"template": key}))
    return {"template": key, "panels": [{"slot": slot} for slot in slots]}


class TemplateDialog(QDialog):
    """Pick a panel layout; `ops` are the ops that clear the page and cut it (applied by the window)."""

    def __init__(self, parent, episode, page) -> None:
        import copy

        from PySide6.QtCore import QSize
        from PySide6.QtGui import QIcon

        from genko.app.studio_widgets import to_pixmap
        from genko.render import render_page
        from genko.studio import layout

        super().__init__(parent)
        self.setWindowTitle("テンプレートでコマを割る")
        self.resize(720, 520)
        self.episode, self.page = episode, page
        self.ops: list[dict] = []
        self.needs_clearing = not layout.is_blank(episode, page)
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.ViewMode.IconMode)
        self.list.setIconSize(QSize(120, 170))
        self.list.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.list.setSpacing(8)
        self.list.setWordWrap(True)
        for key, spec in layout.templates().items():
            trial = copy.deepcopy(episode)
            target = next(p for p in trial.pages if p.index == page.index)
            try:
                layout.clear_page(trial, target, agent="human:preview")
                layout.apply_layout(trial, page.index, _plan(key), agent="human:preview")
                target = next(p for p in trial.pages if p.index == page.index)
                icon = QIcon(to_pixmap(render_page(target, 20, mode="print", episode=trial)))
            except Exception:
                continue
            item = QListWidgetItem(icon, spec.get("description") or key)
            item.setData(Qt.ItemDataRole.UserRole, key)
            self.list.addItem(item)
        self.list.itemDoubleClicked.connect(lambda _: self.choose())
        note = QLabel("コマと台詞は作り直されます（元に戻す で取り消せます）。" if self.needs_clearing else "空のページをテンプレートで割ります。")
        note.setWordWrap(True)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("このテンプレートで割る")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.choose)
        buttons.rejected.connect(self.reject)
        layout_box = QVBoxLayout(self)
        layout_box.addWidget(self.list, 1)
        layout_box.addWidget(note)
        layout_box.addWidget(buttons)

    def choose(self) -> None:
        import copy

        from genko.studio import layout

        item = self.list.currentItem()
        if item is None:
            return
        trial = copy.deepcopy(self.episode)
        target = next(p for p in trial.pages if p.index == self.page.index)
        try:
            layout.clear_page(trial, target, agent="human:preview")
            layout.apply_layout(trial, self.page.index, _plan(item.data(Qt.ItemDataRole.UserRole)), agent="human:preview")
        except Exception as exc:
            from genko.app import wording

            QMessageBox.warning(self, "Genko", wording.error(str(exc)))
            return
        done = next(p for p in trial.pages if p.index == self.page.index)
        # one op with the finished tree (split ids made on the copy would not match the book)
        self.ops = [{"op": "delete_line", "id": line.id} for line in self.episode.story_for_page(self.page.index)]
        self.ops.append({"op": "set_layout", "page": self.page.index, "tree": _tree(done.frames[0]), "force": True})
        self.accept()
