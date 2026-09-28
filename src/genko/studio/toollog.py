"""studio/logs/tools.jsonl: one line per agent tool call (tool, ok, error codes, time).

For evaluating real agent runs (§10.8): wrong tools and bad arguments show up
as failed calls, resubmissions as repeated calls. Arguments are summarised
(numbers and short strings only), never the documents or images themselves.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

MAX_BYTES = 20 * 1024 * 1024


def _summary(kwargs: dict) -> dict:
    out = {}
    for key, value in kwargs.items():
        if isinstance(value, bool) or isinstance(value, (int, float)):
            out[key] = value
        elif isinstance(value, str) and len(value) <= 64:
            out[key] = value
    return out


def record(project: Path, actor: str, tool: str, kwargs: dict, result: dict | None, error: str | None, ms: float) -> None:
    path = Path(project) / "studio" / "logs" / "tools.jsonl"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.is_file() and path.stat().st_size > MAX_BYTES:
            path.replace(path.with_name("tools.1.jsonl"))
        entry = {"at": round(time.time(), 3), "actor": actor, "tool": tool, "args": _summary(kwargs), "ms": round(ms, 1)}
        if error is not None:
            entry.update(ok=False, error=error[:300])
        else:
            entry["ok"] = bool(result.get("ok")) if result else True
            codes = sorted({i.get("code") for i in (result or {}).get("issues", []) if i.get("severity") == "error"})
            if codes:
                entry["codes"] = codes
            if result and not result.get("ok") and result.get("error"):
                entry["error"] = str(result["error"])[:300]
        with path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass  # logging never breaks a tool


def read(project: Path) -> list[dict]:
    out = []
    for name in ("tools.1.jsonl", "tools.jsonl"):
        path = Path(project) / "studio" / "logs" / name
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                try:
                    out.append(json.loads(line))
                except ValueError:
                    continue
    return out
