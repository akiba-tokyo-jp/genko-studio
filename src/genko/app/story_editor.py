"""ストーリーエディター: every line of the book in one table (page, speaker, text, balloon), edited
together, and a script poured in (流し込み) onto the pages' panels in reading order.

Script lines:
  # 3 / ＃3 / 3ページ / P3        the lines that follow go on page 3 ("---" alone: the next page)
  話者「台詞」 / 話者：台詞          a speaker and their line
  （心の声）                         a thought (thought balloon)
  ナレ：… / N: …                     narration (a box)
  anything else                      a line without a speaker
"""

from __future__ import annotations

import copy
import re

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from genko.app.lettering import KINDS, parse_marks, place_new, refit, with_marks

_PAGE = re.compile(r"^\s*(?:[#＃]+\s*(\d+)|(\d+)\s*(?:ページ|頁|P|p)|[PpＰ]\s*(\d+)|[-ー―─]{3,}\s*(\d*))\s*$")
_QUOTE = re.compile(r"^\s*([^「『:：\s][^「『:：]{0,11}?)\s*[「『](.+)[」』]\s*$")
_COLON = re.compile(r"^\s*([^「『:：\s][^「『:：]{0,11}?)\s*[:：]\s*(.+)$")
_NARRATION = ("ナレ", "ナレーション", "N", "n", "Ｎ")


def parse_script(text: str, start_page: int = 1) -> list[dict]:
    """[{page, speaker, text, balloon}] from a script."""
    out = []
    page = start_page
    seen_marker = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        marker = _PAGE.match(line)
        if marker:
            number = next((g for g in marker.groups() if g), None)
            if number:
                page = int(number)
            elif seen_marker or out:
                page += 1
            seen_marker = True
            continue
        speaker, body, balloon = "", line, "speech"
        quote, colon = _QUOTE.match(line), _COLON.match(line)
        if line[0] in "（(" and line[-1] in "）)":
            body, balloon = line[1:-1].strip(), "thought"
        elif quote:
            speaker, body = quote.group(1).strip(), quote.group(2).strip()
        elif colon:
            speaker, body = colon.group(1).strip(), colon.group(2).strip()
            if speaker in _NARRATION:
                speaker, balloon = "", "narration"
        elif line[0] in "「『" and line[-1] in "」』":
            body = line[1:-1].strip()
        if body:
            out.append({"page": page, "speaker": speaker, "text": body, "balloon": balloon})
    return out


class PourDialog(QDialog):
    def __init__(self, parent, first_page: int) -> None:
        super().__init__(parent)
        self.setWindowTitle("台本を流し込む")
        self.text = QPlainTextEdit()
        self.text.setPlaceholderText("# 1\n太郎「おはよう」\n花子：遅いよ\n（また寝坊した…）\nナレ：翌朝\n# 2\n…")
        self.start = QSpinBox()
        self.start.setRange(1, 999)
        self.start.setValue(first_page)
        self.replace = QCheckBox("流し込むページの今の台詞を消す")
        hint = QLabel("「# 3」「3ページ」でページを変えます（「---」だけなら次のページ）。話者「台詞」・話者：台詞・（心の声）・ナレ：… が使えます。"
                      "\nページの中では、コマの読み順に台詞を割り振ります。")
        hint.setWordWrap(True)
        row = QHBoxLayout()
        row.addWidget(QLabel("ページの指定が無い台詞は"))
        row.addWidget(self.start)
        row.addWidget(QLabel("ページから"))
        row.addStretch(1)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("表に入れる")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addWidget(hint)
        layout.addWidget(self.text, 1)
        layout.addLayout(row)
        layout.addWidget(self.replace)
        layout.addWidget(buttons)
        self.resize(560, 480)

    def rows(self) -> list[dict]:
        return parse_script(self.text.toPlainText(), self.start.value())


COLUMNS = ["ページ", "話者", "台詞（｜漢字《かんじ》でルビ）", "フキダシ"]


class StoryEditor(QDialog):
    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.setWindowTitle("ストーリーエディター（全ページの台詞）")
        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.table.setWordWrap(True)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        buttons = QHBoxLayout()
        for title, slot in (("行を追加", self.add_row), ("行を削除", self.delete_rows), ("↑", lambda: self.move(-1)), ("↓", lambda: self.move(1)),
                            ("台本を流し込む…", self.pour)):
            button = QPushButton(title)
            button.clicked.connect(slot)
            buttons.addWidget(button)
        buttons.addStretch(1)
        self.status = QLabel()
        box = QDialogButtonBox(QDialogButtonBox.StandardButton.Apply | QDialogButtonBox.StandardButton.Close)
        box.button(QDialogButtonBox.StandardButton.Apply).setText("原稿に反映")
        box.button(QDialogButtonBox.StandardButton.Apply).clicked.connect(self.apply)
        box.button(QDialogButtonBox.StandardButton.Close).setText("閉じる")
        box.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("台詞を書き換えて「原稿に反映」。新しい行はそのページのコマに読み順で入ります。行の順番がそのページの読み順です。"))
        layout.addWidget(self.table, 1)
        layout.addLayout(buttons)
        layout.addWidget(self.status)
        layout.addWidget(box)
        self.resize(900, 620)
        self.deleted: set[str] = set()
        self.load()

    # --- rows ------------------------------------------------------------------------------------------

    def load(self) -> None:
        self.table.setRowCount(0)
        self.deleted = set()
        episode = self.window.episode
        for page in episode.pages:
            for line in episode.story_for_page(page.index):
                self._append({"id": line.id, "page": page.index, "speaker": line.speaker or "",
                              "text": with_marks(line),
                              "balloon": line.balloon or "speech"})
        self._count()

    def _append(self, row: dict, at: int | None = None) -> None:
        r = self.table.rowCount() if at is None else at
        self.table.insertRow(r)
        page = QTableWidgetItem(str(row["page"]))
        page.setData(Qt.ItemDataRole.UserRole, row.get("id"))
        if row.get("id"):
            page.setFlags(page.flags() & ~Qt.ItemFlag.ItemIsEditable)
            page.setToolTip("置いてある台詞のページは変えられません（新しい行で書き直します）")
        self.table.setItem(r, 0, page)
        self.table.setItem(r, 1, QTableWidgetItem(row.get("speaker", "")))
        self.table.setItem(r, 2, QTableWidgetItem(row.get("text", "")))
        combo = QComboBox()
        for key, label in KINDS:
            combo.addItem(label, key)
        combo.setCurrentIndex(max(0, combo.findData(row.get("balloon") or "speech")))
        self.table.setCellWidget(r, 3, combo)

    def row(self, r: int) -> dict:
        page = self.table.item(r, 0)
        try:
            number = int(page.text())
        except (TypeError, ValueError):
            number = 0
        return {"id": page.data(Qt.ItemDataRole.UserRole), "page": number, "speaker": (self.table.item(r, 1).text() or "").strip(),
                "text": (self.table.item(r, 2).text() or "").strip(), "balloon": self.table.cellWidget(r, 3).currentData()}

    def rows(self) -> list[dict]:
        return [self.row(r) for r in range(self.table.rowCount())]

    def _count(self) -> None:
        rows = self.rows()
        self.status.setText(f"台詞 {len(rows)} 行（新しい行 {sum(1 for r in rows if not r['id'])}、消す行 {len(self.deleted)}）")

    def add_row(self) -> None:
        current = self.table.currentRow()
        page = self.row(current)["page"] if current >= 0 else (self.window.current_page().index if self.window.current_page() else 1)
        at = current + 1 if current >= 0 else self.table.rowCount()
        self._append({"id": None, "page": page, "speaker": "", "text": "", "balloon": "speech"}, at)
        self.table.setCurrentCell(at, 2)
        self._count()

    def delete_rows(self) -> None:
        for r in sorted({i.row() for i in self.table.selectedIndexes()} or {self.table.currentRow()}, reverse=True):
            if r < 0:
                continue
            line_id = self.table.item(r, 0).data(Qt.ItemDataRole.UserRole)
            if line_id:
                self.deleted.add(line_id)
            self.table.removeRow(r)
        self._count()

    def move(self, delta: int) -> None:
        r = self.table.currentRow()
        target = r + delta
        if r < 0 or not 0 <= target < self.table.rowCount():
            return
        a, b = self.row(r), self.row(target)
        if a["page"] != b["page"]:
            return  # the order is within a page
        self.table.removeRow(r)
        self._append(a, target)
        self.table.setCurrentCell(target, 2)

    def pour(self) -> None:
        page = self.window.current_page()
        dialog = PourDialog(self, page.index if page else 1)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        self.pour_rows(dialog.rows(), dialog.replace.isChecked())

    def pour_rows(self, rows: list[dict], replace: bool = False) -> None:
        if replace:
            pages = {r["page"] for r in rows}
            for r in reversed(range(self.table.rowCount())):
                if self.row(r)["page"] in pages:
                    line_id = self.table.item(r, 0).data(Qt.ItemDataRole.UserRole)
                    if line_id:
                        self.deleted.add(line_id)
                    self.table.removeRow(r)
        for new in rows:
            # after the last row of its page (or where the pages come in order)
            at = self.table.rowCount()
            for r in range(self.table.rowCount()):
                if self.row(r)["page"] > new["page"]:
                    at = r
                    break
            self._append({"id": None, **new}, at)
        self._count()

    # --- into the book ------------------------------------------------------------------------------------

    def plan(self) -> list[dict]:
        """The ops that make the book match the table."""
        from genko.models import new_id
        from genko.ops import ApplyError, apply_ops

        episode = self.window.episode
        rows = [r for r in self.rows() if r["text"]]
        ops: list[dict] = []
        want_pages = max([r["page"] for r in rows] + [len(episode.pages)])
        if want_pages > len(episode.pages):
            ops.append({"op": "add_page", "count": want_pages - len(episode.pages)})
        ops += [{"op": "delete_line", "id": line_id} for line_id in sorted(self.deleted)]
        by_id = {line.id: line for line in episode.story}
        for r in rows:
            line = by_id.get(r["id"]) if r["id"] else None
            if line is None:
                continue
            text, runs, marks = parse_marks(r["text"])
            if (text, runs or [], marks, r["speaker"], r["balloon"]) == (
                    line.text, [list(x) for x in line.ruby_runs], list(line.emphasis_runs), line.speaker or "", line.balloon or "speech"):
                continue
            ops.append({"op": "edit_line", "id": line.id, "text": text, "speaker": r["speaker"], "balloon": r["balloon"], "ruby_runs": runs,
                        "emphasis_runs": marks})
            if text != line.text or r["balloon"] != (line.balloon or "speech"):
                page = next((p for p in episode.pages if p.index == line.page_index), None)
                frame = None
                if page is not None and line.frame_id:
                    try:
                        frame = page._find(line.frame_id)
                    except (KeyError, IndexError):
                        frame = None
                ops.append({"op": "move_line", "id": line.id, **refit(line, frame, text, r["balloon"], line.wrap == "vertical")})
        # new rows: placed on a working copy, panel by panel in reading order
        work = copy.deepcopy(episode)
        work.strict_gates = False
        try:
            apply_ops(work, ops)
        except ApplyError as exc:
            raise ValueError(str(exc)) from exc
        new_by_page: dict[int, list[dict]] = {}
        for r in rows:
            if not r["id"]:
                r["id"] = new_id()
                new_by_page.setdefault(r["page"], []).append(r)
        for page_no, fresh in new_by_page.items():
            page = next(p for p in work.pages if p.index == page_no)
            frames = page.leaf_frames()
            for i, r in enumerate(fresh):
                frame = frames[i * len(frames) // len(fresh)] if frames else None
                text, runs, marks = parse_marks(r["text"])
                op = {"op": "add_line", "page": page_no, "id": r["id"], "text": text, "speaker": r["speaker"], "balloon": r["balloon"]}
                if runs:
                    op["ruby_runs"] = runs
                if marks:
                    op["emphasis_runs"] = marks
                if frame is not None:
                    op.update(frame_id=frame.id, **place_new(work, page, frame, text, r["balloon"], True))
                apply_ops(work, [op])
                ops.append(op)
        # the table's order is each page's reading order
        for page in work.pages:
            order = [r["id"] for r in rows if r["page"] == page.index]
            current = [line.id for line in work.story_for_page(page.index)]
            if order and sorted(order) == sorted(current) and order != current:
                ops.append({"op": "reorder_lines", "page": page.index, "order": order})
        return ops

    def apply(self) -> None:
        empty = [r for r in self.rows() if not r["text"]]
        if empty:
            self.status.setText(f"空の行が {len(empty)} 行あります（反映しません）")
        try:
            ops = self.plan()
        except ValueError as exc:
            QMessageBox.warning(self, "Genko", f"反映できませんでした（{exc}）")
            return
        if not ops:
            self.status.setText("変わったところはありません")
            return
        if self.window.apply_ops(ops):
            self.window._reload_pages()
            self.load()
            self.status.setText(f"原稿に反映しました（{len(ops)} 件の変更。元に戻す で一度に戻せます）")
