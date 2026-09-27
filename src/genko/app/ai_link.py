"""AI と作る: how to connect an AI (Claude Desktop, Claude Code, Hermes Agent, others) to Genko, which AI has worked
on the book lately, and what to say to it first. Genko does not start the AI: the AI's own settings start
`genko mcp`, and the text to paste there is made here, with this computer's paths already filled in.

The AI's work comes back into the open window by itself; what it asks a person to decide waits in the approval box.
"""

from __future__ import annotations

import json
import shutil
import sys
import time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QFontDatabase, QGuiApplication
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from genko.app import dialog_look as look
from genko.app import theme

CLIENTS = [
    ("claude-desktop", "Claude Desktop"),
    ("claude-code", "Claude Code"),
    ("hermes", "Hermes Agent"),
    ("other", "そのほかの MCP クライアント"),
]
RECENT_S = 5 * 60  # (an AI that wrote in the last minutes is "working")


def command() -> list[str]:
    """How an AI's settings start Genko's MCP server on this computer (the genko command when it is installed,
    else this Python with -m genko)."""
    found = shutil.which("genko")
    return [found] if found else [sys.executable, "-m", "genko"]


def snippet(client: str, root: Path) -> tuple[str, str]:
    """(where to paste it, what to paste) for one AI client."""
    base = command()
    args = [*base[1:], "mcp", "--root", str(root), "--agent", f"ai:{client if client != 'other' else 'agent'}"]
    if client == "claude-desktop":
        text = json.dumps({"mcpServers": {"genko": {"command": base[0], "args": args}}}, ensure_ascii=False, indent=2)
        return "Claude Desktop の「設定 → 開発者 → 設定を編集」で開くファイル（claude_desktop_config.json）に足します", text
    if client == "claude-code":
        line = " ".join(_quote(p) for p in [*base, *args[len(base) - 1:]])
        return "ターミナルで 1 回だけ実行します（Claude Code に genko を登録します）", f"claude mcp add genko -- {line}"
    if client == "hermes":
        text = ("mcp_servers:\n  genko:\n"
                f"    command: {json.dumps(base[0], ensure_ascii=False)}\n"
                f"    args: {json.dumps(args, ensure_ascii=False)}\n    timeout: 60\n")
        return "Hermes Agent の設定ファイル（~/.hermes/config.yaml）に足します", text
    return "MCP クライアントに stdio のサーバーとして、次のコマンドを登録します", " ".join(_quote(p) for p in [*base, *args[len(base) - 1:]])


def _quote(part: str) -> str:
    return f'"{part}"' if any(c in part for c in ' \t"') else part


def first_request(book: Path | None) -> str:
    """What to say to the AI first (the book's folder name filled in)."""
    name = book.name if book is not None else "新しい原稿"
    return (f"Genko の「{name}」で、4 ページの短い漫画を作ってください。まず genko://guide/skill を読んで手順に従い、"
            "企画書と脚本を作ったら私に見せてください。決めることは承認箱で聞いてください。")


def recent_ai(book: Path | None, now: float | None = None) -> list[dict]:
    """The AIs that wrote this book, newest first: {actor, minutes_ago, working}."""
    if book is None:
        return []
    from genko.studio import presence

    now = time.time() if now is None else now
    actors = (presence._read(Path(book)).get("actors") or {})
    out = [{"actor": name, "minutes_ago": max(0, round((now - float(item.get("at", 0))) / 60)),
            "working": now - float(item.get("at", 0)) < RECENT_S}
           for name, item in actors.items() if str(name).startswith("ai:")]
    return sorted(out, key=lambda item: item["minutes_ago"])


def ai_name(actor: str) -> str:
    """ai:hermes/9204 → hermes（9204）."""
    name = str(actor)[3:] if str(actor).startswith("ai:") else str(actor)
    base, _, session = name.partition("/")
    return f"{base}（{session}）" if session else base


def status_words(book: Path | None) -> tuple[str, bool]:
    """For the status line: ("AI: hermes 作業中", True) / ("AI: hermes 12 分前", False) / ("AI と作る", False)."""
    found = recent_ai(book)
    if not found:
        return "AI と作る", False
    top = found[0]
    when = "作業中" if top["working"] else _ago(top["minutes_ago"])
    more = f" ほか {len(found) - 1}" if len(found) > 1 else ""
    return f"AI: {ai_name(top['actor'])} {when}{more}", top["working"]


def _ago(minutes: int) -> str:
    if minutes < 60:
        return f"{minutes} 分前"
    if minutes < 60 * 48:
        return f"{minutes // 60} 時間前"
    return f"{minutes // (60 * 24)} 日前"


def default_root(book: Path | None) -> Path:
    """The folder the AI may open books in: this book's folder's parent, else the last book's, else home."""
    if book is not None:
        return Path(book).resolve().parent
    try:
        from genko.app.main import recent_projects

        recent = recent_projects()
        if recent:
            return Path(recent[0]).resolve().parent
    except Exception:  # noqa: BLE001 (no list of recent books)
        pass
    return Path.home()


class AiDialog(QDialog):
    """AI と作る: connect an AI, see who worked on the book, and what to ask first."""

    def __init__(self, parent: QWidget | None = None, book: Path | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("AI と作る")
        self.book = Path(book) if book else None
        mono = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)

        self.client = QComboBox()
        for key, label in CLIENTS:
            self.client.addItem(label, key)
        self.client.currentIndexChanged.connect(lambda _: self._fill())
        self.root = QLineEdit(str(default_root(self.book)))
        self.root.setToolTip("AI が原稿を開いてよいフォルダ。この中の .genko の原稿だけを扱います")
        self.root.textChanged.connect(lambda _: self._fill())
        choose = QPushButton("選ぶ…")
        choose.clicked.connect(self._choose_root)
        root_row = QHBoxLayout()
        root_row.addWidget(self.root, 1)
        root_row.addWidget(choose)
        self.where = QLabel()
        self.where.setWordWrap(True)
        theme.role(self.where, "hint")
        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setFont(mono)
        self.text.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)  # (a path broken over two lines reads wrong)
        self.text.setMinimumHeight(120)
        self.text.setAccessibleName("AI の設定に貼る文")
        self.copy = theme.iconic(QPushButton("設定をコピー"), "duplicate")
        self.copy.clicked.connect(lambda: self._copy(self.text.toPlainText(), self.copy, "設定をコピー"))
        steps = QLabel("1. 上の文を AI の設定に貼る　2. AI を立ち上げ直す　3. 下の「最初に頼むこと」を AI に送る")
        steps.setWordWrap(True)
        theme.role(steps, "hint")

        rows = look.form()
        rows.addRow(look.section("1. AI をつなぐ"))
        rows.addRow("使う AI", self.client)
        rows.addRow("原稿のフォルダ", root_row)
        look.quiet_labels(rows)
        body = QVBoxLayout()
        body.addLayout(rows)
        body.addWidget(self.where)
        body.addWidget(self.text, 1)
        copy_row = QHBoxLayout()
        copy_row.addWidget(steps, 1)
        copy_row.addWidget(self.copy)
        body.addLayout(copy_row)
        body.addWidget(look.section("2. 最初に頼むこと"))
        self.ask = QPlainTextEdit(first_request(self.book))
        self.ask.setMaximumHeight(72)
        self.ask.setAccessibleName("AI に最初に送る文")
        ask_copy = theme.iconic(QPushButton("文をコピー"), "duplicate")
        ask_copy.clicked.connect(lambda: self._copy(self.ask.toPlainText(), ask_copy, "文をコピー"))
        ask_row = QHBoxLayout()
        ask_row.addWidget(self.ask, 1)
        ask_row.addWidget(ask_copy, 0, Qt.AlignmentFlag.AlignTop)
        body.addLayout(ask_row)

        side = QVBoxLayout()
        self.people = QLabel()
        self.people.setWordWrap(True)
        self.people.setTextFormat(Qt.TextFormat.RichText)
        side.addWidget(self.people)
        how = QLabel("AI の作業は、開いている画面にすぐ出ます。人が決めること（ネーム・絵・書き出しの承認）は"
                     "右の「承認箱」に届きます。AI が変えたものは「履歴」に AI の名前付きで残ります。")
        how.setWordWrap(True)
        theme.role(how, "hint")
        side.addWidget(how)
        side.addStretch(1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.button(QDialogButtonBox.StandardButton.Close).setText("閉じる")
        buttons.rejected.connect(self.reject)
        look.frame(self, look.header("AI と作る", "Claude などの AI が、Genko の道具でネームから仕上げまで進めます。"
                                     "あなたは画面で見守り、承認箱で決め、好きなところを自分で直せます。"),
                   body, look.card(side, "この原稿の AI"), look.footer(buttons))
        self.resize(900, 600)
        self._fill()

    def _fill(self) -> None:
        where, text = snippet(self.client.currentData(), Path(self.root.text().strip() or "."))
        self.where.setText(where)
        self.text.setPlainText(text)
        found = recent_ai(self.book)
        if self.book is None:
            words = "原稿を開くと、その原稿で動いた AI がここに出ます。"
        elif not found:
            words = "この原稿では、まだ AI が作業していません。"
        else:
            rows = []
            for item in found[:6]:
                mark = "<b>● 作業中</b>" if item["working"] else _ago(item["minutes_ago"])
                rows.append(f"{ai_name(item['actor'])} — {mark}")
            words = "<br>".join(rows)
        self.people.setText(words)

    def _choose_root(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "AI が原稿を開いてよいフォルダ", self.root.text())
        if folder:
            self.root.setText(folder)

    def _copy(self, text: str, button: QPushButton, label: str) -> None:
        QGuiApplication.clipboard().setText(text)
        button.setText("コピーしました")
        QTimer.singleShot(1600, lambda: button.setText(label))
