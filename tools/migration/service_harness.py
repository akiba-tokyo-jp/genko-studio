#!/usr/bin/env python3
"""The Python reference for the studio service's C++ contract tests (native/tests/contract/test_contract_service.cpp):
StudioService's tools, HumanService's approvals, adopt_drafts, stats and audit, and `genko studio …` itself, called one
after another on books under a root, with what each answered and what the books and their files were after it.
Development only; nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/service_harness.py run JOBS

JOBS: a JSON list of jobs, each {"root", "steps", "out", "env"?, "clock"?}. Each job starts as a new process would (ids
counted from 1: genko.models.new_id gives 000000000001, … as genko::core::ScopedIdSource(counting_ids()) does; only the
built-in brushes), with "env" set (GENKO_CONFIG_DIR, GENKO_STYLE_CATALOG, GENKO_USER, … ) and time.strftime giving
"clock" (the chat approvals' "at"). The steps:
  {"tool": NAME, "args": [...], "kwargs": {...}, "actor"?}       StudioService(root, actor).NAME(*args, **kwargs)
  {"human": NAME, "project": REL, "actor", "args", "kwargs"}     HumanService(root/REL, actor).NAME(*args, **kwargs)
  {"fn": NAME, "project": REL, "args"?}                           genko.studio.adopt.adopt_drafts / evaluate.NAME on
                                                                 root/REL (stats, audit, eval_sample, eval_score, eval_atari)
  {"cli": [ARGS], "stdin"?: TEXT, "cwd"?: REL}                   genko.studio.cli.main(ARGS) (stdout and exit code)
  {"write": REL, "text" | "base64"}                              a file put under root (drafts, pictures, a catalog)
  {"asset": REL, "base64"}                                       a picture put in root/REL's assets (its ref in the reply)
Texts in a step (and in "env") may hold "$root" (the root folder); a whole text "$ticket:<book>:<n>" is the id of that
book's n-th ticket, "$frame:<book>:<page>:<n>" the n-th panel of a page, "$candidate:<book>:<character>:<n>" a
character's n-th sheet candidate, "$panelcand:<book>:<page>:<panel>:<n>" the n-th candidate of a page's panel,
"$request:<book>:<n>" the n-th art request and "$proposal:<book>:<n>" the n-th proposal (as the book lists them), and
"$job:<book>:<n>" the n-th background job of the book (studio/jobs, in the order they started), as the book is when the step runs
(make_service_cases.py).
Each step's record: {"reply"} (to_dict(), or the dict a person's call returns), or {"raised": TYPE, "message"}; with
"images" (the previews' sha256) for a tool; {"out", "code"} for the command line; then "books" (each project.json under
root, without its revision, and its audit lines {rev, actor, changes, via}) and "files" (each other file under root
and under the config folder with its sha256, the two folders' paths in it written <root> and <config> first; what an
export wrote only listed, "<exported>": the test compares those at the end of the job without the times they carry, but the journal, the audit, the tool log, the lock and the stored
project.json snapshots, which are this build's v4 files there). The root's path is written <root> everywhere, the
config folder's <config>; "taken_at" (the UTC time a style was taken) is <taken_at>, an audit entry's time "at" <at>;
a background job's id is <job>
and its time "seconds" <seconds> (studio/jobs/ itself, which holds times and the process id, is not compared); a work
item's lease (studio/claims/<item>.json) is compared with its "expires" written <expires>; and the address of a Python
object in a message (" at 0x…>") is left out.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import io
import json
import os
import re
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for path in (str(ROOT / "src"), str(HERE)):
    if path not in sys.path:
        sys.path.insert(0, path)

import pyref_harness as base  # noqa: E402  (ids counted, a fresh process's state)
import studio_harness  # noqa: E402  (time.strftime fixed)

JOURNAL = ("studio/journal.jsonl", "studio/journal.partial", "studio/audit.jsonl", "studio/audit.partial")


def _skipped(rel: str) -> bool:
    """The files the two builds keep differently (v3 here, v4 there) or that hold times: not compared."""
    parts = rel.split("/")
    name = parts[-1]
    return (name in ("project.json", "project.lock") or name.endswith(".tmp") or "/".join(parts[-2:]) in JOURNAL
            or ("studio" in parts and "logs" in parts) or "legacy" in parts or ("assets" in parts and not name.endswith(".png"))
            or "/".join(parts[-3:-1]) == "studio/jobs")


def _exported(rel: str) -> bool:
    """A file an export wrote (in a folder named exports, or under the root's out/): the sRGB profile some of them carry
    holds the time it was made, so the test compares them at the end of the job, with those times left out
    (native/tests/contract/export_files.hpp), and here they are only listed."""
    parts = rel.split("/")
    return "exports" in parts[:-1] or parts[0] == "out"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _resolved(value, root: Path):
    """The step with its placeholders put in ($root, $ticket:…, $frame:…, $candidate:…)."""
    from genko.io import load_episode

    if isinstance(value, dict):
        return {k: _resolved(v, root) for k, v in value.items()}
    if isinstance(value, list):
        return [_resolved(v, root) for v in value]
    if not isinstance(value, str):
        return value
    kind, _, rest = value.partition(":")
    if kind == "$job":
        parts = rest.split(":")
        found = (root / parts[0] / "studio" / "jobs").glob("job_*.json")
        return sorted(found, key=lambda p: json.loads(p.read_text(encoding="utf-8"))["started"])[int(parts[1])].stem
    if kind in ("$ticket", "$frame", "$candidate", "$panelcand", "$request", "$proposal"):
        parts = rest.split(":")
        episode = load_episode(root / parts[0])
        if kind == "$ticket":
            return episode.tickets[int(parts[1])]["id"]
        if kind in ("$frame", "$panelcand"):
            page = next(p for p in episode.pages if p.index == int(parts[1]))
            frame = page.leaf_frames()[int(parts[2])]
            return frame.id if kind == "$frame" else frame.panel["candidates"][int(parts[3])]["id"]
        if kind == "$request":
            return list(episode.studio["requests"])[int(parts[1])]
        if kind == "$proposal":
            return list(episode.studio["proposals"])[int(parts[1])]
        return episode.studio["character_candidates"][parts[1]][int(parts[2])]["id"]
    return value.replace("$root", str(root))


def _claim_file(rel: str) -> bool:
    """A work item's lease (claims.py): studio/claims/<item>.json."""
    parts = rel.split("/")
    return len(parts) >= 3 and parts[-3:-1] == ["studio", "claims"] and parts[-1].endswith(".json")


def _files(folder: Path, prefix: str, root: str, config: str) -> dict:
    """Each file's sha256, the root's and the config folder's paths in it written <root> and <config> first (a file
    that names them is the same on both sides)."""
    out = {}
    if not folder.is_dir():
        return out
    for path in sorted(folder.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(folder).as_posix()
        if _skipped(rel):
            continue
        if prefix == "" and _exported(rel):
            out[rel] = "<exported>"
            continue
        data = path.read_bytes()
        if _claim_file(rel):  # (a lease: its time, the time it was taken plus the lease, written <expires>)
            data = re.sub(rb'"expires": -?[0-9][0-9.eE+-]*', b'"expires": "<expires>"', data)
        data = data.replace(root.encode("utf-8"), b"<root>")
        if config:
            data = data.replace(config.encode("utf-8"), b"<config>")
        out[prefix + rel] = _sha(data)
    return out


def _books(root: Path) -> dict:
    from genko import journal

    out = {}
    for project_json in sorted(root.rglob("project.json")):
        book = project_json.parent
        rel = book.relative_to(root).as_posix()
        try:
            payload = json.loads(project_json.read_text(encoding="utf-8"))
        except ValueError:
            continue
        if isinstance(payload, dict):
            payload.pop("revision", None)
        audit = [{k: e.get(k) for k in ("rev", "actor", "changes", "via")} for e in journal.audit_entries(book)]
        out[rel] = {"project": payload, "audit": audit}
    return out


def _normal(value, root: str, config: str):
    """The record with the folders' paths written <root> and <config>, and the UTC time a style was taken <taken_at>."""
    text = json.dumps(value, ensure_ascii=False)
    text = text.replace(json.dumps(root, ensure_ascii=False)[1:-1], "<root>")
    if config:
        text = text.replace(json.dumps(config, ensure_ascii=False)[1:-1], "<config>")
    data = json.loads(text)

    text = re.sub(r" at 0x[0-9a-f]+>", ">", json.dumps(data, ensure_ascii=False))
    text = re.sub(r"job_[^\"/]*_[0-9a-f]{6}", "<job>", text)
    data = json.loads(text)

    def walk(v):
        if isinstance(v, dict):
            return {k: ("<taken_at>" if k == "taken_at" and isinstance(x, str) else "<seconds>" if k == "seconds" and isinstance(x, float)
                        else "<at>" if k == "at" and isinstance(x, float) else walk(x)) for k, x in v.items()}
        if isinstance(v, list):
            return [walk(x) for x in v]
        return v

    return walk(data)


def _raised(exc: BaseException) -> dict:
    return {"raised": type(exc).__name__, "message": str(exc)}


def _step(step: dict, root: Path) -> dict:
    from genko.studio.service import HumanService, StudioService

    try:
        step = _resolved(step, root)
    except (KeyError, IndexError, StopIteration, ValueError) as exc:  # (what the step names is not in the book)
        return {"unresolved": f"{type(exc).__name__}: {exc}"}

    if "write" in step:
        target = root / step["write"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(base64.b64decode(step["base64"]) if "base64" in step else step["text"].encode("utf-8"))
        return {}
    if "asset" in step:
        from genko.assets import AssetStore

        return {"reply": AssetStore(root / step["asset"]).put_bytes(base64.b64decode(step["base64"]), ".png")}
    if "tool" in step:
        try:
            service = StudioService(root, step.get("actor", "ai:agent"))
            result = getattr(service, step["tool"])(*step.get("args", []), **step.get("kwargs", {}))
        except Exception as exc:  # noqa: BLE001 (what the tool raised is part of its contract)
            return _raised(exc)
        return {"reply": result.to_dict(), "images": [_sha(i) for i in result.images]}
    if "human" in step:
        try:
            human = HumanService(root / step["project"], step["actor"])
            reply = getattr(human, step["human"])(*step.get("args", []), **step.get("kwargs", {}))
        except Exception as exc:  # noqa: BLE001
            return _raised(exc)
        return {"reply": reply}
    if "fn" in step:
        from genko.studio import adopt, evaluate

        fns = {"adopt_drafts": adopt.adopt_drafts, "stats": evaluate.stats, "audit": evaluate.audit, "eval_sample": evaluate.eval_sample,
               "eval_score": evaluate.eval_score, "eval_atari": evaluate.eval_atari}
        try:
            return {"reply": fns[step["fn"]](root / step["project"], *step.get("args", []))}
        except Exception as exc:  # noqa: BLE001
            return _raised(exc)
    if "cli" in step:
        from genko.studio import cli

        out, old_cwd = io.StringIO(), os.getcwd()
        old_stdin = sys.stdin
        try:
            os.chdir(root / step.get("cwd", ""))
            sys.stdin = io.StringIO(step.get("stdin", ""))
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                try:
                    code = cli.main(list(step["cli"]))
                except SystemExit as exc:  # (argparse's usage errors)
                    code = exc.code
                except Exception as exc:  # noqa: BLE001 (a traceback: its last line)
                    return {"out": "", "code": 1, "traceback": f"{type(exc).__name__}: {exc}"}
        finally:
            os.chdir(old_cwd)
            sys.stdin = old_stdin
        text = out.getvalue()
        return {"out": json.loads(text) if text.strip() else None, "code": code}
    raise ValueError(f"unknown step {step}")


def run_job(job: dict) -> None:
    root = Path(job["root"]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    env = {k: str(v).replace("$root", str(root)) for k, v in job.get("env", {}).items()}
    saved = {k: os.environ.get(k) for k in env}
    os.environ.update(env)
    config = str(Path(env["GENKO_CONFIG_DIR"]).resolve()) if env.get("GENKO_CONFIG_DIR") else ""
    records = []
    try:
        base.fresh_process_state(True)
        with studio_harness.fixed_clock(job.get("clock")):
            for step in job["steps"]:
                record = _step(step, root)
                record["books"] = _books(root)
                record["files"] = {**_files(root, "", str(root), config),
                                   **(_files(Path(config), "<config>/", str(root), config) if config else {})}
                records.append(_normal(record, str(root), config))
    except Exception:  # noqa: BLE001 (the harness itself failed: say where)
        traceback.print_exc()
        raise
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    Path(job["out"]).write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="service_harness.py")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("jobs")
    args = parser.parse_args(argv)
    for job in json.loads(Path(args.jobs).read_text(encoding="utf-8")):
        run_job(job)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
