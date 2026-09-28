"""HTTP tokens: tokens.json in the user config dir maps token → actor.

The config dir is $GENKO_CONFIG_DIR, else %APPDATA%\\genko on Windows, else
$XDG_CONFIG_HOME/genko or ~/.config/genko. Tokens never go into a project.
"""

from __future__ import annotations

import json
import os
import secrets
from pathlib import Path


def config_dir() -> Path:
    if os.environ.get("GENKO_CONFIG_DIR"):
        return Path(os.environ["GENKO_CONFIG_DIR"])
    if os.name == "nt" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "genko"
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "genko"


def _path() -> Path:
    return config_dir() / "tokens.json"


def load() -> dict[str, str]:
    path = _path()
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {token: info["actor"] for token, info in data.items()}


def _save(data: dict) -> None:
    path = _path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    os.replace(tmp, path)


def add(actor: str) -> str:
    if not (actor.startswith("human:") or actor.startswith("ai:")):
        raise ValueError("actor must be human:<name> or ai:<name>")
    path = _path()
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    token = secrets.token_urlsafe(32)
    data[token] = {"actor": actor}
    _save(data)
    return token


def listing() -> list[dict]:
    path = _path()
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    return [{"id": token[:8], "actor": info["actor"]} for token, info in data.items()]


def revoke(token_id: str) -> int:
    path = _path()
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    keep = {t: i for t, i in data.items() if not t.startswith(token_id)}
    removed = len(data) - len(keep)
    if removed:
        _save(keep)
    return removed
