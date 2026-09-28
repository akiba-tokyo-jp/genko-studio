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
    allow_front()
    socket.write(link.encode("utf-8") + b"\n")
    socket.flush()
    socket.waitForBytesWritten(wait_ms)
    # the open Genko answers once it has the link: going before that can lose it (Windows' pipes)
    socket.waitForReadyRead(max(wait_ms, 2000))
    socket.disconnectFromServer()
    return True


def allow_front() -> None:
    """Windows lets only the program the person just used bring a window to the front: this start (opened by the
    browser's button) passes that right on, so the open Genko can come forward with its question."""
    if sys.platform == "win32":
        try:
            import ctypes

            ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY
        except Exception:  # noqa: BLE001 (no right to pass on: the question still waits in Genko)
            pass


def come_forward(window) -> None:
    """The window in front of the others (restored when minimized), before it asks something."""
    if window.isMinimized():
        window.showNormal()
    window.raise_()
    window.activateWindow()
    if sys.platform == "win32":
        try:
            import ctypes

            ctypes.windll.user32.SetForegroundWindow(int(window.winId()))
        except Exception:  # noqa: BLE001
            pass


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
        # (Windows' pipes do not always say a later start has come: look now and then too)
        from PySide6.QtCore import QTimer

        self._poll = QTimer(self)
        self._poll.setInterval(250)
        self._poll.timeout.connect(self._look)
        self._poll.start()

    def _look(self) -> None:
        if self.server.isListening():
            self.server.waitForNewConnection(0)  # (a waiting start is taken in through newConnection)
        if self.server.hasPendingConnections():
            self._take()

    def _take(self) -> None:
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            socket.setProperty("genko_buffer", b"")
            socket.readyRead.connect(lambda s=socket: self._read(s))
            socket.disconnected.connect(lambda s=socket: self._closed(s))
            # (on Windows the later start may have written and gone before this runs: read what is there now)
            if socket.bytesAvailable():
                self._read(socket)

    def _read(self, socket: QLocalSocket, final: bool = False) -> None:
        data = bytes(socket.property("genko_buffer") or b"") + bytes(socket.readAll())
        *lines, rest = data.split(b"\n")
        if final:
            lines, rest = [*lines, rest], b""
        socket.setProperty("genko_buffer", rest)
        for raw in lines:
            text = raw.decode("utf-8", "replace").strip()
            if is_link(text):
                self.received.emit(text)
                if socket.state() == QLocalSocket.LocalSocketState.ConnectedState:
                    socket.write(b"ok\n")  # (the later start may go now)
                    socket.flush()

    def _closed(self, socket: QLocalSocket) -> None:
        self._read(socket, final=True)
        socket.deleteLater()


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
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\genko\shell\open") as key:
                winreg.SetValueEx(key, "FriendlyAppName", 0, winreg.REG_SZ, "Genko Studio")  # (the browser's question)
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\genko\shell\open\command") as key:
                winreg.SetValueEx(key, "", 0, winreg.REG_SZ, line)
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\genko\Application") as key:
                winreg.SetValueEx(key, "ApplicationName", 0, winreg.REG_SZ, "Genko Studio")
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
