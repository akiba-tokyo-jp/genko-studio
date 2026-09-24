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
