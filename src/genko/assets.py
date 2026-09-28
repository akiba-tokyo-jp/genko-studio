"""Content-addressed files under <project>/assets/ab/<sha256><suffix>. Never rewritten."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any


def canonical_json(obj: Any) -> str:
    return json.dumps(obj, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


class AssetStore:
    def __init__(self, project: Path) -> None:
        self.root = Path(project) / "assets"

    @staticmethod
    def ref(data: bytes) -> str:
        return "sha256:" + hashlib.sha256(data).hexdigest()

    def relpath(self, ref: str, suffix: str) -> str:
        digest = ref.split(":", 1)[1]
        return f"assets/{digest[:2]}/{digest}{suffix}"

    def path(self, ref: str, suffix: str) -> Path:
        return self.root.parent / self.relpath(ref, suffix)

    def put_bytes(self, data: bytes, suffix: str) -> str:
        ref = self.ref(data)
        path = self.path(ref, suffix)
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
            tmp.write_bytes(data)
            os.replace(tmp, path)
        return ref

    def get_bytes(self, ref: str, suffix: str) -> bytes | None:
        path = self.path(ref, suffix)
        return path.read_bytes() if path.is_file() else None

    def all_files(self) -> list[Path]:
        return [p for p in self.root.rglob("*") if p.is_file() and not p.name.endswith(".tmp")] if self.root.is_dir() else []
