"""Image tools the agent uses, as the user describes them (tools.json in the config dir).

Genko never calls these tools. It only needs their output sizes and what they
accept (reference images, edits, masks) to write better generation requests.
The file belongs to the user's environment, not to a project.
"""

from __future__ import annotations

import json
from pathlib import Path

from genko.tokens import config_dir

GENERIC_SIZES = [[1024, 1024], [1536, 1024], [1024, 1536]]
GENERIC = {"label": "未登録のツール", "sizes_px": GENERIC_SIZES,
           "supports": {"references": True, "edit": True, "mask": True, "seed": False}}
EXAMPLE = {
    "openai:gpt-image-1": {
        "label": "ChatGPT / OpenAI の画像生成",
        "sizes_px": GENERIC_SIZES,
        "supports": {"references": True, "edit": True, "mask": True, "seed": False},
        "notes": "サイズと機能は版で変わる。確かめて直す",
    }
}


def path() -> Path:
    return config_dir() / "tools.json"


def load() -> dict[str, dict]:
    file = path()
    if not file.is_file():
        return {}
    try:
        data = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def get(tool_id: str | None) -> tuple[str | None, dict]:
    """(tool id or None, its description). Unknown or missing tools get generic sizes."""
    if tool_id:
        found = load().get(tool_id)
        if isinstance(found, dict):
            return tool_id, {**GENERIC, **found, "supports": {**GENERIC["supports"], **found.get("supports", {})}}
    return None, GENERIC


def set_tool(tool_id: str, spec: dict) -> dict:
    if not tool_id or not isinstance(spec, dict):
        raise ValueError("tool id and an object are required")
    sizes = spec.get("sizes_px", GENERIC_SIZES)
    if not (isinstance(sizes, list) and sizes and all(isinstance(s, list) and len(s) == 2 and all(int(v) > 0 for v in s) for s in sizes)):
        raise ValueError("sizes_px must be [[w, h], …]")
    data = load()
    data[tool_id] = {**spec, "sizes_px": [[int(s[0]), int(s[1])] for s in sizes]}
    file = path()
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return data[tool_id]


def remove(tool_id: str) -> bool:
    data = load()
    if tool_id not in data:
        return False
    del data[tool_id]
    path().write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return True
