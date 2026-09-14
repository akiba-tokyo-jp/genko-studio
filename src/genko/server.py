from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from genko.export import export_png_sequence
from genko.headless import OPS_SCHEMA, ApplyError, apply_ops, snapshot
from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode


def _json_bytes(payload: dict[str, Any], status: int = 200) -> tuple[int, bytes]:
    return status, json.dumps(payload, ensure_ascii=False).encode("utf-8")


def handle_request(method: str, path: str, body: bytes) -> tuple[int, bytes]:
    parsed = urlparse(path)
    route = parsed.path.rstrip("/") or "/"
    query = {key: values[-1] for key, values in parse_qs(parsed.query).items()}
    data: dict[str, Any] = {}
    if body:
        try:
            data = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            return _json_bytes({"ok": False, "error": "invalid JSON body"}, 400)

    try:
        if method == "GET" and route == "/health":
            return _json_bytes({"ok": True, "service": "genko-headless"})
        if method == "GET" and route == "/schema":
            return _json_bytes({"ok": True, "ops": OPS_SCHEMA})
        if method == "GET" and route == "/v1/inspect":
            project = Path(unquote(query.get("path", "")))
            episode = load_episode(project)
            full = query.get("full") in ("1", "true", "yes")
            return _json_bytes(snapshot(episode, full=full))
        if method == "GET" and route.startswith("/v1/pages/") and route.endswith(".png"):
            from genko.render import render_page
            import io

            project = Path(unquote(query.get("path", "")))
            episode = load_episode(project)
            page_no = int(route.rsplit("/", 1)[-1].removesuffix(".png"))
            page = next(item for item in episode.pages if item.index == page_no)
            mode = query.get("mode", "print")
            image = render_page(page, int(query.get("dpi", "150")), mode=mode, episode=episode)
            buf = io.BytesIO()
            image.save(buf, format="PNG")
            return 200, buf.getvalue()
        if method == "POST" and route == "/v1/new":
            dest = Path(data["dest"])
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

            project = Path(data["path"])
            episode = load_episode(project)
            dry_run = bool(data.get("dry_run"))
            with ProjectLock(project):
                result = apply_ops(episode, data.get("ops") or [], dry_run=dry_run)
                if not dry_run:
                    save_episode(episode, project)
            return _json_bytes(result)
        if method == "POST" and route == "/v1/export":
            project = Path(data["path"])
            episode = load_episode(project)
            out = Path(data["out"])
            paths = export_png_sequence(episode, out, working_dpi=int(data.get("dpi", 150)))
            return _json_bytes({"ok": True, "count": len(paths), "files": [str(p) for p in paths]})
    except ApplyError as exc:
        return _json_bytes({"ok": False, "error": str(exc)}, 400)
    except FileNotFoundError as exc:
        return _json_bytes({"ok": False, "error": f"not found: {exc}"}, 404)
    except (KeyError, TypeError, ValueError) as exc:
        return _json_bytes({"ok": False, "error": str(exc)}, 400)

    return _json_bytes({"ok": False, "error": f"no route {method} {route}"}, 404)


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        return

    def _send(self, status: int, payload: bytes) -> None:
        content_type = (
            "image/png"
            if payload.startswith(b"\x89PNG")
            else "application/json; charset=utf-8"
        )
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        status, payload = handle_request("GET", self.path, b"")
        self._send(status, payload)

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        status, payload = handle_request("POST", self.path, body)
        self._send(status, payload)


class HeadlessServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, host: str = "127.0.0.1", port: int = 8765) -> None:
        super().__init__((host, port), _Handler)

    @property
    def port(self) -> int:
        return int(self.server_address[1])
