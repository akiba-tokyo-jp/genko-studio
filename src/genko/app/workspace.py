"""The screen made one's own (J1): named workspaces, a quick-access panel of chosen commands, a command
bar that can be changed, a search over every command, the dark screen, and the kind of cursor.

Everything is kept in the settings (QSettings), so it stays for every book.
"""

from __future__ import annotations

from PySide6.QtCore import QByteArray, Qt
from PySide6.QtGui import QAction, QColor, QKeySequence, QPalette
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QDialogButtonBox,
    QGridLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from genko.app.preferences import settings

THEMES = [("パソコンの設定のまま", "system"), ("明るい", "light"), ("暗い", "dark")]
CURSORS = [("ブラシの大きさの円", "circle"), ("円と十字", "circle_cross"), ("十字", "cross"), ("点", "dot")]
MODIFIER_TOOLS = [("何もしない", ""), ("スポイト", "picker"), ("選択", "select"), ("レイヤー移動", "move"), ("消しゴム", "eraser")]
DEFAULT_COMMANDBAR = ["元に戻す", "やり直す", "|", "全体を表示", "縮小", "拡大", "|", "◀ 前のページ", "次のページ ▶", "|", "書き出し…"]
DEFAULT_QUICK = ["ペン", "消しゴム", "塗りつぶし", "テキスト", "コマ割り", "選択範囲・選んだコマにトーンを貼る", "集中線", "左右反転して見る"]


# --- commands ---------------------------------------------------------------------------------------------


def commands(window) -> list[QAction]:
    """Every command in the menus (each once), in menu order."""
    out, seen = [], set()

    def walk(menu) -> None:
        for action in menu.actions():
            if action.isSeparator():
                continue
            if action.menu():
                walk(action.menu())
            elif action.text() and action.text() not in seen and not action.text().startswith("（"):
                seen.add(action.text())
                out.append(action)

    for top in window.menuBar().actions():
        if top.menu() is not None:
            walk(top.menu())
    return out


def by_text(window) -> dict[str, QAction]:
    return {action.text(): action for action in commands(window)}


def where(window, action: QAction) -> str:
    """The menu path of a command ('台詞 → フキダシの形')."""
    def find(menu, trail):
        for item in menu.actions():
            if item is action:
                return trail
            if item.menu() is not None:
                found = find(item.menu(), trail + [item.text()])
                if found is not None:
                    return found
        return None

    for top in window.menuBar().actions():
        if top.menu() is not None:
            found = find(top.menu(), [top.text()])
            if found is not None:
                return " → ".join(found)
    return ""


def search(window, words: str) -> list[QAction]:
    """Commands whose name, menu or explanation holds every word (in any order)."""
    parts = [w for w in words.lower().split() if w]
    out = []
    for action in commands(window):
        text = f"{action.text()} {where(window, action)} {action.statusTip()}".lower()
        if all(part in text for part in parts):
            out.append(action)
    return out


class CommandSearch(QDialog):
    """コマンドを探す (Ctrl+Shift+F): type part of a name, Enter does it."""

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.setWindowTitle("コマンドを探す")
        self.resize(520, 420)
        self.query = QLineEdit()
        self.query.setPlaceholderText("例: トーン、見開き、太く、反転…")
        self.results = QListWidget()
        self.query.textChanged.connect(self.refresh)
        self.query.returnPressed.connect(self.run)
        self.results.itemActivated.connect(lambda _: self.run())
        layout = QVBoxLayout(self)
        layout.addWidget(self.query)
        layout.addWidget(self.results, 1)
        self.found: list[QAction] = []
        self.refresh()

    def refresh(self) -> None:
        self.results.clear()
        self.found = [a for a in search(self.window, self.query.text()) if a.isEnabled()][:60]
        for action in self.found:
            keys = action.shortcut().toString(QKeySequence.SequenceFormat.NativeText)
            item = QListWidgetItem(f"{action.text()}　（{where(self.window, action)}）" + (f"　{keys}" if keys else ""))
            item.setToolTip(action.statusTip())
            self.results.addItem(item)
        if self.found:
            self.results.setCurrentRow(0)

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() in (Qt.Key.Key_Down, Qt.Key.Key_Up) and self.results.count():
            row = self.results.currentRow() + (1 if event.key() == Qt.Key.Key_Down else -1)
            self.results.setCurrentRow(max(0, min(self.results.count() - 1, row)))
            return
        super().keyPressEvent(event)

    def run(self) -> None:
        row = self.results.currentRow()
        if 0 <= row < len(self.found):
            action = self.found[row]
            self.accept()
            action.trigger()


# --- choosing commands (quick access, command bar) ------------------------------------------------------------


def chosen(key: str, default: list[str]) -> list[str]:
    value = settings().value(key, None)
    if value in (None, ""):
        return list(default)
    return [v for v in str(value).split("\t") if v]


def keep(key: str, names: list[str]) -> None:
    settings().setValue(key, "\t".join(names))


class ChooseCommands(QDialog):
    """Pick and order commands: all commands on the left, the chosen ones on the right."""

    def __init__(self, window, title: str, current: list[str], separators: bool = False) -> None:
        super().__init__(window)
        self.setWindowTitle(title)
        self.resize(640, 460)
        self.all = QListWidget()
        self.mine = QListWidget()
        self.filter = QLineEdit()
        self.filter.setPlaceholderText("探す")
        self.window_ = window
        self.filter.textChanged.connect(self._fill_all)
        for name in current:
            self.mine.addItem("――（区切り）" if name == "|" else name)
        add = QPushButton("→ 足す")
        add.clicked.connect(self._add)
        remove = QPushButton("← 外す")
        remove.clicked.connect(lambda: self.mine.takeItem(self.mine.currentRow()))
        up = QPushButton("↑")
        up.clicked.connect(lambda: self._move(-1))
        down = QPushButton("↓")
        down.clicked.connect(lambda: self._move(1))
        grid = QGridLayout()
        grid.addWidget(QLabel("コマンド"), 0, 0)
        grid.addWidget(QLabel("並べるもの"), 0, 2)
        grid.addWidget(self.filter, 1, 0)
        grid.addWidget(self.all, 2, 0, 5, 1)
        grid.addWidget(add, 2, 1)
        grid.addWidget(remove, 3, 1)
        grid.addWidget(up, 4, 1)
        grid.addWidget(down, 5, 1)
        if separators:
            line = QPushButton("区切りを足す")
            line.clicked.connect(lambda: self.mine.addItem("――（区切り）"))
            grid.addWidget(line, 6, 1)
        grid.addWidget(self.mine, 1, 2, 6, 1)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
                                   | QDialogButtonBox.StandardButton.RestoreDefaults)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("決める")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.button(QDialogButtonBox.StandardButton.RestoreDefaults).setText("はじめの並びに戻す")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        self.restore = buttons.button(QDialogButtonBox.StandardButton.RestoreDefaults)
        layout = QVBoxLayout(self)
        layout.addLayout(grid, 1)
        layout.addWidget(buttons)
        self._fill_all()

    def _fill_all(self) -> None:
        self.all.clear()
        for action in search(self.window_, self.filter.text()):
            self.all.addItem(action.text())

    def _add(self) -> None:
        item = self.all.currentItem()
        if item is not None and not self.mine.findItems(item.text(), Qt.MatchFlag.MatchExactly):
            self.mine.addItem(item.text())

    def _move(self, delta: int) -> None:
        row = self.mine.currentRow()
        if 0 <= row + delta < self.mine.count() and row >= 0:
            item = self.mine.takeItem(row)
            self.mine.insertItem(row + delta, item)
            self.mine.setCurrentRow(row + delta)

    def names(self) -> list[str]:
        return ["|" if self.mine.item(i).text().startswith("――") else self.mine.item(i).text() for i in range(self.mine.count())]


class QuickAccess(QWidget):
    """クイックアクセス: the commands one uses most, as buttons in one place."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.grid = QGridLayout()
        self.grid.setSpacing(3)
        edit = QPushButton("並べるものを選ぶ…")
        edit.clicked.connect(self.edit)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addLayout(self.grid)
        layout.addStretch(1)
        layout.addWidget(edit)
        self.buttons: list[QPushButton] = []

    def names(self) -> list[str]:
        return chosen("ui/quick", DEFAULT_QUICK)

    def refresh(self) -> None:
        for button in self.buttons:
            button.setParent(None)
        self.buttons = []
        actions = by_text(self.window)
        for n, name in enumerate(self.names()):
            action = actions.get(name)
            if action is None:
                continue
            button = QPushButton(action.text())
            button.setToolTip(action.statusTip() or action.text())
            if not action.icon().isNull():
                button.setIcon(action.icon())
            button.clicked.connect(action.trigger)
            button.setMinimumWidth(60)
            self.buttons.append(button)
        # two to a row; a long name takes a whole row (so it is not cut)
        row = col = 0
        for button in self.buttons:
            wide = len(button.text()) > 7
            if wide and col:
                row, col = row + 1, 0
            self.grid.addWidget(button, row, col, 1, 2 if wide else 1)
            col = 0 if wide else col + 1
            if col == 2 or wide:
                row, col = row + 1, 0

    def edit(self) -> bool:
        dialog = ChooseCommands(self.window, "クイックアクセスに並べるもの", self.names())
        dialog.restore.clicked.connect(lambda: (keep("ui/quick", DEFAULT_QUICK), dialog.reject()))
        if dialog.exec() == QDialog.DialogCode.Accepted:
            keep("ui/quick", dialog.names())
        self.refresh()
        return True


def fill_commandbar(window) -> None:
    """The command bar (under the menus) with the chosen commands."""
    bar = window.command_bar
    bar.clear()
    actions = by_text(window)
    for name in chosen("ui/commandbar", DEFAULT_COMMANDBAR):
        if name == "|":
            bar.addSeparator()
        elif name in actions:
            bar.addAction(actions[name])


def edit_commandbar(window) -> None:
    dialog = ChooseCommands(window, "コマンドバーに並べるもの", chosen("ui/commandbar", DEFAULT_COMMANDBAR), separators=True)
    dialog.restore.clicked.connect(lambda: (keep("ui/commandbar", DEFAULT_COMMANDBAR), dialog.reject()))
    if dialog.exec() == QDialog.DialogCode.Accepted:
        keep("ui/commandbar", dialog.names())
    fill_commandbar(window)


# --- workspaces ---------------------------------------------------------------------------------------------


def workspaces() -> list[str]:
    store = settings()
    store.beginGroup("workspaces")
    names = sorted(store.childGroups())
    store.endGroup()
    return names


def save_workspace(window, name: str) -> None:
    store = settings()
    store.setValue(f"workspaces/{name}/state", window.saveState().toBase64().data().decode("ascii"))
    store.setValue(f"workspaces/{name}/geometry", window.saveGeometry().toBase64().data().decode("ascii"))


def load_workspace(window, name: str) -> bool:
    store = settings()
    state = store.value(f"workspaces/{name}/state", "")
    if not state:
        return False
    window.restoreGeometry(QByteArray.fromBase64(str(store.value(f"workspaces/{name}/geometry", "")).encode("ascii")))
    ok = window.restoreState(QByteArray.fromBase64(str(state).encode("ascii")))
    store.setValue("ui/workspace", name)
    return bool(ok)


def delete_workspace(name: str) -> None:
    settings().remove(f"workspaces/{name}")


# --- the screen's colours and the cursor ------------------------------------------------------------------------


def theme() -> str:
    value = str(settings().value("ui/theme", "system") or "system")
    return value if value in dict((k, 1) for _l, k in THEMES) else "system"


def apply_theme(mode: str | None = None) -> None:
    app = QApplication.instance()
    if app is None:
        return
    mode = mode or theme()
    if not hasattr(app, "_genko_palette"):
        app._genko_palette = QPalette(app.palette())  # (what the system gave, to go back to)
    if mode != "dark":
        app.setPalette(app._genko_palette if mode == "system" else app.style().standardPalette())
        return
    dark = QPalette()
    base, window, text = QColor(38, 40, 44), QColor(50, 53, 58), QColor(226, 228, 232)
    for role, colour in ((QPalette.ColorRole.Window, window), (QPalette.ColorRole.WindowText, text),
                         (QPalette.ColorRole.Base, base), (QPalette.ColorRole.AlternateBase, window),
                         (QPalette.ColorRole.ToolTipBase, window), (QPalette.ColorRole.ToolTipText, text),
                         (QPalette.ColorRole.Text, text), (QPalette.ColorRole.Button, window),
                         (QPalette.ColorRole.ButtonText, text), (QPalette.ColorRole.BrightText, QColor(255, 120, 80)),
                         (QPalette.ColorRole.Highlight, QColor(232, 89, 12)), (QPalette.ColorRole.HighlightedText, QColor("white")),
                         (QPalette.ColorRole.PlaceholderText, QColor(150, 152, 158)), (QPalette.ColorRole.Link, QColor(120, 170, 255))):
        dark.setColor(role, colour)
    dark.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.Text, QColor(120, 122, 128))
    dark.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.ButtonText, QColor(120, 122, 128))
    dark.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.WindowText, QColor(120, 122, 128))
    app.setPalette(dark)


def cursor_kind() -> str:
    value = str(settings().value("ui/cursor", "circle") or "circle")
    return value if value in dict((k, 1) for _l, k in CURSORS) else "circle"


def modifier_tool(key: str) -> str:
    """The tool a held modifier switches to: key "alt" (default スポイト) or "ctrl" (default 選択)."""
    default = {"alt": "picker", "ctrl": "select"}.get(key, "")
    value = settings().value(f"keys/{key}_tool", None)
    value = default if value is None else str(value)
    return value if value in dict((k, 1) for _l, k in MODIFIER_TOOLS) else default
