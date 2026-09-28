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
    QVBoxLayout,
    QWidget,
)
from genko.app import theme

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
    "vector": ("線の編集（Shift+Y）", "ペンの線の制御点を動かす・足す・消す。線をつなぐ・切る・色を変える"),
    "blend": ("色混ぜ（Shift+B）", "なぞった所の色をぼかす・のばす・なじませる"),
    "liquify": ("ゆがみ（Shift+L）", "なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く"),
    "shape": ("図形（O）", "ドラッグで直線・長方形・楕円・多角形。折れ線と曲線はクリックで点を置く"),
    "reshape": ("線の修正（Y）", "線をつまんでドラッグすると、その辺りが滑らかに曲がる"),
    "ruler": ("定規（R）", "下で定規の種類を選び、ドラッグやクリックで置く。□をドラッグで動かす、Delete で消す"),
    "3d": ("3D 操作（J）", "デッサン人形の関節（○）や箱をドラッグ。箱の上の○で回す"),
    "effect": ("効果線（K）", "下で種類を選び、コマの中をクリックで入れる。中心の＋をドラッグで動かす"),
    "stamp": ("素材を置く", "素材パネルで選んだ素材を、クリックした所に置く"),
    "move": ("レイヤー移動（Q）", "描く先のレイヤーの線・塗り・絵を、ドラッグで丸ごと動かす。Shift で縦・横・45° に"),
    "gradient": ("グラデーション（U）", "ドラッグの向きに塗る。始めの点がはじめの色、終わりの点が終わりの色。選択範囲があればその中だけ"),
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


def action_button(action) -> QWidget:
    """A command as a quiet row (its picture and words, left-aligned, lit on hover), or a switch as a check box
    — not a stack of identical framed buttons."""
    from PySide6.QtWidgets import QPushButton

    short = SHORT.get(action.text())
    if short:
        action.setIconText(short)
    if action.isCheckable():
        box = QCheckBox(action.iconText())
        box.setToolTip(action.toolTip() if action.toolTip() != action.text() else action.statusTip())
        box.setChecked(action.isChecked())
        box.toggled.connect(lambda on: action.isChecked() != on and action.trigger())
        action.toggled.connect(lambda on: _alive(box) and box.setChecked(on))
        return box
    button = QPushButton(action.iconText())
    button.setProperty("row", True)  # (drawn as a row: theme.py)
    button.setIcon(_or_blank(action.icon()))
    button.setToolTip(action.statusTip() or action.toolTip())
    button.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
    button.clicked.connect(action.trigger)

    def follow() -> None:
        if _alive(button):
            button.setEnabled(action.isEnabled())
            button.setVisible(action.isVisible())
            button.setIcon(_or_blank(action.icon()))

    action.changed.connect(follow)
    follow()
    return button


def _or_blank(icon):
    """Rows without a picture keep its room, so every row's words start at the same place."""
    if not icon.isNull():
        return icon
    from PySide6.QtGui import QIcon, QPixmap

    blank = QPixmap(16, 16)
    blank.fill(Qt.GlobalColor.transparent)
    return QIcon(blank)


def _alive(widget) -> bool:
    try:
        widget.objectName()
        return True
    except RuntimeError:  # (its panel was closed)
        return False


def section(title: str) -> QLabel:
    """A section's name over its rows: small, bold and quiet."""
    label = QLabel(title)
    theme.role(label, "section")
    return label


def menu_button(label: str, groups: list) -> QWidget:
    """One button that opens a menu of actions (groups split by lines): a long list kept short."""
    from PySide6.QtGui import QIcon
    from PySide6.QtWidgets import QMenu, QPushButton

    button = QPushButton(label + " ▾")
    button.setProperty("row", True)
    button.setIcon(_or_blank(QIcon()))
    menu = QMenu(button)
    for n, group in enumerate(groups):
        if n:
            menu.addSeparator()
        for action in group:
            menu.addAction(action)
    button.setMenu(menu)
    return button


def action_page(actions, extra: list[QWidget] | None = None) -> QWidget:
    """A tool's page: sections (a str in the list starts one) of command rows and switches; None is a gap."""
    page = QWidget()
    page.setObjectName("toolPage")  # (its switches line up with the rows: theme.py)
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(1)
    for action in actions:
        if action is None:
            layout.addSpacing(8)
        elif isinstance(action, str):
            if layout.count():
                layout.addSpacing(8)
            layout.addWidget(section(action))
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
        self.draw_balloon = QCheckBox("フキダシを手で描く")
        self.draw_balloon.setToolTip("ドラッグで囲んだ形がフキダシになり、そのあと台詞を打ちます")
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
        form.addRow("", self.draw_balloon)
        form.addRow("書体", self.font)
        form.addRow("文字の大きさ", self.size)
        note = QLabel("ルビは ｜約束《やくそく》、傍点は 《《強調》》、一部を大きく {大|…}（特大・小・太・赤・青・白も）と打ちます。"
                      "入れた後の台詞は、台詞パネルで直せます。")
        note.setWordWrap(True)
        theme.hint(note)
        form.addRow(note)

    def line_fields(self) -> dict:
        style = {}
        if self.font.currentData():
            style["font"] = self.font.currentData()
        if self.size.value() > 0:
            style["size_mm"] = self.size.value()
        return {"balloon": self.balloon.currentData(), "vertical": self.vertical.isChecked(), "style": style}


class _CurrentStack(QWidget):
    """The tools' pages, one shown at a time, as tall as the page shown (a QStackedWidget measures every page,
    so a short page sat over empty room and a scroll bar)."""

    def __init__(self) -> None:
        super().__init__()
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._pages: list[QWidget] = []
        self._current: QWidget | None = None

    def addWidget(self, widget: QWidget) -> None:  # noqa: N802
        self._pages.append(widget)
        self._layout.addWidget(widget)
        widget.setVisible(self._current is None)
        if self._current is None:
            self._current = widget

    def indexOf(self, widget: QWidget) -> int:  # noqa: N802
        return self._pages.index(widget) if widget in self._pages else -1

    def count(self) -> int:
        return len(self._pages)

    def widget(self, n: int) -> QWidget:
        return self._pages[n]

    def currentWidget(self) -> QWidget | None:  # noqa: N802
        return self._current

    def setCurrentWidget(self, widget: QWidget) -> None:  # noqa: N802
        if widget is self._current:
            return
        if self._current is not None:
            self._current.hide()
        self._current = widget
        widget.show()


class ToolSettings(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.title = QLabel()
        theme.role(self.title, "heading")
        self.hint = QLabel()
        self.hint.setWordWrap(True)
        theme.hint(self.hint)
        self.stack = _CurrentStack()
        from PySide6.QtWidgets import QHBoxLayout, QPushButton

        # the settings fold to the tool's name (room for the approval box below, when an agent works on the book)
        self.fold = QPushButton("▾")
        self.fold.setProperty("iconbtn", True)
        self.fold.setFixedSize(24, 24)
        self.fold.setCheckable(True)
        self.fold.setToolTip("ツールの設定をたたむ・開く")
        self.fold.toggled.connect(self.set_folded)
        head = QHBoxLayout()
        head.addWidget(self.title, 1)
        head.addWidget(self.fold)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)
        layout.addLayout(head)
        layout.addWidget(self.hint)
        layout.addWidget(self.stack, 1)
        self.folded = False
        self.pages: dict[str, QWidget] = {}
        self.tool = "select"

    def add(self, tools: tuple[str, ...], widget: QWidget) -> None:
        if self.stack.indexOf(widget) < 0:
            self.stack.addWidget(widget)
        for tool in tools:
            self.pages[tool] = widget

    def set_folded(self, on: bool) -> None:
        """Only the tool's name (and its dock no taller than that), or everything."""
        self.folded = on
        self.fold.setText("▸" if on else "▾")
        if not on and self.isVisible():
            from genko.app import comfort

            comfort.fade_in(self.stack, 160)
        self.hint.setVisible(not on and theme.show_hints() and bool(self.hint.text()))
        self.stack.setVisible(not on and self.pages.get(self.tool) is not None)
        dock = self.parentWidget()
        while dock is not None and not dock.inherits("QDockWidget"):
            dock = dock.parentWidget()
        if dock is not None:
            dock.setMaximumHeight(self.title.sizeHint().height() + 18 if on else 16777215)

    def show_tool(self, tool: str) -> None:
        self.tool = tool
        title, hint = TITLES.get(tool, (tool, ""))
        self.title.setText(title)
        self.hint.setText(hint)
        widget = self.pages.get(tool)
        if widget is not None:
            if widget is not self.stack.currentWidget() and self.isVisible():
                from genko.app import comfort

                comfort.fade_in(widget, 140)  # (the new tool's settings come in softly)
            self.stack.setCurrentWidget(widget)
            self.stack.setVisible(not self.folded)
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
            theme.hint(hint)
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
