"""ツールの設定: what the tool in hand can do, right next to it — the brush for the pen, the size for the
eraser, the balloon and face for the text tool, the panel commands for the frame tool, and so on."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QLabel,
    QSizePolicy,
    QStackedWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

TITLES = {
    "select": ("選択（V）", "コマをクリックで選ぶ・フキダシをドラッグで動かす・ダブルクリックで打ち直す・何もない所のドラッグで表示を動かす"),
    "pen": ("ペン（B）", "描く先はレイヤー パネルで選んだレイヤー。Shift で直線、[ ] で太さ"),
    "eraser": ("消しゴム（E）", "ペンの線は触れた所で切れる。トーンの上では削る"),
    "text": ("テキスト（T）", "台詞を入れたい所をクリックして打つ（Ctrl+Enter で決定）"),
    "frame": ("コマ割り（F）", "コマの中をドラッグで割る（ほぼ水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで動かす・角をドラッグで形を変える"),
    "picker": ("スポイト（I）", "クリックした所の色をペンの色にする"),
    "fill": ("塗りつぶし（G）", "線で囲まれた所をクリックで塗る。隙間は「隙間を閉じる」の幅まで閉じる"),
    "lassofill": ("囲って塗る（Shift+G）", "ドラッグで囲んだ所を塗る"),
    "marquee": ("範囲選択（M・L・W）", "ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み）"),
    "reshape": ("線の修正（Y）", "線をつまんでドラッグすると、その辺りが滑らかに曲がる"),
    "ruler": ("定規（R）", "下で定規の種類を選び、ドラッグやクリックで置く。□をドラッグで動かす、Delete で消す"),
    "3d": ("3D 操作（J）", "デッサン人形の関節（○）や箱をドラッグ。箱の上の○で回す"),
    "effect": ("効果線（K）", "下で種類を選び、コマの中をクリックで入れる。中心の＋をドラッグで動かす"),
    "stamp": ("素材を置く", "素材パネルで選んだ素材を、クリックした所に置く"),
}


SHORT = {
    "コマを横に割る（上下に分ける）": "横に割る（上下に）", "コマを縦に割る（左右に分ける）": "縦に割る（左右に）",
    "コマを結合（割る前に戻す）": "結合（割る前に戻す）", "テンプレートでコマを割る…": "テンプレートで割る…",
    "選んだコマの枠線の太さ…": "枠線の太さ…", "選んだコマの枠線をなくす": "枠線をなくす",
    "選んだコマを断ち切りにする（紙の端まで）": "断ち切りにする", "選んだコマの形を元に戻す": "形を元に戻す",
    "選択範囲・選んだコマにトーンを貼る": "トーンを貼る", "3D を線にする（描く先のレイヤーへ）": "3D を線にする",
    "このページの定規をすべて消す": "定規をすべて消す", "放射線定規（集中線）": "放射線定規",
    "選択範囲の線の太さ…": "線の太さ…",
}


def action_button(action) -> QToolButton:
    short = SHORT.get(action.text())
    if short:
        action.setIconText(short)
    button = QToolButton()
    button.setDefaultAction(action)
    button.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
    button.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
    return button


def action_page(actions, extra: list[QWidget] | None = None) -> QWidget:
    page = QWidget()
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(3)
    for action in actions:
        if action is None:
            line = QLabel()
            line.setFixedHeight(4)
            layout.addWidget(line)
        elif isinstance(action, QWidget):
            layout.addWidget(action)
        else:
            layout.addWidget(action_button(action))
    for widget in extra or []:
        layout.addWidget(widget)
    layout.addStretch(1)
    return page


class TextToolSettings(QWidget):
    """How a new line typed with the text tool starts: its balloon, direction, face and size."""

    def __init__(self) -> None:
        from genko import fonts
        from genko.app.lettering import KINDS

        super().__init__()
        self.balloon = QComboBox()
        for key, label in KINDS:
            self.balloon.addItem(label, key)
        self.vertical = QCheckBox("縦書き")
        self.vertical.setChecked(True)
        self.font = QComboBox()
        self.font.addItem("いつもの書体（アンチック）", "")
        for key, (label, *_rest) in fonts.BUNDLED.items():
            if key != "antique":
                self.font.addItem(label, key)
        self.size = QDoubleSpinBox()
        self.size.setRange(0, 60)
        self.size.setSingleStep(0.5)
        self.size.setSuffix(" mm")
        self.size.setSpecialValueText("自動（フキダシに合わせる）")
        form = QFormLayout(self)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapAllRows)
        form.setContentsMargins(0, 0, 0, 0)
        form.addRow("フキダシ", self.balloon)
        form.addRow("", self.vertical)
        form.addRow("書体", self.font)
        form.addRow("文字の大きさ", self.size)
        note = QLabel("入れた後の台詞は、台詞パネルで直せます。")
        note.setWordWrap(True)
        note.setStyleSheet("color:#666")
        form.addRow(note)

    def line_fields(self) -> dict:
        style = {}
        if self.font.currentData():
            style["font"] = self.font.currentData()
        if self.size.value() > 0:
            style["size_mm"] = self.size.value()
        return {"balloon": self.balloon.currentData(), "vertical": self.vertical.isChecked(), "style": style}


class ToolSettings(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.title = QLabel()
        self.title.setStyleSheet("font-weight:bold; font-size:13px")
        self.hint = QLabel()
        self.hint.setWordWrap(True)
        self.hint.setStyleSheet("color:#555")
        self.stack = QStackedWidget()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.addWidget(self.title)
        layout.addWidget(self.hint)
        layout.addWidget(self.stack, 1)
        self.pages: dict[str, QWidget] = {}
        self.tool = "select"

    def add(self, tools: tuple[str, ...], widget: QWidget) -> None:
        if self.stack.indexOf(widget) < 0:
            self.stack.addWidget(widget)
        for tool in tools:
            self.pages[tool] = widget

    def show_tool(self, tool: str) -> None:
        self.tool = tool
        title, hint = TITLES.get(tool, (tool, ""))
        self.title.setText(title)
        self.hint.setText(hint)
        widget = self.pages.get(tool)
        if widget is not None:
            self.stack.setCurrentWidget(widget)
            self.stack.setVisible(True)
        else:
            self.stack.setVisible(False)


def fit_narrow(root: QWidget) -> None:
    """Let a side panel shrink to the side's width: lists of choices show their start and open wide, long
    labels wrap, form rows put the field under its name, a box's note goes under its title, and buttons are
    only as wide as their words — instead of pushing the panel past the edge where the rest is cut off."""
    from PySide6.QtWidgets import QGroupBox, QPushButton, QScrollArea

    panel = root.widget() if isinstance(root, QScrollArea) else root
    if panel is not None and panel.layout() is not None:
        margins = panel.layout().contentsMargins()
        panel.layout().setContentsMargins(4, margins.top(), 4, margins.bottom())
    for combo in root.findChildren(QComboBox):
        combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        combo.setMinimumContentsLength(6)
        combo.setMinimumWidth(60)  # (the cached minimum hint would otherwise stay as wide as the longest item)
        combo.view().setMinimumWidth(max(combo.view().sizeHintForColumn(0) + 24, 120))
    for label in root.findChildren(QLabel):
        if len(label.text()) > 12 and label.pixmap().isNull():
            label.setWordWrap(True)
    for form in root.findChildren(QFormLayout):
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
    for box in root.findChildren(QGroupBox):
        title = box.title()
        if "（" in title and title.endswith("）") and box.layout() is not None and len(title) > 10:
            head, note = title[:-1].split("（", 1)
            box.setTitle(head)
            hint = QLabel(note)
            hint.setWordWrap(True)
            hint.setStyleSheet("color:#666")
            box.layout().insertWidget(0, hint) if hasattr(box.layout(), "insertWidget") else box.layout().insertRow(0, hint)
    for button in root.findChildren(QPushButton):
        if button.text() and button.maximumWidth() > 1000:  # (fixed-size buttons stay as they are)
            button.setMinimumWidth(button.fontMetrics().horizontalAdvance(button.text()) + 18
                                   + (button.iconSize().width() + 4 if not button.icon().isNull() else 0))
    # hidden pages don't tell their parents they got narrower; recount every layout once
    for widget in [root, *root.findChildren(QWidget)]:
        if widget.layout() is not None:
            widget.layout().invalidate()
        widget.updateGeometry()
