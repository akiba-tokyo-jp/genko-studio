import json
import sys
from pathlib import Path

import pytest

pytest.importorskip("mcp")

import anyio  # noqa: E402
from mcp import Client, StdioServerParameters  # noqa: E402

from genko.mcp.server import build_server  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _json(result) -> dict:
    return json.loads(result.content[0].text)


def test_mcp_server_refuses_a_human_actor(tmp_path: Path):
    with pytest.raises(Exception):
        build_server(tmp_path, "human:leaf")


def test_mcp_tools_drive_the_name_flow_in_process(tmp_path: Path):
    server = build_server(tmp_path, "ai:hermes")

    async def scenario():
        async with Client(server) as client:
            tools = {t.name for t in (await client.list_tools()).tools}
            assert {"status", "next", "submit_name", "request_approval", "render"} <= tools
            assert not {name for name in tools if "approve" in name and name != "request_approval"}
            assert _json(await client.call_tool("create_project", {"name": "demo.genko", "title": "demo", "pages": 4}))["ok"]
            assert _json(await client.call_tool("set_bible", {"project": "demo.genko", "bible": _load("bible.json"), "commit": True}))["committed"]
            assert _json(await client.call_tool("set_script", {"project": "demo.genko", "script": _load("script.json"), "commit": True}))["committed"]
            result = await client.call_tool("submit_name", {"project": "demo.genko", "plan": _load("p001.json"), "commit": True})
            data = _json(result)
            assert data["committed"] and data["reading_order"] == ["p1", "p2", "p3"]
            assert result.content[1].type == "image" and result.content[1].mime_type == "image/png"
            assert Path(data["files"][0]).is_file()
            blocked = _json(await client.call_tool("apply_ops", {"project": "demo.genko", "ops": [{"op": "name_ok", "page": 1}], "commit": True}))
            assert not blocked["ok"]
            rules = await client.read_resource("genko://guide/manga-rules")
            assert "右から左" in rules.contents[0].text

    anyio.run(scenario)


def test_genko_mcp_runs_over_stdio(tmp_path: Path):
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "genko", "mcp", "--root", str(tmp_path), "--agent", "ai:hermes"],
        env={"PYTHONPATH": str(Path(__file__).resolve().parents[1] / "src")},
    )

    async def scenario():
        async with Client(params) as client:
            result = await client.call_tool("projects", {})
            assert _json(result) == {"ok": True, "projects": [], "issues": [], "files": []}

    anyio.run(scenario)


def test_mcp_art_flow_import_candidate_adopt(tmp_path: Path):
    import base64
    import io

    from PIL import Image

    from genko.io import load_episode
    from genko.models import LayerKind
    from genko.studio.service import HumanService

    server = build_server(tmp_path, "ai:hermes")
    Image.new("RGB", (600, 300), (200, 40, 40)).save(tmp_path / "gen.png")
    buf = io.BytesIO()
    Image.new("RGB", (300, 300), (40, 40, 200)).save(buf, format="PNG")

    async def scenario():
        async with Client(server) as client:
            call = client.call_tool
            await call("create_project", {"name": "demo.genko", "title": "demo", "pages": 4})
            await call("set_bible", {"project": "demo.genko", "bible": _load("bible.json"), "commit": True})
            await call("set_script", {"project": "demo.genko", "script": _load("script.json"), "commit": True})
            await call("submit_name", {"project": "demo.genko", "plan": _load("p002.json"), "commit": True})
            HumanService(tmp_path / "demo.genko", "human:leaf").approve_name([2])
            panels = _json(await call("inspect", {"project": "demo.genko", "target": "panel", "page": 2}))["panels"]
            frame_id = panels[0]["frame_id"]
            assert panels[0]["panel"]["status"] == "briefed" and panels[0]["rect_mm"][2] > 0
            outside = _json(await call("import_image", {"project": "demo.genko", "path": "/etc/hosts"}))
            assert not outside["ok"]
            first = _json(await call("import_image", {"project": "demo.genko", "path": "gen.png"}))
            second = _json(await call("import_image", {"project": "demo.genko", "png_base64": base64.b64encode(buf.getvalue()).decode()}))
            assert first["px"] == [600, 300] and second["asset"].startswith("sha256:")
            ops = [{"op": "import_candidates", "page": 2, "frame_id": frame_id, "candidates": [
                {"id": "a", "asset": first["asset"], "px": first["px"], "origin": {"kind": "agent", "model": "image-gen"}},
                {"id": "b", "asset": second["asset"], "px": second["px"], "origin": {"kind": "agent"}}]},
                   {"op": "review_candidates", "page": 2, "frame_id": frame_id, "reviews": [{"candidate_id": "a", "score": 0.9}]},
                   {"op": "adopt_candidate", "page": 2, "frame_id": frame_id, "candidate_id": "a"}]
            assert _json(await call("apply_ops", {"project": "demo.genko", "ops": ops, "commit": True}))["ok"]
            crop = await call("render", {"project": "demo.genko", "page": 2, "mode": "print", "frame_id": frame_id, "max_px": 300})
            assert crop.content[1].type == "image"
            denied = _json(await call("apply_ops", {"project": "demo.genko", "ops": [{"op": "approve", "gate": "art", "page": 2}], "commit": True}))
            assert not denied["ok"]
            catalog = json.loads((await client.read_resource("genko://ops")).contents[0].text)
            names = {op["op"] for op in catalog}
            assert "adopt_candidate" in names and "approve" not in names and "put_raster" not in names

    anyio.run(scenario)
    page = load_episode(tmp_path / "demo.genko").pages[1]
    assert sum(layer.kind == LayerKind.PLACED for layer in page.layers) == 1
