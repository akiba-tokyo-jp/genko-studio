"""project.lock: one writer at a time.

The lock is an OS file lock (flock on POSIX, msvcrt on Windows), so a crashed
process never leaves a stale lock and no one can delete someone else's lock.
The file content ({token, agent, pid, acquired_at}) is only for display. A file
written by an older Genko (no token, acquired under 15 minutes ago) is still
respected, and on release the content is marked released so older builds see
the project as free.
"""

from __future__ import annotations

import json
import os
import socket
import time
import uuid
from pathlib import Path

from genko.ops import ApplyError

LEGACY_STALE_SECONDS = 15 * 60

if os.name == "nt":  # pragma: no cover - exercised on Windows CI
    import msvcrt

    def _try_lock(fd: int) -> bool:
        os.lseek(fd, 0, os.SEEK_SET)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False

    def _unlock(fd: int) -> None:
        os.lseek(fd, 0, os.SEEK_SET)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)

else:
    import fcntl

    def _try_lock(fd: int) -> bool:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def _unlock(fd: int) -> None:
        fcntl.flock(fd, fcntl.LOCK_UN)


class ProjectLock:
    def __init__(self, project: Path, agent: str = "genko") -> None:
        self.project = Path(project)
        self.path = self.project / "project.lock"
        self.agent = agent
        self.token = uuid.uuid4().hex
        self._fd: int | None = None

    def _read(self, fd: int) -> dict:
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            raw = os.read(fd, 65536)
            return json.loads(raw.decode("utf-8")) if raw.strip() else {}
        except (OSError, ValueError):
            return {}

    def _write(self, fd: int, data: dict) -> None:
        body = json.dumps(data).encode("utf-8")
        os.lseek(fd, 0, os.SEEK_SET)
        os.ftruncate(fd, 0)
        os.write(fd, body)

    def acquire(self) -> None:
        self.project.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
        if not _try_lock(fd):
            holder = self._read(fd).get("agent", "another process")
            os.close(fd)
            raise ApplyError(f"project locked: {self.path} (by {holder})")
        current = self._read(fd)
        legacy_holder = (
            current
            and "token" not in current
            and not current.get("released")
            and time.time() - float(current.get("acquired_at") or 0) < LEGACY_STALE_SECONDS
        )
        if legacy_holder:
            _unlock(fd)
            os.close(fd)
            raise ApplyError(f"project locked: {self.path} (by {current.get('agent', 'an older Genko')})")
        self._write(fd, {"token": self.token, "agent": self.agent, "pid": os.getpid(),
                         "host": socket.gethostname(), "acquired_at": time.time()})
        self._fd = fd

    def release(self) -> None:
        fd, self._fd = self._fd, None
        if fd is None:
            return
        try:
            if self._read(fd).get("token") == self.token:
                self._write(fd, {"released": True, "agent": self.agent, "released_at": time.time()})
        finally:
            _unlock(fd)
            os.close(fd)

    def __enter__(self) -> ProjectLock:
        self.acquire()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.release()
