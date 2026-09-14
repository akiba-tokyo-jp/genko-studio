from __future__ import annotations

import json
import os
import time
from pathlib import Path

from genko.ops import ApplyError


class ProjectLock:
    def __init__(self, project: Path, agent: str = "genko") -> None:
        self.project = Path(project)
        self.path = self.project / "project.lock"
        self.agent = agent

    def acquire(self) -> None:
        self.project.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
            age = time.time() - float(data.get("acquired_at") or 0)
            if age < 15 * 60:
                raise ApplyError(f"project locked: {self.path}")
            self.path.unlink()
        self.path.write_text(
            json.dumps({"agent": self.agent, "pid": os.getpid(), "acquired_at": time.time()}),
            encoding="utf-8",
        )

    def release(self) -> None:
        if self.path.exists():
            self.path.unlink()

    def __enter__(self) -> ProjectLock:
        self.acquire()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.release()
