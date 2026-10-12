#!/usr/bin/env python3
"""The reference runs of native/tests/contract/test_contract_mcp.cpp: Genko's MCP server (`genko mcp`, stdio and
Streamable HTTP) and its HTTP API (`genko serve`), the C++ build's and Python's (src/genko on the MCP SDK of uv.lock),
driven the same way and recorded the same way, so the test can compare the two. Development only; nothing here goes
into the product. Run it with a Python that has the `mcp` extra of pyproject.toml (mcp>=2,<3, pinned by uv.lock) and
Pillow:

    python3 tools/migration/mcp_harness.py run JOB OUT
    python3 tools/migration/mcp_harness.py pyserve ROOT AGENT     (Python's MCP server on Streamable HTTP, a free port)
    python3 tools/migration/mcp_harness.py pygenko ARGS...        (Python's `genko ARGS…`)

Python's side lays its text out as the measured reference does: Pillow's BASIC layout, without raqm (render_harness and
pyref_harness hold it the same way; the C++ drawing is BASIC's).

JOB: {"server": {"kind": "cpp", "genko": EXE} | {"kind": "py", "python": EXE, "repo": REPO}, "root", "config",
"agent", "steps"}. The steps, in order:
  {"write": REL, "text" | "base64" | "json"}          a file put under the root
  {"job_record": BOOK, "id", "status", "result"?}     studio/jobs/<id>.json of the book (started and beat now; a
                                                      running one's beat renewed every 5 s until the job's end)
  {"stdio": {"lines": [...], "gap"?, "wait"?, "silent"?}}
                                                      one `genko mcp` over stdio fed these lines (a JSON value is
                                                      sent as one line, {"raw": text} as it is, {"sleep": s} waits,
                                                      {"await": [ids]} waits until those are answered);
                                                      once the ids in "wait" are answered stdin is closed, and every
                                                      message it wrote until it ended is kept (by id), with the ids
                                                      of "silent" it never answered
  {"http": [REQUEST, ...]}                            the raw Streamable HTTP of one `genko mcp --transport
                                                      streamable-http`: each {"method", "path"?, "headers"?, "body"?,
                                                      "raw"?, "chunked"?, "session"?: "keep" | "use" | "none",
                                                      "stream"?: "head"}; status, the headers that matter (a
                                                      redirect's Location) and the JSON or SSE body
  {"client": {"transport": "stdio" | "http", "mode": "auto" | "legacy", "ops": [...]}}
                                                      the MCP SDK's own Client (mcp.Client): ops info, list_tools,
                                                      list_resources, read_resource, call (with "settle": a job it
                                                      returns asked after until it is done), cancel, ping
  {"api": [REQUEST, ...]}                             `genko serve` (tokens.json in the config folder: tok-ai is
                                                      ai:bot, tok-human human:owner): each
                                                      {"method", "path", "headers"?, "token"?, "json"? | "base64"? |
                                                      "text"?, "chunked"?, "declare"?}: status, content type, CORS
                                                      headers and the JSON body (a PNG as its pixels)
Each record is normalised: the root's and the config folder's paths <root> and <config>, ids (12 hex digits) <id1>,
<id2>, … in the order met, a session's id <session>, a job's id <job>, its "seconds" <seconds>, a time stamp <time>,
how long ago another conversation wrote (<minutes_ago>, in the words too), pictures (PNG) as their mode and size, and the file (OUT.images/NNN.png, in the order met) the test compares the
pixels of.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import io
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

# --- normalising -------------------------------------------------------------------------------------------------------


IMAGES: list[bytes] = []  # (the pictures met, written beside the records: the test compares their pixels)


def _png_summary(data: bytes) -> dict:
    from PIL import Image

    with Image.open(io.BytesIO(data)) as image:
        image.load()
        IMAGES.append(data)
        return {"png": {"mode": image.mode, "size": list(image.size), "file": f"{len(IMAGES):03d}.png"}}


class Normal:
    """Writes the run's own values (paths, random ids, times) as placeholders, the same way on both sides."""

    def __init__(self, root: str, config: str) -> None:
        self.root = root
        self.config = config
        self.ids: dict[str, str] = {}
        # Once an Undo went through, the book holds what the C++ build restored from a state asset, which is canonical
        # JSON (its objects' keys sorted: schema-v4.md), where Python puts back the keys in the order they had, and its
        # revision counts on where Python's goes back; the replies after it are compared with their objects' keys
        # sorted and without the revisions, on both sides (as test_contract_service compares a book an Undo restored).
        # Before it, revisions are compared as they are.
        self.restored = False

    def text(self, value: str) -> str:
        value = value.replace(self.root, "<root>").replace(self.config, "<config>")
        value = re.sub(r" at 0x[0-9a-f]+>", ">", value)  # (a Python object's address in a message)
        value = re.sub(r"job_\d{8}-\d{6}_[0-9a-f]{6}", "<job>", value)

        def one(m: re.Match) -> str:
            key = m.group(0)
            if key not in self.ids:
                self.ids[key] = f"<id{len(self.ids) + 1}>"
            return self.ids[key]

        value = re.sub(r"(?<![0-9a-f])[0-9a-f]{12}(?![0-9a-f])", one, value)
        value = re.sub(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}", "<time>", value)
        value = re.sub(r"\d{8}-\d{6}", "<time>", value)
        value = re.sub(r"（\d+(?:\.\d+)? 分前）", "（<minutes_ago> 分前）", value)  # (book_in_use: how long ago the others wrote)
        if self.restored:  # (a folder named after the book's revision, which counts on through an Undo in v4)
            value = re.sub(r"/proofs/r\d+/", "/proofs/r<revision>/", value)
        return value

    def value(self, value):
        if isinstance(value, dict):
            out = {}
            for k, v in value.items():
                timing = k in ("seconds", "minutes_ago", "ms", "started", "beat", "finished", "at", "expires")
                # (the revision only once an Undo went through: the C++ build's counts on through it, schema-v4.md)
                if (timing or (self.restored and k in ("revision", "at_rev"))) and isinstance(v, (int, float)):
                    out[self.text(k)] = f"<{k}>"
                else:
                    out[self.text(k)] = self.value(v)
            return out
        if isinstance(value, list):
            return [self.value(v) for v in value]
        if isinstance(value, str):
            # (a JSON text inside a text: compared as the value it holds)
            if value[:1] in "{[" and value[-1:] in "}]":
                try:
                    return {"json": self.value(json.loads(value))}
                except ValueError:
                    pass
            return self.text(value)
        return value


# --- servers -------------------------------------------------------------------------------------------------------------


def _env(job: dict) -> dict:
    env = dict(os.environ)
    env["GENKO_CONFIG_DIR"] = job["config"]
    env.pop("PYTHONPATH", None)
    if job["server"]["kind"] == "py":
        env["PYTHONPATH"] = str(Path(job["server"]["repo"]) / "src")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def _mcp_command(job: dict, transport: str) -> list[str]:
    server = job["server"]
    if server["kind"] == "cpp":
        cmd = [server["genko"], "mcp", "--root", job["root"], "--agent", job["agent"]]
        if transport == "http":
            cmd += ["--transport", "streamable-http", "--port", "0"]
        return cmd
    if transport == "http":
        return [server["python"], str(HERE / "mcp_harness.py"), "pyserve", job["root"], job["agent"]]
    return _python_genko(server, ["mcp", "--root", job["root"], "--agent", job["agent"]])


def _python_genko(server: dict, args: list[str]) -> list[str]:
    """`python -m genko <args>` with the text laid out as the measured reference lays it out (pygenko)."""
    return [server["python"], str(HERE / "mcp_harness.py"), "pygenko", *args]


def _basic_layout() -> None:
    try:  # (the measured reference's text layout: BASIC, without raqm, as render_harness and pyref_harness hold it)
        from PIL import ImageFont

        ImageFont.core.HAVE_RAQM = False
    except ImportError:
        pass


class Running:
    """A server process that prints its address on stderr ("… http://127.0.0.1:<port>…")."""

    def __init__(self, cmd: list[str], env: dict) -> None:
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.port = None
        self.err: list[str] = []
        deadline = time.monotonic() + 120
        while self.port is None:
            line = self.proc.stderr.readline().decode("utf-8", "replace")
            if not line:
                raise RuntimeError("the server ended: " + "".join(self.err))
            self.err.append(line)
            m = re.search(r"http://127\.0\.0\.1:(\d+)", line)
            if m:
                self.port = int(m.group(1))
            if time.monotonic() > deadline:
                raise RuntimeError("the server did not say where it listens")
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self) -> None:
        for line in self.proc.stderr:
            self.err.append(line.decode("utf-8", "replace"))

    def stop(self) -> int:
        self.proc.send_signal(signal.SIGINT)
        try:
            return self.proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait()


# --- stdio ---------------------------------------------------------------------------------------------------------------


def _stdio(job: dict, step: dict, norm: Normal) -> dict:
    proc = subprocess.Popen(_mcp_command(job, "stdio"), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=_env(job))
    got: list[dict] = []
    raw_lines: list[str] = []
    lock = threading.Lock()
    answered = threading.Condition(lock)

    def read() -> None:
        for line in proc.stdout:
            text = line.decode("utf-8")
            with lock:
                raw_lines.append(text)
                try:
                    got.append(json.loads(text))
                except ValueError:
                    got.append({"not_json": text})
                answered.notify_all()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    threading.Thread(target=lambda: proc.stderr.read(), daemon=True).start()

    def has(rid) -> bool:
        return any(isinstance(m, dict) and m.get("id") == rid for m in got)

    gap = float(step.get("gap", 0.05))
    for line in step["lines"]:
        if isinstance(line, dict) and "sleep" in line:
            time.sleep(float(line["sleep"]))
            continue
        if isinstance(line, dict) and "await" in line:  # (what follows needs these answered: requests run side by side)
            with lock:
                for rid in line["await"]:
                    answered.wait_for(lambda rid=rid: has(rid), timeout=600)
            continue
        text = line["raw"] if isinstance(line, dict) and "raw" in line else json.dumps(line, ensure_ascii=False) + "\n"
        proc.stdin.write(text.encode("utf-8"))
        proc.stdin.flush()
        if isinstance(line, dict) and line.get("method") == "initialize" and "id" in line:
            with lock:  # (the handshake first: what follows is served once it is answered)
                answered.wait_for(lambda: has(line["id"]), timeout=60)
        time.sleep(gap)
    with lock:
        for rid in step.get("wait", []):
            answered.wait_for(lambda rid=rid: has(rid), timeout=600)
        silent = list(step.get("silent", []))
    proc.stdin.close()
    code = proc.wait(timeout=180)
    reader.join(timeout=30)
    messages = sorted(got, key=lambda m: json.dumps(m.get("id") if isinstance(m, dict) else None))
    return {"stdio": {"messages": norm.value(messages), "silent": [s for s in silent if not has(s)], "exit": code,
                      "lines_end_with_newline": all(t.endswith("\n") for t in raw_lines)}}


# --- raw Streamable HTTP ----------------------------------------------------------------------------------------------------


def _sse_events(text: str) -> list:
    events = []
    for block in re.split(r"\r\n\r\n|\n\n|\r\r", text):
        data = [line[5:].lstrip(" ") for line in re.split(r"\r\n|\n|\r", block) if line.startswith("data:")]
        if data:
            joined = "\n".join(data)
            try:
                events.append(json.loads(joined))
            except ValueError:
                events.append({"not_json": joined})
    return events


def _chunked(body: bytes) -> bytes:
    """The body in two chunks (the first with an extension) and a trailer, as Transfer-Encoding: chunked sends it."""
    half = max(1, len(body) // 2)
    pieces = [body[:half], body[half:]]
    out = b""
    for n, piece in enumerate(pieces):
        if piece:
            out += f"{len(piece):x}".encode() + (b";part=1" if n == 0 else b"") + b"\r\n" + piece + b"\r\n"
    return out + b"0\r\nX-Trailer: done\r\n\r\n"


def _http(job: dict, step: list, norm: Normal) -> dict:
    server = Running(_mcp_command(job, "http"), _env(job))
    port = server.port
    out = []
    session = None
    held = None
    try:
        for req in step:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
            headers = {"Host": f"127.0.0.1:{port}"}
            for k, v in (req.get("headers") or {}).items():
                headers[k] = v.replace("$port", str(port))
            if req.get("session") in ("use", "keep_use") and session:
                headers["Mcp-Session-Id"] = session
            body = None
            if "body" in req:
                body = json.dumps(req["body"], ensure_ascii=False).encode("utf-8")
            elif "raw" in req:
                body = req["raw"].encode("utf-8")
            conn.putrequest(req["method"], req.get("path", "/mcp"), skip_host=True, skip_accept_encoding=True)
            for k, v in headers.items():
                conn.putheader(k, v)
            if "declare" in req:  # (a size said, nothing sent: the server answers before it reads)
                conn.putheader("Content-Length", str(int(req["declare"])))
            elif body is not None and req.get("chunked"):
                conn.putheader("Transfer-Encoding", "chunked")
            elif body is not None:
                conn.putheader("Content-Length", str(len(body)))
            conn.endheaders()
            if body is not None:
                conn.send(_chunked(body) if req.get("chunked") else body)
            resp = conn.getresponse()
            location = resp.getheader("location")
            record = {"status": resp.status,
                      "type": (resp.getheader("content-type") or "").split(";")[0].strip(),
                      "session": "<session>" if resp.getheader("mcp-session-id") else None,
                      "allow": resp.getheader("allow")}
            if location is not None:
                record["location"] = location.replace(str(port), "$port")
            if req.get("session") == "keep" and resp.getheader("mcp-session-id"):
                session = resp.getheader("mcp-session-id")
            if req.get("stream") == "head":
                record["body"] = "<stream>"
                if req.get("hold"):
                    held = conn
                else:
                    conn.close()
            else:
                data = resp.read()
                text = data.decode("utf-8", "replace")
                if record["type"] == "text/event-stream":
                    record["events"] = norm.value(_sse_events(text))
                elif record["type"] == "application/json" and text:
                    record["body"] = norm.value(json.loads(text))
                else:
                    record["body"] = text
                conn.close()
            if req.get("release") and held is not None:
                held.close()
                held = None
            out.append(record)
    finally:
        if held is not None:
            held.close()
        server.stop()
    return {"http": out}


# --- the SDK's own client ------------------------------------------------------------------------------------------------


def _sorted_keys(value):
    if isinstance(value, dict):
        return {k: _sorted_keys(value[k]) for k in sorted(value)}
    if isinstance(value, list):
        return [_sorted_keys(v) for v in value]
    return value


def _content(norm: Normal, result) -> dict:
    blocks = []
    for block in result.content:
        if block.type == "text":
            blocks.append({"text": norm.value(block.text)})
        elif block.type == "image":
            blocks.append({"image": block.mime_type, **_png_summary(base64.b64decode(block.data))})
        else:
            blocks.append({"other": block.type})
    return {"content": blocks, "is_error": bool(result.is_error)}


async def _client_ops(job: dict, step: dict, norm: Normal) -> list:
    import anyio
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    from mcp.shared.exceptions import MCPError

    spec = step["client"]
    server = None
    if spec["transport"] == "stdio":
        cmd = _mcp_command(job, "stdio")
        target = StdioServerParameters(command=cmd[0], args=cmd[1:], env=_env(job))
    else:
        server = Running(_mcp_command(job, "http"), _env(job))
        target = f"http://127.0.0.1:{server.port}/mcp"
    out = []
    try:
        async with Client(target, mode=spec.get("mode", "auto")) as client:
            session = client.session
            for op in spec["ops"]:
                kind = op["op"]
                try:
                    if kind == "info":
                        init = getattr(session, "initialize_result", None)
                        disc = getattr(session, "discover_result", None)
                        if init is not None:
                            out.append({"info": norm.value({"era": "handshake", "version": init.protocol_version,
                                                            "server": init.server_info.model_dump(mode="json", exclude_none=True),
                                                            "capabilities": init.capabilities.model_dump(mode="json", exclude_none=True),
                                                            "instructions": init.instructions})})
                        else:
                            out.append({"info": norm.value({"era": "2026", "versions": disc.supported_versions,
                                                            "capabilities": disc.capabilities.model_dump(mode="json", exclude_none=True),
                                                            "instructions": disc.instructions})})
                    elif kind == "list_tools":
                        tools = await client.list_tools()
                        out.append({"tools": [{"name": t.name, "description": t.description, "schema": t.input_schema} for t in tools.tools]})
                    elif kind == "list_resources":
                        res = await client.list_resources()
                        out.append({"resources": [r.model_dump(mode="json", exclude_none=True) for r in res.resources]})
                    elif kind == "read_resource":
                        res = await client.read_resource(op["uri"])
                        out.append({"read": [{"uri": str(c.uri), "mime": c.mime_type, "sha256": hashlib.sha256(c.text.encode()).hexdigest()}
                                             for c in res.contents]})
                    elif kind == "ping":
                        await client.send_ping()
                        out.append({"ping": True})
                    elif kind == "call":
                        result = await client.call_tool(op["name"], op.get("args") or {})
                        if op.get("settle"):
                            result = await _settled(client, op, result)
                        record = {"call": op["name"], **_content(norm, result)}
                        if norm.restored:
                            record = {"keys_sorted": True, **_sorted_keys(record)}
                        out.append(record)
                        text = record["content"][0].get("text") if record["content"] else None
                        if op["name"] == "undo" and isinstance(text, dict) and text.get("json", {}).get("ok") is True:
                            norm.restored = True
                    elif kind == "cancel":
                        # a slow call given up after a moment: the client sends notifications/cancelled; the server
                        # never answers it and goes on serving
                        done = {}

                        async def slow():
                            done["result"] = await client.call_tool(op["name"], op.get("args") or {})

                        started = time.monotonic()
                        with anyio.move_on_after(float(op.get("after", 1.0))) as scope:
                            await slow()
                        await client.send_ping()
                        out.append({"cancel": op["name"], "cancelled": scope.cancelled_caught and "result" not in done,
                                    "served_after": time.monotonic() - started < float(op.get("after", 1.0)) + 30})
                    else:
                        raise ValueError(f"unknown op {kind}")
                except MCPError as exc:
                    out.append({"error": {"op": kind, "code": exc.error.code, "message": norm.text(exc.error.message)}})
    finally:
        if server is not None:
            server.stop()
    return out


async def _settled(client, op: dict, result):
    """A tool that works in the background answers within 40 s, or returns a job to ask after: its final reply either way
    (export_status until the job is done, the job's result put in the reply's place), so the record does not depend on
    how fast the build is."""
    try:
        reply = json.loads(result.content[0].text)
    except (AttributeError, IndexError, ValueError):
        return result
    if not (isinstance(reply, dict) and reply.get("ok") and reply.get("job") and reply.get("status") == "running"):
        return result
    project = (op.get("args") or {}).get("project")
    for _ in range(600):
        status = await client.call_tool("export_status", {"project": project, "job": reply["job"], "wait_s": 30})
        state = json.loads(status.content[0].text)
        if state.get("status") != "running":
            from types import SimpleNamespace

            # (the reply as the tool gives it when the job ends within its 40 s: StudioService._job of jobs.start's
            # {**reply, "job": id}, its issues left out)
            done = {**(state.get("result") or {}), "job": reply["job"]}
            data = {k: v for k, v in done.items() if k not in ("ok", "issues", "files")}
            final = {"ok": bool(done.get("ok")), **data, "issues": [], "files": list(done.get("files") or []) if done.get("ok") else []}
            text = SimpleNamespace(type="text", text=json.dumps(final, ensure_ascii=False))
            return SimpleNamespace(content=[text], is_error=False)
    raise RuntimeError(f"job {reply['job']} did not end")


def _client(job: dict, step: dict, norm: Normal) -> dict:
    import anyio

    return {"client": anyio.run(_client_ops, job, step, norm)}


# --- `genko serve` -----------------------------------------------------------------------------------------------------------


TOKENS = {"tok-ai": {"actor": "ai:bot"}, "tok-human": {"actor": "human:owner"}}


def _api(job: dict, step: list, norm: Normal) -> dict:
    tokens = Path(job["config"]) / "tokens.json"
    if not tokens.is_file():
        tokens.write_text(json.dumps(TOKENS, indent=2), encoding="utf-8")
    server = job["server"]
    if server["kind"] == "cpp":
        cmd = [server["genko"], "serve", "--root", job["root"], "--port", "0"]
    else:
        cmd = _python_genko(server, ["serve", "--root", job["root"], "--port", "0"])
    for origin in job.get("allow_origins", []):
        cmd += ["--allow-origin", origin]
    running = Running(cmd, _env(job))
    port = running.port
    out = []
    try:
        for req in step:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
            headers = {"Host": f"127.0.0.1:{port}"}
            token = req.get("token", "tok-ai")
            if token:
                headers["Authorization"] = f"Bearer {token}"
            for k, v in (req.get("headers") or {}).items():
                if v is None:
                    headers.pop(k, None)
                else:
                    headers[k] = v.replace("$port", str(port))
            body = b""
            if "json" in req:
                body = json.dumps(req["json"], ensure_ascii=False).replace("$root", job["root"]).encode("utf-8")
                headers.setdefault("Content-Type", "application/json")
            elif "text" in req:
                body = req["text"].replace("$root", job["root"]).encode("utf-8")
            elif "base64" in req:
                body = base64.b64decode(req["base64"])
            path = req["path"].replace("$root", job["root"])
            conn.putrequest(req["method"], path, skip_host=True, skip_accept_encoding=True)
            for k, v in headers.items():
                conn.putheader(k, v)
            if "declare" in req:  # (a size said, nothing sent: the server answers before it reads)
                conn.putheader("Content-Length", str(int(req["declare"])))
            elif req.get("chunked"):  # (http.server knows no chunks: no Content-Length, no body)
                conn.putheader("Transfer-Encoding", "chunked")
            elif body or req["method"] == "POST":
                conn.putheader("Content-Length", str(len(body)))
            conn.endheaders()
            if body:
                conn.send(_chunked(body) if req.get("chunked") else body)
            try:
                resp = conn.getresponse()
            except (ConnectionError, http.client.HTTPException) as exc:
                out.append({"dropped": type(exc).__name__})
                conn.close()
                continue
            data = resp.read()
            kind = (resp.getheader("content-type") or "").split(";")[0].strip()
            record = {"status": resp.status, "type": kind,
                      "cors": resp.getheader("access-control-allow-origin"), "vary": resp.getheader("vary")}
            if kind == "image/png":
                record.update(_png_summary(data))
            elif kind == "application/json":
                record["body"] = norm.value(json.loads(data.decode("utf-8")) if data else None)
            else:
                record["body"] = norm.text(data.decode("utf-8", "replace"))
            out.append(record)
            conn.close()
    finally:
        running.stop()
    return {"api": out}


# --- the job ---------------------------------------------------------------------------------------------------------------


def _write_record(path: Path, record: dict) -> None:
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(record), encoding="utf-8")
    os.replace(temp, path)  # (whole: a server reading it never sees half a record)


class Heartbeat:
    """The "beat" of the job records a step wrote as running, renewed every few seconds for as long as the job runs, as
    the process that runs a job renews it: a record never looks lost (90 s without a beat) however slow the build under
    test is."""

    def __init__(self) -> None:
        self.records: list[tuple[Path, dict]] = []
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def add(self, path: Path, record: dict) -> None:
        with self.lock:
            self.records.append((path, record))

    def _run(self) -> None:
        while not self.stopped.wait(5):
            with self.lock:
                for path, record in self.records:
                    if path.is_file():
                        record["beat"] = time.time()
                        _write_record(path, record)

    def stop(self) -> None:
        self.stopped.set()
        self.thread.join()


HEARTBEAT: Heartbeat


def run_job(job: dict, out_path: str) -> None:
    global HEARTBEAT
    HEARTBEAT = Heartbeat()
    try:
        _run_job(job, out_path)
    finally:
        HEARTBEAT.stop()


def _run_job(job: dict, out_path: str) -> None:
    root = Path(job["root"])
    root.mkdir(parents=True, exist_ok=True)
    Path(job["config"]).mkdir(parents=True, exist_ok=True)
    job["root"] = str(root.resolve())
    job["config"] = str(Path(job["config"]).resolve())
    norm = Normal(job["root"], job["config"])
    records = []
    for step in job["steps"]:
        if "write" in step:
            target = root / step["write"]
            target.parent.mkdir(parents=True, exist_ok=True)
            if "base64" in step:
                target.write_bytes(base64.b64decode(step["base64"]))
            elif "json" in step:
                target.write_text(json.dumps(step["json"], ensure_ascii=False), encoding="utf-8")
            else:
                target.write_text(step["text"], encoding="utf-8")
            if step.get("executable"):
                target.chmod(0o755)
            records.append({"write": step["write"]})
        elif "symlink" in step:
            link = root / step["symlink"]
            link.symlink_to(step["to"], target_is_directory=True)
            records.append({"symlink": step["symlink"]})
        elif "cli" in step:
            server = job["server"]
            args = [a.replace("$root", job["root"]) for a in step["cli"]]
            cmd = [server["genko"], *args] if server["kind"] == "cpp" else _python_genko(server, args)
            done = subprocess.run(cmd, env=_env(job), capture_output=True, timeout=300)
            text = done.stdout.decode("utf-8")
            reply = json.loads(text) if text.strip() else None
            if isinstance(reply, dict):
                reply.pop("code", None)  # (this build's failures also carry a code)
            records.append({"cli": norm.value(reply), "exit": done.returncode})
        elif "job_record" in step:
            now = time.time()
            folder = root / step["job_record"] / "studio" / "jobs"
            folder.mkdir(parents=True, exist_ok=True)
            record = {"id": step["id"], "kind": "export", "actor": job["agent"], "status": step["status"], "started": now, "beat": now,
                      "pid": os.getpid()}
            if "result" in step:
                record.update(finished=now, result=step["result"])
            target = folder / f"{step['id']}.json"
            _write_record(target, record)
            if step["status"] == "running":
                HEARTBEAT.add(target, record)
            records.append({"job_record": step["id"]})
        elif "stdio" in step:
            records.append(_stdio(job, step["stdio"], norm))
        elif "http" in step:
            records.append(_http(job, step["http"], norm))
        elif "client" in step:
            records.append(_client(job, step, norm))
        elif "api" in step:
            records.append(_api(job, step["api"], norm))
        elif "files" in step:
            # what the MCP layer itself keeps in a book: the tool log (tool, ok, codes, args' keys) and the presence
            book = root / step["files"]
            log = []
            for name in ("tools.1.jsonl", "tools.jsonl"):
                path = book / "studio" / "logs" / name
                if path.is_file():
                    for line in path.read_text(encoding="utf-8").split("\n"):
                        if line.strip():
                            entry = json.loads(line)
                            log.append({k: entry.get(k) for k in ("actor", "tool", "args", "ok", "codes", "error") if k in entry})
            presence = book / "studio" / "presence.json"
            actors = sorted(json.loads(presence.read_text(encoding="utf-8")).get("actors", {})) if presence.is_file() else []
            records.append({"files": norm.value({"log": log, "presence": actors})})
        else:
            raise ValueError(f"unknown step {step}")
    Path(out_path).write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")
    folder = Path(out_path + ".images")
    folder.mkdir(exist_ok=True)
    for n, data in enumerate(IMAGES, 1):
        (folder / f"{n:03d}.png").write_bytes(data)


def pyserve(root: str, agent: str) -> None:
    """Python's MCP server (build_server, as `genko mcp` makes it) on Streamable HTTP as MCPServer.run("streamable-http")
    serves it, on a socket bound to a free port first (no race for the port)."""
    import uvicorn

    _basic_layout()
    sys.path.insert(0, str(HERE.parents[1] / "src"))
    from genko.mcp.server import build_server

    Path(root).mkdir(parents=True, exist_ok=True)
    app = build_server(Path(root), agent).streamable_http_app(host="127.0.0.1")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 0))
    sock.listen(128)
    print(f"genko mcp http://127.0.0.1:{sock.getsockname()[1]}/mcp root={root}", file=sys.stderr, flush=True)
    uvicorn.Server(uvicorn.Config(app, log_level="warning")).run(sockets=[sock])


def pygenko(args: list[str]) -> None:
    """Python's genko as `python -m genko <args>` runs it (src/genko on PYTHONPATH), its text laid out with Pillow's
    BASIC layout (the C++ drawing is BASIC's; a Pillow with raqm would otherwise use raqm)."""
    import runpy

    _basic_layout()
    sys.path[0] = os.getcwd()  # (as -m puts it)
    sys.argv = ["genko", *args]
    runpy.run_module("genko", run_name="__main__", alter_sys=True)


def main(argv: list[str]) -> int:
    if argv and argv[0] == "pygenko":
        pygenko(argv[1:])
        return 0
    parser = argparse.ArgumentParser(prog="mcp_harness.py")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("job")
    p.add_argument("out")
    p = sub.add_parser("pyserve")
    p.add_argument("root")
    p.add_argument("agent")
    args = parser.parse_args(argv)
    if args.cmd == "run":
        run_job(json.loads(Path(args.job).read_text(encoding="utf-8")), args.out)
    else:
        pyserve(args.root, args.agent)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
