"""Short leases on work items so parallel agents do not take the same one (§7.4).

A claim is studio/claims/<item id>.json made with O_EXCL. It expires after
LEASE_SECONDS; the holder renews it by claiming again. Claims never block
writes (use a page lock for that); they only steer `next`.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

LEASE_SECONDS = 600


def _dir(project: Path) -> Path:
    return Path(project) / "studio" / "claims"


def holder(project: Path, item_id: str, now: float | None = None) -> str | None:
    path = _dir(project) / f"{item_id}.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if float(data.get("expires", 0)) < (now or time.time()):
        return None
    return str(data.get("actor"))


def claim(project: Path, item_id: str, actor: str, now: float | None = None) -> bool:
    """True when `actor` holds the item afterwards (new claim, renewal, or an expired one taken over)."""
    now = now or time.time()
    folder = _dir(project)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{item_id}.json"
    body = json.dumps({"actor": actor, "expires": now + LEASE_SECONDS}).encode("utf-8")
    current = holder(project, item_id, now)
    if current is not None and current != actor:
        return False
    if current == actor or path.exists():
        # renew our own claim, or replace an expired one: write a fresh file, then check we won
        tmp = folder / f"{item_id}.{os.getpid()}.{actor.replace(':', '_')}.tmp"
        tmp.write_bytes(body)
        if current is None:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                tmp.unlink()
                return holder(project, item_id, now) == actor
            with os.fdopen(fd, "wb") as handle:
                handle.write(body)
            tmp.unlink()
            return True
        os.replace(tmp, path)
        return True
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return holder(project, item_id, now) == actor
    with os.fdopen(fd, "wb") as handle:
        handle.write(body)
    return True


def release(project: Path, item_id: str, actor: str) -> None:
    if holder(project, item_id) == actor:
        try:
            (_dir(project) / f"{item_id}.json").unlink()
        except FileNotFoundError:
            pass
