from pathlib import Path

from genko.lock import ProjectLock
from genko.ops import ApplyError


def test_lock_blocks_second_acquire(tmp_path: Path):
    lock = ProjectLock(tmp_path / "p.genko")
    lock.acquire()
    try:
        other = ProjectLock(tmp_path / "p.genko")
        try:
            other.acquire()
            raise AssertionError("expected ApplyError")
        except ApplyError:
            pass
    finally:
        lock.release()
    other.acquire()
    other.release()
