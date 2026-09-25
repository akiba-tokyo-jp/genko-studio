from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QPointF, QSettings, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QColor, QIcon, QImage, QKeySequence, QPixmap
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
    QSpinBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from genko.app import wording
from genko.app.brush_panel import BrushPanel
from genko.app.canvas import PageCanvas
from genko.app.guide_panel import PRESETS, GuidePanel
from genko.app.material_panel import MaterialPanel
from genko.app.check_panel import CheckPanel
from genko.app.pages_panel import PageList, nombre_dialog
from genko.app.dialogs import ExportDialog, NewProjectDialog, StartDialog  # noqa: F401  (StartDialog is re-exported)
from genko.app.session import Session
from genko.app.studio_widgets import ApprovalBox, Library, PanelView, ProcessBar
from genko.models import LayerKind, LayerRole, PageSpec, new_episode
from genko.ops import ApplyError

COMMIT_AFTER_MS = 1000
SIDE_WIDTH = 230  # the side panels; the rest of the window is the page  # changes reach the disk after a second without edits


def _pixmap(image) -> QPixmap:
    rgb = image.convert("RGB")
    data = rgb.tobytes()
    qimage = QImage(data, rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888)
    return QPixmap.fromImage(qimage)  # (fromImage copies the pixels, so data may go)


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
        self.text.setPlaceholderText("台詞（改行で次の列へ。ルビは ｜約束《やくそく》、傍点は 《《強調》》、一部を大きく {大|…}・太く {太|…}・赤く {赤|…}）")
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
        self.rotate = spin(-180, 180, 5, "°")
        self.rotate.setDecimals(0)
        self.rotate.setToolTip("フキダシごと回します（選択ツールで、フキダシの上の○をドラッグしても回せます）")
        self.skew = spin(-60, 60, 5, "°")
        self.skew.setDecimals(0)
        self.skew.setToolTip("文字を傾けます（描き文字・効果音に）")
        self.arc = spin(-1, 1, 0.1, "")
        self.arc.setToolTip("文字を弓なりに曲げます（1 で真ん中が大きく持ち上がる。マイナスで逆向き）")
        self.latin = QComboBox()
        self.latin.addItem("4 文字以上は寝かせる", "rotate")
        self.latin.addItem("1 文字ずつ立てる", "upright")
        self.latin.setToolTip("縦書きの中の半角の英数字の組み方")
        self.latin.activated.connect(lambda _: self._style_changed())
        self.mark = QComboBox()
        self.mark.addItem("ゴマ（﹅）", "sesame")
        self.mark.addItem("黒丸（・）", "dot")
        self.mark.setToolTip("《《強調》》と書いた所に付く傍点の形")
        self.mark.activated.connect(lambda _: self._style_changed())
        self.weight = QComboBox()
        for label, key in (("標準", "normal"), ("太", "bold"), ("極太", "heavy")):
            self.weight.addItem(label, key)
        self.weight.setToolTip("文字の太さ。書体に太い字がないときは、字の線を太らせて作ります")
        self.weight.activated.connect(lambda _: self._style_changed())
        self.italic = QCheckBox("斜体")
        self.italic.clicked.connect(lambda _: self._style_changed())
        self.outline_colour = QPushButton("フチの色…")
        self.outline_colour.setToolTip("白フチの色（黒フチなど）")
        self.outline_colour.clicked.connect(self._pick_outline_colour)
        self.wobble = spin(0, 1, 0.1, "")
        self.wobble.setToolTip("フキダシの線を手描きのように揺らす（0 でまっすぐ）")
        self.double = QCheckBox("二重線")
        self.double.clicked.connect(lambda _: self._style_changed())
        self.spikes = QSpinBox()
        self.spikes.setRange(0, 80)
        self.spikes.setSpecialValueText("自動")
        self.spikes.setToolTip("叫びのフキダシのトゲの数")
        self.spikes.editingFinished.connect(self._style_changed)
        self.spike_depth = spin(0.05, 0.6, 0.05, "")
        self.spike_depth.setToolTip("叫びのフキダシのトゲの長さ（大きいほど鋭い）")
        self.color = QPushButton("文字の色…")
        self.color.clicked.connect(self._pick_color)
        reset = QPushButton("既定の設定に戻す")
        reset.setToolTip("この台詞の文字とフキダシの設定を既定に戻します")
        reset.clicked.connect(self._reset_style)
        order = QHBoxLayout()
        order.addWidget(up)
        order.addWidget(down)
        row = QHBoxLayout()
        row.addWidget(self.kind, 1)
        row.addWidget(self.vertical)
        buttons = QGridLayout()
        buttons.addWidget(add, 0, 0)
        buttons.addWidget(self.delete_button, 0, 1)
        buttons.addWidget(self.apply_button, 1, 0, 1, 2)
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
        form.addRow("欧文", self.latin)
        form.addRow("傍点", self.mark)
        faces = QHBoxLayout()
        faces.addWidget(self.weight)
        faces.addWidget(self.italic)
        form.addRow("太さ", faces)
        form.addRow("", self.outline_colour)
        form.addRow("線の揺れ", self.wobble)
        form.addRow("", self.double)
        form.addRow("トゲの数", self.spikes)
        form.addRow("トゲの長さ", self.spike_depth)
        form.addRow("回転", self.rotate)
        form.addRow("傾き", self.skew)
        form.addRow("弓なり", self.arc)
        form.addRow("", self.color)
        hint = QLabel("フキダシはダブルクリックで打ち直し、四隅で大きさ、●でしっぽの先、◇でしっぽの曲がり、上の○で回転。"
                      "右クリックで形・しっぽ・結合。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#666")
        # the lettering and balloon settings of the chosen line sit beside the tool (ツールの設定), where
        # there is room; this panel keeps the list and the words
        self.style_box = QWidget()
        self.style_title = QLabel()
        self.style_title.setStyleSheet("font-weight:bold")
        self.style_title.setWordWrap(True)
        sl = QVBoxLayout(self.style_box)
        sl.setContentsMargins(0, 6, 0, 0)
        sl.addWidget(self.style_title)
        # (hidden while no line is chosen: a column of greyed-out fields only says "not here")
        self.style_body = QWidget()
        bl = QVBoxLayout(self.style_body)
        bl.setContentsMargins(0, 0, 0, 0)
        bl.addLayout(form)
        bl.addWidget(reset)
        bl.addWidget(hint)
        sl.addWidget(self.style_body)
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.addWidget(QLabel("このページの台詞（読み順）"))
        layout.addWidget(self.list, 1)
        layout.addLayout(order)
        layout.addWidget(self.speaker)
        layout.addWidget(self.text)
        layout.addLayout(row)
        layout.addLayout(buttons)
        more = QLabel("文字・フキダシの設定は左の「ツールの設定」に出ます")
        more.setWordWrap(True)
        more.setStyleSheet("color:#666")
        layout.addWidget(more)
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
        from genko.app.lettering import with_marks
        from genko.balloons import style_of

        line = self._line()
        for widget in (self.apply_button, self.delete_button):
            widget.setEnabled(line is not None)
        self.style_body.setVisible(line is not None)
        self.style_title.setStyleSheet("font-weight:bold" if line is not None else "color:#666")
        self.style_title.setText(f"選んだ台詞の文字とフキダシ: 「{line.text[:12]}{'…' if len(line.text) > 12 else ''}」"
                                 if line is not None else "台詞をクリックすると、ここに文字とフキダシの設定が出ます")
        self.window.canvas.selected_line_id = line.id if line else None
        self.window.canvas.update()
        if line is None:
            return
        self._loading = True
        self.speaker.setText(line.speaker)
        self.text.setPlainText(with_marks(line))
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
        self.rotate.setValue(float(st["rotate_deg"] or 0))
        self.skew.setValue(float(st["skew_deg"] or 0))
        self.arc.setValue(float(st["arc"] or 0))
        self.latin.setCurrentIndex(max(0, self.latin.findData(st["latin"])))
        self.mark.setCurrentIndex(max(0, self.mark.findData(st["emphasis_mark"])))
        from genko.balloons import line_weight

        self.weight.setCurrentIndex(line_weight(st))
        self.italic.setChecked(bool(st["italic"]))
        self.wobble.setValue(float(st["wobble"] or 0))
        self.double.setChecked(bool(st["double"]))
        self.spikes.setValue(int(st["spikes"] or 0))
        self.spike_depth.setValue(float(st["spike_depth"] or 0.2))
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
                     "align": self.align.currentData(), "fill": self.fill.currentData(), "tcy": self.tcy.isChecked(),
                     "rotate_deg": self.rotate.value() or None, "skew_deg": self.skew.value() or None, "arc": self.arc.value() or None,
                     "latin": self.latin.currentData(), "emphasis_mark": self.mark.currentData(),
                     "weight": self.weight.currentData() if self.weight.currentIndex() else None, "bold": None, "italic": self.italic.isChecked() or None, "wobble": self.wobble.value() or None,
                     "double": self.double.isChecked() or None, "spikes": self.spikes.value() or None,
                     "spike_depth": self.spike_depth.value() if abs(self.spike_depth.value() - 0.2) > 1e-6 else None})

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
            self.window.flash("日本語を表示できる書体が、このパソコンに見つかりませんでした", 6000)
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

    def _pick_outline_colour(self) -> None:
        from PySide6.QtWidgets import QColorDialog

        from genko.balloons import style_of

        line = self._line()
        if line is None:
            return
        rgb = style_of(line)["outline_rgb"] or (255, 255, 255)
        color = QColorDialog.getColor(QColor(*rgb), self, "フチの色")
        if color.isValid():
            self._style({"outline_rgb": [color.red(), color.green(), color.blue()],
                         "outline_mm": style_of(line)["outline_mm"] or 0.6})

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
        from genko.app.lettering import parse_marks, place_new

        page = self.window.current_page()
        typed = self.text.toPlainText().strip()
        if page is None or not typed:
            self.window.flash("台詞を書いてから追加します", 6000)
            return
        frame = self.window.selected_frame()
        if frame is None:
            self.window.flash("先に編集画面でコマをクリックして選びます（テキストツール T なら、置きたい所をクリック）", 6000)
            return
        text, runs, marks, styles = parse_marks(typed)
        box = place_new(self.window.episode, page, frame, text, self.kind.currentData(), self.vertical.isChecked())
        before = {ln.id for ln in self._lines()}
        op = {"op": "add_line", "page": page.index, "text": text, "speaker": self.speaker.text().strip(),
              "frame_id": frame.id, "balloon": self.kind.currentData(), **box}
        if runs:
            op["ruby_runs"] = runs
        if marks:
            op["emphasis_runs"] = marks
        if styles:
            op["style_runs"] = styles
        if self.window.apply_ops([op]):
            self.text.clear()
            added = next((ln.id for ln in self._lines() if ln.id not in before), None)
            self.refresh()
            self.select(added)

    def apply_edit(self) -> None:
        from genko.app.lettering import parse_marks, refit

        line = self._line()
        if line is None:
            return
        typed = self.text.toPlainText().strip()
        if not typed:
            self.window.flash("台詞が空です。消すときは「削除」を押します", 6000)
            return
        text, runs, marks, styles = parse_marks(typed)
        kind, vertical = self.kind.currentData(), self.vertical.isChecked()
        ops = [{"op": "edit_line", "id": line.id, "text": text, "speaker": self.speaker.text().strip(), "balloon": kind,
                "wrap": "vertical" if vertical else "horizontal", "ruby_runs": runs, "emphasis_runs": marks, "style_runs": styles}]
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
        duplicate = QPushButton("複製")
        duplicate.setToolTip("選んだレイヤーの写しを、すぐ上に作ります")
        duplicate.clicked.connect(self._duplicate)
        merge = QPushButton("下と結合")
        merge.setToolTip("選んだレイヤーを、すぐ下のレイヤーに合わせます（ペン同士は線のまま）")
        merge.clicked.connect(self._merge_down)
        self.draft = QCheckBox("下描き（書き出さない）")
        self.draft.setToolTip("画面には見えますが、書き出し・印刷には出ません")
        self.draft.clicked.connect(lambda on: self._set("exportable", not on))
        self.tint = QComboBox()
        for label, rgb in (("表示色: そのまま", None), ("表示色: 青", [40, 110, 230]), ("表示色: 赤", [220, 50, 50]),
                           ("表示色: 緑", [40, 150, 70]), ("表示色: 灰", [150, 150, 150])):
            self.tint.addItem(label, rgb)
        self.tint.setToolTip("画面でだけ、このレイヤーをこの色で見ます（印刷は元の色）")
        self.tint.activated.connect(lambda _: self._set("color", self.tint.currentData()))
        self.mask_button = QPushButton("マスク ▾")
        self.mask_button.setToolTip("レイヤーの一部を隠す。選択範囲から作り、ペンで見せる所を足し、消しゴムで隠す")
        mask_menu = QMenu(self.mask_button)
        self.act_mask_from_sel = mask_menu.addAction("選択範囲からマスクを作る", self._mask_from_selection)
        self.act_mask_all = mask_menu.addAction("全部見せるマスクを作る", lambda: self._mask({"fill": "show"}))
        self.act_mask_edit = mask_menu.addAction("マスクを編集する（ペンで見せる・消しゴムで隠す）")
        self.act_mask_edit.setCheckable(True)
        self.act_mask_edit.toggled.connect(self._mask_edit)
        mask_menu.addAction("マスクを反転", lambda: self._mask({"invert": True}))
        self.act_mask_off = mask_menu.addAction("マスクを使わない")
        self.act_mask_off.setCheckable(True)
        self.act_mask_off.toggled.connect(lambda on: self._loading or self._mask({"enabled": not on}))
        mask_menu.addAction("マスクを消す", lambda: self._mask({"delete": True}))
        self.mask_button.setMenu(mask_menu)
        self.list.setIconSize(QSize(18, 24))
        self.filter = QComboBox()
        for key, label in wording.FILTERS:
            self.filter.addItem(label, key)
        apply_filter = QPushButton("フィルターをかける…")
        apply_filter.clicked.connect(self._filter)
        adds = QGridLayout()
        for i, button in enumerate((add_pen, add_paint, add_folder, delete, up, down, duplicate, merge)):
            adds.addWidget(button, i // 2, i % 2)
        up.setText("↑ 前へ")
        down.setText("↓ 後ろへ")
        props = QFormLayout()
        props.addRow("不透明度", self.opacity)
        props.addRow("合成", self.blend)
        apply_filter.setText("かける…")
        frow = QHBoxLayout()
        frow.addWidget(self.filter, 1)
        frow.addWidget(apply_filter)
        layout = QVBoxLayout(self)
        layout.addWidget(self.target)
        layout.addWidget(self.list, 1)
        layout.addLayout(adds)
        layout.addWidget(self.name)
        layout.addLayout(props)
        layout.addWidget(self.clip)
        layout.addWidget(self.protect)
        layout.addWidget(self.locked)
        layout.addWidget(self.overhang)
        layout.addWidget(self.draft)
        layout.addWidget(self.tint)
        layout.addWidget(self.mask_button)
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
            masked = " ◐" if getattr(layer, "mask", None) else ""
            draft = " （下描き）" if not layer.exportable and layer.role not in (LayerRole.NAME, LayerRole.DRAFT) else ""
            item = QListWidgetItem(f"{indent}{icon} {wording.layer_label(layer)}{draft}{masked}{lock}")
            picture = self._thumbnail(page, layer)
            if picture is not None:
                item.setIcon(picture)
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
            if self.window.target_layer() is not layer and self.act_mask_edit.isChecked():
                self.act_mask_edit.setChecked(False)  # the mask being edited was the other layer's
            self.window.set_target_layer(layer.id)
        self._loading = True
        self.name.setText(wording.layer_label(layer))
        self.opacity.setValue(int(round(100 * (layer.opacity if layer.opacity is not None else 1.0))))
        self.blend.setCurrentIndex(max(0, self.blend.findData(layer.blend or "normal")))
        self.clip.setChecked(bool(layer.clip))
        self.protect.setChecked(bool(layer.lock_alpha))
        self.locked.setChecked(bool(getattr(layer, "locked", False)))
        self.overhang.setChecked(not getattr(layer, "panel_clip", True))
        self.draft.setChecked(not layer.exportable)
        self.draft.setEnabled(layer.role not in (LayerRole.NAME, LayerRole.DRAFT))  # (those never print)
        colour = list(layer.color) if getattr(layer, "color", None) else None
        self.tint.setCurrentIndex(max(0, self.tint.findData(colour)) if colour else 0)
        self.act_mask_off.setChecked(bool(layer.mask) and not layer.mask.get("enabled", True))
        self.mask_button.setText("マスク ◐ ▾" if layer.mask else "マスク ▾")
        self._loading = False
        drawable = self.window.drawable(layer)
        prints = layer.exportable and layer.role not in (LayerRole.NAME, LayerRole.DRAFT)
        note = "" if prints else " <span style='color:#c92a2a'>（印刷されません）</span>"
        self.target.setText(f"描く先: <b>{wording.layer_label(layer)}</b>{note}" if drawable else
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

    def _thumbnail(self, page, layer):
        """A small picture of the layer alone (cached until the layer changes)."""
        from PIL import Image

        from genko import render
        from genko.io import _layer_to_dict

        if layer.kind == LayerKind.FOLDER:
            return None
        if not (layer.strokes or layer.patches or layer.raster_png or layer.kind in (LayerKind.PLACED, LayerKind.TONE)
                or getattr(layer, "tone", None)):
            return None  # (an empty layer has nothing to show)
        cache = self.__dict__.setdefault("_thumbs", {})
        try:
            key = (page.index, layer.id, hash(repr(_layer_to_dict(layer))), len(layer.raster_png or b""),
                   len((layer.mask or {}).get("png", b"")))
        except Exception:
            return None
        if key not in cache:
            image = render.layer_image(page, layer, 10, self.window.episode)
            paper = Image.new("RGBA", image.size, (255, 255, 255, 255))
            paper.alpha_composite(image)
            data = paper.convert("RGB").tobytes()
            qimage = QImage(data, paper.width, paper.height, paper.width * 3, QImage.Format.Format_RGB888).copy()
            cache[key] = QIcon(QPixmap.fromImage(qimage))
        return cache[key]

    def _duplicate(self) -> None:
        page, layer = self._layer()
        if layer is None:
            return
        from genko.models import new_id

        new = new_id()
        if self.window.apply_ops([{"op": "duplicate_layer", "page": page.index, "id": layer.id, "new_id": new}]):
            self.window.set_target_layer(new)
            self.refresh()

    def _merge_down(self) -> None:
        page, layer = self._layer()
        if layer is None:
            return
        below = page.layers.index(layer) - 1
        if self.window.apply_ops([{"op": "merge_down", "page": page.index, "id": layer.id}]) and below >= 0:
            self.window.set_target_layer(self.window.current_page().layers[below].id)
            self.refresh()

    def _mask(self, change: dict) -> None:
        page, layer = self._layer()
        if layer is not None and not self._loading:
            self.window.apply_ops([{"op": "set_layer_mask", "page": page.index, "id": layer.id, **change}])
            self._selected(from_list=False)

    def _mask_from_selection(self) -> None:
        selection = self.window.canvas.selection
        if not selection:
            self.window.flash("先に範囲選択（M・L・W）で見せたい所を選びます", 5000)
            return
        self._mask({"area": selection["area"]})

    def _mask_edit(self, on: bool) -> None:
        page, layer = self._layer()
        if on and layer is not None and not layer.mask:
            self._mask({"fill": "show"})
        self.window.mask_edit = on
        self.window.flash("マスクの編集中: ペンで見せる所を足し、消しゴムで隠します" if on else "マスクの編集を終えました", 4000)

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
        params = filter_params(self, self.filter.currentData())
        if params is None:
            return
        self.window.apply_ops([{"op": "filter_raster", "page": page.index, "id": layer.id, "kind": self.filter.currentData(), **params}])


def filter_params(parent, kind: str) -> dict | None:
    """The numbers of a colour adjustment, asked once (None: the person stopped)."""
    from PySide6.QtWidgets import QDialog, QDialogButtonBox, QDoubleSpinBox, QFormLayout

    fields = {"blur": [("radius", "ぼかしの強さ", 0.5, 30, 2.0)],
              "levels": [("black", "黒くする所（0〜255）", 0, 254, 20), ("white", "白くする所（1〜255）", 1, 255, 235)],
              "curve": [("gamma", "明るさ（1 より大きいと暗く、小さいと明るく）", 0.2, 5, 1.0)],
              "hue": [("shift", "色相（°）", -180, 180, 30), ("saturation", "彩度（倍）", 0, 3, 1.0), ("value", "明度（倍）", 0, 3, 1.0)],
              "mosaic": [("block", "モザイクの大きさ（px）", 2, 64, 8)]}.get(kind)
    if not fields:
        return {}
    dialog = QDialog(parent)
    dialog.setWindowTitle("フィルターの強さ")
    form = QFormLayout(dialog)
    boxes = {}
    for key, label, lo, hi, value in fields:
        box = QDoubleSpinBox()
        box.setRange(lo, hi)
        box.setSingleStep(0.1 if hi <= 5 else 1)
        box.setValue(value)
        form.addRow(label, box)
        boxes[key] = box
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
    buttons.button(QDialogButtonBox.StandardButton.Ok).setText("かける")
    buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    if not dialog.exec():
        return None
    return {key: round(box.value(), 3) for key, box in boxes.items()}


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

        self.pages = PageList(self)
        self.pages.setMinimumWidth(120)
        self.pages.currentRowChanged.connect(self._select_page)
        self.canvas = PageCanvas()
        self.canvas.renderer = self._render_current
        self.canvas.detail_job = self._detail_job
        self.canvas.needs_rough = self._needs_rough
        self.canvas.changed.connect(self._refresh_status)
        self.canvas.changed.connect(lambda: self._refresh_zoom() if hasattr(self, "zoom_label") else None)
        self.canvas.strokeCommitted.connect(self._on_stroke)
        self.canvas.frameSelected.connect(self._on_frame_selected)
        self.canvas.textMoved.connect(self._on_text_moved)
        self.canvas.contextMenuAt.connect(self._context_menu)
        self.canvas.lineSelected.connect(self._on_line_selected)
        self.canvas.lineGeometry.connect(lambda line_id, change: self.apply_ops(
            [{"op": "edit_line" if "style" in change else "move_line", "id": line_id, **change}]))
        self.canvas.balloonDrawn.connect(self._balloon_drawn)
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
        self.canvas.selectionWarped.connect(self._warp_selection)
        self.canvas.layerMoveStarted.connect(self._layer_move_started)
        self.canvas.layerMoved.connect(self._layer_moved)
        self.canvas.gradientRequested.connect(self._gradient)
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
        self.mask_edit = False  # pen and eraser work on the target layer's mask
        self._target_layer_id: str | None = None
        self.eraser_mm = 2.0
        self._dock_timer = QTimer(self)
        self._dock_timer.setSingleShot(True)
        self._dock_timer.setInterval(250)
        self._dock_timer.timeout.connect(self._refresh_visible_docks)
        self._stale_docks: set = set()
        self.canvas.zoomChanged.connect(lambda _: self._refresh_zoom())

        self.setCentralWidget(self.canvas)  # the page gets the room; everything else sits in panels around it

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
        # building the docks while hidden stretches the window to their summed heights; settle the layout
        # and put the window back to a size that fits a laptop screen
        from genko.app import preferences

        preferences.name_commands(self)  # (every command's words are its lasting name; keys can be changed)
        preferences.apply_all(self)
        self.layout().activate()
        self.resize(1280, 800)

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
            self.flash(wording.error(str(exc)), 6000, error=True)
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
        if page is not None:
            self.pages.update_page(page, self._page_text(page))
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
            self.flash("エージェントの変更と重なったため、次の操作は入りませんでした:\n" + "\n".join(lines), 6000)
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
            self.flash(f"エージェントの変更と重なった操作が {len(result.conflicts)} 件あり、入りませんでした", 6000)
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
        self.act_print = a("印刷…", self._print, "Ctrl+P", "プリンターで紙に印刷します（仕上がりで切る・用紙全体）")
        self.act_undo = a("元に戻す", self._undo, std.Undo)
        self.act_redo = a("やり直す", self._redo, [QKeySequence(std.Redo), QKeySequence("Ctrl+Y")])
        self.act_prefs = a("環境設定…", self._preferences, "Ctrl+,", "ショートカット・ペンタブレット・文字の大きさ・新しい原稿の用紙・保存の間隔")
        self.act_help_keys = a("ショートカット一覧", lambda: self._help("keys"), "F1")
        self.act_help_guide = a("はじめての使い方", lambda: self._help("guide"))
        self.act_help_faq = a("困ったとき（よくある質問）", lambda: self._help("faq"))
        self.act_about = a("Genko Studio について", lambda: self._help("about"))
        self.act_history = a("履歴…", lambda: self.show_dock("履歴"), "Ctrl+H", "変更の一覧。クリックでその時点まで戻る・進む")
        self.act_fit = a("全体を表示", self.canvas.fit_page, "Ctrl+0")
        self.act_zoom_in = a("拡大", lambda: self.canvas.zoom_by(1.25), [QKeySequence(std.ZoomIn), QKeySequence("Ctrl+=")])
        self.act_zoom_out = a("縮小", lambda: self.canvas.zoom_by(0.8), std.ZoomOut)
        self.act_actual = a("原寸（紙の大きさ）", self.canvas.actual_size, "Ctrl+1")
        self.act_turn_left = a("左に回す（15°）", lambda: self.canvas.rotate_view(-15), "Ctrl+Alt+Left",
                               "表示だけを回します（原稿は回りません）。Shift＋スペースを押しながらドラッグでも回せます")
        self.act_turn_right = a("右に回す（15°）", lambda: self.canvas.rotate_view(15), "Ctrl+Alt+Right",
                                "表示だけを回します（原稿は回りません）")
        self.act_turn_reset = a("回転・反転を戻す", self.canvas.reset_view, "Ctrl+Alt+0")
        self.act_mirror = a("左右反転して見る", lambda on: self.canvas.flip_view(on), "H",
                            "表示だけを左右反転します（絵の歪みを見つける）。原稿は変わりません", True)
        self.act_tool_names = a("道具の名前を表示", self._show_tool_names, None,
                                "左の道具にアイコンと名前を並べます（環境に残ります）", True)
        self.act_overview = a("ページを並べて見る", self._page_overview, "Ctrl+Shift+O", "全ページを縮小図で並べ、ダブルクリックで開きます")
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
        self.act_frame = a("コマ割り", lambda: self._tool("frame"), "F",
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
        self.act_3d = a("3D 操作", lambda: self._tool("3d"), "J", "デッサン人形の関節（○）や箱をドラッグして動かす。箱の上の○で回す", True)
        self.act_effect = a("効果線", lambda: self._tool("effect"), "K",
                            "コマの中をクリックすると、選んだ効果線（集中線など）が入る。中心の＋をドラッグで動かす", True)
        self.act_stamp = a("素材を置く", lambda: self._tool("stamp"), tip="素材パネルで選んだ素材を、クリックした所に置く", checkable=True)
        self.act_move = a("レイヤー移動", lambda: self._tool("move"), "Q",
                          "描く先のレイヤーを丸ごとドラッグで動かす（Shift で縦・横・45°）", True)
        self.act_gradient = a("グラデーション", lambda: self._tool("gradient"), "U",
                              "ドラッグの向きに色をなめらかに変えて塗る（選択範囲があればその中だけ）", True)
        tools = QActionGroup(self)
        self.tool_actions = {"move": self.act_move, "gradient": self.act_gradient, "select": self.act_select, "pen": self.act_pen, "eraser": self.act_eraser, "text": self.act_text,
                             "frame": self.act_frame, "picker": self.act_picker, "fill": self.act_fill,
                             "lassofill": self.act_lassofill, "rect": self.act_marquee, "lasso": self.act_lasso,
                             "wand": self.act_wand, "reshape": self.act_reshape, "ruler": self.act_ruler, "3d": self.act_3d,
                             "effect": self.act_effect, "stamp": self.act_stamp}
        for act in self.tool_actions.values():
            tools.addAction(act)
        self.act_select.setChecked(True)
        self.act_color = a("ペンの色…", self._pick_color, "C")  # (kept for its key; the colour is in ツールの設定)
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
        self.act_warp_perspective = a("自由変形（遠近・4 隅）", lambda: self._start_warp("perspective"), "Ctrl+Shift+P",
                                      "選択範囲の 4 隅を好きな所へ引っぱる。Enter で確定、Esc でやめる")
        self.act_warp_mesh = a("自由変形（メッシュ・3×3）", lambda: self._start_warp("mesh"), "Ctrl+Shift+W",
                               "選択範囲の 3×3 の点を引っぱって曲げる。Enter で確定、Esc でやめる")
        self.act_warp_apply = a("自由変形を確定", lambda: self.canvas.finish_warp())
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
        self.act_add_cylinder = a("3D の円柱を置く", lambda: self._add_prim("cylinder"))
        self.act_add_stairs = a("3D の階段を置く", lambda: self._add_prim("stairs"))
        self.act_add_floor = a("床（パースの格子）を置く", lambda: self._add_prim("floor"), tip="地面の格子で、背景のパースの目安にします")
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
        self.act_add_page = a("ページを追加（この後ろに）", self._add_page)
        self.act_del_page = a("このページを削除…", self._del_page)
        self.act_dup_page = a("このページを複製", lambda: self._current() and self.duplicate_page(self._current().index))
        self.act_page_up = a("このページを前へ", lambda: self._current() and self.pages.move_page(self._current().index, -1), "Ctrl+Shift+Up")
        self.act_page_down = a("このページを後ろへ", lambda: self._current() and self.pages.move_page(self._current().index, 1),
                               "Ctrl+Shift+Down")
        self.act_spread = a("次のページと見開きにする／解除", self._toggle_spread)
        self.act_nombre = a("ノンブルの設定…", lambda: nombre_dialog(self), tip="位置・書体・大きさ・始まりの番号・隠しノンブル")
        self.act_paper = a("原稿用紙の設定…", self._paper_settings, tip="用紙・仕上がり・裁ち落とし・基本枠。変えるとコマや台詞も新しい枠に合わせて動きます")
        self.act_page_nombre = a("このページのノンブルを隠す／出す", self._toggle_page_nombre)
        self.act_story_editor = a("ストーリーエディター…", self.open_story_editor, "Ctrl+Shift+L", "全ページの台詞をまとめて直す・台本を流し込む")
        self.act_checks = a("入稿前の点検", self._run_checks, "F9", "はみ出し・文字の重なりや小ささ・解像度などを探します")
        self.act_name_ok = a("ネーム完了 → 作画へ進む", self._name_ok, tip="承認の要らない原稿（エージェントを使わない原稿）で使います")

        # layers and lines get their own menus too (not only their panels)
        L = lambda method: (lambda *_: getattr(self.layers, method)())  # noqa: E731
        self.act_layer_pen = a("新しいペンのレイヤー", lambda: self.layers._add("pen", "ペン"), "Ctrl+Shift+N")
        self.act_layer_paint = a("新しいペイントのレイヤー", lambda: self.layers._add("paint", "ペイント"))
        self.act_layer_folder = a("新しいフォルダ", lambda: self.layers._add("folder", "フォルダ"))
        self.act_layer_dup = a("レイヤーを複製", L("_duplicate"), "Ctrl+J")
        self.act_layer_merge = a("下のレイヤーと結合", L("_merge_down"), "Ctrl+Shift+E")
        self.act_layer_delete = a("レイヤーを削除", L("_delete"))
        self.act_layer_up = a("レイヤーを前へ", lambda: self.layers._move(1), "Ctrl+]")
        self.act_layer_down = a("レイヤーを後ろへ", lambda: self.layers._move(-1), "Ctrl+[")
        self.act_layer_draft = a("下描きにする（書き出さない）／戻す", lambda: self.layers.draft.click())

        self.act_line_type = a("台詞を入れる（テキストの道具）", lambda: self._tool("text"))
        self.act_balloon_pen = a("フキダシを手で描く", lambda on: (self._tool("text"), self.text_settings.draw_balloon.setChecked(on)),
                                 tip="ドラッグで囲んだ形のフキダシに台詞を入れます", checkable=True)
        self.act_line_edit = a("選んだ台詞をその場で直す", self._edit_selected_line, "F2")
        self.act_line_delete = a("選んだ台詞を消す", self._delete_selected_line)
        self.act_line_wrap = a("縦書き・横書きを切り替える", self._toggle_selected_wrap)
        self.act_close = a("閉じる", self.close, QKeySequence.StandardKey.Close)
        self.act_quit = a("Genko を終わる", lambda: QApplication.instance().closeAllWindows(), QKeySequence.StandardKey.Quit)

        bar = self.menuBar()
        menus = [
            ("ファイル", [self.act_new, self.act_open, "recent", None, self.act_save, self.act_save_as, None, self.act_import,
                         self.act_export, self.act_print, None, self.act_prefs, None, self.act_close, self.act_quit]),
            ("編集", [self.act_undo, self.act_redo, self.act_history, None, self.act_cut, self.act_copy, self.act_paste,
                      self.act_delete_area, None, self.act_select_all, self.act_deselect]),
            ("表示", [self.act_fit, self.act_zoom_in, self.act_zoom_out, self.act_actual, None, self.act_turn_left,
                      self.act_turn_right, self.act_mirror, self.act_turn_reset, None, self.act_overview, self.act_prev, self.act_next,
                      None, self.act_guides, self.act_onion, None, self.act_tool_names]),
            ("ツール", [self.act_select, self.act_move, self.act_pen, self.act_eraser, self.act_text, self.act_frame, None,
                        self.act_picker, self.act_fill, self.act_lassofill, self.act_gradient, self.act_reshape, None, self.act_marquee, self.act_lasso, self.act_wand, None,
                        self.act_ruler, self.act_3d, self.act_effect, self.act_stamp, None, self.act_thicker, self.act_thinner]),
            ("レイヤー", [self.act_layer_pen, self.act_layer_paint, self.act_layer_folder, None, self.act_layer_dup,
                          self.act_layer_merge, self.act_layer_delete, None, self.act_layer_up, self.act_layer_down, None,
                          self.act_layer_draft, "mask"]),
            ("台詞", [self.act_line_type, self.act_balloon_pen, None, self.act_line_edit, self.act_line_wrap, "shapes",
                      self.act_line_delete, None, self.act_story_editor]),
            ("トーン・効果線", [self.act_tone_here, self.act_tone_click, None, self.act_effect, *self.effect_actions, None,
                               self.act_materials]),
            ("選択", [self.act_marquee, self.act_lasso, self.act_wand, None, self.act_select_all, self.act_deselect, None,
                      self.act_cut, self.act_copy, self.act_paste, self.act_delete_area, None, self.act_flip_h, self.act_flip_v,
                      self.act_warp_perspective, self.act_warp_mesh, self.act_warp_apply, None,
                      self.act_fill_selection, self.act_line_width]),
            ("定規・3D", [self.act_ruler, None, *self.ruler_actions, None, self.act_snap, self.act_show_rulers, self.act_del_ruler,
                          self.act_clear_rulers, None, self.act_grid, self.act_grid_snap, self.act_grid_mm, None, self.act_3d,
                          self.act_add_figure, self.act_add_box, self.act_add_cylinder, self.act_add_stairs, self.act_add_floor, "poses",
                          self.act_trace, self.act_del_prim]),
            ("コマ", [self.act_frame, None, self.act_split_h, self.act_split_v, self.act_merge, None, self.act_template, None,
                      self.act_gutters, self.act_border, self.act_no_border, self.act_bleed, self.act_reset_shape]),
            ("ページ", [self.act_add_page, self.act_dup_page, self.act_del_page, None, self.act_page_up, self.act_page_down, self.act_spread,
                        None, self.act_paper, self.act_nombre, self.act_page_nombre, None, self.act_story_editor, self.act_checks, None, self.act_name_ok]),
        ]
        from genko.app.lettering import KINDS

        for title, actions in menus:
            menu = bar.addMenu(title)
            for act in actions:
                if act is None:
                    menu.addSeparator()
                elif act == "recent":
                    self.recent_menu = menu.addMenu("最近使った原稿")
                    self.recent_menu.aboutToShow.connect(self._fill_recent)
                elif act == "shapes":
                    shapes = menu.addMenu("フキダシの形")
                    for key, label in KINDS:
                        shapes.addAction(label, lambda k=key: self._set_selected_balloon(k))
                elif act == "mask":
                    self.layer_mask_menu = menu.addMenu("マスク")  # (filled with the layer panel's own, below)
                elif act == "poses":
                    poses = menu.addMenu("ポーズ")
                    for pose in self.pose_actions:
                        poses.addAction(pose)
                else:
                    menu.addAction(act)
        self.view_menu = bar.addMenu("ウィンドウ")
        help_menu = bar.addMenu("ヘルプ")
        for act in (self.act_help_guide, self.act_help_keys, self.act_help_faq, None, self.act_about):
            if act is None:
                help_menu.addSeparator()
            else:
                help_menu.addAction(act)

        from genko.app.icons import icon

        pictures = {"select": self.act_select, "pen": self.act_pen, "eraser": self.act_eraser, "text": self.act_text,
                    "frame": self.act_frame, "picker": self.act_picker, "fill": self.act_fill, "lassofill": self.act_lassofill,
                    "rect": self.act_marquee, "lasso": self.act_lasso, "wand": self.act_wand, "reshape": self.act_reshape,
                    "ruler": self.act_ruler, "3d": self.act_3d, "effect": self.act_effect, "stamp": self.act_stamp,
                    "move": self.act_move, "gradient": self.act_gradient, "undo": self.act_undo, "redo": self.act_redo, "fit": self.act_fit, "zoom_in": self.act_zoom_in,
                    "zoom_out": self.act_zoom_out, "prev": self.act_prev, "next": self.act_next, "export": self.act_export}
        for name, act in pictures.items():
            act.setIcon(icon(name))
            keys = act.shortcut().toString()
            if keys and act in self.tool_actions.values():
                act.setToolTip(f"{act.text()}（{keys}）" + (f"\n{act.statusTip()}" if act.statusTip() else ""))
        palette = QToolBar("道具")
        palette.setObjectName("tools")
        palette.setMovable(False)
        palette.setOrientation(Qt.Orientation.Vertical)
        palette.setIconSize(QSize(24, 24))
        palette.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonIconOnly)
        for act in (self.act_select, self.act_move, self.act_pen, self.act_eraser, self.act_fill, self.act_lassofill, self.act_gradient,
                    self.act_picker, None,
                    self.act_text, self.act_frame, None, self.act_marquee, self.act_lasso, self.act_wand, self.act_reshape, None,
                    self.act_ruler, self.act_3d, self.act_effect):
            if act is None:
                palette.addSeparator()
            else:
                palette.addAction(act)
        self.addToolBar(Qt.ToolBarArea.LeftToolBarArea, palette)
        self.tool_palette = palette
        for act, short in ((self.act_marquee, "長方形選択"), (self.act_lasso, "投げ縄選択"), (self.act_reshape, "線の修正"),
                           (self.act_3d, "3D")):
            act.setIconText(short)  # (the palette's names stay short; menus keep the full name)
        from genko.app.preferences import settings as prefs

        self.act_tool_names.blockSignals(True)
        self.act_tool_names.setChecked(str(prefs().value("ui/tool_names", "0")) == "1")
        self.act_tool_names.blockSignals(False)
        self._show_tool_names(self.act_tool_names.isChecked(), save=False)
        commands = QToolBar("操作")
        commands.setObjectName("commands")
        commands.setMovable(False)
        commands.setIconSize(QSize(18, 18))
        commands.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        for act in (self.act_undo, self.act_redo, None, self.act_fit, self.act_zoom_out, self.act_zoom_in, None, self.act_prev,
                    self.act_next, None, self.act_export):
            if act is None:
                commands.addSeparator()
            else:
                commands.addAction(act)
        self.addToolBar(commands)

    def _print(self) -> None:
        from genko.app.printing import PrintDialog

        self.commit_now()
        PrintDialog(self).exec()

    def _show_tool_names(self, on: bool, save: bool = True) -> None:
        """Icons alone, or icons with their names (easier while learning 18 tools)."""
        style = Qt.ToolButtonStyle.ToolButtonTextBesideIcon if on else Qt.ToolButtonStyle.ToolButtonIconOnly
        self.tool_palette.setToolButtonStyle(style)
        if save:
            from genko.app.preferences import settings as prefs

            prefs().setValue("ui/tool_names", "1" if on else "0")

    def _build_studio(self) -> None:
        from genko.app.tool_settings import TextToolSettings, ToolSettings, action_page, fit_narrow

        self.process = ProcessBar()
        self.statusBar().addPermanentWidget(self.process)
        self.statusBar().addPermanentWidget(self.zoom_label)
        self.approvals = ApprovalBox(self)
        self.panel_view = PanelView(self)
        self.story = StoryPanel(self)
        self.layers = LayerPanel(self)
        for act in self.layers.mask_button.menu().actions():
            self.layer_mask_menu.addAction(act)
        self.library = Library(self)
        self.guides = GuidePanel(self)
        self.materials = MaterialPanel(self)
        self.checks = CheckPanel(self)
        from genko.app.history import HistoryPanel

        self.history = HistoryPanel(self)
        for widget in (self.approvals, self.panel_view):
            widget.changed.connect(self._reload_pages)
        # ツールの設定 (left): what the tool in hand can do
        self.brush = BrushPanel()
        self.brush.changed.connect(self._brush_changed)
        self.brush.make.clicked.connect(self._make_brush)
        self.brush.forget.clicked.connect(self._forget_brush)
        self._brush_changed()
        self.text_settings = TextToolSettings()
        self.text_settings.draw_balloon.toggled.connect(lambda on: setattr(self.canvas, "balloon_pen", on))
        self.text_settings.draw_balloon.toggled.connect(lambda on: self.act_balloon_pen.setChecked(on))
        self.tool_settings = ToolSettings()
        ts = self.tool_settings
        ts.add(("pen", "fill", "lassofill", "picker"), self.brush)
        eraser_size = QDoubleSpinBox()
        eraser_size.setRange(0.2, 50)
        eraser_size.setSingleStep(0.5)
        eraser_size.setSuffix(" mm")
        eraser_size.setValue(self.eraser_mm)
        eraser_size.valueChanged.connect(self._eraser_size)
        self.eraser_size = eraser_size
        eraser_page = QWidget()
        eraser_form = QFormLayout(eraser_page)
        eraser_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapAllRows)
        eraser_form.setContentsMargins(0, 0, 0, 0)
        eraser_form.addRow("消しゴムの太さ（[ ] でも変わる）", eraser_size)
        eraser_form.addRow(self.brush.crossing)
        scrape = QLabel("トーンのレイヤーでは削ります（ぼかすかは素材パネルのトーンの欄で）")
        scrape.setWordWrap(True)
        scrape.setStyleSheet("color:#666")
        eraser_form.addRow(scrape)
        ts.add(("eraser",), eraser_page)
        ts.add(("text",), self.text_settings)
        ts.add(("frame",), action_page([self.act_split_h, self.act_split_v, self.act_merge, None, self.act_template, self.act_gutters,
                                        self.act_border, self.act_no_border, self.act_bleed, self.act_reset_shape, None, self.act_paper]))
        ts.add(("marquee",), action_page([self.act_marquee, self.act_lasso, self.act_wand, None, self.act_select_all, self.act_deselect,
                                          None, self.act_copy, self.act_cut, self.act_paste, self.act_delete_area, None, self.act_flip_h,
                                          self.act_flip_v, self.act_warp_perspective, self.act_warp_mesh, self.act_warp_apply, None,
                                          self.act_fill_selection, self.act_line_width, self.act_tone_here]))
        radius = QDoubleSpinBox()
        radius.setRange(1, 60)
        radius.setSuffix(" mm")
        radius.setValue(self.canvas.reshape_radius_mm)
        radius.valueChanged.connect(lambda v: setattr(self.canvas, "reshape_radius_mm", v))
        radius_page = QWidget()
        radius_form = QFormLayout(radius_page)
        radius_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapAllRows)
        radius_form.setContentsMargins(0, 0, 0, 0)
        radius_form.addRow("つまんだ所から動く範囲", radius)
        ts.add(("reshape",), radius_page)
        ts.add(("ruler",), action_page([*self.ruler_actions, None, self.act_snap, self.act_show_rulers, self.act_del_ruler,
                                        self.act_clear_rulers, None, self.act_grid, self.act_grid_snap, self.act_grid_mm]))
        ts.add(("3d",), action_page([self.act_add_figure, self.act_add_box, self.act_add_cylinder, self.act_add_stairs, self.act_add_floor, None, *self.pose_actions, None, self.act_trace,
                                     self.act_del_prim]))
        ts.add(("effect",), action_page([*self.effect_actions, None, self.act_materials]))
        ts.add(("stamp",), action_page([self.act_materials]))
        select_page = action_page([self.act_fit, self.act_actual, None, self.act_story_editor, self.act_checks])
        select_page.layout().insertWidget(0, self.story.style_box)
        ts.add(("select",), select_page)
        ts.add(("move",), action_page([self.act_layer_dup, None, self.act_select_all]))
        self.gradient_mode = QComboBox()
        for label, key in (("ペンの色 → 透明", "fade"), ("ペンの色 → 白", "white"), ("黒 → 白", "bw"), ("円（中心からペンの色 → 透明）", "radial")):
            self.gradient_mode.addItem(label, key)
        ts.add(("gradient",), action_page([], [QLabel("色の変わり方"), self.gradient_mode]))
        settings_dock = QDockWidget("ツールの設定", self)
        settings_scroll = QScrollArea()
        settings_scroll.setWidgetResizable(True)
        settings_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        settings_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        settings_scroll.setWidget(ts)
        settings_dock.setWidget(settings_scroll)
        settings_dock.setObjectName("ツールの設定")
        settings_dock.setMinimumWidth(SIDE_WIDTH)
        settings_dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, settings_dock)
        self.view_menu.addAction(settings_dock.toggleViewAction())
        from genko.app.navigator import Navigator

        self.navigator = Navigator(self)
        nav_dock = QDockWidget("全体図", self)
        nav_dock.setObjectName("全体図")
        nav_dock.setWidget(self.navigator)
        nav_dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable
                             | QDockWidget.DockWidgetFeature.DockWidgetClosable)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, nav_dock)
        self.splitDockWidget(settings_dock, nav_dock, Qt.Orientation.Vertical)
        self.view_menu.addAction(nav_dock.toggleViewAction())
        self.navigator_dock = nav_dock
        self.brush_dock = settings_dock
        ts.show_tool("select")
        # the panels on the right; the ones for books made with agents only show for those books
        docks = []
        self.agent_docks = []
        groups: dict[str, list] = {"upper": [], "lower": [], "agent": []}
        for title, widget, group in (("承認箱", self.approvals, "agent"), ("ページ", self.pages, "upper"),
                                     ("レイヤー", self.layers, "upper"), ("履歴", self.history, "upper"), ("台詞", self.story, "lower"),
                                     ("素材", self.materials, "lower"), ("定規・3D", self.guides, "lower"),
                                     ("点検", self.checks, "lower"), ("コマの詳細", self.panel_view, "agent"),
                                     ("資料", self.library, "agent")):
            dock = QDockWidget(title, self)
            if widget is not self.pages:
                # tall panels scroll on a small screen instead of making the window taller
                scroll = QScrollArea()
                scroll.setWidgetResizable(True)
                scroll.setFrameShape(QScrollArea.Shape.NoFrame)
                scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
                scroll.setWidget(widget)
                dock.setWidget(scroll)
            else:
                dock.setWidget(widget)
            dock.setObjectName(title)
            dock.setMinimumWidth(SIDE_WIDTH)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable | QDockWidget.DockWidgetFeature.DockWidgetFloatable)
            dock.setTitleBarWidget(QWidget())  # the tab already names it; the room goes to the panel
            area = Qt.DockWidgetArea.LeftDockWidgetArea if group == "agent" else Qt.DockWidgetArea.RightDockWidgetArea
            self.addDockWidget(area, dock)
            self.view_menu.addAction(dock.toggleViewAction())
            dock.visibilityChanged.connect(lambda shown, d=dock: shown and d in self._stale_docks and self._refresh_dock(d))
            docks.append(dock)
            groups[group].append(dock)
            if group == "agent":
                self.agent_docks.append(dock)
        # two stacks on the right (pages and layers above, the lettering and the other panels below); the
        # agent's panels sit under the tool settings on the left, only for books made with agents
        self.splitDockWidget(groups["upper"][0], groups["lower"][0], Qt.Orientation.Vertical)
        self.splitDockWidget(settings_dock, groups["agent"][0], Qt.Orientation.Vertical)
        for group in groups.values():
            for other in group[1:]:
                self.tabifyDockWidget(group[0], other)
        self.setTabPosition(Qt.DockWidgetArea.LeftDockWidgetArea, QTabWidget.TabPosition.North)
        self.setTabPosition(Qt.DockWidgetArea.RightDockWidgetArea, QTabWidget.TabPosition.North)
        self.studio_docks = docks
        for dock in [*docks, settings_dock]:
            fit_narrow(dock.widget())
        self._agent_view(self._agent_book())
        self.resizeDocks([docks[1], settings_dock], [SIDE_WIDTH, SIDE_WIDTH], Qt.Orientation.Horizontal)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not getattr(self, "_settled", False):
            self._settled = True
            self._settle_docks()
        QTimer.singleShot(0, self._hide_stray_tabs)

    def _hide_stray_tabs(self) -> None:
        """Tabbing panels inside a split leaves an old tab bar behind (a Qt quirk) that the first show
        brings up over the panels; hide any bar whose tabs are only the start of another bar's."""
        from PySide6.QtWidgets import QTabBar

        bars = [bar for bar in self.findChildren(QTabBar) if bar.parentWidget() is self]
        tabs = {bar: [bar.tabText(i) for i in range(bar.count())] for bar in bars}
        shown = {dock.windowTitle() for dock in self.findChildren(QDockWidget) if dock.isVisible()}
        for bar in bars:
            mine = tabs[bar]
            if not mine or not set(mine) & shown or any(other is not bar and len(tabs[other]) > len(mine) and tabs[other][:len(mine)] == mine
                               for other in bars):
                bar.hide()

    def _agent_book(self) -> bool:
        """A book made with agents (strict gates or a studio), or one they have sent requests to."""
        ep = self.episode
        return bool(ep.strict_gates or ep.studio)

    def _agent_view(self, on: bool) -> None:
        """Show the approval box and the agent panels only for books made with agents."""
        if getattr(self, "_agent_mode", None) == on:
            return
        self._agent_mode = on
        for dock in self.agent_docks:
            dock.setVisible(on)
            dock.toggleViewAction().setVisible(on)
        self.process.setVisible(on)
        self.act_name_ok.setVisible(on)  # (stages and their approvals are for books made with agents)
        if hasattr(self, "navigator_dock"):
            self.navigator_dock.setVisible(not on)  # (the approval box needs the room; ウィンドウ → 全体図 brings it back)
        if on:
            self.resizeDocks([self.brush_dock, self.agent_docks[0]], [1, 1], Qt.Orientation.Vertical)
        if self.isVisible():
            QTimer.singleShot(0, self._settle_docks)

    def _settle_docks(self) -> None:
        """The panels in front: the approval box (for agent books), the layers and the lines; the lower
        stack (the lines, the materials) gets the larger share of the height."""
        upper = next((d for d in self.studio_docks if d.windowTitle() == "レイヤー"), None)
        lower = next((d for d in self.studio_docks if d.windowTitle() == "台詞"), None)
        if upper is not None and lower is not None:
            self.resizeDocks([upper, lower], [2, 3], Qt.Orientation.Vertical)
        if hasattr(self, "navigator_dock"):  # the navigator stays small under the tool settings
            self.resizeDocks([self.brush_dock, self.navigator_dock], [max(300, self.height() - 330), 170], Qt.Orientation.Vertical)
        for dock in self.studio_docks:
            if dock.windowTitle() in ("承認箱", "レイヤー", "台詞") and dock.isVisible():
                dock.raise_()
        self._hide_stray_tabs()

    def show_dock(self, title: str) -> None:
        for dock in self.studio_docks:
            if dock.windowTitle() == title:
                dock.show()
                dock.raise_()

    def _dock_visible(self, dock) -> bool:
        # (a panel behind another tab is moved out of the window; one in front sits inside it)
        return dock.isVisible() and (dock.isFloating() or self.rect().intersects(dock.geometry()))

    def _refresh_dock(self, dock) -> None:
        self._stale_docks.discard(dock)
        widget = {"承認箱": self.approvals, "コマの詳細": self.panel_view, "台詞": self.story, "レイヤー": self.layers,
                  "素材": self.materials, "定規・3D": self.guides, "点検": self.checks, "資料": self.library,
                  "履歴": self.history, "ページ": None}[dock.windowTitle()]
        if widget is None:
            return
        if widget is self.panel_view:
            self._sync_panel_view()
        widget.refresh()

    def _refresh_visible_docks(self) -> None:
        self.process.refresh(self.episode)
        if self.canvas.selected_line_id:  # the chosen line's settings beside the tool stay current
            self.story.refresh()
            self.story.select(self.canvas.selected_line_id)
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
        extra = ""
        if page.spread_with:
            pair = sorted((page.index, page.spread_with))
            extra += f"\n見開き {pair[0]}–{pair[1]}"
        if not page.numero:
            extra += "\nノンブルなし"
        return f"{page.index} ページ\n{name}{art}{done}{extra}"

    def _reload_pages(self) -> None:
        if hasattr(self, "agent_docks"):
            self._agent_view(self._agent_book())  # an agent may have started working on this book
        self.pages.fill(self.episode.pages, self._page_text, dirty="all")
        self.pages.blockSignals(True)
        self.pages.setCurrentRow(min(self._page_index, len(self.episode.pages) - 1))
        self.pages.blockSignals(False)
        self._show_page()

    # --- the book's pages (M16) --------------------------------------------------------------------------

    def reorder_pages(self, order: list[int], follow: int | None = None) -> None:
        """Put the pages in this order; the page being worked on stays selected."""
        current = follow if follow is not None else (self._current().index if self._current() else 1)
        if self.apply_ops([{"op": "reorder", "order": order}]):
            self._page_index = order.index(current) if current in order else 0
        self._reload_pages()

    def add_page_after(self, index: int) -> None:
        if self.apply_ops([{"op": "add_page", "count": 1, "after": index}]):
            self._page_index = index  # the new page
            self._reload_pages()

    def duplicate_page(self, index: int) -> None:
        if self.apply_ops([{"op": "duplicate_page", "page": index, "next_to": True}]):
            self._page_index = index
            self._reload_pages()

    def set_spread(self, index: int, other: int | None) -> None:
        page = next((p for p in self.episode.pages if p.index == index), None)
        ops = []
        if other is None and page is not None and page.spread_with:
            ops = [{"op": "set_spread", "page": index, "with": None}, {"op": "set_spread", "page": page.spread_with, "with": None}]
        elif other is not None:
            ops = [{"op": "set_spread", "page": index, "with": other}, {"op": "set_spread", "page": other, "with": index}]
        if ops and self.apply_ops(ops):
            self._reload_pages()

    def show_issue(self, issue: dict) -> None:
        """Go to a problem the checks found and mark it on the page."""
        if issue.get("page"):
            self.go_to_page(int(issue["page"]))
        self.canvas.highlight_box = issue.get("box")
        target = issue.get("target") or {}
        if target.get("kind") == "line" and target.get("id"):
            self.canvas.selected_line_id = target["id"]
        elif target.get("kind") == "layer" and target.get("id") and any(layer.id == target["id"] for layer in self._current().layers):
            self._target_layer_id = target["id"]
        self.canvas.update()
        self.flash(issue.get("message", ""), 5000)

    def open_story_editor(self) -> None:
        from genko.app.story_editor import StoryEditor

        self.story_editor = StoryEditor(self)
        self.story_editor.show()

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
            self.canvas.highlight_box = None
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

    def _render_current(self, dpi: int, rough: bool = False) -> QPixmap | None:
        page = self._current()
        if page is None:
            return None
        from genko.render import render_page

        # (a person alone sees the page as it will print, name lines in blue; the name view is for the
        # agent's name stage)
        mode = "name" if not page.name_ok and getattr(self, "_agent_mode", False) else "proof"
        try:
            return _pixmap(render_page(page, dpi, mode=mode, episode=self.episode, rough=rough))
        except Exception:  # a broken asset must not take the editor down
            return None

    def _needs_rough(self, dpi: int) -> bool:
        from genko.render import rough_needed

        page = self._current()
        return page is not None and rough_needed(page, dpi)

    def _detail_job(self, dpi: int):
        """The current page, rendered finer off the GUI thread. Pages are never changed in place (an
        edit makes new copies of what it touches), so the thread can read these while people draw on."""
        page, episode = self._current(), self.episode
        mode = "name" if page is not None and not page.name_ok and getattr(self, "_agent_mode", False) else "proof"

        def job():
            if page is None:
                return None
            from genko.render import render_page

            rgb = render_page(page, dpi, mode=mode, episode=episode).convert("RGB")
            data = rgb.tobytes()
            return QImage(data, rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888).copy()

        return job

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
        if self._agent_book():
            self.status.setText(f"{page.index} ページ（{wording.STAGE.get(page.stage, page.stage)}） ・ コマ {len(page.leaf_frames())}"
                                f"{selected} ・ {saved} ・ {wording.actor(self.session.actor)}")
        else:
            self.status.setText(f"{page.index} / {len(self.episode.pages)} ページ ・ コマ {len(page.leaf_frames())} 個{selected} ・ {saved}")
        self._refresh_zoom()

    def flash(self, message: str, ms: int = 3000, error: bool = False) -> None:
        """A notice in the status line that never stops the work (it goes back to the page's status after
        `ms`); problems show in red and stay a little longer. Only questions before something that cannot
        be taken back open a window."""
        from html import escape

        text = escape(str(message)).replace("\n", " ・ ")
        if error:
            self.status.setText(f"<span style='color:#c92a2a'><b>⚠ {text}</b></span>")
            ms = max(ms, 6000)
        else:
            self.status.setText(f"<b>{text}</b>")
        self.last_notice = message
        self.last_error = message if error else getattr(self, "last_error", None)
        QTimer.singleShot(ms, self._refresh_status)

    def _refresh_zoom(self) -> None:
        turned = f" ・ 回転 {self.canvas.rotation:+.0f}°" if self.canvas.rotation else ""
        mirrored = " ・ 左右反転" if self.canvas.flipped else ""
        self.zoom_label.setText(f"表示 {self.canvas.zoom_percent()}%{turned}{mirrored}")
        if hasattr(self, "act_mirror") and self.act_mirror.isChecked() != self.canvas.flipped:
            self.act_mirror.setChecked(self.canvas.flipped)

    # --- editing -----------------------------------------------------------------------------

    def _tool(self, tool: str) -> None:
        if tool in ("rect", "lasso", "wand"):
            self.canvas.marquee = tool
            self.canvas.set_tool("marquee")
        else:
            self.canvas.set_tool(tool)
        self.tool_actions[tool].setChecked(True)
        if hasattr(self, "tool_settings"):
            self.tool_settings.show_tool(self.canvas.tool)

    # --- the layer the pen works on ---------------------------------------------------------------

    def target_layer(self):
        from genko.models import LayerRole

        page = self._current()
        if page is None:
            return None
        found = next((layer for layer in page.layers if layer.id == self._target_layer_id), None)
        if found is not None:
            return found
        # a person drawing alone starts on the ink (it prints); a book made with agents starts with the name
        agent = self._agent_book() if hasattr(self, "agent_docks") else bool(self.episode.strict_gates or self.episode.studio)
        role = LayerRole.NAME if page.stage == "name" and agent else LayerRole.INK
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
        if self.mask_edit and self.canvas.tool in ("pen", "eraser"):
            erase = self.canvas.tool == "eraser"
            self.apply_ops([{"op": "paint_mask", "page": page.index, "id": layer.id, "points": [[p[0], p[1]] for p in points],
                             "width_mm": self.eraser_mm if erase else max(0.5, self.brush.size.value()), "show": not erase}])
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
        ops = [op]
        kind = op.get("kind") or ""
        if kind.startswith("my_") and kind not in self.episode.brush_custom:
            from genko import brushes

            # the book keeps the brush's settings, so the line looks the same on any computer
            ops.insert(0, {"op": "define_brush", "key": kind, **brushes.to_dict(brushes.brush(kind))})
        if self.canvas.snap_rulers and page.rulers:
            op["snap_ruler"] = True
        self.apply_ops(ops)

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
            self.eraser_size.blockSignals(True)
            self.eraser_size.setValue(self.eraser_mm)
            self.eraser_size.blockSignals(False)
            self.flash(f"消しゴムの太さ {self.eraser_mm:g} mm", 2000)
            self.canvas.update()
            return
        width = self.brush.nudge_size(step)
        self.canvas.brush_width_mm = width
        self.canvas.update()
        self.flash(f"ペンの太さ {width:g} mm", 2000)

    def _eraser_size(self, value: float) -> None:
        self.eraser_mm = float(value)
        self.canvas.eraser_mm = self.eraser_mm
        self.canvas.update()

    def _preferences(self) -> None:
        from genko.app.preferences import PreferencesDialog

        PreferencesDialog(self).exec()

    def _help(self, what: str) -> None:
        from genko.app import help as helps

        if what == "keys":
            self.help_dialog = helps.show(self, "ショートカット一覧", helps.shortcut_html(self))
        elif what == "guide":
            self.help_dialog = helps.show(self, "はじめての使い方", helps.GUIDE)
        elif what == "faq":
            self.help_dialog = helps.show(self, "困ったとき", helps.FAQ)
        else:
            from genko import __version__

            self.help_dialog = helps.show(self, "Genko Studio について",
                                          f"<h2>Genko Studio</h2><p>版 {__version__}</p><p>マンガの原稿を、ネームから入稿まで描く道具。"
                                          "エージェント（AI）と分担して進めることもできます。</p>")

    def _fill_recent(self) -> None:
        self.recent_menu.clear()
        items = recent_projects()
        if not items:
            self.recent_menu.addAction("（まだありません）").setEnabled(False)
        for path in items[:12]:
            self.recent_menu.addAction(path.stem, lambda p=path: self.open_project(p))

    def _selected_line_or_say(self):
        line = self._line(self.canvas.selected_line_id) if self.canvas.selected_line_id else None
        if line is None:
            self.flash("先に選択ツール（V）で台詞（フキダシ）をクリックして選びます", 4000)
        return line

    def _edit_selected_line(self) -> None:
        line = self._selected_line_or_say()
        if line is not None:
            self._edit_line_inline(line.id)

    def _delete_selected_line(self) -> None:
        line = self._selected_line_or_say()
        if line is not None and self.apply_ops([{"op": "delete_line", "id": line.id}]):
            self.canvas.selected_line_id = None

    def _toggle_selected_wrap(self) -> None:
        from genko.app.lettering import refit

        line = self._selected_line_or_say()
        if line is None:
            return
        vertical = line.wrap != "vertical"
        size = refit(line, self.frame_by_id(line.frame_id), line.text, line.balloon, vertical)
        self.apply_ops([{"op": "edit_line", "id": line.id, "wrap": "vertical" if vertical else "horizontal"},
                        {"op": "move_line", "id": line.id, **size}])

    def _set_selected_balloon(self, kind: str) -> None:
        line = self._selected_line_or_say()
        if line is not None:
            self.apply_ops([{"op": "edit_line", "id": line.id, "balloon": kind}])

    def _make_brush(self) -> None:
        from genko import brushes
        from genko.app.brush_panel import BrushDialog
        from genko.models import new_id

        dialog = BrushDialog(self, self.brush.kind())
        if not dialog.exec():
            return
        key = f"my_{new_id()}"
        data = dialog.data()
        try:
            brushes.CUSTOM[key] = brushes.from_dict(key, data)
        except ValueError as exc:
            self.flash(wording.error(str(exc)), 6000, error=True)
            return
        brushes.save_to_library(key, brushes.to_dict(brushes.CUSTOM[key]))
        self.brush.reload_kinds(select=key)
        self.flash(f"ブラシ「{data['label']}」を作りました（ブラシの一覧の ★）", 4000)

    def _forget_brush(self) -> None:
        from genko import brushes

        key = self.brush.kind()
        if not key.startswith("my_"):
            return
        brushes.save_to_library(key, None)
        if key not in self.episode.brush_custom:
            brushes.CUSTOM.pop(key, None)
        self.brush.reload_kinds(select="gpen")
        self.flash("自作のブラシを一覧から消しました（描いた線はそのまま）", 4000)

    def _brush_changed(self) -> None:
        self.canvas.brush_width_mm = self.brush.size.value()
        self.canvas.live_brush = self.brush.stroke_fields()  # the line being drawn looks like the pen in hand
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

    def _start_warp(self, kind: str) -> None:
        if self._need_area() is None:
            return
        if self.canvas.tool != "marquee":
            self.canvas.set_tool("marquee")
        self.canvas.start_warp(kind)
        self.flash("点を引っぱって形を決め、Enter（または「自由変形を確定」）で確定します。Esc でやめます", 6000)

    def _warp_selection(self, warp: dict) -> None:
        area, layer = self._area(), self._paint_layer()
        if area is None or layer is None:
            return
        if self.apply_ops([{"op": "transform_area", "page": self._current().index, "layer_id": layer.id, "area": area, "warp": warp}]):
            self.canvas.set_selection(None)

    def _page_overview(self) -> None:
        from genko.app.navigator import PageOverview

        self.overview = PageOverview(self)
        self.overview.show()

    def _layer_move_started(self) -> None:
        """The picture of the layer being moved, for the canvas to carry under the pen."""
        from genko.render import layer_image

        page, layer = self._current(), self.target_layer()
        if page is None or layer is None:
            return
        dpi = max(24, min(150, round(self.canvas._scale * 25.4)))
        image = layer_image(page, layer, dpi, self.episode)
        box = image.getbbox()
        if box is None:
            self.canvas.move_image = None
            return
        piece = image.crop(box)
        data = piece.tobytes()
        qimage = QImage(data, piece.width, piece.height, piece.width * 4, QImage.Format.Format_RGBA8888).copy()
        mm = 25.4 / dpi
        self.canvas.move_image = (qimage, box[0] * mm, box[1] * mm, piece.width * mm, piece.height * mm)

    def _layer_moved(self, dx: float, dy: float) -> None:
        page, layer = self._current(), self._paint_layer()
        if page is None or layer is None:
            return
        w, h = page.spec.width_mm, page.spec.height_mm
        m = 30.0  # everything on the layer, and a little beyond the paper
        whole = {"poly": [[-m, -m], [w + m, -m], [w + m, h + m], [-m, h + m]]}
        self.apply_ops([{"op": "transform_area", "page": page.index, "layer_id": layer.id, "area": whole, "matrix": [1, 0, 0, 1, dx, dy]}])

    def _gradient(self, start, end) -> None:
        page, layer = self._current(), self._paint_layer()
        if page is None or layer is None:
            return
        rgb = list(self.brush.rgb)
        mode = self.gradient_mode.currentData()
        op = {"op": "gradient_fill", "page": page.index, "layer_id": layer.id, "from": start, "to": end}
        if mode == "white":
            op.update(rgb_from=rgb, rgb_to=[255, 255, 255])
        elif mode == "bw":
            op.update(rgb_from=[20, 20, 20], rgb_to=[255, 255, 255])
        else:
            op.update(rgb_from=rgb, opacity_to=0.0, shape="radial" if mode == "radial" else "linear")
        area = self._area()
        if area is not None:
            op["area"] = area
        self.apply_ops([op])

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
            size = {"floor": [min(r.width, 200.0), 1, min(r.width, 200.0)], "stairs": [side, side, side * 1.4],
                    "cylinder": [side * 0.8, side * 1.3, side * 0.8]}.get(kind, [side, side, side])
            op = {"op": "add_prim3d", "page": page.index, "kind": kind, "id": prim_id, "pos": [cx, cy, 0], "size": size}
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
            from genko.app.lettering import parse_marks, place_at

            if not typed:
                return
            text, runs, marks, styles = parse_marks(typed)
            frame = page.frame_at(x_mm, y_mm)
            fields = self.text_settings.line_fields()
            box = place_at(x_mm, y_mm, text, fields["balloon"], fields["vertical"], frame)
            op = {"op": "add_line", "page": page.index, "text": text, "balloon": fields["balloon"], **box}
            if fields["style"]:
                op["style"] = fields["style"]
            if frame is not None:
                op["frame_id"] = frame.id
            if runs:
                op["ruby_runs"] = runs
            if marks:
                op["emphasis_runs"] = marks
            if styles:
                op["style_runs"] = styles
            before = {ln.id for ln in self.episode.story}
            if self.apply_ops([op]):
                added = next((ln.id for ln in self.episode.story if ln.id not in before), None)
                self.canvas.selected_line_id = added
                self._tool("select")
                self._on_line_selected(added, False)

        self.canvas.open_editor(x_mm, y_mm, "", done)

    def _balloon_drawn(self, outline: list) -> None:
        """The text tool's balloon pen: the drawn outline becomes the balloon, then the words are typed."""
        page = self._current()
        if page is None:
            return
        xs, ys = [p[0] for p in outline], [p[1] for p in outline]
        cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2

        def done(typed: str | None) -> None:
            from genko.app.lettering import parse_marks

            if not typed:
                return
            text, runs, marks, styles = parse_marks(typed)
            fields = self.text_settings.line_fields()
            kind = fields["balloon"] if fields["balloon"] not in ("sfx", "none", "narration") else "speech"
            op = {"op": "add_line", "page": page.index, "text": text, "balloon": kind, "path": outline,
                  "wrap": "vertical" if fields["vertical"] else "horizontal"}
            frame = page.frame_at(cx, cy)
            if frame is not None:
                op["frame_id"] = frame.id
            if fields["style"]:
                op["style"] = fields["style"]
            if runs:
                op["ruby_runs"] = runs
            if marks:
                op["emphasis_runs"] = marks
            if styles:
                op["style_runs"] = styles
            before = {ln.id for ln in self.episode.story}
            if self.apply_ops([op]):
                added = next((ln.id for ln in self.episode.story if ln.id not in before), None)
                self.canvas.selected_line_id = added
                self._on_line_selected(added, False)

        self.canvas.open_editor(min(xs), min(ys), "", done)

    def _edit_line_inline(self, line_id: str) -> None:
        from genko.app.lettering import with_marks

        line = self._line(line_id)
        if line is None:
            return

        def done(typed: str | None) -> None:
            from genko.app.lettering import parse_marks, refit

            current = self._line(line_id)
            if not typed or current is None:
                return
            text, runs, marks, styles = parse_marks(typed)
            if (text, [list(r) for r in runs], marks, styles) == (current.text, [list(r) for r in current.ruby_runs],
                                                                  list(current.emphasis_runs), [list(r) for r in current.style_runs]):
                return
            ops = [{"op": "edit_line", "id": line_id, "text": text, "ruby_runs": runs, "emphasis_runs": marks, "style_runs": styles}]
            size = refit(current, self.frame_by_id(current.frame_id), text, current.balloon, current.wrap == "vertical")
            # keep the balloon's centre where it was
            cx, cy = current.x_mm + current.w_mm / 2, current.y_mm + current.h_mm / 2
            ops.append({"op": "move_line", "id": line_id, "x_mm": round(cx - size["w_mm"] / 2, 2), "y_mm": round(cy - size["h_mm"] / 2, 2),
                        "w_mm": size["w_mm"], "h_mm": size["h_mm"]})
            self.apply_ops(ops)

        self.canvas.open_editor(line.x_mm, line.y_mm, with_marks(line), done)

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
            self.flash("画像を読み込む前に、原稿を保存します（ファイル → 別の場所に保存）", 6000)
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
            self.flash(f"読み込めない画像です:\n{exc}", 6000, error=True)
            return
        self.commit_now()
        ref = AssetStore(self.path).put_bytes(buf.getvalue(), ".png")
        frame = self.selected_frame()
        place = {"op": "place_asset", "page": page.index, "asset": ref, "to": "art", "title": Path(path).stem}
        if frame is not None:
            place["frame_id"] = frame.id
        else:
            b = page.bleed_rect_mm()  # the whole page, out to the bleed
            place["placement_mm"] = [b.x, b.y, b.width, b.height]
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
            show.triggered.connect(lambda: self.show_dock("コマの詳細"))
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
            self.flash("先にコマをクリックして選びます（選択ツール）", 6000)
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
            self.flash("先にコマをクリックして選びます", 6000)
            return
        self.apply_ops([{"op": "set_frame", "page": self._current().index, "frame_id": frame.id, **change}])

    def _border_width(self) -> None:
        from PySide6.QtWidgets import QInputDialog

        frame = self.selected_frame()
        if frame is None:
            self.flash("先にコマをクリックして選びます", 6000)
            return
        value, ok = QInputDialog.getDouble(self, "枠線の太さ", "枠線の太さ（mm）", float(frame.border_mm), 0.0, 5.0, 2)
        if ok:
            self._set_selected_frame({"border_mm": value})

    def _toggle_bleed(self) -> None:
        frame = self.selected_frame()
        if frame is None:
            self.flash("先にコマをクリックして選びます", 6000)
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
            self.flash("先にコマをクリックして選びます（選択ツール）", 6000)
            return
        self.apply_ops([{"op": "merge_frame", "page": page.index, "frame_id": page.selected_frame_id}])

    def _add_page(self) -> None:
        page = self._current()
        if page is None:
            self.apply_ops([{"op": "add_page"}])
            self._page_index = len(self.episode.pages) - 1
            self._reload_pages()
            return
        self.add_page_after(page.index)

    def _paper_settings(self) -> None:
        from genko.app.dialogs import PaperDialog

        dialog = PaperDialog(self, self.episode.spec, changing=True)
        if dialog.exec() == QDialog.DialogCode.Accepted and self.apply_ops([dialog.op()]):
            self._reload_pages()
            self.canvas.fit_page()
            self.flash(f"原稿用紙を変えました: {self.episode.spec.describe()}", 5000)

    def _toggle_spread(self) -> None:
        page = self._current()
        if page is None:
            return
        if page.spread_with:
            self.set_spread(page.index, None)
        elif page.index < len(self.episode.pages):
            self.set_spread(page.index, page.index + 1)

    def _toggle_page_nombre(self) -> None:
        page = self._current()
        if page is not None:
            self.apply_ops([{"op": "set_nombre", "page": page.index, "numero": not page.numero}])

    def _run_checks(self) -> None:
        self.show_dock("点検")
        report = self.checks.run()
        self.flash("直すところは見つかりませんでした" if not report["issues"] else
                   f"止まる問題 {report['errors']} 件・確かめた方がよいこと {report['warnings']} 件（点検パネル）", 4000)

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
        page = self._current()
        dialog = ExportDialog(self, self.episode, self.path, self.session.actor, current_page=page.index if page else 1)
        dialog.exec()
        if dialog.fix_requested:
            self._run_checks()
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
    from genko.app import preferences

    if preferences.ui_font_pt():  # the size of the letters chosen in the preferences
        font = app.font()
        font.setPointSize(preferences.ui_font_pt())
        app.setFont(font)
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
