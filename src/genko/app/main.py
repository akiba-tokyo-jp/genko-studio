from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QPointF, QSettings, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QColor, QImage, QKeySequence, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
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
from genko.app.brush_panel import BrushPanel
from genko.app.canvas import PageCanvas
from genko.app.guide_panel import PRESETS, GuidePanel
from genko.app.material_panel import MaterialPanel
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
    """The page's lines in reading order. Pick one to edit its words, speaker, balloon and lettering
    style; add one to the selected panel; reorder; delete. Ruby is typed as ｜約束《やくそく》."""

    def __init__(self, window) -> None:
        from PySide6.QtWidgets import QDoubleSpinBox, QFormLayout, QPlainTextEdit

        from genko import fonts
        from genko.app.lettering import KINDS

        super().__init__()
        self.window = window
        self.line_ids: list[str] = []
        self._loading = False
        self.list = QListWidget()
        self.list.currentRowChanged.connect(lambda _: self._picked())
        up = QPushButton("↑ 前へ")
        up.clicked.connect(lambda: self._move(-1))
        down = QPushButton("↓ 後へ")
        down.clicked.connect(lambda: self._move(1))
        self.speaker = QLineEdit()
        self.speaker.setPlaceholderText("話者（空でもよい）")
        self.text = QPlainTextEdit()
        self.text.setPlaceholderText("台詞（改行で次の列へ。ルビは ｜約束《やくそく》）")
        self.text.setMaximumHeight(80)
        self.kind = QComboBox()
        for key, label in KINDS:
            self.kind.addItem(label, key)
        self.vertical = QCheckBox("縦書き")
        self.vertical.setChecked(True)
        add = QPushButton("選んだコマに追加")
        add.clicked.connect(self.add)
        self.apply_button = QPushButton("この台詞を直す")
        self.apply_button.clicked.connect(self.apply_edit)
        self.delete_button = QPushButton("削除")
        self.delete_button.clicked.connect(self.delete)
        # lettering style of the selected line (applied at once)
        self.font = QComboBox()
        for key, (label, _k, _o) in fonts.BUNDLED.items():
            self.font.addItem(label, key)
        self.font.addItem("パソコンの書体を選ぶ…", "__pick__")
        self.font.activated.connect(lambda _: self._font_changed())

        def spin(lo, hi, step, suffix, special=None):
            box = QDoubleSpinBox()
            box.setRange(lo, hi)
            box.setSingleStep(step)
            box.setDecimals(2)
            box.setSuffix(suffix)
            if special:
                box.setSpecialValueText(special)
            box.editingFinished.connect(self._style_changed)
            return box

        self.size = spin(0, 40, 0.5, " mm", "自動")
        self.tracking = spin(-0.3, 1.0, 0.05, " 字")
        self.leading = spin(-0.3, 2.0, 0.05, " 字")
        self.outline = spin(0, 5, 0.1, " mm", "なし")
        self.border = spin(0, 3, 0.05, " mm")
        self.align = QComboBox()
        for key, label in (("top", "上"), ("center", "中央"), ("bottom", "下"), ("left", "左"), ("right", "右")):
            self.align.addItem(label, key)
        self.align.activated.connect(lambda _: self._style_changed())
        self.fill = QComboBox()
        self.fill.addItem("白", "white")
        self.fill.addItem("塗らない（透明）", "none")
        self.fill.activated.connect(lambda _: self._style_changed())
        self.tcy = QCheckBox("数字と !? を縦中横にする")
        self.tcy.clicked.connect(lambda _: self._style_changed())
        self.color = QPushButton("文字の色…")
        self.color.clicked.connect(self._pick_color)
        reset = QPushButton("文字とフキダシの設定を既定に戻す")
        reset.clicked.connect(self._reset_style)
        order = QHBoxLayout()
        order.addWidget(up)
        order.addWidget(down)
        row = QHBoxLayout()
        row.addWidget(self.kind, 1)
        row.addWidget(self.vertical)
        buttons = QHBoxLayout()
        buttons.addWidget(add)
        buttons.addWidget(self.apply_button)
        buttons.addWidget(self.delete_button)
        form = QFormLayout()
        form.addRow("書体", self.font)
        form.addRow("文字の大きさ", self.size)
        form.addRow("字間", self.tracking)
        form.addRow("行間", self.leading)
        form.addRow("揃え", self.align)
        form.addRow("白フチ", self.outline)
        form.addRow("フキダシの線", self.border)
        form.addRow("フキダシの中", self.fill)
        form.addRow("", self.tcy)
        form.addRow("", self.color)
        hint = QLabel("編集画面: テキストツール（T）でクリックした所に入力。フキダシはダブルクリックで打ち直し、"
                      "四隅で大きさ、●でしっぽの先、◇でしっぽの曲がり。右クリックで形・しっぽ・結合。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#666")
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("このページの台詞（読み順）"))
        layout.addWidget(self.list, 1)
        layout.addLayout(order)
        layout.addWidget(self.speaker)
        layout.addWidget(self.text)
        layout.addLayout(row)
        layout.addLayout(buttons)
        layout.addLayout(form)
        layout.addWidget(reset)
        layout.addWidget(hint)
        self._picked()

    def _lines(self):
        page = self.window.current_page()
        return self.window.episode.story_for_page(page.index) if page else []

    def _line(self):
        return next((ln for ln in self._lines() if ln.id == self.current_id()), None)

    def refresh(self) -> None:
        from genko.app.lettering import KIND_LABEL

        current = self.current_id()
        self.list.blockSignals(True)
        self.list.clear()
        self.line_ids = []
        for n, line in enumerate(self._lines(), 1):
            who = f"{line.speaker}: " if line.speaker else ""
            kind = KIND_LABEL.get(line.balloon, line.balloon)
            self.list.addItem(f"{n}. {who}{line.text.replace(chr(10), ' ')}　〔{kind}〕")
            self.line_ids.append(line.id)
        row = self.line_ids.index(current) if current in self.line_ids else -1
        self.list.setCurrentRow(row)
        self.list.blockSignals(False)
        self._picked()

    def current_id(self) -> str | None:
        row = self.list.currentRow()
        return self.line_ids[row] if 0 <= row < len(self.line_ids) else None

    def select(self, line_id: str | None) -> None:
        if line_id in self.line_ids:
            self.list.setCurrentRow(self.line_ids.index(line_id))

    def _picked(self) -> None:
        from genko.app.lettering import with_ruby
        from genko.balloons import style_of

        line = self._line()
        for widget in (self.apply_button, self.delete_button):
            widget.setEnabled(line is not None)
        self.window.canvas.selected_line_id = line.id if line else None
        self.window.canvas.update()
        if line is None:
            return
        self._loading = True
        self.speaker.setText(line.speaker)
        self.text.setPlainText(with_ruby(line.text, line.ruby_runs))
        self.kind.setCurrentIndex(max(0, self.kind.findData(line.balloon)))
        self.vertical.setChecked(line.wrap == "vertical")
        st = style_of(line)
        font = st["font"] or ("sfx" if line.balloon == "sfx" else "antique")
        index = self.font.findData(font)
        if index < 0:
            self.font.insertItem(self.font.count() - 1, Path(font).stem, font)
            index = self.font.findData(font)
        self.font.setCurrentIndex(index)
        self.size.setValue(float(st["size_mm"] or 0))
        self.tracking.setValue(float(st["tracking"] or 0))
        self.leading.setValue(float(st["leading"] or 0))
        self.outline.setValue(float(st["outline_mm"] or 0))
        self.border.setValue(float(st["border_mm"] if st["border_mm"] is not None else 0.35))
        self.align.setCurrentIndex(max(0, self.align.findData(st["align"])))
        self.fill.setCurrentIndex(max(0, self.fill.findData(st["fill"])))
        self.tcy.setChecked(bool(st["tcy"]))
        self._loading = False

    def _style(self, change: dict) -> None:
        line = self._line()
        if line is not None and not self._loading:
            self.window.apply_ops([{"op": "edit_line", "id": line.id, "style": change}])

    def _style_changed(self) -> None:
        if self._loading:
            return
        self._style({"size_mm": self.size.value() or None, "tracking": self.tracking.value(), "leading": self.leading.value(),
                     "outline_mm": self.outline.value() or None, "border_mm": self.border.value(),
                     "align": self.align.currentData(), "fill": self.fill.currentData(), "tcy": self.tcy.isChecked()})

    def _font_changed(self) -> None:
        if self._loading:
            return
        key = self.font.currentData()
        if key == "__pick__":
            key = self._pick_system_font()
            if not key:
                self._picked()
                return
        self._style({"font": key})
        self._picked()

    def _pick_system_font(self) -> str | None:
        from PySide6.QtWidgets import QApplication, QInputDialog

        from genko import fonts

        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            found = fonts.system_fonts()
        finally:
            QApplication.restoreOverrideCursor()
        if not found:
            QMessageBox.information(self, "Genko", "日本語を表示できる書体が、このパソコンに見つかりませんでした")
            return None
        names = [f"{item['name']} {item['style']}" for item in found]
        name, ok = QInputDialog.getItem(self, "パソコンの書体", "書体", names, 0, False)
        return found[names.index(name)]["path"] if ok and name in names else None

    def _pick_color(self) -> None:
        from PySide6.QtWidgets import QColorDialog

        from genko.balloons import style_of

        line = self._line()
        if line is None:
            return
        rgb = style_of(line)["rgb"] or (10, 10, 10)
        color = QColorDialog.getColor(QColor(*rgb), self, "文字の色")
        if color.isValid():
            self._style({"rgb": [color.red(), color.green(), color.blue()]})

    def _reset_style(self) -> None:
        from genko.ops import STYLE_KEYS

        line = self._line()
        if line is not None:
            keep = {k: None for k in STYLE_KEYS if k != "group"}
            self._style(keep)
            self._picked()

    def _move(self, delta: int) -> None:
        page = self.window.current_page()
        line_id = self.current_id()
        if page is None or line_id is None:
            return
        order = list(self.line_ids)
        i = order.index(line_id)
        j = i + delta
        if not 0 <= j < len(order):
            return
        order[i], order[j] = order[j], order[i]
        if self.window.apply_ops([{"op": "reorder_lines", "page": page.index, "order": order}]):
            self.refresh()
            self.select(line_id)

    def add(self) -> None:
        from genko.app.lettering import parse_ruby, place_new

        page = self.window.current_page()
        typed = self.text.toPlainText().strip()
        if page is None or not typed:
            QMessageBox.information(self, "Genko", "台詞を書いてから追加します")
            return
        frame = self.window.selected_frame()
        if frame is None:
            QMessageBox.information(self, "Genko", "先に編集画面でコマをクリックして選びます（テキストツール T なら、置きたい所をクリック）")
            return
        text, runs = parse_ruby(typed)
        box = place_new(self.window.episode, page, frame, text, self.kind.currentData(), self.vertical.isChecked())
        before = {ln.id for ln in self._lines()}
        op = {"op": "add_line", "page": page.index, "text": text, "speaker": self.speaker.text().strip(),
              "frame_id": frame.id, "balloon": self.kind.currentData(), **box}
        if runs:
            op["ruby_runs"] = runs
        if self.window.apply_ops([op]):
            self.text.clear()
            added = next((ln.id for ln in self._lines() if ln.id not in before), None)
            self.refresh()
            self.select(added)

    def apply_edit(self) -> None:
        from genko.app.lettering import parse_ruby, refit

        line = self._line()
        if line is None:
            return
        typed = self.text.toPlainText().strip()
        if not typed:
            QMessageBox.information(self, "Genko", "台詞が空です。消すときは「削除」を押します")
            return
        text, runs = parse_ruby(typed)
        kind, vertical = self.kind.currentData(), self.vertical.isChecked()
        ops = [{"op": "edit_line", "id": line.id, "text": text, "speaker": self.speaker.text().strip(), "balloon": kind,
                "wrap": "vertical" if vertical else "horizontal", "ruby_runs": runs}]
        if (text, kind, vertical) != (line.text, line.balloon, line.wrap == "vertical"):
            frame = self.window.frame_by_id(line.frame_id)
            ops.append({"op": "move_line", "id": line.id, **refit(line, frame, text, kind, vertical)})
        self.window.apply_ops(ops)

    def delete(self) -> None:
        line_id = self.current_id()
        if line_id and self.window.apply_ops([{"op": "delete_line", "id": line_id}]):
            self.refresh()


LAYER_ICON = {"strokes": "✎", "raster": "▦", "folder": "▸", "placed": "🖼", "tone": "░", "fill": "■"}


class LayerPanel(QWidget):
    """Layers, front first. The selected layer is where the pen and the eraser work."""

    def __init__(self, window) -> None:
        from PySide6.QtWidgets import QSlider

        super().__init__()
        self.window = window
        self.ids: list[str] = []
        self.target = QLabel()
        self.target.setWordWrap(True)
        self.list = QListWidget()
        self.list.itemChanged.connect(self._visibility)
        self.list.currentRowChanged.connect(lambda _: self._selected())
        self.name = QLineEdit()
        self.name.setPlaceholderText("レイヤーの名前")
        self.name.editingFinished.connect(self._rename)
        self.opacity = QSlider(Qt.Orientation.Horizontal)
        self.opacity.setRange(0, 100)
        self.opacity.sliderReleased.connect(self._set_opacity)
        self.blend = QComboBox()
        for key, label in wording.BLEND:
            self.blend.addItem(label, key)
        self.blend.activated.connect(lambda _: self._set("blend", self.blend.currentData()))
        self.clip = QCheckBox("下のレイヤーでクリップ")
        self.clip.clicked.connect(lambda on: self._set("clip", on))
        self.protect = QCheckBox("透明部分を保護")
        self.protect.clicked.connect(lambda on: self._set("lock_alpha", on))
        self.locked = QCheckBox("ロック（描けなくする）")
        self.locked.clicked.connect(lambda on: self._set("locked", on))
        self.overhang = QCheckBox("コマの外にもはみ出す")
        self.overhang.setToolTip("このレイヤーの線を、コマの枠で切らずに間の白や外まで描きます")
        self.overhang.clicked.connect(lambda on: self._set("panel_clip", not on))
        add_pen = QPushButton("＋ペン")
        add_pen.setToolTip("線を描くレイヤー（線はあとから消しゴムで切れる）")
        add_pen.clicked.connect(lambda: self._add("pen", "ペン"))
        add_paint = QPushButton("＋ペイント")
        add_paint.setToolTip("塗りや画像のレイヤー")
        add_paint.clicked.connect(lambda: self._add("paint", "ペイント"))
        add_folder = QPushButton("＋フォルダ")
        add_folder.clicked.connect(lambda: self._add("folder", "フォルダ"))
        up = QPushButton("↑")
        up.setToolTip("前へ")
        up.clicked.connect(lambda: self._move(1))
        down = QPushButton("↓")
        down.setToolTip("後ろへ")
        down.clicked.connect(lambda: self._move(-1))
        delete = QPushButton("削除")
        delete.clicked.connect(self._delete)
        self.filter = QComboBox()
        for key, label in wording.FILTERS:
            self.filter.addItem(label, key)
        apply_filter = QPushButton("フィルターをかける…")
        apply_filter.clicked.connect(self._filter)
        adds = QHBoxLayout()
        for button in (add_pen, add_paint, add_folder):
            adds.addWidget(button)
        moves = QHBoxLayout()
        for button in (up, down, delete):
            moves.addWidget(button)
        props = QHBoxLayout()
        props.addWidget(QLabel("不透明度"))
        props.addWidget(self.opacity, 1)
        props.addWidget(self.blend)
        frow = QHBoxLayout()
        frow.addWidget(self.filter, 1)
        frow.addWidget(apply_filter)
        layout = QVBoxLayout(self)
        layout.addWidget(self.target)
        layout.addWidget(self.list, 1)
        layout.addLayout(adds)
        layout.addLayout(moves)
        layout.addWidget(self.name)
        layout.addLayout(props)
        layout.addWidget(self.clip)
        layout.addWidget(self.protect)
        layout.addWidget(self.locked)
        layout.addWidget(self.overhang)
        layout.addLayout(frow)
        self._loading = False

    def refresh(self) -> None:
        page = self.window.current_page()
        self._loading = True
        self.list.clear()
        self.ids = []
        layers = list(reversed(page.layers)) if page else []  # front first
        for layer in layers:
            kind = getattr(layer.kind, "value", str(layer.kind))
            icon = LAYER_ICON.get(kind, "")
            indent = "　" if layer.parent_id else ""
            lock = " 🔒" if getattr(layer, "locked", False) else ""
            item = QListWidgetItem(f"{indent}{icon} {wording.layer_label(layer)}{lock}")
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked if layer.visible else Qt.CheckState.Unchecked)
            self.list.addItem(item)
            self.ids.append(layer.id)
        target = self.window.target_layer()
        if target is not None and target.id in self.ids:
            self.list.setCurrentRow(self.ids.index(target.id))
        self._loading = False
        self._selected(from_list=False)

    def _layer(self):
        page = self.window.current_page()
        row = self.list.currentRow()
        if page is None or not 0 <= row < len(self.ids):
            return page, None
        return page, next((layer for layer in page.layers if layer.id == self.ids[row]), None)

    def _selected(self, from_list: bool = True) -> None:
        page, layer = self._layer()
        if layer is None:
            self.target.setText("")
            return
        if from_list and not self._loading:
            self.window.set_target_layer(layer.id)
        self._loading = True
        self.name.setText(wording.layer_label(layer))
        self.opacity.setValue(int(round(100 * (layer.opacity if layer.opacity is not None else 1.0))))
        self.blend.setCurrentIndex(max(0, self.blend.findData(layer.blend or "normal")))
        self.clip.setChecked(bool(layer.clip))
        self.protect.setChecked(bool(layer.lock_alpha))
        self.locked.setChecked(bool(getattr(layer, "locked", False)))
        self.overhang.setChecked(not getattr(layer, "panel_clip", True))
        self._loading = False
        drawable = self.window.drawable(layer)
        self.target.setText(f"描く先: <b>{wording.layer_label(layer)}</b>" if drawable else
                            f"<span style='color:#c92a2a'>「{wording.layer_label(layer)}」には描けません。ペンかペイントのレイヤーを選びます</span>")

    def _set(self, key: str, value) -> None:
        page, layer = self._layer()
        if layer is not None and not self._loading:
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": layer.id, key: value}])

    def _set_opacity(self) -> None:
        self._set("opacity", self.opacity.value() / 100)

    def _rename(self) -> None:
        page, layer = self._layer()
        name = self.name.text().strip()
        if layer is not None and name and name != wording.layer_label(layer):
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": layer.id, "name": name}])

    def _visibility(self, item: QListWidgetItem) -> None:
        if self._loading:
            return
        page = self.window.current_page()
        row = self.list.row(item)
        if page is None or not 0 <= row < len(self.ids):
            return
        layer = next(layer for layer in page.layers if layer.id == self.ids[row])
        visible = item.checkState() == Qt.CheckState.Checked
        if layer.visible != visible:
            self.window.apply_ops([{"op": "set_layer", "page": page.index, "id": layer.id, "visible": visible}])

    def _add(self, kind: str, title: str) -> None:
        from genko.models import new_id

        page, layer = self._layer()
        if page is None:
            return
        new = new_id()
        op = {"op": "add_layer", "page": page.index, "kind": kind, "name": title, "id": new}
        if layer is not None:
            op["after"] = layer.id  # just in front of the selected layer
        if self.window.apply_ops([op]) and kind != "folder":
            self.window.set_target_layer(new)
            self.refresh()

    def _move(self, delta: int) -> None:
        page, layer = self._layer()
        if layer is None:
            return
        order = [item.id for item in page.layers]
        i = order.index(layer.id)
        j = i + delta
        if not 0 <= j < len(order):
            return
        order[i], order[j] = order[j], order[i]
        self.window.apply_ops([{"op": "reorder_layers", "page": page.index, "order": order}])

    def _delete(self) -> None:
        page, layer = self._layer()
        if layer is None:
            return
        if QMessageBox.question(self, "Genko", f"レイヤー「{wording.layer_label(layer)}」を削除しますか？\n（元に戻す で取り消せます）") \
                != QMessageBox.StandardButton.Yes:
            return
        self.window.apply_ops([{"op": "delete_layer", "page": page.index, "id": layer.id}])

    def _filter(self) -> None:
        page, layer = self._layer()
        if layer is None:
            return
        label = self.filter.currentText()
        extra = "\nペンの線は画像になり、あとから線として消せなくなります。" if layer.strokes else ""
        if QMessageBox.question(self, "Genko", f"レイヤー「{wording.layer_label(layer)}」全体に「{label}」をかけます。{extra}\n"
                                "（元に戻す で取り消せます）") != QMessageBox.StandardButton.Yes:
            return
        self.window.apply_ops([{"op": "filter_raster", "page": page.index, "id": layer.id, "kind": self.filter.currentData(), "radius": 2}])


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
        self.pages.setMaximumWidth(180)
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.renderer = self._render_current
        self.canvas.changed.connect(self._refresh_status)
        self.canvas.strokeCommitted.connect(self._on_stroke)
        self.canvas.frameSelected.connect(self._on_frame_selected)
        self.canvas.textMoved.connect(self._on_text_moved)
        self.canvas.contextMenuAt.connect(self._context_menu)
        self.canvas.lineSelected.connect(self._on_line_selected)
        self.canvas.lineGeometry.connect(lambda line_id, change: self.apply_ops([{"op": "move_line", "id": line_id, **change}]))
        self.canvas.lineEditRequested.connect(self._edit_line_inline)
        self.canvas.lineContextMenu.connect(self._line_menu)
        self.canvas.textRequested.connect(self._type_new_line)
        self.canvas.gutterMoved.connect(lambda node, index, delta: self.apply_ops(
            [{"op": "move_gutter", "page": self._current().index, "frame_id": node, "index": index, "delta_mm": delta}]))
        self.canvas.cutRequested.connect(self._cut_frame)
        self.canvas.frameShaped.connect(lambda frame_id, poly: self.apply_ops(
            [{"op": "set_frame", "page": self._current().index, "frame_id": frame_id, "poly": poly}]))
        self.canvas.colourPicked.connect(self._on_colour_picked)
        self.canvas.fillRequested.connect(self._fill_at)
        self.canvas.areaFilled.connect(lambda pts: self._fill_area({"poly": pts}))
        self.canvas.wandRequested.connect(self._wand)
        self.canvas.selectionTransformed.connect(self._transform_selection)
        self.canvas.strokeReshaped.connect(self._reshape)
        self.canvas.strokes_for_reshape = lambda: list(getattr(self.target_layer(), "strokes", []) or [])
        self.canvas.rulerPlaced.connect(self._place_ruler)
        self.canvas.effectRequested.connect(self._effect_at)
        self.canvas.effectSelected.connect(lambda effect_id: self.materials.select_effect(effect_id))
        self.canvas.effectMoved.connect(lambda effect_id, centre: self.apply_ops(
            [{"op": "edit_effect", "page": self._current().index, "id": effect_id, "params": {"center": centre}}]))
        self.canvas.stampRequested.connect(self._stamp_at)
        self._effect_kind = "focus"
        self._pending_material: dict | None = None
        self.canvas.rulerEdited.connect(lambda ruler_id, change: self.apply_ops(
            [{"op": "edit_ruler", "page": self._current().index, "id": ruler_id, **change}]))
        self.canvas.primSelected.connect(lambda prim_id: self.guides.select_prim(prim_id))
        self.canvas.primEdited.connect(lambda prim_id, change: self.apply_ops(
            [{"op": "edit_prim", "page": self._current().index, "id": prim_id, **change}]))
        self.canvas.primPosed.connect(lambda prim_id, handle, to: self.apply_ops(
            [{"op": "pose_mannequin", "page": self._current().index, "id": prim_id, "drag": {"handle": handle, "to": to}}]))
        self._clipboard: dict | None = None
        self._target_layer_id: str | None = None
        self.eraser_mm = 2.0
        self._dock_timer = QTimer(self)
        self._dock_timer.setSingleShot(True)
        self._dock_timer.setInterval(250)
        self._dock_timer.timeout.connect(self._refresh_visible_docks)
        self._stale_docks: set = set()
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
        self.canvas.grid_mm = float(QSettings("Genko", "Genko Studio").value("guides/grid_mm", 5.0))
        self._guide_toggles()
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
        if self.pages.count() != len(self.episode.pages):
            self._reload_pages()
        else:
            self._after_edit()
        return True

    def _after_edit(self) -> None:
        """An edit on this page: redraw the page at once, the side panels a little later (drawing stays quick)."""
        page = self._current()
        item = self.pages.item(self._page_index)
        if page is not None and item is not None:
            item.setText(self._page_text(page))
        self._show_page(light=True)

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
        if result.rebased or result.conflicts:
            self._reload_pages()  # someone else's changes came in
        else:
            self._refresh_status()

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
        self.act_guides = a("仕上がり線・基本枠を表示", self._toggle_guides, "Ctrl+;", "断ち切り（裁ち落とし）・仕上がり線・基本枠", True)
        self.act_guides.setChecked(True)
        self.act_import = a("画像を読み込む…", self._import_image, "Ctrl+Shift+I", "選んだコマに（選んでいなければページに）画像を置きます")
        self.act_select = a("選択", lambda: self._tool("select"), "V", "コマを選ぶ・フキダシを動かす・ドラッグで表示を動かす", True)
        self.act_pen = a("ペン", lambda: self._tool("pen"), "B", "レイヤー パネルで選んだレイヤーに描きます", True)
        self.act_eraser = a("消しゴム", lambda: self._tool("eraser"), "E", "ペンの線は触れた所で切れます", True)
        self.act_text = a("テキスト", lambda: self._tool("text"), "T", "クリックした所に台詞を入力します（縦書き）", True)
        self.act_frame = a("コマ", lambda: self._tool("frame"), "F",
                           "コマの中をドラッグして割る（斜めも。水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで間隔を動かす・選んだコマの角をドラッグで形を変える", True)
        self.act_picker = a("スポイト", lambda: self._tool("picker"), "I", "クリックした所の色をペンの色にします", True)
        self.act_fill = a("塗りつぶし", lambda: self._tool("fill"), "G",
                          "線で囲まれた所をクリックで塗ります（隙間閉じ・見る範囲はブラシ パネルで）", True)
        self.act_lassofill = a("囲って塗る", lambda: self._tool("lassofill"), "Shift+G", "ドラッグで囲んだ所を塗ります", True)
        self.act_marquee = a("範囲選択（長方形）", lambda: self._tool("rect"), "M",
                             "ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み・縦横比を保つ）", True)
        self.act_lasso = a("範囲選択（投げ縄）", lambda: self._tool("lasso"), "L", "ドラッグで囲んで選びます", True)
        self.act_wand = a("自動選択", lambda: self._tool("wand"), "W", "クリックした所の、線で囲まれた範囲を選びます", True)
        self.act_reshape = a("線の修正（つまむ）", lambda: self._tool("reshape"), "Y",
                             "描いた線をつまんでドラッグすると、その辺りが滑らかに動きます", True)
        self.act_ruler = a("定規", lambda: self._tool("ruler"), "R",
                           "「定規」メニューで選んだ定規を置く（ドラッグ・クリック）。置いた定規の□をドラッグで動かす", True)
        self.act_3d = a("3D", lambda: self._tool("3d"), "J", "デッサン人形の関節（○）や箱をドラッグして動かす。箱の上の○で回す", True)
        self.act_effect = a("効果線", lambda: self._tool("effect"), "K",
                            "コマの中をクリックすると、選んだ効果線（集中線など）が入る。中心の＋をドラッグで動かす", True)
        self.act_stamp = a("素材を置く", lambda: self._tool("stamp"), tip="素材パネルで選んだ素材を、クリックした所に置く", checkable=True)
        tools = QActionGroup(self)
        self.tool_actions = {"select": self.act_select, "pen": self.act_pen, "eraser": self.act_eraser, "text": self.act_text,
                             "frame": self.act_frame, "picker": self.act_picker, "fill": self.act_fill,
                             "lassofill": self.act_lassofill, "rect": self.act_marquee, "lasso": self.act_lasso,
                             "wand": self.act_wand, "reshape": self.act_reshape, "ruler": self.act_ruler, "3d": self.act_3d,
                             "effect": self.act_effect, "stamp": self.act_stamp}
        for act in self.tool_actions.values():
            tools.addAction(act)
        self.act_select.setChecked(True)
        self.act_color = a("ペンの色…", self._pick_color, "C")
        self.act_select_all = a("すべて選択", self._select_all, std.SelectAll)
        self.act_deselect = a("選択を解除", lambda: self.canvas.set_selection(None), "Ctrl+D")
        self.act_copy = a("コピー", self._copy, std.Copy)
        self.act_cut = a("切り取り", self._cut, std.Cut)
        self.act_paste = a("貼り付け", self._paste, std.Paste, "新しいレイヤーに貼り付けます（そのまま動かせます）")
        self.act_delete_area = a("選択範囲を消す", self._delete_area, [QKeySequence(std.Delete), QKeySequence("Backspace")])
        self.act_flip_h = a("左右反転", lambda: self._flip(-1, 1))
        self.act_flip_v = a("上下反転", lambda: self._flip(1, -1))
        self.act_fill_selection = a("選択範囲を塗る", lambda: self._fill_area(self._area()), "Alt+Backspace")
        self.act_line_width = a("選択範囲の線の太さ…", self._line_width)
        settings = QSettings("Genko", "Genko Studio")
        self.ruler_kinds = [
            ("直線定規", "line", {}, "ドラッグで置く。近くで描いた線がまっすぐ沿う"),
            ("曲線定規", "curve", {}, "クリックで点を打ち、ダブルクリック（Enter）で終わる"),
            ("平行線定規", "parallel", {}, "ドラッグで角度を決める。どこで描いてもその角度の直線になる"),
            ("同心円定規", "concentric", {}, "中心からドラッグ（Alt で楕円）。描いた線が円に沿う"),
            ("放射線定規（集中線）", "radial", {}, "中心をクリック。描いた線が中心へ向かう"),
            ("パース定規（1 点）", "perspective", {"vps": 1}, "消失点をクリック"),
            ("パース定規（2 点）", "perspective", {"vps": 2}, "消失点を 2 つクリック（アイレベルが引かれる）"),
            ("パース定規（3 点）", "perspective", {"vps": 3}, "消失点を 3 つクリック"),
            ("対称定規（左右）", "symmetry", {"copies": 2}, "対称の軸をドラッグ。描いた線が反対側にも描かれる"),
            ("対称定規（回転）…", "symmetry", {"ask": True}, "中心から軸をドラッグ。描いた線が中心の周りに写される"),
        ]
        self.ruler_actions = [a(title, lambda _=False, k=kind, o=opts: self._choose_ruler(k, **o), tip=tip)
                              for title, kind, opts, tip in self.ruler_kinds]
        self.act_snap = a("定規にスナップ", self._guide_toggles, "Ctrl+2", "ペンの線を定規に沿わせる（切ると自由に描ける）", True)
        self.act_show_rulers = a("定規を表示", self._guide_toggles, "Ctrl+Shift+R", "", True)
        self.act_grid = a("グリッドを表示", self._guide_toggles, "Ctrl+'", "", True)
        self.act_grid_snap = a("グリッドにスナップ", self._guide_toggles, "", "Shift で引く直線と定規の点がグリッドに吸い付く", True)
        for act, key, default in ((self.act_snap, "snap", True), (self.act_show_rulers, "show", True),
                                  (self.act_grid, "grid", False), (self.act_grid_snap, "grid_snap", False)):
            act.setChecked(str(settings.value(f"guides/{key}", default)).lower() == "true")
        self.act_grid_mm = a("グリッドの間隔…", self._grid_spacing)
        self.act_del_ruler = a("選んだ定規を消す", lambda: self.guides.delete_ruler())
        self.act_clear_rulers = a("このページの定規をすべて消す", self._clear_rulers)
        self.act_add_figure = a("デッサン人形を置く", lambda: self._add_prim("mannequin"), tip="選んだコマ（なければページ）の真ん中に置きます")
        self.act_add_box = a("3D の箱を置く", lambda: self._add_prim("box"))
        self.act_trace = a("3D を線にする（描く先のレイヤーへ）", lambda: self.trace_prims(selected_only=False),
                           tip="このページの 3D を鉛筆の線にして下描きにします")
        self.act_del_prim = a("選んだ 3D を消す", lambda: self.guides.delete_prim())
        self.act_tone_here = a("選択範囲・選んだコマにトーンを貼る", self._tone_here, "Ctrl+Shift+T",
                               "素材パネルで選んだトーン（なければ網点 60 線 30%）を貼ります")
        self.act_tone_click = a("クリックした所にトーンを貼る", self._tone_click, tip="線で囲まれた所をクリックすると、そこにトーンが入ります")
        self.effect_actions = [a(label, lambda _=False, k=key: self._choose_effect(k)) for key, label in
                               (("focus", "集中線"), ("speed", "流線"), ("uni_flash", "ウニフラッシュ"), ("beta_flash", "ベタフラッシュ"))]
        self.act_materials = a("素材パネルを開く", lambda: self.show_dock("素材"))
        self.pose_actions = [a(f"ポーズ: {label}", lambda _=False, k=key: self._pose(k)) for key, label in PRESETS.items()]
        self.act_thicker = a("太く（ペン・消しゴム）", lambda: self._nudge_brush(1), "]")
        self.act_thinner = a("細く（ペン・消しゴム）", lambda: self._nudge_brush(-1), "[")
        self.act_split_h = a("コマを横に割る（上下に分ける）", lambda: self._split("horizontal"), "Ctrl+Shift+H")
        self.act_split_v = a("コマを縦に割る（左右に分ける）", lambda: self._split("vertical"), "Ctrl+Shift+V")
        self.act_merge = a("コマを結合（割る前に戻す）", self._merge, "Ctrl+Shift+M")
        self.act_gutters = a("コマ間隔の設定…", self._gutter_settings, tip="新しく割るときの上下・左右の間隔")
        self.act_border = a("選んだコマの枠線の太さ…", self._border_width)
        self.act_no_border = a("選んだコマの枠線をなくす", lambda: self._set_selected_frame({"border_mm": 0}))
        self.act_bleed = a("選んだコマを断ち切りにする（紙の端まで）", self._toggle_bleed)
        self.act_reset_shape = a("選んだコマの形を元に戻す", lambda: self._set_selected_frame({"poly": None}))
        self.act_template = a("テンプレートでコマを割る…", self._templates, tip="今のページのコマと台詞を作り直します")
        self.act_add_page = a("ページを追加", self._add_page)
        self.act_del_page = a("このページを削除…", self._del_page)
        self.act_name_ok = a("ネーム完了 → 作画へ進む", self._name_ok, tip="承認の要らない原稿（エージェントを使わない原稿）で使います")

        bar = self.menuBar()
        menus = [
            ("ファイル", [self.act_new, self.act_open, None, self.act_save, self.act_save_as, None, self.act_import, self.act_export]),
            ("編集", [self.act_undo, self.act_redo, None, self.act_cut, self.act_copy, self.act_paste]),
            ("表示", [self.act_fit, self.act_zoom_in, self.act_zoom_out, self.act_actual, None, self.act_prev, self.act_next,
                      None, self.act_guides, self.act_onion]),
            ("ツール", [self.act_select, self.act_pen, self.act_eraser, self.act_text, self.act_frame, None, self.act_picker,
                        self.act_fill, self.act_lassofill, self.act_reshape, None, self.act_marquee, self.act_lasso, self.act_wand, None,
                        self.act_color, self.act_thicker, self.act_thinner]),
            ("定規", [self.act_ruler, None, *self.ruler_actions, None, self.act_snap, self.act_show_rulers, self.act_del_ruler,
                      self.act_clear_rulers, None, self.act_grid, self.act_grid_snap, self.act_grid_mm]),
            ("3D", [self.act_3d, None, self.act_add_figure, self.act_add_box, None, *self.pose_actions, None, self.act_trace,
                    self.act_del_prim]),
            ("トーン・効果線", [self.act_tone_here, self.act_tone_click, None, self.act_effect, *self.effect_actions, None,
                               self.act_materials]),
            ("選択", [self.act_marquee, self.act_lasso, self.act_wand, None, self.act_select_all, self.act_deselect, None,
                      self.act_cut, self.act_copy, self.act_paste, self.act_delete_area, None, self.act_flip_h, self.act_flip_v, None,
                      self.act_fill_selection, self.act_line_width]),
            ("コマ", [self.act_frame, None, self.act_split_h, self.act_split_v, self.act_merge, None, self.act_template, None,
                      self.act_gutters, self.act_border, self.act_no_border, self.act_bleed, self.act_reset_shape]),
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
        for act in (self.act_select, self.act_pen, self.act_eraser, self.act_fill, self.act_marquee, self.act_picker, self.act_text,
                    self.act_frame, self.act_ruler, self.act_3d, self.act_effect, None, self.act_undo, self.act_redo, None,
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
        self.guides = GuidePanel(self)
        self.materials = MaterialPanel(self)
        for widget in (self.approvals, self.panel_view):
            widget.changed.connect(self._reload_pages)
        self.brush = BrushPanel()
        self.brush.changed.connect(self._brush_changed)
        brush_dock = QDockWidget("ブラシ", self)
        brush_scroll = QScrollArea()
        brush_scroll.setWidgetResizable(True)
        brush_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        brush_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        brush_scroll.setWidget(self.brush)
        brush_dock.setWidget(brush_scroll)
        brush_dock.setObjectName("ブラシ")
        brush_dock.setMinimumWidth(self.brush.minimumSizeHint().width() + 20)
        brush_dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, brush_dock)
        self.view_menu.addAction(brush_dock.toggleViewAction())
        self.brush_dock = brush_dock
        docks = []
        for title, widget in (("承認箱", self.approvals), ("コマ", self.panel_view), ("台詞", self.story),
                              ("レイヤー", self.layers), ("素材", self.materials), ("定規・3D", self.guides), ("ライブラリ", self.library)):
            dock = QDockWidget(title, self)
            if widget in (self.panel_view, self.story, self.layers, self.guides, self.materials):
                # tall panels scroll on a small screen instead of making the window taller
                scroll = QScrollArea()
                scroll.setWidgetResizable(True)
                scroll.setFrameShape(QScrollArea.Shape.NoFrame)
                scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
                if widget not in (self.guides, self.materials):  # these two grow and shrink with their contents
                    widget.setMinimumHeight(max(420, widget.minimumSizeHint().height()))
                scroll.setWidget(widget)
                dock.setWidget(scroll)
            else:
                dock.setWidget(widget)
            dock.setObjectName(title)
            dock.setMinimumWidth(300)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable)
            self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
            self.view_menu.addAction(dock.toggleViewAction())
            dock.visibilityChanged.connect(lambda shown, d=dock: shown and d in self._stale_docks and self._refresh_dock(d))
            docks.append(dock)
        for first, second in zip(docks, docks[1:]):
            self.tabifyDockWidget(first, second)
        docks[0].raise_()
        self.resizeDocks([docks[0], self.brush_dock], [340, self.brush.minimumSizeHint().width() + 20], Qt.Orientation.Horizontal)
        self.studio_docks = docks

    def show_dock(self, title: str) -> None:
        for dock in self.studio_docks:
            if dock.windowTitle() == title:
                dock.show()
                dock.raise_()

    def _dock_visible(self, dock) -> bool:
        return dock.isVisible() and not dock.visibleRegion().isEmpty()

    def _refresh_dock(self, dock) -> None:
        self._stale_docks.discard(dock)
        widget = {"承認箱": self.approvals, "コマ": self.panel_view, "台詞": self.story, "レイヤー": self.layers,
                  "素材": self.materials, "定規・3D": self.guides, "ライブラリ": self.library}[dock.windowTitle()]
        if widget is self.panel_view:
            self._sync_panel_view()
        widget.refresh()

    def _refresh_visible_docks(self) -> None:
        self.process.refresh(self.episode)
        for dock in self.studio_docks:
            if self._dock_visible(dock):
                self._refresh_dock(dock)
            else:
                self._stale_docks.add(dock)

    def _sync_panel_view(self) -> None:
        page = self._current()
        if self.panel_view.frame_id and (page is None or not self._has_frame(page, self.panel_view.frame_id)):
            self.panel_view.frame_id = None
        if self.panel_view.frame_id is None and page is not None and page.selected_frame_id and self._has_frame(page, page.selected_frame_id):
            self.panel_view.frame_id = page.selected_frame_id

    def _refresh_studio(self) -> None:
        self._stale_docks.clear()
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
            self.canvas.set_selection(None)
        self._page_index = row
        self._show_page()

    def _show_page(self, light: bool = False) -> None:
        page = self._current()
        lines = self.episode.story_for_page(page.index) if page else []
        self.canvas.overlay_name_strokes = bool(page and page.name_ok and page.stage != "name")
        self.canvas.brush_width_mm = self.brush.size.value() if hasattr(self, "brush") else float(self.episode.brush_width_mm)
        self.canvas.eraser_mm = self.eraser_mm
        if page is not None and self._target_layer_id and not any(layer.id == self._target_layer_id for layer in page.layers):
            self._target_layer_id = None  # another page: back to its default layer
        self.canvas.set_page(page, lines)
        self._refresh_status()
        if not hasattr(self, "process"):
            return
        if light:
            self._dock_timer.start()
        else:
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

    def flash(self, message: str, ms: int = 3000) -> None:
        """A short notice in the status line (it goes back to the page's status after `ms`)."""
        self.status.setText(f"<b>{message}</b>")
        self.last_notice = message
        QTimer.singleShot(ms, self._refresh_status)

    def _refresh_zoom(self) -> None:
        self.zoom_label.setText(f"表示 {self.canvas.zoom_percent()}%")

    # --- editing -----------------------------------------------------------------------------

    def _tool(self, tool: str) -> None:
        if tool in ("rect", "lasso", "wand"):
            self.canvas.marquee = tool
            self.canvas.set_tool("marquee")
        else:
            self.canvas.set_tool(tool)
        self.tool_actions[tool].setChecked(True)

    # --- the layer the pen works on ---------------------------------------------------------------

    def target_layer(self):
        from genko.models import LayerRole

        page = self._current()
        if page is None:
            return None
        found = next((layer for layer in page.layers if layer.id == self._target_layer_id), None)
        if found is not None:
            return found
        role = LayerRole.NAME if page.stage == "name" else LayerRole.INK
        return next((layer for layer in page.layers if layer.role == role), None)

    def set_target_layer(self, layer_id: str) -> None:
        self._target_layer_id = layer_id
        layer = self.target_layer()
        if layer is not None:
            tone = getattr(layer.kind, "value", "") == "tone"
            self.flash(f"描く先: {wording.layer_label(layer)}" + ("（ペンでトーンを足す・消しゴムで削る）" if tone else ""), 2500)
        if hasattr(self, "materials"):
            self.materials.refresh()

    @staticmethod
    def drawable(layer) -> bool:
        kind = getattr(layer.kind, "value", str(layer.kind))
        return kind in ("strokes", "raster", "tone") and not getattr(layer, "locked", False)

    def selected_frame(self):
        page = self._current()
        if page is None or not page.selected_frame_id or not self._has_frame(page, page.selected_frame_id):
            return None
        return page._find(page.selected_frame_id)

    def frame_by_id(self, frame_id: str | None):
        page = self._current()
        if page is None or not frame_id or not self._has_frame(page, frame_id):
            return None
        return page._find(frame_id)

    def _on_stroke(self, points: list) -> None:
        page = self._current()
        layer = self.target_layer()
        if page is None or layer is None:
            return
        if not self.drawable(layer):
            why = "ロックされています" if getattr(layer, "locked", False) else "ペンかペイントのレイヤーではありません"
            self.flash(f"「{wording.layer_label(layer)}」には描けません（{why}）。レイヤー パネルで選び直します", 4000)
            return
        if self.canvas.tool == "eraser":
            op = {"op": "erase", "page": page.index, "layer_id": layer.id, "points": [[p[0], p[1]] for p in points],
                  "width_mm": self.eraser_mm}
            if getattr(layer.kind, "value", "") == "tone":
                if self.materials.soft.isChecked():
                    op["soft"] = True
            elif self.brush.crossing.isChecked():
                op["mode"] = "to_crossing"
            self.apply_ops([op])
            return
        op = {"op": "add_stroke", "page": page.index, "layer_id": layer.id, "points": points, **self.brush.stroke_fields()}
        if self.canvas.snap_rulers and page.rulers:
            op["snap_ruler"] = True
        self.apply_ops([op])

    def _onion(self) -> None:
        page = self._current()
        if page is None or page.index < 2:
            return
        self.apply_ops([{"op": "step_onion", "page": page.index, "delta": -1}])

    def _pick_color(self) -> None:
        self.brush._pick()

    def _nudge_brush(self, step: int) -> None:
        sizes = [0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0, 15.0, 20.0]

        def nxt(value: float) -> float:
            i = min(range(len(sizes)), key=lambda k: abs(sizes[k] - value))
            return sizes[max(0, min(len(sizes) - 1, i + step))]

        if self.canvas.tool == "eraser":
            self.eraser_mm = nxt(self.eraser_mm)
            self.canvas.eraser_mm = self.eraser_mm
            self.flash(f"消しゴムの太さ {self.eraser_mm:g} mm", 2000)
            self.canvas.update()
            return
        width = self.brush.nudge_size(step)
        self.canvas.brush_width_mm = width
        self.canvas.update()
        self.flash(f"ペンの太さ {width:g} mm", 2000)

    def _brush_changed(self) -> None:
        self.canvas.brush_width_mm = self.brush.size.value()
        self.canvas.update()

    # --- colour, fills, selections, line fixes (M13) ---------------------------------------------

    def _paint_layer(self):
        """The target layer when it can be painted on; otherwise a notice and None."""
        page, layer = self._current(), self.target_layer()
        if page is None or layer is None:
            return None
        if not self.drawable(layer):
            why = "ロックされています" if getattr(layer, "locked", False) else "ペンかペイントのレイヤーではありません"
            self.flash(f"「{wording.layer_label(layer)}」には描けません（{why}）。レイヤー パネルで選び直します", 4000)
            return None
        return layer

    def _on_colour_picked(self, rgb) -> None:
        self.brush.set_colour(rgb)
        self.flash(f"色を拾いました {tuple(rgb)}", 2000)

    def _paint_fields(self) -> dict:
        out = {"rgb": list(self.brush.rgb)}
        opacity = self.brush.opacity.value() / 100
        if opacity < 1:
            out["opacity"] = round(opacity, 3)
        return out

    def _fill_at(self, x_mm: float, y_mm: float) -> None:
        layer = self._paint_layer()
        if layer is None:
            return
        self.apply_ops([{"op": "fill", "page": self._current().index, "layer_id": layer.id, "x_mm": round(x_mm, 2),
                         "y_mm": round(y_mm, 2), "gap_mm": self.brush.gap.value(), "reference": self.brush.reference.currentData(),
                         **self._paint_fields()}])

    def _area(self) -> dict | None:
        return self.canvas.selection["area"] if self.canvas.selection else None

    def _need_area(self) -> dict | None:
        area = self._area()
        if area is None:
            self.flash("先に範囲を選びます（範囲選択 M・投げ縄 L・自動選択 W）", 3000)
        return area

    def _fill_area(self, area: dict | None) -> None:
        layer = self._paint_layer()
        if layer is None or area is None:
            if area is None:
                self._need_area()
            return
        self.apply_ops([{"op": "fill_area", "page": self._current().index, "layer_id": layer.id, "area": area, **self._paint_fields()}])

    def _wand(self, x_mm: float, y_mm: float) -> None:
        from genko import fill as fills
        from genko import selection
        from genko.ops import _fill_reference

        page, layer = self._current(), self.target_layer()
        if page is None or layer is None:
            return
        dpi = fills.FILL_DPI
        reference = _fill_reference(self.episode, page, layer, self.brush.reference.currentData() or "page", dpi)
        window = None
        panel = page.frame_at(x_mm, y_mm)
        if panel is not None:
            r = panel.rect
            window = (max(0, fills.px(r.x - 2, dpi)), max(0, fills.px(r.y - 2, dpi)),
                      min(reference.width, fills.px(r.x + r.width + 2, dpi)), min(reference.height, fills.px(r.y + r.height + 2, dpi)))
        mask = fills.region_mask(reference, (fills.px(x_mm, dpi), fills.px(y_mm, dpi)), gap_px=fills.px(self.brush.gap.value(), dpi),
                                 window=window)
        area = selection.wand_area(mask, dpi) if mask is not None else None
        if area is None:
            self.flash("そこは線の上です。線で囲まれた中をクリックします", 3000)
            return
        self.canvas.set_selection(area)

    @staticmethod
    def _moved_area(area: dict, matrix) -> dict:
        from genko import selection

        if area.get("poly"):
            return {"poly": [[round(v, 3) for v in selection.apply(matrix, float(x), float(y))] for x, y in area["poly"]]}
        import base64

        patch = selection.transform_patch({"box": area["mask"]["box"], "mode": "mask",
                                           "png": base64.b64decode(area["mask"]["png"])}, tuple(matrix))
        return {"mask": {"box": patch["box"], "png": base64.b64encode(patch["png"]).decode("ascii")}} if patch else area

    def _transform_selection(self, matrix) -> None:
        area, layer = self._area(), self._paint_layer()
        if area is None or layer is None:
            return
        matrix = [round(float(v), 5) for v in matrix]
        if self.apply_ops([{"op": "transform_area", "page": self._current().index, "layer_id": layer.id, "area": area, "matrix": matrix}]):
            outline = self.canvas._apply(matrix, self.canvas.selection["outline"])
            self.canvas.set_selection(self._moved_area(area, matrix), outline)

    def _flip(self, sx: int, sy: int) -> None:
        area = self._need_area()
        if area is None:
            return
        x0, y0, x1, y1 = self.canvas._sel_box()
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        self._transform_selection([sx, 0, 0, sy, cx - sx * cx, cy - sy * cy])

    def _select_all(self) -> None:
        page = self._current()
        if page is None:
            return
        w, h = page.spec.width_mm, page.spec.height_mm
        if self.canvas.tool != "marquee":
            self._tool("rect")
        self.canvas.set_selection({"poly": [[0, 0], [w, 0], [w, h], [0, h]]})

    def _delete_area(self) -> None:
        if self.canvas.tool == "ruler" and self.canvas.selected_ruler_id:
            self.guides.delete_ruler()
            return
        if self.canvas.tool == "3d" and self.canvas.selected_prim_id:
            self.guides.delete_prim()
            return
        if self._area() is None and self.canvas.tool == "select" and self.canvas.selected_line_id:
            self.apply_ops([{"op": "delete_line", "id": self.canvas.selected_line_id}])  # Delete on a picked balloon
            self.canvas.selected_line_id = None
            return
        area, layer = self._need_area(), None
        if area is None:
            return
        layer = self._paint_layer()
        if layer is not None:
            self.apply_ops([{"op": "delete_area", "page": self._current().index, "layer_id": layer.id, "area": area}])

    def _copy(self) -> bool:
        import copy

        from genko import selection

        area, layer = self._need_area(), self.target_layer()
        if area is None or layer is None:
            return False
        items = selection.lift(copy.deepcopy(layer), area, self._current())
        if not items["strokes"] and not items["patches"]:
            self.flash("選んだ範囲に、このレイヤーの絵がありません", 3000)
            return False
        self._clipboard = selection.items_to_json(items)
        self._clipboard_outline = [list(p) for p in self.canvas.selection["outline"]]
        self._clipboard_area = area
        self.flash("コピーしました（Ctrl+V で新しいレイヤーに貼り付け）", 2500)
        return True

    def _cut(self) -> None:
        if self._copy():
            self._delete_area()

    def _paste(self) -> None:
        from genko.models import new_id

        page, layer = self._current(), self.target_layer()
        if page is None or not self._clipboard:
            self.flash("貼り付けるものがありません（先にコピー）", 2500)
            return
        new_layer = new_id()
        ops = [{"op": "add_layer", "page": page.index, "name": "貼り付け", "kind": "pen", "id": new_layer},
               {"op": "paste", "page": page.index, "layer_id": new_layer, "items": self._clipboard}]
        if layer is not None:
            ops[0]["after"] = layer.id
        if self.apply_ops(ops):
            self._target_layer_id = new_layer
            if self.canvas.tool != "marquee":
                self._tool("rect")
            self.canvas.set_selection(self._clipboard_area, self._clipboard_outline)
            self.flash("新しいレイヤー「貼り付け」に置きました。選択範囲の中をドラッグで動かせます", 3500)

    def _line_width(self) -> None:
        from PySide6.QtWidgets import QInputDialog

        area, layer = self._need_area(), self._paint_layer()
        if area is None or layer is None:
            return
        value, ok = QInputDialog.getDouble(self, "線の太さ", "選んだ範囲の線の太さ（mm）", self.brush.size.value(), 0.05, 50, 2)
        if ok:
            self.apply_ops([{"op": "set_stroke_width", "page": self._current().index, "layer_id": layer.id, "area": area,
                             "width_mm": value}])

    # --- rulers, grid, 3D (M14) ----------------------------------------------------------------------

    def _guide_toggles(self) -> None:
        self.canvas.snap_rulers = self.act_snap.isChecked()
        self.canvas.rulers_visible = self.act_show_rulers.isChecked()
        self.canvas.grid_visible = self.act_grid.isChecked()
        self.canvas.grid_snap = self.act_grid_snap.isChecked()
        settings = QSettings("Genko", "Genko Studio")
        for act, key in ((self.act_snap, "snap"), (self.act_show_rulers, "show"), (self.act_grid, "grid"), (self.act_grid_snap, "grid_snap")):
            settings.setValue(f"guides/{key}", act.isChecked())
        self.canvas.update()

    def _grid_spacing(self) -> None:
        from PySide6.QtWidgets import QInputDialog

        value, ok = QInputDialog.getDouble(self, "グリッドの間隔", "間隔（mm）", self.canvas.grid_mm, 0.5, 100, 1)
        if ok:
            self.canvas.grid_mm = value
            QSettings("Genko", "Genko Studio").setValue("guides/grid_mm", value)
            if not self.act_grid.isChecked():
                self.act_grid.setChecked(True)
                self._guide_toggles()
            self.canvas.update()

    def _choose_ruler(self, kind: str, vps: int = 1, copies: int = 2, ask: bool = False) -> None:
        if ask:
            from PySide6.QtWidgets import QInputDialog

            copies, ok = QInputDialog.getInt(self, "対称定規", "写しの数（中心の周りに）", 6, 3, 32)
            if not ok:
                return
        self.canvas.ruler_kind = kind
        self.canvas.ruler_vps = vps
        self.canvas.ruler_copies = copies
        self._tool("ruler")
        title = next(t for t, k, o, _ in self.ruler_kinds if k == kind and (kind != "perspective" or o.get("vps") == vps)
                     and (kind != "symmetry" or bool(o.get("ask")) == ask))
        tip = next(tp for t, _k, _o, tp in self.ruler_kinds if t == title)
        self.flash(f"{title.rstrip('…')}: {tip}", 5000)

    def _place_ruler(self, ruler: dict) -> None:
        page = self._current()
        if page is None:
            return
        from genko.models import new_id

        ruler_id = new_id()
        if self.apply_ops([{"op": "add_ruler", "page": page.index, "id": ruler_id, **ruler}]):
            self.canvas.selected_ruler_id = ruler_id
            if not self.act_snap.isChecked():
                self.act_snap.setChecked(True)
                self._guide_toggles()
            self.flash("定規を置きました。ペン（B）で描くと沿います（Ctrl+2 で切り替え）", 3500)

    def _clear_rulers(self) -> None:
        page = self._current()
        if page is not None and page.rulers:
            self.apply_ops([{"op": "delete_ruler", "page": page.index}])
            self.canvas.selected_ruler_id = None

    def _add_prim(self, kind: str) -> None:
        from genko.frames import contains as geo_contains
        from genko.models import new_id

        page = self._current()
        if page is None:
            return
        frame = self.selected_frame()
        r = frame.rect if frame is not None else page.inner_rect_mm()
        cx, cy = r.x + r.width / 2, r.y + r.height / 2
        shift = 12.0 * sum(1 for p in page.prims if frame is None or geo_contains(frame, *(p.get("pos") or [0, 0])[:2]))
        cx, cy = cx + shift, cy + shift * 0.5  # the next one beside the last, not on top of it
        prim_id = new_id()
        if kind == "mannequin":
            height = round(max(30.0, min(140.0, r.height * 0.8)), 1)
            op = {"op": "add_mannequin", "page": page.index, "id": prim_id, "pos": [cx, cy + height * 0.05, 0], "height_mm": height}
        else:
            side = round(max(15.0, min(80.0, min(r.width, r.height) * 0.4)), 1)
            op = {"op": "add_prim3d", "page": page.index, "kind": "box", "id": prim_id, "pos": [cx, cy, 0], "size": [side, side, side]}
        if self.apply_ops([op]):
            self.canvas.selected_prim_id = prim_id
            self._tool("3d")
            self.show_dock("定規・3D")
            self.guides.refresh()

    def _pose(self, preset: str) -> None:
        page, prim_id = self._current(), self.canvas.selected_prim_id
        prim = next((p for p in (page.prims if page else []) if p.get("id") == prim_id and p.get("kind") == "mannequin"), None)
        if prim is None:
            prim = next((p for p in (page.prims if page else []) if p.get("kind") == "mannequin"), None)
        if prim is None:
            self.flash("先にデッサン人形を置きます（3D → デッサン人形を置く）", 3000)
            return
        self.apply_ops([{"op": "pose_mannequin", "page": page.index, "id": prim["id"], "preset": preset}])

    def trace_prims(self, selected_only: bool = False) -> None:
        page = self._current()
        layer = self._paint_layer()
        if page is None or layer is None:
            return
        if not page.prims:
            self.flash("このページに 3D がありません", 2500)
            return
        op = {"op": "trace_prims", "page": page.index, "layer_id": layer.id}
        if selected_only and self.canvas.selected_prim_id:
            op["ids"] = [self.canvas.selected_prim_id]
        if self.apply_ops([op]):
            self.flash(f"「{wording.layer_label(layer)}」に線で写しました", 3000)

    # --- tones, effect lines, materials (M15) -------------------------------------------------------------

    def _default_tone(self) -> dict:
        item = self.materials.current_material()
        if item is not None and item.get("kind") == "tone":
            return item
        from genko.materials import get_material

        return get_material("dot-60-30")

    def _after_tone(self, layer_id: str) -> None:
        self.set_target_layer(layer_id)
        self.layers.refresh()

    def _tone_here(self) -> None:
        self._put_tone(self._default_tone(), ask_click=True)

    def _tone_click(self) -> None:
        self._pending_material = self._default_tone()
        self._tool("stamp")
        self.flash(f"「{self._pending_material.get('name')}」: 線で囲まれた所をクリックすると、そこにトーンが入ります", 4000)

    def _put_tone(self, item: dict, ask_click: bool = False) -> None:
        from genko.models import new_id

        page = self._current()
        if page is None:
            return
        op = {"op": "stamp_material", "page": page.index, "material_id": item["id"], "id": new_id()}
        if self.canvas.selection:
            op["area"] = self.canvas.selection["area"]
        elif self.selected_frame() is not None:
            op["frame_id"] = self.selected_frame().id
        elif ask_click:
            self._pending_material = item
            self._tool("stamp")
            self.flash("選択範囲もコマも選ばれていません。トーンを貼る所をクリックします", 4000)
            return
        if self.apply_ops([op]):
            self._after_tone(op["id"])
            self.flash(f"「{item.get('name')}」を貼りました。ペンで足す・消しゴムで削る", 3500)

    def use_material(self, item: dict) -> None:
        kind = item.get("kind")
        if kind == "tone":
            self._put_tone(item, ask_click=True)
            return
        if kind == "effect" and self.selected_frame() is not None:
            self.apply_ops([{"op": "stamp_material", "page": self._current().index, "material_id": item["id"],
                             "frame_id": self.selected_frame().id}])
            return
        self._pending_material = item
        self._tool("stamp")
        where = "コマの中" if kind == "effect" else "置きたい所"
        self.flash(f"「{item.get('name')}」: {where}をクリックします", 3500)

    def _stamp_at(self, x_mm: float, y_mm: float) -> None:
        from genko.models import new_id

        page, item = self._current(), self._pending_material or self.materials.current_material()
        if page is None or item is None:
            self.flash("素材パネルで素材を選びます", 2500)
            return
        op = {"op": "stamp_material", "page": page.index, "material_id": item["id"]}
        kind = item.get("kind")
        if kind == "tone":
            op["id"] = new_id()
            op["at"] = {"x_mm": round(x_mm, 2), "y_mm": round(y_mm, 2), "gap_mm": self.brush.gap.value()}
            if self.apply_ops([op]):
                self._after_tone(op["id"])
            return
        if kind == "effect":
            frame = page.frame_at(x_mm, y_mm)
            op.update({"frame_id": frame.id if frame else None, "x_mm": round(x_mm, 2), "y_mm": round(y_mm, 2)})
            self.apply_ops([op])
            return
        layer = self._paint_layer()
        if layer is None:
            return
        op.update({"layer_id": layer.id, "x_mm": round(x_mm, 2), "y_mm": round(y_mm, 2)})
        self.apply_ops([op])

    def _choose_effect(self, kind: str) -> None:
        from genko.effects import LABELS

        self._effect_kind = kind
        self._tool("effect")
        self.flash(f"{LABELS[kind]}: コマの中をクリックします（集中線・フラッシュはそこが中心）", 3500)

    def _effect_at(self, x_mm: float, y_mm: float) -> None:
        from genko.models import new_id

        page = self._current()
        if page is None:
            return
        frame = page.frame_at(x_mm, y_mm)
        params = {}
        if self._effect_kind in ("focus", "uni_flash", "beta_flash"):
            params["center"] = [round(x_mm, 2), round(y_mm, 2)]
        effect_id = new_id()
        if self.apply_ops([{"op": "add_effect", "page": page.index, "kind": self._effect_kind, "id": effect_id,
                            "frame_id": frame.id if frame else None, "params": params}]):
            self.canvas.selected_effect_id = effect_id
            self.show_dock("素材")
            self.materials.refresh()
            self.materials.select_effect(effect_id)

    def copy_selection_items(self) -> dict | None:
        import copy

        from genko import selection

        area, layer = self._need_area(), self.target_layer()
        if area is None or layer is None:
            return None
        items = selection.lift(copy.deepcopy(layer), area, self._current())
        if not items["strokes"] and not items["patches"]:
            self.flash("選んだ範囲に、描く先のレイヤーの絵がありません", 3000)
            return None
        return selection.items_to_json(items)

    def _reshape(self, stroke_id: str, points) -> None:
        layer = self._paint_layer()
        if layer is not None:
            self.apply_ops([{"op": "reshape_stroke", "page": self._current().index, "layer_id": layer.id, "stroke_id": stroke_id,
                             "points": [[round(float(v), 3) for v in p] for p in points]}])

    def _toggle_guides(self) -> None:
        self.canvas.show_guides = self.act_guides.isChecked()
        self.canvas.update()

    # --- lettering on the page ------------------------------------------------------------------------

    def _line(self, line_id: str):
        return next((ln for ln in self.episode.story if ln.id == line_id), None)

    def _type_new_line(self, x_mm: float, y_mm: float) -> None:
        page = self._current()
        if page is None:
            return

        def done(typed: str | None) -> None:
            from genko.app.lettering import parse_ruby, place_at

            if not typed:
                return
            text, runs = parse_ruby(typed)
            frame = page.frame_at(x_mm, y_mm)
            box = place_at(x_mm, y_mm, text, "speech", True, frame)
            op = {"op": "add_line", "page": page.index, "text": text, "balloon": "speech", **box}
            if frame is not None:
                op["frame_id"] = frame.id
            if runs:
                op["ruby_runs"] = runs
            before = {ln.id for ln in self.episode.story}
            if self.apply_ops([op]):
                added = next((ln.id for ln in self.episode.story if ln.id not in before), None)
                self.canvas.selected_line_id = added
                self._tool("select")
                self._on_line_selected(added, False)

        self.canvas.open_editor(x_mm, y_mm, "", done)

    def _edit_line_inline(self, line_id: str) -> None:
        from genko.app.lettering import with_ruby

        line = self._line(line_id)
        if line is None:
            return

        def done(typed: str | None) -> None:
            from genko.app.lettering import parse_ruby, refit

            current = self._line(line_id)
            if not typed or current is None:
                return
            text, runs = parse_ruby(typed)
            if (text, [list(r) for r in runs]) == (current.text, [list(r) for r in current.ruby_runs]):
                return
            ops = [{"op": "edit_line", "id": line_id, "text": text, "ruby_runs": runs}]
            size = refit(current, self.frame_by_id(current.frame_id), text, current.balloon, current.wrap == "vertical")
            # keep the balloon's centre where it was
            cx, cy = current.x_mm + current.w_mm / 2, current.y_mm + current.h_mm / 2
            ops.append({"op": "move_line", "id": line_id, "x_mm": round(cx - size["w_mm"] / 2, 2), "y_mm": round(cy - size["h_mm"] / 2, 2),
                        "w_mm": size["w_mm"], "h_mm": size["h_mm"]})
            self.apply_ops(ops)

        self.canvas.open_editor(line.x_mm, line.y_mm, with_ruby(line.text, line.ruby_runs), done)

    def _line_menu(self, line_id: str, pos: QPointF) -> None:
        from genko.app.lettering import KINDS
        from genko.balloons import style_of

        line = self._line(line_id)
        if line is None:
            return
        self._on_line_selected(line_id, False)
        menu = QMenu(self)
        edit = menu.addAction("打ち直す（ダブルクリック）")
        edit.triggered.connect(lambda: self._edit_line_inline(line_id))
        shapes = menu.addMenu("フキダシの形")
        for key, label in KINDS:
            act = shapes.addAction(label)
            act.setCheckable(True)
            act.setChecked(line.balloon == key)
            act.triggered.connect(lambda _=False, k=key: self.apply_ops([{"op": "edit_line", "id": line_id, "balloon": k}]))
        direction = menu.addAction("横書きにする" if line.wrap == "vertical" else "縦書きにする")
        direction.triggered.connect(lambda: self.apply_ops([{"op": "edit_line", "id": line_id,
                                                             "wrap": "horizontal" if line.wrap == "vertical" else "vertical"}]))
        menu.addSeparator()
        tails = self.canvas._tails(line)
        add_tail = menu.addAction("しっぽを足す")
        add_tail.triggered.connect(lambda: self.apply_ops([{"op": "move_line", "id": line_id, "tails": tails + [
            {"to": [round(line.x_mm - line.w_mm * 0.2, 2), round(line.y_mm + line.h_mm * 1.25, 2)]}]}]))
        if tails:
            drop = menu.addAction("しっぽを 1 本消す")
            drop.triggered.connect(lambda: self.apply_ops([{"op": "move_line", "id": line_id, "tails": tails[:-1]}]))
            straight = menu.addAction("しっぽをまっすぐにする")
            straight.triggered.connect(lambda: self.apply_ops([{"op": "move_line", "id": line_id,
                                                                "tails": [{"to": t["to"]} for t in tails]}]))
        menu.addSeparator()
        group = style_of(line)["group"]
        lines = self.episode.story_for_page(line.page_index)
        index = next(i for i, ln in enumerate(lines) if ln.id == line_id)
        if index + 1 < len(lines):
            nxt = lines[index + 1]
            join = menu.addAction("次の台詞のフキダシとつなげる")
            key = group or f"g_{line_id[:8]}"
            join.triggered.connect(lambda: self.apply_ops([{"op": "edit_line", "id": line_id, "style": {"group": key}},
                                                          {"op": "edit_line", "id": nxt.id, "style": {"group": key}}]))
        if group:
            split = menu.addAction("つなげたフキダシを離す")
            split.triggered.connect(lambda: self.apply_ops([{"op": "edit_line", "id": ln.id, "style": {"group": None}}
                                                           for ln in lines if style_of(ln)["group"] == group]))
        menu.addSeparator()
        delete = menu.addAction("削除")
        delete.triggered.connect(lambda: self.apply_ops([{"op": "delete_line", "id": line_id}]))
        menu.exec(pos.toPoint())

    def _on_line_selected(self, line_id: str, open_panel: bool) -> None:
        self.story.refresh()
        self.story.select(line_id)
        if open_panel:
            self.show_dock("台詞")

    def _import_image(self) -> None:
        from io import BytesIO

        from PIL import Image

        from genko.assets import AssetStore

        page = self._current()
        if page is None:
            return
        if self.path is None:
            QMessageBox.information(self, "Genko", "画像を読み込む前に、原稿を保存します（ファイル → 別の場所に保存）")
            return
        path, _ = QFileDialog.getOpenFileName(self, "画像を読み込む", "", "画像 (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.psd)")
        if not path:
            return
        try:
            with Image.open(path) as img:
                img.load()
                buf = BytesIO()
                img.convert("RGBA" if img.mode in ("RGBA", "LA", "P") else "RGB").save(buf, format="PNG")
        except Exception as exc:  # unreadable file
            QMessageBox.warning(self, "Genko", f"読み込めない画像です:\n{exc}")
            return
        self.commit_now()
        ref = AssetStore(self.path).put_bytes(buf.getvalue(), ".png")
        frame = self.selected_frame()
        place = {"op": "place_asset", "page": page.index, "asset": ref, "to": "art", "title": Path(path).stem}
        if frame is not None:
            place["frame_id"] = frame.id
        else:
            place["placement_mm"] = [0, 0, page.spec.width_mm, page.spec.height_mm]
        ops = [{"op": "register_assets", "assets": {ref: {"kind": "image", "origin": {"kind": "self", "file": Path(path).name}}}}, place]
        if self.apply_ops(ops):
            where = "選んだコマ" if frame is not None else "ページ全体"
            self.flash(f"{where}に「{Path(path).name}」を置きました", 4000)

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
        self.panel_view.refresh()  # a deliberate click: show the panel at once

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
        gutter = self.gutter_mm("horizontal" if axis == "horizontal" else "vertical")
        self.apply_ops([{"op": "split_frame", "page": page.index, "axis": axis, "frame_id": page.selected_frame_id, "gutter_mm": gutter}])

    # --- panels: gutters, borders, templates ------------------------------------------------------------

    def gutter_mm(self, cut: str) -> float:
        """The gutter for a new cut: between tiers (a horizontal cut) or between side-by-side panels."""
        from PySide6.QtCore import QSettings

        settings = QSettings("Genko", "Genko Studio")
        default = 6.0 if cut == "horizontal" else 3.0
        try:
            return float(settings.value(f"gutter_{cut}", default))
        except (TypeError, ValueError):
            return default

    def _gutter_settings(self) -> None:
        from PySide6.QtCore import QSettings
        from PySide6.QtWidgets import QDialogButtonBox, QDoubleSpinBox, QFormLayout

        dialog = QDialog(self)
        dialog.setWindowTitle("コマ間隔")
        spins = {}
        form = QFormLayout(dialog)
        for key, label in (("horizontal", "上下の間隔（段と段の間）"), ("vertical", "左右の間隔（横に並ぶコマの間）")):
            spin = QDoubleSpinBox()
            spin.setRange(0, 30)
            spin.setSingleStep(0.5)
            spin.setSuffix(" mm")
            spin.setValue(self.gutter_mm(key))
            form.addRow(label, spin)
            spins[key] = spin
        form.addRow(QLabel("これから割るコマに使います。今ある間隔は、コマ ツール（F）で間の白をドラッグして変えます。"))
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow(buttons)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            settings = QSettings("Genko", "Genko Studio")
            for key, spin in spins.items():
                settings.setValue(f"gutter_{key}", spin.value())

    def _cut_frame(self, frame_id: str, p0: QPointF, p1: QPointF) -> None:
        page = self._current()
        horizontal = abs(p1.x() - p0.x()) >= abs(p1.y() - p0.y())
        self.apply_ops([{"op": "cut_frame", "page": page.index, "frame_id": frame_id, "p0": [round(p0.x(), 2), round(p0.y(), 2)],
                         "p1": [round(p1.x(), 2), round(p1.y(), 2)], "gutter_mm": self.gutter_mm("horizontal" if horizontal else "vertical")}])

    def _set_selected_frame(self, change: dict) -> None:
        frame = self.selected_frame()
        if frame is None:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選びます")
            return
        self.apply_ops([{"op": "set_frame", "page": self._current().index, "frame_id": frame.id, **change}])

    def _border_width(self) -> None:
        from PySide6.QtWidgets import QInputDialog

        frame = self.selected_frame()
        if frame is None:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選びます")
            return
        value, ok = QInputDialog.getDouble(self, "枠線の太さ", "枠線の太さ（mm）", float(frame.border_mm), 0.0, 5.0, 2)
        if ok:
            self._set_selected_frame({"border_mm": value})

    def _toggle_bleed(self) -> None:
        frame = self.selected_frame()
        if frame is None:
            QMessageBox.information(self, "Genko", "先にコマをクリックして選びます")
            return
        self._set_selected_frame({"bleed": not frame.bleed})
        self.flash("断ち切りにしました（紙の端に接する辺は枠線なし）" if not frame.bleed else "断ち切りをやめました")

    def _templates(self) -> None:
        from genko.app.dialogs import TemplateDialog

        page = self._current()
        if page is None:
            return
        dialog = TemplateDialog(self, self.episode, page)
        if dialog.exec() != QDialog.DialogCode.Accepted or not dialog.ops:
            return
        if dialog.needs_clearing and QMessageBox.question(
                self, "Genko", f"{page.index} ページのコマと台詞を消して、テンプレートで割り直します。\n（元に戻す で取り消せます）") \
                != QMessageBox.StandardButton.Yes:
            return
        self.apply_ops(dialog.ops)

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
            self.flash(wording.error(str(exc)), 3000)
        self._watch()
        self._reload_pages()

    def _redo(self) -> None:
        try:
            self.session.redo()
        except ApplyError as exc:
            self.flash(wording.error(str(exc)), 3000)
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
        self.flash(f"保存しました: {self.path}", 3000)

    def _save_as(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "原稿を保存する場所", f"{self.episode.title or '無題'}.genko")
        if not path:
            return
        target = Path(path) if path.endswith(".genko") else Path(path + ".genko")
        self.session.save_as(target)
        remember_project(target)
        self._watch()
        self._refresh_status()
        self.flash(f"保存しました: {target}", 3000)

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
