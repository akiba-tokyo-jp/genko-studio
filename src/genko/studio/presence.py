"""Who is writing a book from an agent (M2): each conversation writes under its own name (ai:hermes/9204), and the
first write while another conversation has written in the last minutes is held back once with a warning, so the
agent can stop (and copy the book) or go on knowingly.

studio/presence.json: {"actors": {actor: {"at": epoch}}, "acks": {actor: {"others": [actor…], "at": epoch}}}
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path

ACTIVE_S = 15 * 60  # (another conversation that wrote this recently is "using" the book)
ACK_S = 30 * 60  # (a warning once seen holds for this long)
SESSION = re.compile(r"[A-Za-z0-9_.:@-]{1,40}")


def actor_for(base: str, session: str | None) -> str:
    """The recorded name of this conversation: the server's agent name, plus the conversation's own name."""
    if not session:
        return base
    session = str(session).strip()
    if not SESSION.fullmatch(session):
        raise ValueError("session は 1〜40 文字の英数字と _ . : @ - だけ（例 9204、telegram-9204）")
    return f"{base}/{session}"


def _file(project: Path) -> Path:
    return Path(project) / "studio" / "presence.json"


def _read(project: Path) -> dict:
    try:
        data = json.loads(_file(project).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"actors": {}, "acks": {}}
    return data if isinstance(data, dict) else {"actors": {}, "acks": {}}


def _write(project: Path, data: dict) -> None:
    target = _file(project)
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    tmp.replace(target)


def others(project: Path, actor: str, now: float | None = None) -> list[dict]:
    """The other agents (conversations) that wrote this book in the last ACTIVE_S seconds."""
    now = time.time() if now is None else now
    actors = _read(project).get("actors") or {}
    return [{"actor": name, "minutes_ago": round((now - float(item.get("at", 0))) / 60, 1)}
            for name, item in sorted(actors.items())
            if name != actor and now - float(item.get("at", 0)) < ACTIVE_S]


def check(project: Path, actor: str, now: float | None = None) -> list[dict]:
    """Before a write: the others this actor has not yet been told about ([] = go on). Telling counts once:
    the same write again goes through."""
    now = time.time() if now is None else now
    found = others(project, actor, now)
    if not found:
        return []
    data = _read(project)
    ack = (data.get("acks") or {}).get(actor) or {}
    known = set(ack.get("others") or []) if now - float(ack.get("at", 0)) < ACK_S else set()
    new = [item for item in found if item["actor"] not in known]
    if new:
        data.setdefault("acks", {})[actor] = {"others": sorted(known | {item["actor"] for item in found}), "at": now}
        _write(project, data)
    return new


def touch(project: Path, actor: str, now: float | None = None) -> None:
    """After a write: this actor is using the book."""
    now = time.time() if now is None else now
    data = _read(project)
    data.setdefault("actors", {})[actor] = {"at": now}
    data["actors"] = {name: item for name, item in data["actors"].items() if now - float(item.get("at", 0)) < 24 * 3600}
    _write(project, data)
