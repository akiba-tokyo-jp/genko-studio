"""Headless HTTP API.

Every route except /health needs `Authorization: Bearer <token>`; the token
decides the actor (tokens.json in the config dir, see genko.tokens). Requests
from browsers are refused unless their Origin is allowed, the Host header must
name this server (DNS rebinding), bodies must be JSON, and every path in a
request must stay inside --root.
"""

from __future__ import annotations

import io
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from genko.headless import OPS_SCHEMA, ApplyError, apply_ops, snapshot
from genko.io import load_episode, save_episode
from genko.migrate import UnsupportedProjectVersion
from genko.models import PageSpec, new_episode

JOBS: dict[str, dict[str, Any]] = {}
MAX_BODY = 64 * 1024 * 1024
PUBLIC_ROUTES = ("/health",)


class Forbidden(Exception):
    pass


def openapi_spec() -> dict[str, Any]:
    return {
        "openapi": "3.0.3",
        "info": {"title": "Genko Headless", "version": "0.3.0"},
        "components": {"securitySchemes": {"bearer": {"type": "http", "scheme": "bearer"}}},
        "security": [{"bearer": []}],
        "paths": {
            "/health": {"get": {"security": [], "responses": {"200": {"description": "ok"}}}},
            "/schema": {"get": {"responses": {"200": {"description": "ops"}}}},
            "/openapi.json": {"get": {"responses": {"200": {"description": "spec"}}}},
            "/v1/inspect": {"get": {"parameters": [{"name": "path", "in": "query", "required": True}]}},
            "/v1/pages/{n}.png": {"get": {"parameters": [{"name": "path", "in": "query"}]}},
            "/v1/new": {"post": {"requestBody": {"required": True}}},
            "/v1/apply": {"post": {"requestBody": {"required": True}}},
            "/v1/export": {"post": {"requestBody": {"required": True}}},
            "/v1/jobs/{id}": {"get": {"parameters": [{"name": "id", "in": "path"}]}},
        },
    }


def _json_bytes(payload: dict[str, Any], status: int = 200) -> tuple[int, bytes]:
    return status, json.dumps(payload, ensure_ascii=False).encode("utf-8")


def confine(ctx: dict | None, raw: str) -> Path:
    """Resolve a request path inside ctx['root'] (relative paths are taken from root)."""
    if not ctx or not ctx.get("root"):
        raise Forbidden("paths need a server started with --root")
    root = Path(ctx["root"]).resolve()
    text = unquote(str(raw or ""))
    if not text:
        raise Forbidden("path is required")
    candidate = Path(text)
    resolved = (candidate if candidate.is_absolute() else root / candidate).resolve()
    if resolved != root and root not in resolved.parents:
        raise Forbidden(f"path outside --root: {text}")
    return resolved


def _confine_ops(ctx: dict | None, ops: list) -> list:
    """Local file paths inside ops (put_raster path, set_meta font_path) must stay in root too."""
    out = []
    for op in ops:
        if isinstance(op, dict):
            op = dict(op)
            if op.get("path"):
                op["path"] = str(confine(ctx, op["path"]))
            if op.get("font_path"):
                op["font_path"] = str(confine(ctx, op["font_path"]))
        out.append(op)
    return out


def handle_request(method: str, path: str, body: bytes, ctx: dict | None = None) -> tuple[int, bytes]:
    """Route one request. `ctx` ({root, actor}) comes from the HTTP handler after authentication;
    without it only the routes that touch no files answer."""
    parsed = urlparse(path)
    route = parsed.path.rstrip("/") or "/"
    query = {key: values[-1] for key, values in parse_qs(parsed.query).items()}
    data: dict[str, Any] = {}
    if body:
        try:
            data = json.loads(body.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return _json_bytes({"ok": False, "error": "invalid JSON body"}, 400)
        if not isinstance(data, dict):
            return _json_bytes({"ok": False, "error": "body must be a JSON object"}, 400)
    actor = (ctx or {}).get("actor", "genko")

    try:
        if method == "GET" and route == "/health":
            return _json_bytes({"ok": True, "service": "genko-headless"})
        if method == "GET" and route == "/schema":
            return _json_bytes({"ok": True, "ops": OPS_SCHEMA})
        if method == "GET" and route == "/openapi.json":
            return _json_bytes(openapi_spec())
        if method == "GET" and route.startswith("/v1/jobs/"):
            job_id = route.rsplit("/", 1)[-1]
            job = JOBS.get(job_id)
            if job is None:
                return _json_bytes({"ok": False, "error": "unknown job"}, 404)
            return _json_bytes(job)
        if method == "GET" and route == "/v1/inspect":
            episode = load_episode(confine(ctx, query.get("path", "")))
            full = query.get("full") in ("1", "true", "yes")
            return _json_bytes(snapshot(episode, full=full))
        if method == "GET" and route.startswith("/v1/pages/") and route.endswith(".png"):
            from genko.render import render_page

            episode = load_episode(confine(ctx, query.get("path", "")))
            page_no = int(route.rsplit("/", 1)[-1].removesuffix(".png"))
            page = next(item for item in episode.pages if item.index == page_no)
            mode = query.get("mode", "print")
            image = render_page(page, _dpi(query.get("dpi", "150")), mode=mode, episode=episode)
            buf = io.BytesIO()
            image.save(buf, format="PNG")
            return 200, buf.getvalue()
        if method == "GET" and route.startswith("/v1/spreads/") and route.endswith(".png"):
            from genko.render import render_spread

            episode = load_episode(confine(ctx, query.get("path", "")))
            pair = route.rsplit("/", 1)[-1].removesuffix(".png")
            left_s, right_s = pair.split("-")
            image = render_spread(episode, int(left_s), int(right_s), dpi=_dpi(query.get("dpi", "150")), mode=query.get("mode", "print"))
            buf = io.BytesIO()
            image.save(buf, format="PNG")
            return 200, buf.getvalue()
        if method == "POST" and route == "/v1/new":
            dest = confine(ctx, data["dest"])
            spec = PageSpec.webtoon() if data.get("webtoon") else PageSpec.a4_mono()
            episode = new_episode(
                str(data.get("title", "無題")),
                int(data.get("episode", 1)),
                int(data.get("pages", 8)),
                spec,
            )
            save_episode(episode, dest)
            return _json_bytes({"ok": True, "path": str(dest), "snapshot": snapshot(episode)})
        if method == "POST" and route == "/v1/apply":
            from genko.lock import ProjectLock

            project = confine(ctx, data["path"])
            ops = _confine_ops(ctx, data.get("ops") or [])
            dry_run = bool(data.get("dry_run"))
            with ProjectLock(project, agent=actor):
                episode = load_episode(project)
                expected = data.get("expect_revision")
                if expected is not None and episode.revision != int(expected):
                    return _json_bytes({"ok": False, "error": f"revision conflict: expected {expected}, found {episode.revision}"}, 409)
                result = apply_ops(episode, ops, dry_run=dry_run, agent=actor)
                if not dry_run:
                    save_episode(episode, project)
            if result.get("job_id"):
                JOBS[str(result["job_id"])] = result
            return _json_bytes(result)
        if method == "POST" and route == "/v1/export":
            from genko.export import export_print

            project = confine(ctx, data["path"])
            out = confine(ctx, data["out"])
            fmt = str(data.get("format", "png"))
            if fmt not in ("png", "tiff", "pdf"):
                return _json_bytes({"ok": False, "error": "format must be png, tiff or pdf"}, 400)
            episode = load_episode(project)
            dpi = _dpi(data.get("dpi") or episode.spec.dpi)
            paths = export_print(episode, out, fmt=fmt, dpi=dpi)
            return _json_bytes({"ok": True, "count": len(paths), "files": [str(p) for p in paths]})
    except Forbidden as exc:
        return _json_bytes({"ok": False, "error": str(exc)}, 403)
    except UnsupportedProjectVersion as exc:
        return _json_bytes({"ok": False, "error": str(exc)}, 409)
    except ApplyError as exc:
        status = 409 if "locked" in str(exc).lower() else 400
        return _json_bytes({"ok": False, "error": str(exc)}, status)
    except FileNotFoundError as exc:
        return _json_bytes({"ok": False, "error": f"not found: {exc}"}, 404)
    except (KeyError, TypeError, ValueError, StopIteration) as exc:
        return _json_bytes({"ok": False, "error": str(exc)}, 400)

    return _json_bytes({"ok": False, "error": f"no route {method} {route}"}, 404)


def _dpi(value: Any) -> int:
    dpi = int(value)
    if not 36 <= dpi <= 1200:
        raise ValueError("dpi must be 36-1200")
    return dpi


class _Handler(BaseHTTPRequestHandler):
    server: HeadlessServer

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        return

    def _send(self, status: int, payload: bytes) -> None:
        content_type = "image/png" if payload.startswith(b"\x89PNG") else "application/json; charset=utf-8"
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        origin = self.headers.get("Origin")
        if origin and origin in self.server.allow_origins:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _guard(self, route: str, has_body: bool) -> tuple[int, str] | dict:
        host = (self.headers.get("Host") or "").lower()
        if host not in self.server.allowed_hosts():
            return 421, "unexpected Host header"
        origin = self.headers.get("Origin")
        if origin and origin not in self.server.allow_origins:
            return 403, "origin not allowed"
        if has_body:
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if ctype != "application/json":
                return 415, "Content-Type must be application/json"
        if route in PUBLIC_ROUTES:
            return {"root": self.server.root, "actor": "anonymous"}
        auth = self.headers.get("Authorization") or ""
        token = auth[7:].strip() if auth.lower().startswith("bearer ") else ""
        actor = self.server.tokens.get(token) if token else None
        if not actor:
            return 401, "Authorization: Bearer <token> is required"
        return {"root": self.server.root, "actor": actor}

    def _serve(self, method: str, body: bytes) -> None:
        route = urlparse(self.path).path.rstrip("/") or "/"
        ctx = self._guard(route, bool(body))
        if isinstance(ctx, tuple):
            status, message = ctx
            self._send(*_json_bytes({"ok": False, "error": message}, status))
            return
        status, payload = handle_request(method, self.path, body, ctx)
        self._send(status, payload)

    def do_OPTIONS(self) -> None:  # noqa: N802
        origin = self.headers.get("Origin")
        if not origin or origin not in self.server.allow_origins:
            self._send(*_json_bytes({"ok": False, "error": "origin not allowed"}, 403))
            return
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", origin)
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
        self.send_header("Vary", "Origin")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        self._serve("GET", b"")

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length > MAX_BODY:
            self._send(*_json_bytes({"ok": False, "error": "body too large"}, 413))
            return
        body = self.rfile.read(length) if length else b""
        self._serve("POST", body)


class HeadlessServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 8765,
        *,
        root: Path,
        tokens: dict[str, str],
        allow_origins: tuple[str, ...] = (),
        allow_hosts: tuple[str, ...] = (),
    ) -> None:
        self.root = Path(root).resolve()
        self.tokens = dict(tokens)
        self.allow_origins = tuple(allow_origins)
        self.extra_hosts = tuple(h.lower() for h in allow_hosts)
        super().__init__((host, port), _Handler)

    @property
    def port(self) -> int:
        return int(self.server_address[1])

    def allowed_hosts(self) -> set[str]:
        port = self.port
        names = {"127.0.0.1", "localhost", "[::1]", *self.extra_hosts}
        return {f"{name}:{port}" for name in names} | set(self.extra_hosts)
