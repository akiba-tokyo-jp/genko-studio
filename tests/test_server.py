import json
from threading import Thread

from genko.server import HeadlessServer


def test_http_new_apply_inspect_export(tmp_path):
    server = HeadlessServer(host="127.0.0.1", port=0)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.port
        import time
        import urllib.request
        import urllib.error

        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=1)
                break
            except (urllib.error.URLError, OSError):
                time.sleep(0.05)

        def post(path: str, payload: dict) -> dict:
            req = urllib.request.Request(
                f"http://127.0.0.1:{port}{path}",
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                return json.loads(resp.read().decode("utf-8"))

        def get(path: str) -> dict:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as resp:
                return json.loads(resp.read().decode("utf-8"))

        dest = tmp_path / "http.genko"
        created = post("/v1/new", {"dest": str(dest), "title": "http", "pages": 2})
        assert created["ok"] is True
        applied = post(
            "/v1/apply",
            {
                "path": str(dest),
                "ops": [{"op": "add_line", "page": 1, "text": "ヘッドレス", "speaker": "AI"}],
            },
        )
        assert applied["snapshot"]["pages"][0]["story"][0]["text"] == "ヘッドレス"
        from urllib.parse import quote

        inspected = get(f"/v1/inspect?path={quote(str(dest))}")
        assert inspected["title"] == "http"
        out = tmp_path / "png"
        exported = post("/v1/export", {"path": str(dest), "out": str(out)})
        assert exported["count"] == 2
        health = get("/health")
        assert health["ok"] is True
    finally:
        server.shutdown()
