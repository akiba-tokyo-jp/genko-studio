"""genko:// links from a web page (the style catalog's "この絵柄を使う"): the system starts `genko app <link>`; when
Genko is open already, that new start hands the link to the open one over a local socket and ends, so the link lands
in the window the person is working in.

`register()` tells the system that genko:// links open Genko (Windows: the user's registry; Linux: a desktop entry).
A Mac learns it from the app's own Info.plist, so there is nothing to do from here.
"""

from __future__ import annotations

import getpass
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PySide6.QtCore import QObject, Signal
from PySide6.QtNetwork import QLocalServer, QLocalSocket


def is_link(text: str | None) -> bool:
    return str(text or "").lower().startswith("genko:")


def _name() -> str:
    try:
        user = getpass.getuser()
    except Exception:  # noqa: BLE001 (no user name: one shared name)
        user = "user"
    return f"genko-studio-links-{''.join(c for c in user if c.isalnum()) or 'user'}"


def send(link: str, wait_ms: int = 800) -> bool:
    """Hand the link to a Genko that is open already (False: none is)."""
    socket = QLocalSocket()
    socket.connectToServer(_name())
    if not socket.waitForConnected(wait_ms):
        return False
    socket.write(link.encode("utf-8") + b"\n")
    socket.flush()
    socket.waitForBytesWritten(wait_ms)
    socket.disconnectFromServer()
    return True


class Listener(QObject):
    """Links handed over by later starts."""

    received = Signal(str)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.server = QLocalServer(self)
        if not self.server.listen(_name()):
            QLocalServer.removeServer(_name())  # (a name left by a Genko that did not close cleanly)
            self.server.listen(_name())
        self.server.newConnection.connect(self._take)

    def _take(self) -> None:
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            socket.readyRead.connect(lambda s=socket: self._read(s))
            socket.disconnected.connect(socket.deleteLater)

    def _read(self, socket: QLocalSocket) -> None:
        for raw in bytes(socket.readAll()).decode("utf-8", "replace").splitlines():
            if is_link(raw.strip()):
                self.received.emit(raw.strip())


def command() -> list[str]:
    """How the system should start Genko for a link (without a console window on Windows)."""
    exe = Path(sys.executable)
    quiet = exe.with_name("pythonw.exe")
    if sys.platform == "win32" and quiet.is_file():
        exe = quiet
    return [str(exe), "-m", "genko", "app"]


def register() -> tuple[bool, str]:
    """Make genko:// links open Genko for this user. (done, what happened, in words for the person)"""
    if sys.platform == "win32":
        import winreg

        line = " ".join(f'"{part}"' for part in command()) + ' "%1"'
        try:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\genko") as key:
                winreg.SetValueEx(key, "", 0, winreg.REG_SZ, "URL:Genko Studio")
                winreg.SetValueEx(key, "URL Protocol", 0, winreg.REG_SZ, "")
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\genko\shell\open\command") as key:
                winreg.SetValueEx(key, "", 0, winreg.REG_SZ, line)
        except OSError as exc:
            return False, f"登録できませんでした（{exc}）"
        return True, "ブラウザの「この絵柄を使う」で Genko が開くようになりました"
    if sys.platform == "darwin":
        return False, "Mac では Genko のアプリ（.app）が自分で登録します。今は Genko の「絵柄を選ぶ」から選んでください"
    folder = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "applications"
    folder.mkdir(parents=True, exist_ok=True)
    entry = folder / "genko-links.desktop"
    entry.write_text("[Desktop Entry]\nType=Application\nName=Genko Studio\nNoDisplay=true\n"
                     f"Exec={' '.join(command())} %u\nMimeType=x-scheme-handler/genko;\n", encoding="utf-8")
    tool = shutil.which("xdg-mime")
    if tool is None:
        return False, f"{entry} を作りました。xdg-mime が無いので、ブラウザへの登録は手で行ってください"
    done = subprocess.run([tool, "default", entry.name, "x-scheme-handler/genko"], capture_output=True, check=False).returncode == 0
    return done, ("ブラウザの「この絵柄を使う」で Genko が開くようになりました" if done else "登録できませんでした（xdg-mime が失敗）")
