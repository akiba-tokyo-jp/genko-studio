"""入稿前の点検: every problem in the book with its page; clicking one shows it on the page."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QLabel, QListWidget, QListWidgetItem, QPushButton, QVBoxLayout, QWidget


class CheckPanel(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.summary = QLabel("「点検する」で、入稿の前に直すところを探します。")
        self.summary.setWordWrap(True)
        run = QPushButton("点検する")
        run.clicked.connect(self.run)
        self.list = QListWidget()
        self.list.setWordWrap(True)
        self.list.itemClicked.connect(self._show)
        self.list.itemActivated.connect(self._show)
        top = QHBoxLayout()
        top.addWidget(run)
        top.addStretch(1)
        layout = QVBoxLayout(self)
        layout.addLayout(top)
        layout.addWidget(self.summary)
        layout.addWidget(self.list, 1)
        self.report: dict | None = None
        self._stale = False

    def refresh(self) -> None:
        """After edits: the list may be out of date (checking is on demand, it renders every line)."""
        if self.report is not None and not self._stale:
            self._stale = True
            self.summary.setText(self.summary.text().split("\n")[0] + "\n原稿が変わりました。「点検する」で最新にします。")

    def run(self) -> dict:
        from genko import checks

        self.report = checks.book(self.window.episode, self.window.session.path)
        self._stale = False
        self.list.clear()
        issues = sorted(self.report["issues"], key=lambda i: (i["level"] != "error", i["page"] or 0))
        for issue in issues:
            mark = "⛔" if issue["level"] == "error" else "⚠"
            where = f"{issue['page']} ページ ・ " if issue.get("page") else ""
            item = QListWidgetItem(f"{mark} {where}{issue['message']}")
            item.setData(Qt.ItemDataRole.UserRole, issue)
            item.setToolTip("クリックでそのページのその場所を表示")
            self.list.addItem(item)
        if not issues:
            self.summary.setText("直すところは見つかりませんでした。書き出せます。")
        else:
            self.summary.setText(f"止まる問題 {self.report['errors']} 件、確かめた方がよいこと {self.report['warnings']} 件。")
        return self.report

    def _show(self, item: QListWidgetItem) -> None:
        issue = item.data(Qt.ItemDataRole.UserRole)
        if not issue:
            return
        self.window.show_issue(issue)
