"""The history panel: every change in the order it was made, named in words ("ペンで描いた", "コマを割った"
…), with the ones undone greyed below the present. Clicking one goes back (or forward) to just after it.

The changes come from the book's journal on disk (saved changes) and from the session (the ones still in
memory, and the ones undone there). Going back and forth is plain undo and redo, one step at a time.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QLabel, QListWidget, QListWidgetItem, QVBoxLayout, QWidget

NAMES = {
    "add_stroke": "ペンで描いた", "erase": "消しゴムで消した", "fill": "塗りつぶした", "fill_area": "囲って塗った",
    "delete_area": "範囲を消した", "transform_area": "範囲を動かした・変形した", "paste": "貼り付けた",
    "set_stroke_width": "線の太さを変えた", "reshape_stroke": "線を修正した",
    "split_frame": "コマを割った", "cut_frame": "コマを割った", "merge_frame": "コマを結合した", "move_gutter": "コマの間を動かした",
    "set_frame": "コマの形を変えた", "apply_template": "テンプレートでコマを割った", "set_border": "枠線を変えた",
    "add_line": "台詞を入れた", "edit_line": "台詞を直した", "move_line": "フキダシを動かした", "delete_line": "台詞を消した",
    "reorder_lines": "台詞の順番を変えた", "set_balloon_path": "フキダシの形を描いた",
    "add_layer": "レイヤーを足した", "delete_layer": "レイヤーを消した", "set_layer": "レイヤーの設定を変えた",
    "reorder_layers": "レイヤーの順番を変えた", "duplicate_layer": "レイヤーを複製した", "merge_down": "レイヤーを結合した",
    "set_layer_mask": "マスクを変えた", "paint_mask": "マスクを描いた", "filter_raster": "フィルターをかけた",
    "add_page": "ページを足した", "delete_page": "ページを消した", "duplicate_page": "ページを複製した", "move_page": "ページを並べ替えた",
    "set_spread": "見開きを変えた", "set_page_spec": "原稿用紙を変えた", "set_nombre": "ノンブルを変えた",
    "add_tone": "トーンを貼った", "set_tone": "トーンを変えた", "add_effect": "効果線を入れた", "edit_effect": "効果線を変えた",
    "delete_effect": "効果線を消した", "effect_to_layer": "効果線を線にした", "stamp_material": "素材を置いた",
    "add_ruler": "定規を置いた", "edit_ruler": "定規を動かした", "delete_ruler": "定規を消した",
    "add_prim3d": "3D を置いた", "add_scene": "背景の 3D を置いた", "add_mannequin": "デッサン人形を置いた", "edit_prim": "3D を動かした", "pose_mannequin": "ポーズを変えた",
    "delete_prim": "3D を消した", "trace_prims": "3D を線にした", "import_raster": "画像を読み込んだ", "place_image": "画像を置いた",
    "define_brush": "ブラシを作った", "approve": "承認した", "reject": "差し戻した", "advance": "工程を進めた",
}


def describe(ops: list[dict]) -> str:
    """A change in words: the first thing it did (and how many more)."""
    names = [NAMES.get(str(op.get("op")), "原稿を変えた") for op in ops or []]
    if not names:
        return "原稿を変えた"
    first = names[0]
    same = all(name == first for name in names)
    if len(names) > 1 and same:
        return f"{first}（{len(names)} 回）"
    if len(names) > 1:
        return f"{first} ほか {len(names) - 1} 件"
    return first


def timeline(session) -> tuple[list[dict], list[dict]]:
    """(done, undone): the changes up to now, oldest first, and the ones that can be redone, next first."""
    from genko import journal

    done: list[dict] = []
    later: list[dict] = []
    if session.path is not None:
        undo_stack, redo_stack = journal.stacks(journal.entries(session.path))
        for item in undo_stack:
            if item.get("before") is None:
                continue  # the book being made: nothing before it to go back to
            done.append({"label": describe(item.get("ops") or []), "actor": item.get("actor", ""), "saved": True})
        disk_redo = [{"label": describe(item.get("ops") or []), "actor": item.get("actor", ""), "saved": True}
                     for item in reversed(redo_stack)]
    else:
        disk_redo = []
    for batch in session.pending:
        done.append({"label": describe(batch), "actor": session.actor, "saved": False})
    for batch in reversed(session.undone):
        later.append({"label": describe(batch), "actor": session.actor, "saved": False})
    later.extend(disk_redo)
    return done, later


class HistoryPanel(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.list = QListWidget()
        self.list.itemClicked.connect(self._go)
        note = QLabel("クリックで、その変更をした直後まで戻る（進む）。灰色は取り消した変更（やり直せる）。")
        note.setWordWrap(True)
        note.setStyleSheet("color:#666")
        layout = QVBoxLayout(self)
        layout.addWidget(self.list, 1)
        layout.addWidget(note)
        self.done = 0

    def refresh(self) -> None:
        done, later = timeline(self.window.session)
        self.done = len(done)
        self.list.clear()
        start = QListWidgetItem("（はじめ）")
        start.setData(Qt.ItemDataRole.UserRole, 0)
        self.list.addItem(start)
        me = self.window.session.actor
        for n, entry in enumerate(done, 1):
            who = "" if not entry["actor"] or entry["actor"] == me else f"　〔{entry['actor'].split(':')[-1]}〕"
            item = QListWidgetItem(f"{entry['label']}{who}")
            item.setData(Qt.ItemDataRole.UserRole, n)
            self.list.addItem(item)
        for n, entry in enumerate(later, len(done) + 1):
            item = QListWidgetItem(f"{entry['label']}（取り消し済み）")
            item.setForeground(QColor(150, 150, 150))
            item.setData(Qt.ItemDataRole.UserRole, n)
            self.list.addItem(item)
        now = self.list.item(len(done))
        now.setText("▶ " + now.text())
        font = now.font()
        font.setBold(True)
        now.setFont(font)
        self.list.setCurrentRow(len(done))
        self.list.scrollToItem(now)

    def _go(self, item: QListWidgetItem) -> None:
        self.go_to(int(item.data(Qt.ItemDataRole.UserRole)))

    def go_to(self, target: int) -> None:
        """Undo or redo until `target` changes are done."""
        from genko.app import wording
        from genko.ops import ApplyError

        session = self.window.session
        steps = 0
        try:
            while self.done > target:
                session.undo()
                self.done -= 1
                steps += 1
            while self.done < target:
                session.redo()
                self.done += 1
                steps += 1
        except ApplyError as exc:
            self.window.flash(wording.error(str(exc)), 5000, error=True)
        if steps:
            self.window._watch()
            self.window._reload_pages()
        self.refresh()
