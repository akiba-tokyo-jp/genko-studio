"""studio/journal.jsonl: one line per save, with the project.json before and after (as asset refs).

Undo and redo restore those snapshots, so they work across processes. The
snapshots are small in v3 (strokes and rasters are hash references).
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path
from typing import Any

from genko.assets import AssetStore

JOURNAL = Path("studio") / "journal.jsonl"
KEEP_ENTRIES = 100


def path(project: Path) -> Path:
    return Path(project) / JOURNAL


def append(project: Path, entry: dict) -> None:
    p = path(project)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, ensure_ascii=False) + "\n")


def entries(project: Path) -> list[dict]:
    p = path(project)
    if not p.is_file():
        return []
    return [json.loads(line) for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]


def stacks(items: list[dict]) -> tuple[list[dict], list[dict]]:
    """Replay commit/undo/redo lines into (undoable, redoable) stacks."""
    undo: list[dict] = []
    redo: list[dict] = []
    for item in items:
        kind = item.get("kind", "commit")
        if kind == "commit":
            undo.append(item)
            redo.clear()
        elif kind == "undo" and undo:
            redo.append(undo.pop())
        elif kind == "redo" and redo:
            undo.append(redo.pop())
    return undo, redo


def restore(project: Path, *, actor: str, redo: bool = False, force: bool = False) -> dict:
    """Undo (or redo) the latest change. Refuses another actor's change unless force."""
    from genko.io import _write_atomic
    from genko.ops import ApplyError

    project = Path(project)
    store = AssetStore(project)
    undo_stack, redo_stack = stacks(entries(project))
    stack = redo_stack if redo else undo_stack
    if not stack:
        raise ApplyError("nothing to redo" if redo else "nothing to undo")
    target = stack[-1]
    current = (project / "project.json").read_bytes()
    expected = target["before"] if redo else target["after"]
    if AssetStore.ref(current) != expected and not force:
        raise ApplyError("project.json changed outside the journal; use --force to restore anyway")
    if not redo and target.get("actor") != actor and not force:
        raise ApplyError(f"the latest change is by {target.get('actor')}; use --force to undo it as {actor}")
    wanted = target["after"] if redo else target["before"]
    if wanted is None:
        raise ApplyError("the project did not exist before this change")
    data = store.get_bytes(wanted, ".project.json")
    if data is None:
        raise ApplyError(f"snapshot {wanted} is missing from assets/")
    _write_atomic(project / "project.json", data)
    append(project, {"kind": "redo" if redo else "undo", "rev": target["rev"], "actor": actor, "at": time.time(),
                     "before": AssetStore.ref(current), "after": wanted})
    return {"ok": True, "kind": "redo" if redo else "undo", "rev": target["rev"]}


def referenced_assets(project: Path) -> set[str]:
    """Refs kept alive by the journal's last KEEP_ENTRIES snapshots and by their contents."""
    store = AssetStore(project)
    refs: set[str] = set()
    snapshots: list[bytes] = []
    for item in entries(project)[-KEEP_ENTRIES:]:
        for key in ("before", "after"):
            if item.get(key):
                refs.add(item[key])
                data = store.get_bytes(item[key], ".project.json")
                if data:
                    snapshots.append(data)
    current = project / "project.json"
    if current.is_file():
        snapshots.append(current.read_bytes())
    for data in snapshots:
        refs |= refs_in(json.loads(data))
    return refs


_REF = re.compile(r"sha256:[0-9a-f]{64}")


def refs_in(payload: Any) -> set[str]:
    """Every asset ref anywhere in a project payload: layers, panel candidates, studio refs and orphans."""
    out: set[str] = set()
    stack = [payload]
    while stack:
        item = stack.pop()
        if isinstance(item, dict):
            stack.extend(item.values())
        elif isinstance(item, list):
            stack.extend(item)
        elif isinstance(item, str) and _REF.fullmatch(item):
            out.add(item)
    return out
