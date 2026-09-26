"""Long work in the background (M2): an export that takes minutes is started, waited on for a little, and if it is
not done by then the agent gets a job id and asks export_status later (an agent's call times out after a minute).

studio/jobs/<id>.json: {id, kind, actor, status: running | done | failed, started, finished?, result?}
The work runs in this process (the MCP server keeps running between calls).
"""

from __future__ import annotations

import json
import os
import threading
import time
import uuid
from pathlib import Path
from typing import Callable

WAIT_S = 40.0  # (under the agent's usual 60 s time limit)
LOST_S = 6 * 3600  # (a job still "running" after this long died with its server)


def _folder(project: Path) -> Path:
    return Path(project) / "studio" / "jobs"


def _write(project: Path, record: dict) -> None:
    folder = _folder(project)
    folder.mkdir(parents=True, exist_ok=True)
    tmp = folder / f".{record['id']}.tmp"
    tmp.write_text(json.dumps(record, ensure_ascii=False), encoding="utf-8")
    tmp.replace(folder / f"{record['id']}.json")


def start(project: Path, kind: str, work: Callable[[], dict], *, actor: str, wait: float = WAIT_S) -> dict:
    """Run `work` (it returns the reply as a dict) in a thread. Its reply when it ends within `wait` seconds;
    else {"job", "status": "running"}."""
    job_id = "job_" + time.strftime("%Y%m%d-%H%M%S") + "_" + uuid.uuid4().hex[:6]
    record = {"id": job_id, "kind": kind, "actor": actor, "status": "running", "started": time.time(), "pid": os.getpid()}
    _write(project, record)
    box: dict = {}

    def run() -> None:
        try:
            reply = work()
            status = "done" if reply.get("ok", True) else "failed"
        except Exception as exc:  # (whatever broke, the job says so instead of staying "running")
            reply, status = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}, "failed"
        box["reply"] = reply
        _write(project, {**record, "status": status, "finished": time.time(), "result": reply})

    thread = threading.Thread(target=run, name=f"genko-{job_id}", daemon=True)
    thread.start()
    thread.join(wait)
    if "reply" in box:
        return {**box["reply"], "job": job_id}
    return {"ok": True, "job": job_id, "status": "running", "kind": kind,
            "hint": "まだ書き出している。しばらく（1〜5 分）待って export_status に job を渡すと結果が返る"}


def status(project: Path, job_id: str) -> dict:
    """The job's state; a finished job carries its reply in "result"."""
    if not job_id or "/" in job_id or "\\" in job_id or job_id.startswith("."):
        return {"ok": False, "error": f"job {job_id} はない"}
    try:
        record = json.loads((_folder(project) / f"{job_id}.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"ok": False, "error": f"job {job_id} はない"}
    elapsed = time.time() - float(record.get("started", 0))
    if record.get("status") == "running" and elapsed > LOST_S:
        record["status"] = "lost"
    out = {"ok": True, "job": job_id, "status": record["status"], "kind": record.get("kind"), "seconds": round(elapsed, 1)}
    if record["status"] in ("done", "failed"):
        out["seconds"] = round(float(record.get("finished", time.time())) - float(record.get("started", 0)), 1)
        out["result"] = record.get("result")
    if record["status"] == "lost":
        out["hint"] = "書き出しの途中で Genko が止まった。もう一度 export を呼ぶ"
    return out


def recent(project: Path, limit: int = 10) -> list[dict]:
    folder = _folder(project)
    if not folder.is_dir():
        return []
    items = sorted(folder.glob("job_*.json"), reverse=True)[:limit]
    return [status(project, p.stem) for p in items]
