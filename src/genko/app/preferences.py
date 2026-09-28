"""環境設定: the person's own ways of working, kept in the app's settings (not in the book).

- Shortcuts: any command's keys can be changed; two commands on the same keys are refused.
- The pen tablet: a scribble pad measures how hard the person presses and sets the pressure curve to
  match; the pen's side button can be the eyedropper, the hand (move the view) or the right click.
- The screen: the size of the letters, the paper a new book starts on, how soon changes are saved.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QSettings, Qt
from PySide6.QtGui import QAction, QKeySequence, QPainter, QPen
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QHeaderView,
    QKeySequenceEdit,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)
from genko.app import theme

PEN_BUTTONS = [("右クリック（メニュー）", "menu"), ("スポイト", "picker"), ("手のひら（表示を動かす）", "hand")]
SAVE_AFTER = (500, 30000)


def settings() -> QSettings:
    return QSettings("Genko", "Genko Studio")


def ui_font_pt() -> int:
    return int(settings().value("ui/font_pt", 0) or 0)  # 0: the system's size


def default_paper() -> str:
    return str(settings().value("new/paper", "") or "")


def save_after_ms() -> int:
    value = int(settings().value("save/after_ms", 1000) or 1000)
    return max(SAVE_AFTER[0], min(SAVE_AFTER[1], value))


def tablet_gamma() -> float | None:
    value = settings().value("tablet/gamma", None)
    return float(value) if value not in (None, "") else None


def pen_button() -> str:
    value = str(settings().value("tablet/button", "menu") or "menu")
    return value if value in dict((k, 1) for _l, k in PEN_BUTTONS) else "menu"


def gamma_for(pressures: list[float]) -> float:
    """The pressure curve that turns the person's usual pressing (the median) into the middle width."""
    usable = sorted(p for p in pressures if 0.02 < p < 0.999)
    if len(usable) < 10:
        raise ValueError("もう少し長く試し描きします")
    median = usable[len(usable) // 2]
    return round(max(0.3, min(3.0, math.log(0.5) / math.log(median))), 2)


def _commands(window) -> list[QAction]:
    seen, out = set(), []
    for action in window.findChildren(QAction):
        text = action.text()
        if not text or action.isSeparator() or action.menu() or text in seen or not action.objectName().startswith("cmd:"):
            continue
        seen.add(text)
        out.append(action)
    return sorted(out, key=lambda a: a.objectName())


def name_commands(window) -> None:
    """Give every command a lasting name (its words) and remember its own keys."""
    window.default_shortcuts = {}
    for action in window.findChildren(QAction):
        text = action.text()
        if text and not action.isSeparator() and not action.menu() and not action.objectName():
            action.setObjectName(f"cmd:{text}")
            window.default_shortcuts[text] = [QKeySequence(k) for k in action.shortcuts()]


RENAMED = {"レイヤーを消す": "レイヤーを削除", "このページを消す…": "このページを削除…"}  # (new name: old name)


def apply_shortcuts(window) -> None:
    store = settings()
    for action in _commands(window):
        key = f"shortcuts/{action.text()}"
        old = RENAMED.get(action.text())
        if not store.contains(key) and old and store.contains(f"shortcuts/{old}"):  # (keys chosen under the old name)
            key = f"shortcuts/{old}"
        if store.contains(key):
            value = str(store.value(key) or "")
            action.setShortcuts([QKeySequence(v) for v in value.split("|") if v] if value else [])
    retip(window)


def retip(window) -> None:
    """Every command's tooltip says its name, its keys as they are now, and what it does (written again when the
    keys change, so a changed key never shows the old one)."""
    for action in _commands(window):
        keys = action.shortcut().toString(QKeySequence.SequenceFormat.NativeText)
        what = action.statusTip()
        action.setToolTip(action.text().replace("&", "") + (f"（{keys}）" if keys else "") + (f"\n{what}" if what else ""))


def conflicts(mapping: dict[str, list[str]]) -> list[tuple[str, str, str]]:
    """(keys, one command, the other) for keys given to two commands."""
    owner: dict[str, str] = {}
    out = []
    for name, keys in mapping.items():
        for key in keys:
            if not key:
                continue
            if key in owner and owner[key] != name:
                out.append((key, owner[key], name))
            owner.setdefault(key, name)
    return out


def apply_all(window) -> None:
    """Everything the preferences change, applied to an open window."""
    apply_shortcuts(window)
    window._commit_timer.setInterval(save_after_ms())
    window.canvas.pen_button = pen_button()
    window.brush.set_personal_pressure(tablet_gamma())
    from genko.app import workspace

    workspace.apply_theme()
    from genko.app import comfort

    comfort.apply_motion()
    comfort.apply_font()
    if hasattr(window, "refresh_icons"):
        window.refresh_icons()
    window.canvas.update()
    window.canvas.cursor_kind = workspace.cursor_kind()
    window.canvas.modifier_tools = {"alt": workspace.modifier_tool("alt"), "ctrl": workspace.modifier_tool("ctrl")}
    window.canvas.tool_modifiers = workspace.tool_modifiers()
    window.canvas._update_cursor()


def tool_keys_dialog(parent) -> bool:
    """道具ごとの修飾キー: for each tool, what Alt, Ctrl and Shift held switch to (共通の設定のまま: the common one)."""
    from PySide6.QtWidgets import QDialog, QDialogButtonBox, QGridLayout, QLabel

    from genko.app import workspace

    dialog = QDialog(parent)
    dialog.setWindowTitle("道具ごとの修飾キー")
    grid = QGridLayout(dialog)
    for col, head in enumerate(("", "Alt", "Ctrl", "Shift")):
        grid.addWidget(QLabel(head), 0, col)
    now = workspace.tool_modifiers()
    boxes = {}
    for row, (label, tool) in enumerate(workspace.KEYED_TOOLS, start=1):
        grid.addWidget(QLabel(label), row, 0)
        for col, key in enumerate(("alt", "ctrl", "shift"), start=1):
            box = QComboBox()
            for text, value in workspace.HELD_TOOLS:
                box.addItem(text, value)
            box.setCurrentIndex(max(0, box.findData((now.get(tool) or {}).get(key, ""))))
            grid.addWidget(box, row, col)
            boxes[(tool, key)] = box
    note = QLabel("Shift は、決めた道具だけで持ち替えます（それ以外は直線を引くなど、いつもの働き）。")
    note.setWordWrap(True)
    grid.addWidget(note, len(workspace.KEYED_TOOLS) + 1, 0, 1, 4)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    grid.addWidget(buttons, len(workspace.KEYED_TOOLS) + 2, 0, 1, 4)
    dialog.boxes = boxes
    if not dialog.exec():
        return False
    for (tool, key), box in boxes.items():
        workspace.set_tool_modifier(tool, key, box.currentData())
    window = parent.window() if hasattr(parent, "window") else None
    canvas = getattr(getattr(parent, "main", None) or window, "canvas", None)
    if canvas is not None:
        canvas.tool_modifiers = workspace.tool_modifiers()
    return True


class PressurePad(QWidget):
    """Scribble here as usual: the pad keeps the pressures the pen reported."""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(320, 140)
        self.setAttribute(Qt.WidgetAttribute.WA_TabletTracking, True)
        self.pressures: list[float] = []
        self.points: list[tuple[QPointF, float]] = []

    def tabletEvent(self, event) -> None:  # noqa: N802
        from PySide6.QtCore import QEvent

        if event.type() in (QEvent.Type.TabletPress, QEvent.Type.TabletMove) and event.pressure() > 0:
            self.pressures.append(float(event.pressure()))
            self.points.append((event.position(), float(event.pressure())))
            self.update()
        event.accept()

    def clear(self) -> None:
        self.pressures, self.points = [], []
        self.update()

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.fillRect(self.rect(), Qt.GlobalColor.white)
        painter.setPen(QPen(Qt.GlobalColor.lightGray, 1))
        painter.drawRect(self.rect().adjusted(0, 0, -1, -1))
        if not self.points:
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "ここにペンでいつものように描きます")
        for position, pressure in self.points:
            painter.setPen(QPen(Qt.GlobalColor.black, 0.5 + 6 * pressure, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawPoint(position)


class PreferencesDialog(QDialog):
    def __init__(self, window) -> None:
        super().__init__(window)
        from genko.models import PAPER_PRESETS

        self.window = window
        self.setWindowTitle("環境設定")
        self.setMinimumSize(560, 480)
        tabs = QTabWidget()
        # shortcuts
        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(["コマンド", "ショートカット"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.table.verticalHeader().setVisible(False)
        self.editors: dict[str, QKeySequenceEdit] = {}
        for action in _commands(window):
            row = self.table.rowCount()
            self.table.insertRow(row)
            item = QTableWidgetItem(action.text())
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
            self.table.setItem(row, 0, item)
            editor = QKeySequenceEdit(action.shortcut())
            field = editor.findChild(QLineEdit)
            if field is not None:
                field.setPlaceholderText("キーを押す")
            editor.setAccessibleName(f"{action.text()} のショートカット")
            self.table.setCellWidget(row, 1, editor)
            self.editors[action.text()] = editor
        self.clash = QLabel()
        theme.role(self.clash, "error")
        self.clash.setWordWrap(True)
        reset = QPushButton("すべて最初のキーに戻す")
        reset.clicked.connect(self._reset_keys)
        keys = QWidget()
        kl = QVBoxLayout(keys)
        kl.addWidget(QLabel("キーを変えたいコマンドの右をクリックして、新しいキーを押します。"))
        kl.addWidget(self.table, 1)
        kl.addWidget(self.clash)
        kl.addWidget(reset)
        tabs.addTab(keys, "ショートカット")
        # the pen tablet
        self.pad = PressurePad()
        self.gamma_note = QLabel()
        self.gamma_note.setWordWrap(True)
        self.gamma = tablet_gamma()
        measure = QPushButton("この描き方に合わせる")
        measure.clicked.connect(self._measure)
        again = QPushButton("描き直す")
        again.clicked.connect(self.pad.clear)
        forget = QPushButton("合わせるのをやめる")
        forget.clicked.connect(self._forget_gamma)
        self.button = QComboBox()
        for label, key in PEN_BUTTONS:
            self.button.addItem(label, key)
        self.button.setCurrentIndex(max(0, self.button.findData(pen_button())))
        tablet = QWidget()
        tl = QVBoxLayout(tablet)
        tl.addWidget(QLabel("筆圧の試し書き: いつもの強さで線を何本か引き、「この描き方に合わせる」を押します。"))
        tl.addWidget(self.pad, 1)
        row = QFormLayout()
        row.addRow("", measure)
        row.addRow("", again)
        row.addRow("", forget)
        row.addRow("", self.gamma_note)
        row.addRow("ペンのサイドボタン", self.button)
        tl.addLayout(row)
        tabs.addTab(tablet, "ペンタブレット")
        # the screen and work
        self.ui_font = QComboBox()
        for label, key in theme.FONT_CHOICES:
            self.ui_font.addItem(label, key)
        self.ui_font.setCurrentIndex(max(0, self.ui_font.findData(theme.font_choice())))
        self.ui_font.setToolTip("Genko の標準は、どのパソコンでも同じ見た目になるよう同梱した書体です（SIL OFL）")
        self.font_pt = QSpinBox()
        self.font_pt.setRange(0, 24)
        self.font_pt.setSpecialValueText("パソコンの設定のまま")
        self.font_pt.setSuffix(" pt")
        self.font_pt.setValue(ui_font_pt())
        self.paper = QComboBox()
        self.paper.addItem("前回と同じ（B4 投稿用）", "")
        for key, (label, _make) in PAPER_PRESETS.items():
            self.paper.addItem(label, key)
        self.paper.setCurrentIndex(max(0, self.paper.findData(default_paper())))
        self.save_after = QSpinBox()
        self.save_after.setRange(1, 30)
        self.save_after.setSuffix(" 秒")
        self.save_after.setValue(max(1, round(save_after_ms() / 1000)))
        from genko.app import workspace

        self.theme = QComboBox()
        for label, key in workspace.THEMES:
            self.theme.addItem(label, key)
        self.theme.setCurrentIndex(max(0, self.theme.findData(workspace.theme())))
        from PySide6.QtWidgets import QCheckBox, QSlider

        self.brightness = QSlider(Qt.Orientation.Horizontal)
        self.brightness.setRange(-2, 2)
        self.brightness.setPageStep(1)
        self.brightness.setTickPosition(QSlider.TickPosition.TicksBelow)
        self.brightness.setValue(theme.brightness())
        self.brightness.setToolTip("パネルのグレーの明るさ（左ほど暗く、右ほど明るい）")
        self.surround = QComboBox()
        for label, key in (("自動（画面の色に合わせる）", "auto"), ("暗いグレー", "58"), ("中間のグレー", "128"),
                           ("明るいグレー", "190"), ("黒", "20")):
            self.surround.addItem(label, key)
        self.surround.setCurrentIndex(max(0, self.surround.findData(theme.surround_setting())))
        self.surround.setToolTip("原稿のまわりの色。無彩色のグレーにしておくと、原稿のグレーの見え方が狂いません")
        self.mono_icons = QCheckBox("アイコンをグレーだけで描く")
        self.mono_icons.setChecked(theme.mono_icons())
        self.hints = QCheckBox("パネルに説明の文を出す")
        self.hints.setToolTip("切ると、説明はツールチップに入ります")
        self.hints.setChecked(theme.show_hints())
        from genko.app import comfort

        self.radial = QCheckBox("描く道具の右クリックで円形のメニュー")
        self.radial.setToolTip("Shift＋右クリックは、ふだんのメニューになります")
        self.radial.setChecked(comfort.radial_on())
        self.rest = QComboBox()
        for label, minutes in (("知らせない", 0), ("30 分ごと", 30), ("45 分ごと", 45), ("60 分ごと", 60), ("90 分ごと", 90)):
            self.rest.addItem(label, minutes)
        self.rest.setCurrentIndex(max(0, self.rest.findData(int(settings().value("ui/rest_minutes", 0) or 0))))
        self.rest.setToolTip("作業を続けた時間（手を止めていた時間は数えない）で、目を休める頃を知らせます")
        self.requests = QCheckBox("承認の依頼が届いたら、承認箱を前に出す")
        self.requests.setToolTip("AI から承認の依頼や相談が届いたとき")
        self.requests.setChecked(comfort.raise_requests())
        self.gpu = QCheckBox("原稿の表示にグラフィックボードを使う")
        self.gpu.setChecked(str(settings().value("ui/gpu_canvas", "false")).lower() in ("1", "true", "yes"))
        self.gpu.setToolTip("拡大・回転がなめらかになります（次に開いた窓から）。原稿が白いままになったら外してください。"
                            "対応していないパソコンでは、入れていても使いません")
        self.motion = QCheckBox("動きを減らす")
        self.motion.setToolTip("メニューの開き方・拡大の寄り方・知らせの出方などの動きを止めます（決めていなければパソコンの設定に合わせます）")
        self.motion.setChecked(comfort.reduce_motion())
        self.cursor = QComboBox()
        for label, key in workspace.CURSORS:
            self.cursor.addItem(label, key)
        self.cursor.setCurrentIndex(max(0, self.cursor.findData(workspace.cursor_kind())))
        self.alt_tool = QComboBox()
        self.ctrl_tool = QComboBox()
        for box, key in ((self.alt_tool, "alt"), (self.ctrl_tool, "ctrl")):
            for label, tool in workspace.MODIFIER_TOOLS:
                box.addItem(label, tool)
            box.setCurrentIndex(max(0, box.findData(workspace.modifier_tool(key))))
            box.setToolTip("押している間だけ、この道具になります（離すと元の道具に戻る）")
        from PySide6.QtWidgets import QHBoxLayout

        store0 = settings()
        self.backup_folder = QLineEdit(str(store0.value("backup/folder", "") or ""))
        self.backup_folder.setPlaceholderText("残さない")
        self.backup_folder.setToolTip("保存したとき、原稿をまるごと zip にしてこのフォルダに残します（原稿の外のフォルダ）")
        pick_backup = QPushButton("選ぶ…")
        pick_backup.clicked.connect(self._pick_backup)
        self.backup_row = QWidget()
        brow = QHBoxLayout(self.backup_row)
        brow.setContentsMargins(0, 0, 0, 0)
        brow.addWidget(self.backup_folder, 1)
        brow.addWidget(pick_backup)
        self.backup_minutes = QSpinBox()
        self.backup_minutes.setRange(1, 1440)
        self.backup_minutes.setSuffix(" 分ごと")
        self.backup_minutes.setValue(int(float(store0.value("backup/minutes", 30) or 30)))
        self.backup_keep = QSpinBox()
        self.backup_keep.setRange(1, 200)
        self.backup_keep.setSuffix(" 個まで残す")
        self.backup_keep.setValue(int(store0.value("backup/keep", 10) or 10))
        self.hold_swap = QCheckBox("道具のキーを押している間だけ持ち替える")
        self.hold_swap.setChecked(workspace.hold_swap())
        self.hold_swap.setToolTip("道具のキー（E の消しゴムなど）を長く押していると、離したときに前の道具に戻ります。短く押すと持ち替えたまま")
        self.per_tool = QPushButton("道具ごとの修飾キー…")
        self.per_tool.setToolTip("ペン・消しゴムなど道具ごとに、Alt・Ctrl・Shift を押している間の道具を決めます")
        self.per_tool.clicked.connect(lambda: tool_keys_dialog(self))
        from genko.app import dialog_look as look

        work = QWidget()
        wl = look.form()
        work.setLayout(wl)
        note = QLabel("文字の大きさは、決めるとすぐに変わります。")
        theme.role(note, "hint")
        for head, rows in (("見た目", [("画面の色", self.theme), ("パネルの明るさ", self.brightness), ("原稿のまわり", self.surround),
                                       ("画面の書体", self.ui_font), ("画面の文字の大きさ", self.font_pt), ("", note),
                                       ("", self.mono_icons), ("", self.hints), ("", self.gpu)]),
                           ("描く・操作", [("ペンのカーソル", self.cursor), ("Alt を押している間", self.alt_tool),
                                         ("Ctrl を押している間", self.ctrl_tool), ("", self.per_tool), ("", self.hold_swap), ("", self.radial)]),
                           ("長い時間の作業", [("休憩の案内", self.rest), ("", self.motion), ("", self.requests)]),
                           ("原稿と保存", [("新しい原稿の用紙", self.paper), ("変更を保存するまで", self.save_after),
                                        ("バックアップの置き場所", self.backup_row), ("バックアップの間隔", self.backup_minutes),
                                        ("バックアップの数", self.backup_keep)])):
            wl.addRow(look.section(head))
            for label, widget in rows:
                wl.addRow(label, widget)
        look.quiet_labels(wl)
        from PySide6.QtWidgets import QScrollArea

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setWidget(work)
        work = scroll
        tabs.addTab(work, "表示・作業")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("決める")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        # the pages chosen from a list at the side (as the main window's panels are chosen), not tabs across the top
        from PySide6.QtWidgets import QHBoxLayout, QListWidget, QListWidgetItem

        from genko.app.icons import icon

        tabs.tabBar().hide()
        tabs.setDocumentMode(True)
        self.pages_list = QListWidget()
        self.pages_list.setObjectName("prefsNav")
        self.pages_list.setFixedWidth(150)
        for i, (name, picture) in enumerate((("ショートカット", "search"), ("ペンタブレット", "pen"), ("表示・作業", "settings"))):
            self.pages_list.addItem(QListWidgetItem(icon(picture), tabs.tabText(i) or name))
        self.pages_list.currentRowChanged.connect(tabs.setCurrentIndex)
        tabs.currentChanged.connect(lambda i: self.pages_list.currentRow() != i and self.pages_list.setCurrentRow(i))
        self.pages_list.setCurrentRow(tabs.currentIndex())
        middle = QHBoxLayout()
        middle.setSpacing(16)
        middle.addWidget(self.pages_list)
        middle.addWidget(tabs, 1)
        body = QWidget()
        body.setLayout(middle)
        middle.setContentsMargins(0, 0, 0, 0)
        look.frame(self, look.header("環境設定", "このパソコンでの Genko の使い心地です。原稿には残りません。"), body, None, look.footer(buttons))
        self.resize(820, 620)
        self.tabs = tabs
        self._show_gamma()

    def _show_gamma(self) -> None:
        if self.gamma is None:
            self.gamma_note.setText("筆圧は「ふつう」の曲線です。")
        else:
            feel = "やわらかめ" if self.gamma < 0.9 else "かため" if self.gamma > 1.1 else "ふつう"
            self.gamma_note.setText(f"あなたに合わせた曲線（{feel}、γ={self.gamma:g}）。ブラシの「筆圧」に「自分に合わせた」が出ます。")

    def _measure(self) -> None:
        try:
            self.gamma = gamma_for(self.pad.pressures)
        except ValueError as exc:
            self.gamma_note.setText(str(exc))
            return
        self._show_gamma()

    def _forget_gamma(self) -> None:
        self.gamma = None
        self._show_gamma()

    def _reset_keys(self) -> None:
        defaults = getattr(self.window, "default_shortcuts", {})
        for name, editor in self.editors.items():
            keys = defaults.get(name) or []
            editor.setKeySequence(keys[0] if keys else QKeySequence())

    def mapping(self) -> dict[str, list[str]]:
        return {name: [editor.keySequence().toString()] if not editor.keySequence().isEmpty() else []
                for name, editor in self.editors.items()}

    def _pick_backup(self) -> None:
        from PySide6.QtWidgets import QFileDialog

        folder = QFileDialog.getExistingDirectory(self, "バックアップを残すフォルダ", self.backup_folder.text())
        if folder:
            self.backup_folder.setText(folder)

    def save(self) -> None:
        from genko.app import comfort

        mapping = self.mapping()
        clashes = conflicts(mapping)
        if clashes:
            key, first, second = clashes[0]
            self.clash.setText(f"「{key}」が「{first}」と「{second}」の両方に付いています。どちらかを変えます。")
            self.tabs.setCurrentIndex(0)
            return
        store = settings()
        defaults = getattr(self.window, "default_shortcuts", {})
        for action in _commands(self.window):
            keys = mapping.get(action.text(), [])
            first_default = [k.toString() for k in defaults.get(action.text(), [])][:1]
            key = f"shortcuts/{action.text()}"
            if keys == first_default:
                store.remove(key)
                action.setShortcuts(defaults.get(action.text(), []))
            else:
                store.setValue(key, "|".join(keys))
        store.setValue("ui/font_pt", self.font_pt.value())
        store.setValue("ui/ui_font", self.ui_font.currentData())
        store.setValue("new/paper", self.paper.currentData())
        store.setValue("save/after_ms", self.save_after.value() * 1000)
        store.setValue("tablet/gamma", "" if self.gamma is None else self.gamma)
        store.setValue("tablet/button", self.button.currentData())
        store.setValue("ui/theme", self.theme.currentData())
        store.setValue("ui/brightness", self.brightness.value())
        store.setValue("ui/surround", self.surround.currentData())
        store.setValue("ui/mono_icons", "true" if self.mono_icons.isChecked() else "false")
        theme.set_hints(self.hints.isChecked())
        store.setValue("ui/radial", "true" if self.radial.isChecked() else "false")
        store.setValue("ui/rest_minutes", self.rest.currentData())
        store.setValue("ui/raise_requests", "true" if self.requests.isChecked() else "false")
        store.setValue("ui/gpu_canvas", "true" if self.gpu.isChecked() else "false")
        if self.motion.isChecked() == comfort.system_reduces_motion():  # (as the computer is set: keep following it)
            store.remove("ui/reduce_motion")
        else:
            store.setValue("ui/reduce_motion", "true" if self.motion.isChecked() else "false")
        store.setValue("ui/cursor", self.cursor.currentData())
        store.setValue("keys/alt_tool", self.alt_tool.currentData())
        store.setValue("keys/ctrl_tool", self.ctrl_tool.currentData())
        store.setValue("keys/hold_swap", self.hold_swap.isChecked())
        store.setValue("backup/folder", self.backup_folder.text().strip())
        store.setValue("backup/minutes", self.backup_minutes.value())
        store.setValue("backup/keep", self.backup_keep.value())
        apply_all(self.window)
        self.accept()
