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
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

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


def apply_shortcuts(window) -> None:
    store = settings()
    for action in _commands(window):
        key = f"shortcuts/{action.text()}"
        if store.contains(key):
            value = str(store.value(key) or "")
            action.setShortcuts([QKeySequence(v) for v in value.split("|") if v] if value else [])


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
            self.table.setCellWidget(row, 1, editor)
            self.editors[action.text()] = editor
        self.clash = QLabel()
        self.clash.setStyleSheet("color:#c92a2a")
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
        work = QWidget()
        wl = QFormLayout(work)
        wl.addRow("画面の文字の大きさ", self.font_pt)
        wl.addRow("新しい原稿の用紙", self.paper)
        wl.addRow("変更を保存するまで", self.save_after)
        note = QLabel("文字の大きさは、次に Genko を開いたときから変わります。")
        note.setStyleSheet("color:#666")
        wl.addRow("", note)
        tabs.addTab(work, "表示・作業")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("決める")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addWidget(tabs, 1)
        layout.addWidget(buttons)
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

    def save(self) -> None:
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
        store.setValue("new/paper", self.paper.currentData())
        store.setValue("save/after_ms", self.save_after.value() * 1000)
        store.setValue("tablet/gamma", "" if self.gamma is None else self.gamma)
        store.setValue("tablet/button", self.button.currentData())
        apply_all(self.window)
        self.accept()
