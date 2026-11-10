"""Byte-exact saved-book oracle for Hermes safety regressions (all paths, not CLI brief snapshots)."""
import hashlib
import json
from pathlib import Path


def book_fingerprint(book: Path) -> dict[str, str | None]:
    out = {}
    for p in sorted(book.rglob('*')):
        relative = p.relative_to(book).as_posix()
        if p.is_dir():
            out[relative] = None
            continue
        data = p.read_bytes()
        if relative == 'project.lock':
            # Even a refused op releases its OS lock and rewrites only this operational timestamp.
            # Retain the lock's path and every other field; require a released lock rather than hiding a live lease.
            lock = json.loads(data)
            assert lock.get('released') is True, 'saved book lock not released'
            lock.pop('released_at', None)
            data = json.dumps(lock, sort_keys=True, ensure_ascii=False).encode('utf-8')
        out[relative] = hashlib.sha256(data).hexdigest()
    return out
