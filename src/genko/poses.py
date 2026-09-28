"""A person's own poses for the 3D figure (ポーズを素材として保存): kept with the app's settings, so every book
has them. A pose is its joints and hands (the build and the size stay the figure's own)."""

from __future__ import annotations

import json


def _path():
    from genko.tokens import config_dir

    return config_dir() / "poses.json"


def user_poses() -> list[dict]:
    """[{name, joints, hands}], oldest first."""
    try:
        data = json.loads(_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return [p for p in data if isinstance(p, dict) and p.get("name")] if isinstance(data, list) else []


def save_pose(name: str, prim: dict) -> dict:
    name = name.strip()
    if not name:
        raise ValueError("a pose needs a name")
    pose = {"name": name, "joints": {k: dict(v) for k, v in (prim.get("joints") or {}).items()},
            "hands": dict(prim.get("hands") or {})}
    kept = [p for p in user_poses() if p.get("name") != name] + [pose]
    _write(kept)
    return pose


def delete_pose(name: str) -> None:
    _write([p for p in user_poses() if p.get("name") != name])


def find(name: str) -> dict | None:
    return next((p for p in user_poses() if p.get("name") == name), None)


def _write(poses: list[dict]) -> None:
    path = _path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(poses, ensure_ascii=False, indent=1), encoding="utf-8")
