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
        self.pages.setMaximumWidth(220)
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
        tools = QActionGroup(self)
        for act in (self.act_select, self.act_pen, self.act_eraser, self.act_text):
            tools.addAction(act)
        self.act_select.setChecked(True)
        self.act_color = a("ペンの色…", self._pick_color, "C")
        self.act_thicker = a("太く（ペン・消しゴム）", lambda: self._nudge_brush(1), "]")
        self.act_thinner = a("細く（ペン・消しゴム）", lambda: self._nudge_brush(-1), "[")
        self.act_split_h = a("コマを横に割る（上下に分ける）", lambda: self._split("horizontal"), "Ctrl+Shift+H")
        self.act_split_v = a("コマを縦に割る（左右に分ける）", lambda: self._split("vertical"), "Ctrl+Shift+V")
        self.act_merge = a("コマを結合（割る前に戻す）", self._merge, "Ctrl+Shift+M")
        self.act_add_page = a("ページを追加", self._add_page)
        self.act_del_page = a("このページを削除…", self._del_page)
        self.act_name_ok = a("ネーム完了 → 作画へ進む", self._name_ok, tip="承認の要らない原稿（エージェントを使わない原稿）で使います")

        bar = self.menuBar()
        menus = [
            ("ファイル", [self.act_new, self.act_open, None, self.act_save, self.act_save_as, None, self.act_import, self.act_export]),
            ("編集", [self.act_undo, self.act_redo]),
            ("表示", [self.act_fit, self.act_zoom_in, self.act_zoom_out, self.act_actual, None, self.act_prev, self.act_next,
                      None, self.act_guides, self.act_onion]),
            ("ツール", [self.act_select, self.act_pen, self.act_eraser, self.act_text, None, self.act_color, self.act_thicker, self.act_thinner]),
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
        for act in (self.act_select, self.act_pen, self.act_eraser, self.act_text, None, self.act_undo, self.act_redo, None,
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
            if widget in (self.panel_view, self.story, self.layers):
                # tall panels scroll on a small screen instead of making the window taller
                scroll = QScrollArea()
                scroll.setWidgetResizable(True)
                scroll.setFrameShape(QScrollArea.Shape.NoFrame)
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
        self.resizeDocks([docks[0]], [380], Qt.Orientation.Horizontal)
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
                  "ライブラリ": self.library}[dock.windowTitle()]
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
        self._page_index = row
        self._show_page()

    def _show_page(self, light: bool = False) -> None:
        page = self._current()
        lines = self.episode.story_for_page(page.index) if page else []
        self.canvas.overlay_name_strokes = bool(page and page.name_ok and page.stage != "name")
        self.canvas.brush_width_mm = float(self.episode.brush_width_mm)
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
        self.canvas.set_tool(tool)
        {"select": self.act_select, "pen": self.act_pen, "eraser": self.act_eraser, "text": self.act_text}[tool].setChecked(True)

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
            self.flash(f"描く先: {wording.layer_label(layer)}", 2000)

    @staticmethod
    def drawable(layer) -> bool:
        kind = getattr(layer.kind, "value", str(layer.kind))
        return kind in ("strokes", "raster") and not getattr(layer, "locked", False)

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
            self.apply_ops([{"op": "erase", "page": page.index, "layer_id": layer.id,
                             "points": [[p[0], p[1]] for p in points], "width_mm": self.eraser_mm}])
            return
        self.apply_ops([{"op": "add_stroke", "page": page.index, "layer_id": layer.id, "points": points}])

    def _onion(self) -> None:
        page = self._current()
        if page is None or page.index < 2:
            return
        self.apply_ops([{"op": "step_onion", "page": page.index, "delta": -1}])

    def _pick_color(self) -> None:
        color = QColorDialog.getColor(QColor(*self.episode.brush_rgb), self, "ペンの色")
        if color.isValid():
            self.apply_ops([{"op": "set_brush", "rgb": [color.red(), color.green(), color.blue()]}])

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
        width = nxt(float(self.episode.brush_width_mm))
        self.canvas.brush_width_mm = width
        self.apply_ops([{"op": "set_brush", "width_mm": width}])
        self.flash(f"ペンの太さ {width:g} mm", 2000)

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
