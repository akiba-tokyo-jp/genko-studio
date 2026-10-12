#!/usr/bin/env python3
"""Writes native/tests/contract/mcp_cases.json: the jobs test_contract_mcp.cpp runs on the C++ build and on Python
(tools/migration/mcp_harness.py run): `genko mcp` over stdio and Streamable HTTP, its protocol (raw JSON-RPC and HTTP)
and the MCP SDK's own Client on a small book (the 41 tools listed, the 4 resources read, tools that work, fail, are
cancelled, send pictures and run background jobs, the refusals an agent meets), and the HTTP API `genko serve`.
Development only.

    python3 tools/migration/make_mcp_cases.py
"""
from __future__ import annotations

import base64
import io
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = REPO / "native" / "tests" / "contract" / "mcp_cases.json"
DEMO = REPO / "tests" / "fixtures" / "studio" / "demo4"

V = "2026-07-28"
META = {"io.modelcontextprotocol/protocolVersion": V, "io.modelcontextprotocol/clientCapabilities": {},
        "io.modelcontextprotocol/clientInfo": {"name": "harness", "version": "1"}}
INIT = {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "harness", "version": "1"}}
BOOK = "book.genko"


def req(i, method, params=None):
    out = {"jsonrpc": "2.0", "id": i, "method": method}
    if params is not None:
        out["params"] = params
    return out


def note(method, params=None):
    out = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        out["params"] = params
    return out


def call(i, name, args=None, meta=False):
    params = {"name": name, "arguments": args or {}}
    if meta:
        params["_meta"] = META
    return req(i, "tools/call", params)


def fixture(name: str):
    return json.loads((DEMO / name).read_text(encoding="utf-8"))


def png(size=(24, 16), color=(200, 40, 40)) -> str:
    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGB", size, color).save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def stdio_legacy() -> dict:
    """The handshake era over stdio: what is answered before initialize, the handshake at each version, the params the
    SDK refuses, methods it does not serve, the 2026 envelope refused, tools that fail their arguments (pydantic's
    words), lines that are not messages (dropped), invalid UTF-8 (replaced), lines ended by \\r."""
    lines = [
        req(0, "tools/list"), req("p", "ping"),
        req(1, "initialize", {**INIT, "protocolVersion": "1999-01-01"}),
        note("notifications/initialized"),
        req(2, "tools/list", {"cursor": "x"}), req(3, "resources/read", {"uri": "genko://guide/skill"}),
        req(4, "prompts/get", {"name": "nope"}), req(5, "tools/call", {"name": 5}),
        req(6, "tools/call", {"name": "projects", "arguments": [1]}), req(7, "resources/read", {}),
        req(8, "server/discover", {}), req(9, "initialize", {**INIT, "capabilities": "x"}),
        req(10, "tools/call", {"name": "projects", "_meta": {"progressToken": "t1"}}),
        req(11, "resources/subscribe", {"uri": "x"}), req(12, "logging/setLevel", {"level": "info"}),
        req(13, "tools/call", {"name": "projects", "_meta": META}), req(14, "ping", {"_meta": "x"}),
        call(15, "status"),
        call(16, "render", {"project": "x", "page": "abc", "max_px": 3.5, "frame_id": 7, "session": "a/b"}),
        call(17, "render", {"project": "x", "page": "3", "session": "bad session"}),
        call(18, "check", {"project": "x", "pages": "[1,\"2\",true, 3.0]"}),
        call(19, "next", {"project": "a" * 72, "limit": "a" * 80, "claim": "maybe"}),
        call(20, "create_project", {"name": BOOK, "title": "題", "pages": 2, "spec_preset": "doujin-a5"}),
        {"await": [20]},
        call(21, "status", {"project": BOOK}),
        call(22, "nosuch", {}), req(23, "resources/list"), req(24, "resources/read", {"uri": "genko://ops"}),
        req(25, "resources/read", {"uri": "genko://nope"}), req(26, "resources/templates/list"), req(27, "prompts/list"),
        {"raw": "{not json\n"}, {"raw": "\n"}, {"raw": "[1, 2]\n"},
        {"raw": '{"jsonrpc": "2.0", "id": 28, "method": "tools/call", "params": {"name": "status", "arguments": {"project": "\xff\xfe"}}}\r'},
        {"raw": '{"jsonrpc": "1.0", "id": 29, "method": "ping"}\n'},
        {"raw": '{"jsonrpc": "2.0", "id": 30.0, "method": "ping"}\n'},
        {"raw": '{"jsonrpc": "2.0", "id": true, "method": "ping"}\n'},
        {"raw": '{"jsonrpc": "2.0", "id": 31, "method": "ping", "params": [1]}\n'},
        {"raw": '{"jsonrpc": "2.0", "id": 32, "result": {}}\n'},
        req(33, "foo/bar"),
        call(34, "inspect", {"project": BOOK, "target": "page", "page": 1}),
        call(35, "next", {"project": BOOK, "limit": True, "claim": 1}),
        call(36, "export_status", {"project": BOOK, "job": "job_none", "wait_s": "0"}),
        call(37, "adopt", {"project": BOOK, "candidate_id": "c", "offset_mm": [1, "2", "x"], "scale": "2"}),
        call(38, "adopt", {"project": BOOK, "candidate_id": "c", "offset_mm": "[1.5, \"2\"]", "upscale": "no"}),
        call(39, "next", {"project": BOOK, "limit": "12_3"}),
        call(40, "check", {"project": BOOK, "pages": "null"}),
        call(41, "use_style", {"project": BOOK}),
        call(42, "apply_ops", {"project": BOOK, "ops": [{"op": "approve", "gate": "name", "page": 1}], "commit": True}),
    ]
    # every request that is answered (28 too; not 29–32: lines the SDK drops), so none is still running when input ends
    answered = [m["id"] for m in lines if isinstance(m, dict) and "method" in m and "id" in m] + [28]
    return {"name": "stdio_legacy", "steps": [{"stdio": {"lines": lines, "wait": answered}}]}


def stdio_modern() -> dict:
    """The 2026 era over stdio: server/discover and each request's envelope, its refusals, its results' shape."""
    m = {"_meta": META}
    lines = [
        note("notifications/initialized"),
        req(1, "server/discover", m), req(2, "tools/list", m), req(3, "resources/list", m),
        req(4, "resources/read", {"uri": "genko://guide/manga-rules", **m}), call(5, "projects", {}, meta=True),
        req(6, "ping", m), req(7, "tools/list"), req(8, "initialize", INIT),
        req(9, "resources/read", {"uri": "genko://nope", **m}),
        req(10, "tools/call", {"name": "projects", "_meta": {**META, "io.modelcontextprotocol/protocolVersion": "2099-01-01"}}),
        req(11, "resources/templates/list", m), req(12, "prompts/list", m), req(13, "prompts/get", {"name": "x", **m}),
        req(14, "tools/call", {"name": "projects", "_meta": {"io.modelcontextprotocol/protocolVersion": V}}),
        req(15, "tools/call", {"name": "projects", "_meta": {**META, "io.modelcontextprotocol/protocolVersion": 5}}),
        req(16, "tools/call", {"name": "projects", "_meta": {**META, "io.modelcontextprotocol/clientCapabilities": 5}}),
        req(17, "tools/call", {"name": "projects", "_meta": {**META, "io.modelcontextprotocol/clientInfo": 5}}),
        req(18, "logging/setLevel", {"level": "info", **m}), req(19, "foo/bar", m),
        call(20, "status", {"project": "x"}, meta=True), req(21, "tools/call", {"name": 5, **m}),
        req(22, "tools/list", {"cursor": 5, **m}),
        req(23, "tools/call", {"name": "projects", "_meta": {**META, "progressToken": 1.5}}),
        call(24, "create_project", {"name": BOOK, "title": "t", "pages": 1, "spec_preset": "doujin-a5"}, meta=True),
        {"await": [24]},
        call(25, "render", {"project": BOOK, "page": 1, "max_px": 120}, meta=True),
    ]
    answered = [m["id"] for m in lines if isinstance(m, dict) and "method" in m and "id" in m]
    return {"name": "stdio_modern", "steps": [{"stdio": {"lines": lines, "wait": answered}}]}


def stdio_cancel() -> dict:
    """notifications/cancelled: the request is never answered, the server goes on answering; at the end of input a
    request still running finishes and is not answered."""
    lines = [
        req(1, "initialize", INIT), note("notifications/initialized"),
        call(2, "export_status", {"project": BOOK, "job": "job_slow", "wait_s": 3}),
        {"sleep": 0.5}, note("notifications/cancelled", {"requestId": 2, "reason": "user"}),
        req(3, "ping"), note("notifications/cancelled", {"requestId": 99}),
        call(4, "export_status", {"project": BOOK, "job": "job_slow", "wait_s": 1.5}),
    ]
    return {"name": "stdio_cancel", "steps": [
        {"write": f"{BOOK}/.keep", "text": ""},
        {"stdio": {"lines": [req(1, "initialize", INIT), call(2, "create_project", {"name": BOOK, "title": "t", "pages": 1,
                                                                                     "spec_preset": "doujin-a5"})], "wait": [2]}},
        {"job_record": BOOK, "id": "job_slow", "status": "running"},
        {"stdio": {"lines": lines, "wait": [3], "silent": [2, 4]}},
    ]}


def client_ops_first() -> list:
    bible, script = fixture("bible.json"), fixture("script.json")
    for scene in script["scenes"]:  # (a book of two pages)
        scene["beats"] = [b for b in scene["beats"] if b.get("page", 1) <= 2]
    plans = [fixture("p001.json"), fixture("p002.json")]
    return [
        {"op": "info"}, {"op": "list_tools"}, {"op": "list_resources"},
        *({"op": "read_resource", "uri": u} for u in ("genko://ops", "genko://guide/skill", "genko://guide/drawing",
                                                    "genko://guide/manga-rules")),
        {"op": "ping"},
        {"op": "call", "name": "projects"},
        {"op": "call", "name": "create_project", "args": {"name": BOOK, "title": "試験", "pages": 2, "spec_preset": "doujin-a5"}},
        {"op": "call", "name": "status", "args": {"project": BOOK}},
        {"op": "call", "name": "set_bible", "args": {"project": BOOK, "bible": bible, "commit": True}},
        {"op": "call", "name": "set_script", "args": {"project": BOOK, "script": script, "commit": True}},
        {"op": "call", "name": "submit_name", "args": {"project": BOOK, "plan": plans[0]}},
        {"op": "call", "name": "submit_name", "args": {"project": BOOK, "plan": plans[0], "commit": True}},
        {"op": "call", "name": "submit_name", "args": {"project": BOOK, "plan": plans[1], "commit": True}},
        {"op": "call", "name": "next", "args": {"project": BOOK, "limit": 2}},
        {"op": "call", "name": "render", "args": {"project": BOOK, "page": 1, "max_px": 300}},
        {"op": "call", "name": "render", "args": {"project": BOOK, "page": 2, "mode": "print", "max_px": 200}},
        {"op": "call", "name": "inspect", "args": {"project": BOOK, "target": "snapshot", "page": 1}},
        {"op": "call", "name": "check", "args": {"project": BOOK}},
        {"op": "call", "name": "preflight", "args": {"project": BOOK}},
        {"op": "call", "name": "tickets", "args": {"project": BOOK, "status": "all"}},
    ]


def client_ops_second() -> list:
    return [
        {"op": "info"},
        # the refusals an agent meets: a gate, a lock, the person's switch for chat approvals, another
        # conversation's change and a person's (each conversation's first write is held back once: book_in_use)
        {"op": "call", "name": "apply_ops", "args": {"project": BOOK, "ops": [{"op": "approve", "gate": "name", "page": 1}], "commit": True}},
        {"op": "call", "name": "apply_ops", "args": {"project": BOOK, "ops": [{"op": "lock_page", "page": 1}], "commit": True}},
        {"op": "call", "name": "apply_ops", "args": {"project": BOOK, "ops": [{"op": "allow_chat_approval", "on": True}], "commit": True}},
        {"op": "call", "name": "record_chat_approval", "args": {"project": BOOK, "gate": "name", "message": "OK", "pages": [1],
                                                                "person": "owner"}},
        {"op": "call", "name": "request_approval", "args": {"project": BOOK, "pages": [1, 2], "gate": "name", "note": "見てください"}},
        *[{"op": "call", "name": "apply_ops", "args": {"project": BOOK, "ops": [{"op": "set_note", "page": 1, "note": "s1 のメモ"}],
                                                       "commit": True, "session": "s1"}} for _ in range(2)],
        # failures: the arguments, the book, the tool
        {"op": "call", "name": "status", "args": {"project": "nope.genko"}},
        {"op": "call", "name": "render", "args": {"project": BOOK, "page": "abc"}},
        {"op": "call", "name": "nosuch"},
        {"op": "call", "name": "status", "args": {"project": BOOK, "session": "a b"}},
        # jobs: a background export and its record
        {"op": "call", "name": "export", "args": {"project": BOOK, "format": "png", "pages": [1], "dpi": 72}},
    ]


def client_ops_jobs() -> list:
    """(A conversation of its own, started right after the job records are written: a "running" record stays alive
    for 90 s after its last beat, however slow the build under test is.)"""
    return [
        # a finished job's record, one that runs, one that is not there
        {"op": "call", "name": "export_status", "args": {"project": BOOK, "job": "job_done", "wait_s": 0}},
        {"op": "call", "name": "export_status", "args": {"project": BOOK, "job": "job_slow", "wait_s": 0}},
        {"op": "call", "name": "export_status", "args": {"project": BOOK, "job": "job_none"}},
        # cancelled: a call given up after a moment (the server answers what follows)
        {"op": "cancel", "name": "export_status", "args": {"project": BOOK, "job": "job_slow", "wait_s": 3}, "after": 0.5},
        {"op": "call", "name": "projects"},
        # Undo last (the replies after one that goes through are compared with their keys sorted: mcp_harness.Normal)
        *[{"op": "call", "name": "undo", "args": {"project": BOOK, "session": "s2"}} for _ in range(2)],
        *[{"op": "call", "name": "undo", "args": {"project": BOOK, "session": "s1"}} for _ in range(2)],
        {"op": "call", "name": "undo", "args": {"project": BOOK}},
    ]


def client_job(transport: str, mode: str) -> dict:
    return {"name": f"client_{transport}_{mode}", "steps": [
        {"client": {"transport": transport, "mode": mode, "ops": client_ops_first()}},
        {"cli": ["studio", "chat-approval", f"$root/{BOOK}", "off", "--as", "human:owner"]},
        {"client": {"transport": transport, "mode": mode, "ops": client_ops_second()}},
        {"job_record": BOOK, "id": "job_slow", "status": "running"},
        {"job_record": BOOK, "id": "job_done", "status": "done", "result": {"ok": True, "files": ["a.png"], "issues": []}},
        {"client": {"transport": transport, "mode": mode, "ops": client_ops_jobs()}},
        # a person's change last: an agent's undo is refused (and the ticket it made is the agent's to resolve only
        # when it is a fix for the agent)
        {"cli": ["studio", "comment", f"$root/{BOOK}", "セリフを短く", "--page", "1", "--as", "human:owner"]},
        {"client": {"transport": transport, "mode": mode, "ops": [
            {"op": "call", "name": "undo", "args": {"project": BOOK}},
            {"op": "call", "name": "tickets", "args": {"project": BOOK}},
            {"op": "call", "name": "undo", "args": {"project": BOOK}},
        ]}},
        {"files": BOOK},
    ]}


def http_raw() -> dict:
    """Streamable HTTP as the SDK serves it: the handshake era's sessions (POST answered on SSE, 202, GET stream,
    DELETE), its refusals (Accept, Content-Type, session, Host, Origin, size, method), and the 2026 era's single POSTs
    (JSON answers with the error's HTTP status, the routing headers, notifications)."""
    j = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    modern = {**j, "MCP-Protocol-Version": V}
    return {"name": "http_raw", "steps": [{"http": [
        {"method": "POST", "headers": j, "body": req(1, "initialize", INIT), "session": "keep"},
        {"method": "POST", "headers": j, "body": note("notifications/initialized"), "session": "use"},
        {"method": "POST", "headers": j, "body": req(2, "tools/list"), "session": "use"},
        {"method": "POST", "headers": j, "body": call(3, "projects"), "session": "use"},
        {"method": "POST", "headers": j, "body": call(4, "status", {"project": "x"}), "session": "use"},
        {"method": "POST", "headers": j, "body": req(5, "ping")},
        {"method": "POST", "headers": {**j, "Mcp-Session-Id": "nope"}, "body": req(6, "ping")},
        {"method": "POST", "headers": {**j, "Accept": "application/json"}, "body": req(7, "ping"), "session": "use"},
        {"method": "POST", "headers": {**j, "Content-Type": "text/plain"}, "body": req(8, "ping"), "session": "use"},
        {"method": "POST", "headers": {**j, "Host": "evil.example:80"}, "body": req(9, "ping"), "session": "use"},
        {"method": "POST", "headers": {**j, "Origin": "http://evil.example"}, "body": req(10, "ping"), "session": "use"},
        {"method": "POST", "headers": {**j, "Origin": "http://localhost:3000"}, "body": req(11, "ping"), "session": "use"},
        {"method": "POST", "headers": j, "declare": 5 * 1024 * 1024, "session": "use"},
        {"method": "PUT", "headers": j, "body": req(12, "ping"), "session": "use"},
        {"method": "GET", "headers": {"Accept": "text/event-stream"}, "session": "use", "stream": "head", "hold": True},
        {"method": "GET", "headers": {"Accept": "text/event-stream"}, "session": "use"},
        {"method": "GET", "headers": {"Accept": "application/json"}, "session": "use", "release": True},
        {"method": "POST", "headers": j, "body": req(13, "resources/read", {"uri": "genko://nope"}), "session": "use"},
        {"method": "POST", "path": "/other", "headers": j, "body": req(14, "ping"), "session": "use"},
        {"method": "DELETE", "headers": {}, "session": "use"},
        {"method": "POST", "headers": j, "body": req(15, "ping"), "session": "use"},
        # the 2026 era
        {"method": "POST", "headers": {**modern, "Mcp-Method": "server/discover"}, "body": req(20, "server/discover", {"_meta": META})},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "tools/call", "Mcp-Name": "projects"},
         "body": call(21, "projects", {}, meta=True)},
        {"method": "POST", "headers": modern, "body": req(22, "tools/list", {"_meta": META})},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "foo/bar"}, "body": req(23, "foo/bar", {"_meta": META})},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "tools/call", "Mcp-Name": "other"},
         "body": call(24, "projects", {}, meta=True)},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "notifications/initialized"}, "body": note("notifications/initialized")},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "tools/list"}, "raw": "{not json"},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "tools/list"}, "body": [req(25, "tools/list", {"_meta": META})]},
        {"method": "GET", "headers": modern},
        {"method": "POST", "headers": {**modern, "Accept": "application/json"}, "body": req(26, "tools/list", {"_meta": META})},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "resources/read", "Mcp-Name": "genko://nope"},
         "body": req(27, "resources/read", {"uri": "genko://nope", "_meta": META})},
        {"method": "POST", "headers": {**j, "MCP-Protocol-Version": "2099-01-01", "Mcp-Method": "tools/list"},
         "body": req(28, "tools/list", {"_meta": {**META, "io.modelcontextprotocol/protocolVersion": "2099-01-01"}})},
        {"method": "POST", "headers": {**modern, "Mcp-Method": "ping"}, "body": req(29, "ping", {"_meta": META})},
    ]}]}


def api_job() -> dict:
    """`genko serve`: the guard (Host, Origin, Content-Type, the token), the routes and their refusals (paths outside
    the root, links, a request's files outside its folder, bad numbers, bad JSON, size), a book made, changed (as an
    agent: the gates hold), drawn and written out; uploads that are not pictures."""
    img = png()
    return {"name": "api", "allow_origins": ["http://ok.example"], "steps": [
        {"write": "../secret/secret.png", "base64": img},
        {"symlink": "link_out", "to": "../secret"},
        # a generation request's files: names that climb out of its folder, and a link in it to a file outside the root
        {"write": "api.genko/studio/requests/rq_t/request.json", "text": "{}"},
        {"write": "api.genko/studio/requests/rq_t/guide.png", "base64": img},
        {"write": "api.genko/studio/private.txt", "text": "not a request's"},
        {"symlink": "api.genko/studio/requests/rq_t/out.png", "to": "../../../../../secret/secret.png"},
        {"api": [
            {"method": "GET", "path": "/health", "token": None},
            {"method": "GET", "path": "/schema", "token": None},
            {"method": "GET", "path": "/schema", "token": "nope"},
            {"method": "GET", "path": "/schema", "headers": {"Authorization": "bearer tok-ai"}, "token": None},
            {"method": "GET", "path": "/openapi.json"},
            {"method": "GET", "path": "/health", "token": None, "headers": {"Host": "evil.example"}},
            {"method": "GET", "path": "/health", "token": None, "headers": {"Host": None}},
            {"method": "GET", "path": "/health", "token": None, "headers": {"Origin": "http://evil.example"}},
            {"method": "GET", "path": "/health", "token": None, "headers": {"Origin": "http://ok.example"}},
            {"method": "OPTIONS", "path": "/v1/new", "token": None, "headers": {"Origin": "http://ok.example"}},
            {"method": "OPTIONS", "path": "/v1/new", "token": None, "headers": {"Origin": "http://evil.example"}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "api.genko", "title": "API", "pages": 2}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "../escape.genko"}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "/etc/escape.genko"}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "a%2F..%2F..%2Fescape.genko"}},
            {"method": "POST", "path": "/v1/new", "json": {"title": "no dest"}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "n.genko", "pages": "x"}},
            {"method": "POST", "path": "/v1/new", "text": "{not json", "headers": {"Content-Type": "application/json"}},
            {"method": "POST", "path": "/v1/new", "text": "[1]", "headers": {"Content-Type": "application/json"}},
            {"method": "POST", "path": "/v1/new", "json": {"dest": "x.genko"}, "headers": {"Content-Type": "text/plain"}},
            {"method": "POST", "path": "/v1/new", "declare": 64 * 1024 * 1024 + 1, "headers": {"Content-Type": "application/json"}},
            {"method": "GET", "path": "/v1/inspect?path=api.genko"},
            {"method": "GET", "path": "/v1/inspect?path=api.genko&full=1"},
            {"method": "GET", "path": "/v1/inspect?path=nope.genko"},
            {"method": "GET", "path": "/v1/inspect?path=..%2F"},
            {"method": "GET", "path": "/v1/inspect"},
            {"method": "GET", "path": "/v1/inspect?path=link_out"},
            {"method": "POST", "path": "/v1/assets?path=link_out", "base64": img, "headers": {"Content-Type": "image/png"}},
            {"method": "POST", "path": "/v1/export", "json": {"path": "api.genko", "out": "link_out/x"}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "add_line", "page": 1, "text": "こんにちは",
                                                                                          "x_mm": 30, "y_mm": 40}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "set_note", "page": 1, "text": "x"}],
                                                             "dry_run": True}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "set_note", "page": 1, "text": "x"}],
                                                             "expect_revision": 99}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "name_ok", "page": 1}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "allow_chat_approval", "on": False}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "lock_page", "page": 2}]}, "token": "tok-human"},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "set_note", "page": 2, "text": "ai"}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "unlock_page", "page": 2}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "undo"}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "put_raster", "page": 1, "path": "/etc/passwd"}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "set_meta", "font_path": "../x.ttf"}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": [{"op": "put_raster", "page": 1, "path": "link_out/secret.png"}]}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": "abc"}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko", "ops": 5}},
            {"method": "POST", "path": "/v1/apply", "json": {"path": "api.genko"}},
            {"method": "GET", "path": "/v1/pages/1.png?path=api.genko&dpi=40"},
            {"method": "GET", "path": "/v1/pages/1.png?path=api.genko&dpi=4000"},
            {"method": "GET", "path": "/v1/pages/9.png?path=api.genko&dpi=40"},
            {"method": "GET", "path": "/v1/pages/x.png?path=api.genko"},
            {"method": "GET", "path": "/v1/spreads/1-2.png?path=api.genko&dpi=36"},
            {"method": "GET", "path": "/v1/spreads/1.png?path=api.genko"},
            {"method": "GET", "path": "/v1/pages/1/frames/nope.png?path=api.genko&dpi=40"},
            {"method": "GET", "path": "/v1/pages/7/frames/nope.png?path=api.genko&dpi=40"},
            {"method": "POST", "path": "/v1/export", "json": {"path": "api.genko", "out": "out1", "format": "png", "dpi": 40}},
            {"method": "POST", "path": "/v1/export", "json": {"path": "api.genko", "out": "out2", "format": "gif"}},
            {"method": "POST", "path": "/v1/export", "json": {"path": "api.genko", "out": "../out3"}},
            {"method": "POST", "path": "/v1/assets?path=api.genko", "base64": img, "headers": {"Content-Type": "image/png"}},
            {"method": "POST", "path": "/v1/assets?path=api.genko", "text": "PK\u0003\u0004 not an image", "headers": {"Content-Type": "image/png"}},
            {"method": "POST", "path": "/v1/assets?path=api.genko", "base64": img, "headers": {"Content-Type": "application/json"}},
            {"method": "POST", "path": "/v1/assets?path=nope.genko", "base64": img, "headers": {"Content-Type": "image/png"}},
            {"method": "POST", "path": "/v1/assets?path=..%2Fx", "base64": img, "headers": {"Content-Type": "image/png"}},
            {"method": "GET", "path": "/v1/requests/rq_x/files/a.png?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/guide.png?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/..%2F..%2Fprivate.txt?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/../../private.txt?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/%2E%2E/request.json?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/out.png?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/..%2Frq_t/files/guide.png?path=api.genko"},
            {"method": "GET", "path": "/v1/requests/rq_t/files/guide.png?path=link_out"},
            {"method": "GET", "path": "/v1/jobs/nope"},
            {"method": "GET", "path": "/v1/nope"},
            {"method": "DELETE", "path": "/v1/new"},
        ]},
    ]}


def main() -> None:
    jobs = [stdio_legacy(), stdio_modern(), stdio_cancel(), http_raw(), api_job(),
            client_job("stdio", "auto"), client_job("stdio", "legacy"), client_job("http", "auto"), client_job("http", "legacy")]
    OUT.write_text(json.dumps(jobs, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{OUT}: {len(jobs)} jobs")


if __name__ == "__main__":
    main()
