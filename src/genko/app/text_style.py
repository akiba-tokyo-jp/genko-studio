"""Styling the characters chosen in a line being typed (選んだ文字を大きく・太く・色を変える), without typing the
notation: the chosen part is wrapped in it ({太|…}, 《《…》》, ｜…《…》), and the page shows it styled."""

from __future__ import annotations

from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import QInputDialog, QMenu, QPlainTextEdit

# (label, the mark put around the chosen part; "" = 傍点, "ruby" = asks for the reading)
CHOICES = [("大きく", "大"), ("とても大きく", "特大"), ("小さく", "小"), ("太く", "太"), ("極太に", "極太"),
           ("赤に", "赤"), ("青に", "青"), ("白に", "白"), ("縦中横（数字などを横に並べる）", "縦中横"), ("傍点を付ける", ""),
           ("ルビを付ける…", "ruby")]
SHORTCUTS = {"太": "Ctrl+B", "大": "Ctrl+Shift+.", "小": "Ctrl+Shift+,"}


def wrap(edit: QPlainTextEdit, mark: str) -> bool:
    """The chosen characters wrapped in the notation for `mark` (False: nothing is chosen)."""
    cursor = edit.textCursor()
    chosen = cursor.selectedText().replace("\u2029", "\n")
    if not chosen:
        return False
    if mark == "ruby":
        reading, ok = QInputDialog.getText(edit, "ルビ", f"「{chosen}」の読み")
        if not ok or not reading.strip():
            return False
        text = f"｜{chosen}《{reading.strip()}》"
    elif mark == "":
        text = f"《《{chosen}》》"
    else:
        text = f"{{{mark}|{chosen}}}"
    cursor.insertText(text)
    edit.setTextCursor(cursor)
    return True


def menu_for(edit: QPlainTextEdit, parent_menu: QMenu | None = None) -> QMenu:
    """「選んだ文字を」: each style, enabled while characters are chosen."""
    menu = QMenu("選んだ文字を", parent_menu or edit)
    chosen = edit.textCursor().hasSelection()
    for label, mark in CHOICES:
        action = menu.addAction(label, lambda m=mark: wrap(edit, m))
        action.setEnabled(chosen)
        if mark in SHORTCUTS:
            action.setShortcut(QKeySequence(SHORTCUTS[mark]))
    if not chosen:
        menu.setToolTip("先に文字をドラッグして選びます")
    return menu


def install(edit: QPlainTextEdit) -> None:
    """The right-click menu gets 「選んだ文字を」, and Ctrl+B and the others work while typing."""
    def show_menu(pos) -> None:
        menu = edit.createStandardContextMenu()
        menu.addSeparator()
        menu.addMenu(menu_for(edit, menu))
        edit._menu_open = True  # (an in-place editor keeps the line open while its menu is up)
        try:
            menu.exec(edit.mapToGlobal(pos))
        finally:
            edit._menu_open = False

    from PySide6.QtCore import Qt

    edit.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
    edit.customContextMenuRequested.connect(show_menu)
    for mark, keys in SHORTCUTS.items():
        action = QAction(edit)
        action.setShortcut(QKeySequence(keys))
        action.setShortcutContext(Qt.ShortcutContext.WidgetShortcut)
        action.triggered.connect(lambda _=False, m=mark: wrap(edit, m))
        edit.addAction(action)
