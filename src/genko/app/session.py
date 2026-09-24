"""The GUI's editing session (Qt-free, so it can be tested without a display).

The episode in memory is the truth while a person works: every change is an
op applied to it at once (`apply`), as `human:<name>`. Changes reach the disk
in batches (`commit`): after a pause, on a page switch, before an approval and
on close. If someone else (an agent) committed in between, the session reloads
the project and replays its own pending ops on top (rebase); ops that no longer
apply are reported as conflicts instead of overwriting the other change.
"""

from __future__ import annotations

import getpass
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

from genko.io import load_episode, save_episode
from genko.lock import ProjectLock
from genko.models import Episode
from genko.ops import ApplyError, apply_ops


def default_actor() -> str:
    """human:<name> from $GENKO_USER, else the login name."""
    name = os.environ.get("GENKO_USER") or ""
    if not name:
        try:
            name = getpass.getuser()
        except Exception:
            name = "user"
    name = re.sub(r"[^\w.-]", "_", name) or "user"
    return name if name.startswith("human:") else f"human:{name}"


def disk_revision(path: Path) -> int | None:
    try:
        with (Path(path) / "project.json").open(encoding="utf-8") as handle:
            return int(json.load(handle).get("revision", 0))
    except (OSError, ValueError):
        return None


@dataclass
class CommitResult:
    ok: bool
    revision: int | None = None
    rebased: bool = False
    conflicts: list[dict] = field(default_factory=list)
    error: str | None = None


class Session:
    def __init__(self, episode: Episode, path: Path | None = None, actor: str | None = None) -> None:
        self.episode = episode
        self.path = Path(path) if path else None
        self.actor = actor or default_actor()
        self.base_revision = episode.revision
        self.pending: list[list[dict]] = []  # batches applied in memory, not yet on disk

    # --- opening -------------------------------------------------------------------

    @classmethod
    def open(cls, path: Path, actor: str | None = None) -> "Session":
        return cls(load_episode(Path(path)), path, actor)

    @property
    def dirty(self) -> bool:
        return bool(self.pending)

    def outside_change(self) -> bool:
        """Did anyone else commit since we last read or wrote?"""
        if self.path is None:
            return False
        found = disk_revision(self.path)
        return found is not None and found != self.base_revision

    # --- editing -------------------------------------------------------------------

    def apply(self, ops: list[dict]) -> dict:
        """Apply at once in memory (raises ApplyError). Undo pops the last pending batch."""
        if len(ops) == 1 and ops[0].get("op") == "undo":
            return self.undo()
        result = apply_ops(self.episode, ops, agent=self.actor)
        self.pending.append([dict(op) for op in ops])
        return result

    def undo(self) -> dict:
        if self.pending:
            result = apply_ops(self.episode, [{"op": "undo"}], agent=self.actor)
            self.pending.pop()
            return result
        if self.path is None:
            raise ApplyError("nothing to undo")
        # the last change is on disk: undo it through the journal (only our own, unless forced elsewhere)
        from genko.journal import restore

        with ProjectLock(self.path, agent=self.actor):
            result = restore(self.path, actor=self.actor)
        self.reload()
        return result

    # --- disk ---------------------------------------------------------------------------

    def reload(self) -> None:
        if self.path is None:
            return
        self.episode = load_episode(self.path)
        self.base_revision = self.episode.revision
        self.pending = []

    def commit(self) -> CommitResult:
        """Write pending changes. Rebases first when the project changed on disk."""
        if self.path is None:
            return CommitResult(False, error="no project path")
        if not self.pending and not self.outside_change():
            return CommitResult(True, self.base_revision)
        conflicts: list[dict] = []
        rebased = False
        with ProjectLock(self.path, agent=self.actor):
            if self.outside_change() or not (self.path / "project.json").exists():
                rebased = (self.path / "project.json").exists()
                if rebased:
                    fresh = load_episode(self.path)
                    for batch in self.pending:
                        try:
                            apply_ops(fresh, batch, agent=self.actor)
                        except ApplyError as exc:
                            conflicts.append({"ops": batch, "error": str(exc)})
                    self.episode = fresh
            something_to_write = bool(self.pending) and len(conflicts) < len(self.pending)
            if something_to_write or not rebased:
                save_episode(self.episode, self.path, actor=self.actor)
            self.base_revision = self.episode.revision
            self.pending = []
        return CommitResult(True, self.base_revision, rebased, conflicts)

    def sync(self) -> CommitResult:
        """Called when the project changed on disk: reload, keeping our pending work on top."""
        if not self.outside_change():
            return CommitResult(True, self.base_revision)
        if not self.pending:
            self.reload()
            return CommitResult(True, self.base_revision, rebased=True)
        return self.commit()

    def save_as(self, path: Path) -> CommitResult:
        self.path = Path(path)
        with ProjectLock(self.path, agent=self.actor):
            save_episode(self.episode, self.path, actor=self.actor)
        self.base_revision = self.episode.revision
        self.pending = []
        return CommitResult(True, self.base_revision)
