#!/usr/bin/env python3
"""F3 (ACCEPTANCE.md §2, FIXTURES.md): the 8-page offline making — bible, script, names, the fixed synthetic candidate
pictures, adoption, finishing and the export — run on Genko's MCP server and its command line: the FakeAgent
(native/tests/support/agents/fake_agent.py) driving `genko mcp` over stdio through the MCP SDK's own client (mcp.Client),
and a scripted person answering its requests with `genko studio …` (a process each time). Nothing goes out: the
pictures are native/tests/support/agents/images.py's, no AI, no network. For native/tests/contract/
test_contract_pipeline.cpp and native/tests/perf/test_perf_pipeline.cpp (PERF-F). Development only; nothing here goes
into the product. Run it with a Python that has the MCP SDK and Pillow (tools/migration/mcp_reference_requirements.txt):

    python3 tools/migration/pipeline_harness.py run JOB OUT

JOB: {"server": {"kind": "cpp", "genko": EXE}, "root", "config", "out", "agent", "pages"?: 8,
"perf"?: {"each": N, "proofs": M}, "time_limit_s"?}. The run writes OUT:
  "calls"   every call in the order made: the agent's tools ({"by": "agent", "tool", "args", "reply", "images", "ms"}:
            the reply as the tool's JSON text, its pictures as {"png": {mode, size, file}} with the file in
            OUT.images/NNN.png) and the person's commands ({"by": "person", "cli", "reply", "exit", "ms"}), "ms" the
            time from sending the call to its whole answer (the agent's: over JSON-RPC; the person's: the process run)
  "end"     `genko studio audit` and `genko studio stats` on the book at the end (their replies)
  "agent"   how many work items the agent did ("steps", "log"); "making_seconds": how long the making took (the
            server's start and "perf" left out), "seconds" the whole run
  "perf"    (with "perf") after the making, on the same book: "each" calls of each kind of PERF-F — information
            ("info": status, inspect of a page and a panel, candidates, next, tickets and a page's preview (render,
            1024 px) in turn), an edit committed ("edit": apply_ops with a page's note, committed), a candidate
            adopted ("adopt": a panel's good picture adopted again, which Genko enlarges and adopts as the pipeline
            does) and a page finished ("finish":
            finish_page committed; the person approves the page's art again between the two, as they must after an
            agent adopts) — each {"tool", "ms"}; and "proofs": the 8-page proof (export_proof as a background job) M
            times, each into a new folder (studio/proofs/r<revision>: a note committed first), with the time the job was
            taken ("accepted_ms": the reply that names the job, or its result when it ended within the tool's wait) and
            the time its file was whole ("done_ms": the job's result read back, the PDF there with its 8 pages)
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import copy
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True  # (it imports the agents and mcp_harness from the repository: no __pycache__ left there)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "native" / "tests" / "support"))

import mcp_harness as mh  # noqa: E402  (the server's command and environment, the pictures)

PROJECT = "story.genko"
TITLE = "夏の午後の約束"


# --- the agent's calls -------------------------------------------------------------------------------------------------


class Recorder:
    """The MCP SDK's client as FakeAgent uses it (call_tool), each call written down with its time.

    A tool that works in the background (export_proof is always a job: it answers within its 40 s, or returns the job
    to ask after) is followed the same way however fast the build is: its job's record (studio/jobs/<id>.json) is
    waited on until the job has ended, then export_status is asked once; the tool's reply is kept in the form it has
    when the job ended in time (when it came back running, the job's result in its place: StudioService._job of
    jobs.start's {**reply, "job": id}, as mcp_harness._settled puts it), and its time runs until the job ended."""

    def __init__(self, client, calls: list, root: Path) -> None:
        self.client = client
        self.calls = calls
        self.root = root

    async def _call(self, name: str, args: dict):
        started = time.perf_counter()
        result = await self.client.call_tool(name, args)
        return result, (time.perf_counter() - started) * 1000.0, started

    def _record(self, name: str, args: dict, result, ms: float) -> None:
        text = result.content[0].text if result.content and result.content[0].type == "text" else None
        images = []
        for block in result.content[1:]:
            if block.type == "image":
                images.append({"image": block.mime_type, **mh._png_summary(base64.b64decode(block.data))})
            else:
                images.append({"other": block.type})
        self.calls.append({"by": "agent", "tool": name, "args": args, "reply": json.loads(text) if text is not None else None,
                           "is_error": bool(result.is_error), "images": images, "ms": ms})

    async def call_tool(self, name: str, args: dict):
        from types import SimpleNamespace

        sent = copy.deepcopy(args)
        result, ms, started = await self._call(name, args)
        try:
            reply = json.loads(result.content[0].text)
        except (AttributeError, IndexError, ValueError):
            reply = None
        if name == "export_status" or not (isinstance(reply, dict) and reply.get("ok") and reply.get("job")):
            self._record(name, sent, result, ms)
            return result
        record = self.root / str(args.get("project", "")) / "studio" / "jobs" / f"{reply['job']}.json"
        deadline = time.monotonic() + 3600
        while True:
            try:
                state = json.loads(record.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                state = {"status": "running"}  # (not there yet, or being replaced)
            if state.get("status") != "running":
                break
            if time.monotonic() > deadline:
                raise RuntimeError(f"job {reply['job']} did not end")
            await asyncio.sleep(0.2)
        ms = (time.perf_counter() - started) * 1000.0
        status, status_ms, _ = await self._call("export_status", {"project": args.get("project"), "job": reply["job"], "wait_s": 0})
        if reply.get("status") == "running":
            done = {**(json.loads(status.content[0].text).get("result") or {}), "job": reply["job"]}
            data = {k: v for k, v in done.items() if k not in ("ok", "issues", "files")}
            final = {"ok": bool(done.get("ok")), **data, "issues": [], "files": list(done.get("files") or []) if done.get("ok") else []}
            result = SimpleNamespace(content=[SimpleNamespace(type="text", text=json.dumps(final, ensure_ascii=False))], is_error=False)
        self._record(name, sent, result, ms)
        self._record("export_status", {"project": args.get("project"), "job": reply["job"], "wait_s": 0}, status, status_ms)
        return result


# --- the person ----------------------------------------------------------------------------------------------------------


class Person:
    """The scripted person: answers the agent's approval requests with `genko studio …`, a process each time, reading
    what was asked from project.json."""

    def __init__(self, job: dict, calls: list) -> None:
        self.job = job
        self.project = Path(job["root"]) / PROJECT
        self.out = Path(job["out"])
        self.calls = calls
        self.exports: list[dict] = []
        self.refused: list[dict] = []

    def run(self, *args) -> dict:
        argv = ["studio", *map(str, args)]
        server = self.job["server"]
        cmd = [server["genko"], *argv]
        started = time.perf_counter()
        done = subprocess.run(cmd, env=mh._env(self.job), capture_output=True, timeout=1800)
        ms = (time.perf_counter() - started) * 1000.0
        lines = done.stdout.decode("utf-8").strip().splitlines()
        if not lines:
            raise RuntimeError(f"genko {' '.join(argv)} printed nothing (exit {done.returncode}): {done.stderr.decode('utf-8', 'replace')[-2000:]}")
        reply = json.loads(lines[-1])
        self.calls.append({"by": "person", "cli": argv, "reply": reply, "exit": done.returncode, "ms": ms})
        assert (done.returncode == 0) == reply["ok"], (argv, reply, done.stderr.decode("utf-8", "replace")[-2000:])
        return reply

    def book(self) -> dict:
        return json.loads((self.project / "project.json").read_text(encoding="utf-8"))

    def __call__(self, waiting: list[dict]) -> None:
        book = self.book()
        asked = {t.get("gate") for t in book.get("tickets", []) if t.get("kind") == "gate" and t.get("status") == "open"}
        for gate in waiting:
            assert gate["gate"] in asked, f"the agent did not ask for {gate}"  # (people answer requests, they are not polled)
            if gate["gate"] in ("name", "art"):
                pages = ",".join(map(str, gate["pages"]))
                assert self.run("approve", self.project, gate["gate"], "--pages", pages, "--as", "human:test")["ok"]
            elif gate["gate"] == "sheet":
                for cid in gate["characters"]:
                    cand = book["studio"]["character_candidates"][cid][0]["id"]
                    assert self.run("approve", self.project, "sheet", "--character", cid, "--candidate", cand, "--as", "human:test")["ok"]
            elif gate["gate"] == "export":
                refused = self.run("export", self.project, "--format", "pdf", "--out", self.out / "refused", "--dpi", 72, "--as", "human:test")
                self.refused.append(refused)
                for fmt in ("pdf", "tiff"):
                    self.exports.append(self.run("export", self.project, "--format", fmt, "--out", self.out / fmt, "--dpi", 72,
                                                 "--allow-fixture", "--force", "--as", "human:test"))


# --- PERF-F after the making ------------------------------------------------------------------------------------------------


async def _perf(job: dict, agent, recorder: Recorder, person: Person, client) -> dict:
    each = int(job["perf"].get("each", 100))
    proofs = int(job["perf"].get("proofs", 10))
    book = person.book()
    pages = [p["index"] for p in book["pages"]]

    def leaves(page: dict) -> list[dict]:
        out = []

        def walk(frame: dict) -> None:
            kids = frame.get("children") or []
            if kids:
                for kid in kids:
                    walk(kid)
            else:
                out.append(frame)

        for frame in page.get("frames", []):
            walk(frame)
        return out

    panels = [(p["index"], f["id"]) for p in book["pages"] for f in leaves(p) if (f.get("panel") or {}).get("adopted")]
    if not panels:
        raise RuntimeError("no adopted panel to measure on")

    async def timed(kind: str, name: str, **args) -> dict:
        before = len(recorder.calls)
        reply = await agent.tool(name, **args)
        call = recorder.calls[before]
        if not reply.get("ok"):
            raise RuntimeError(f"{kind} {name} {args} failed: {json.dumps(reply, ensure_ascii=False)[:2000]}")
        return {"kind": kind, "tool": name, "ms": call["ms"]}

    out: dict = {"info": [], "edit": [], "adopt": [], "finish": [], "approve_ms": [], "proofs": []}
    # information: what an agent reads to decide its next step
    info = [lambda i: ("status", {}),
            lambda i: ("inspect", {"target": "page", "page": pages[i % len(pages)]}),
            lambda i: ("inspect", {"target": "panel", "page": panels[i % len(panels)][0], "frame_id": panels[i % len(panels)][1]}),
            lambda i: ("candidates", {"page": panels[i % len(panels)][0], "frame_id": panels[i % len(panels)][1]}),
            lambda i: ("next", {"limit": 5}),
            lambda i: ("tickets", {}),
            lambda i: ("render", {"page": pages[i % len(pages)], "mode": "print"})]
    for i in range(each):
        name, args = info[i % len(info)](i // len(info))
        out["info"].append(await timed("info", name, **args))
    # an edit committed: a page's note
    for i in range(each):
        page = pages[i % len(pages)]
        ops = [{"op": "set_note", "page": page, "note": f"計測 {i}"}]
        out["edit"].append(await timed("edit", "apply_ops", ops=ops, commit=True))
    # a candidate adopted (the panel's good picture: Genko enlarges it and adopts the enlargement), the person's approval
    # of the page's art again (an agent's adoption takes it back), the page finished
    for i in range(each):
        page, frame_id = panels[i % len(panels)]
        listed = await agent.tool("candidates", page=page, frame_id=frame_id)  # (finding the candidate: not measured)
        good = [c for c in listed["candidates"] if c["origin"].get("model") == "fixture-good"]
        out["adopt"].append(await timed("adopt", "adopt", page=page, frame_id=frame_id, candidate_id=good[-1]["id"]))
        approved = person.run("approve", person.project, "art", "--pages", page, "--as", "human:test")
        assert approved["ok"]
        out["approve_ms"].append(person.calls[-1]["ms"])
        out["finish"].append(await timed("finish", "finish_page", page=page, commit=True))
    # the 8-page proof, M times, each into a folder of its own (the revision moved on by a note). MCP's export_proof is
    # always a job, and the tool waits up to 40 s for it before it answers (with the job's result, or "running"): the
    # time to the reply holds that wait. The job's own record (studio/jobs/<id>.json) says when it started and ended.
    for i in range(proofs):
        await agent.tool("apply_ops", ops=[{"op": "set_note", "page": 1, "note": f"校正 {i}"}], commit=True)
        revision = person.book()["revision"]
        called = time.time()
        started = time.perf_counter()
        result = await client.call_tool("export_proof", {"project": PROJECT, "format": "pdf"})
        accepted_ms = (time.perf_counter() - started) * 1000.0
        reply = json.loads(result.content[0].text)
        if not reply.get("ok"):
            raise RuntimeError(f"export_proof failed: {reply}")
        if reply.get("status") == "running":
            while True:
                status = json.loads((await client.call_tool("export_status", {"project": PROJECT, "job": reply["job"], "wait_s": 30})).content[0].text)
                if status.get("status") != "running":
                    reply = {**(status.get("result") or {}), "job": reply["job"], "status": status.get("status")}
                    break
        done_ms = (time.perf_counter() - started) * 1000.0
        record = {}
        if reply.get("job"):
            record_file = Path(job["root"]) / PROJECT / "studio" / "jobs" / f"{reply['job']}.json"
            if record_file.is_file():
                record = json.loads(record_file.read_text(encoding="utf-8"))
        job_started, job_finished = record.get("started"), record.get("finished")
        files = [Path(f) for f in reply.get("files") or []]
        pdf = files[0].read_bytes() if files and files[0].is_file() else b""
        folder = Path(job["root"]) / PROJECT / "studio" / "proofs" / f"r{revision:04d}"
        out["proofs"].append({"accepted_ms": accepted_ms, "done_ms": done_ms, "job": reply.get("job"), "ok": bool(reply.get("ok")),
                              # (the job's record: from the call to its start, and from its start to its end)
                              "job_start_ms": (job_started - called) * 1000.0 if isinstance(job_started, (int, float)) else None,
                              "job_ms": (job_finished - job_started) * 1000.0 if isinstance(job_started, (int, float)) and
                              isinstance(job_finished, (int, float)) else None,
                              "file": str(files[0]) if files else None, "new_folder": bool(files) and files[0].parent == folder,
                              "pdf": pdf[:5] == b"%PDF-", "pages": len(re.findall(rb"/Type\s*/Page(?![A-Za-z])", pdf))})
    return out


# --- the run -----------------------------------------------------------------------------------------------------------------


async def _scenario(job: dict, calls: list) -> dict:
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters

    from agents.fake_agent import FakeAgent

    cmd = mh._mcp_command(job, "stdio")
    target = StdioServerParameters(command=cmd[0], args=cmd[1:], env=mh._env(job))
    person = Person(job, calls)
    out: dict = {}
    async with Client(target) as client:
        recorder = Recorder(client, calls, Path(job["root"]))
        agent = FakeAgent(recorder, Path(job["root"]), PROJECT, person, pages=int(job.get("pages", 8)))
        started = time.perf_counter()
        created = await agent.tool("create_project", name=PROJECT, title=TITLE, pages=int(job.get("pages", 8)))
        assert created["ok"], created
        steps = await agent.run()
        out["making_seconds"] = time.perf_counter() - started
        out["agent"] = {"steps": steps, "log": len(agent.log)}
        out["person"] = {"refused": person.refused, "exports": person.exports}
        # (what follows is not the making: its calls are kept apart)
        recorder.calls = person.calls = []
        out["end"] = {"audit": person.run("audit", person.project), "stats": person.run("stats", person.project)}
        if job.get("perf"):
            recorder.calls = person.calls = []
            out["perf"] = await _perf(job, agent, recorder, person, client)
    return out


def _time_limit(job: dict) -> None:
    """job["time_limit_s"], when there is one: at it, where every thread is gets printed, and a second later the run stops
    with a TimeoutError (what was asked last and OUT.partial.json then, as for any failure), before the test that
    started this gives up on it and kills it without a word."""
    limit = job.get("time_limit_s")
    if not limit:
        return
    import faulthandler
    import signal

    faulthandler.dump_traceback_later(float(limit), exit=False)
    if hasattr(signal, "SIGALRM"):
        def stop(signum, frame):
            raise TimeoutError(f"the time limit of {limit} s was reached")

        signal.signal(signal.SIGALRM, stop)
        signal.alarm(int(limit) + 1)


def run_job(job: dict, out_path: str) -> None:
    _time_limit(job)
    root = Path(job["root"])
    root.mkdir(parents=True, exist_ok=True)
    Path(job["config"]).mkdir(parents=True, exist_ok=True)
    Path(job["out"]).mkdir(parents=True, exist_ok=True)
    job["root"] = str(root.resolve())
    job["config"] = str(Path(job["config"]).resolve())
    job["out"] = str(Path(job["out"]).resolve())
    calls: list = []
    started = time.perf_counter()
    try:
        out = asyncio.run(_scenario(job, calls))
    except BaseException:
        # (what was asked last and what came back, to read why the run stopped; all of it beside OUT)
        Path(out_path + ".partial.json").write_text(json.dumps(calls, ensure_ascii=False, indent=1, default=str), encoding="utf-8")
        for call in calls[-3:]:
            print("last calls:", json.dumps({k: v for k, v in call.items() if k != "images"}, ensure_ascii=False, default=str)[:2000],
                  file=sys.stderr)
        raise
    out["seconds"] = time.perf_counter() - started
    out["calls"] = calls
    out["root"], out["config"], out["out"] = job["root"], job["config"], job["out"]
    Path(out_path).write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    folder = Path(out_path + ".images")
    folder.mkdir(exist_ok=True)
    for n, data in enumerate(mh.IMAGES, 1):
        (folder / f"{n:03d}.png").write_bytes(data)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="pipeline_harness.py")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("job")
    p.add_argument("out")
    args = parser.parse_args(argv)
    run_job(json.loads(Path(args.job).read_text(encoding="utf-8")), args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
