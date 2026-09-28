"""Fewer calls and fewer words for an agent: slim tool schemas, the ops looked up one by one, a failure said
once, unknown keys warned of, the next work item on each write, adopt finding its panel, the snapshot of one
page, candidates in brief, and an export waited for."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from genko.studio.service import StudioService

pytest.importorskip("mcp")

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


def _book(tmp_path: Path) -> StudioService:
    agent = StudioService(tmp_path, "ai:hermes")
    assert agent.create_project("demo.genko", "demo", 4).ok
    return agent


def test_tool_schemas_carry_no_titles_or_nulls():
    from genko.mcp.server import slim_schema

    schema = {"title": "x", "type": "object", "properties": {"title": {"title": "Title", "type": "string"},
        "page": {"title": "Page", "anyOf": [{"type": "integer"}, {"type": "null"}], "default": None},
        "ops": {"title": "Ops", "type": "array", "items": {"type": "object", "additionalProperties": True}}},
        "required": ["ops"]}
    assert slim_schema(schema) == {"type": "object", "properties": {"title": {"type": "string"}, "page": {"type": "integer"},
                                                                     "ops": {"type": "array", "items": {"type": "object"}}},
                                   "required": ["ops"]}


def test_ops_are_looked_up_by_name_and_the_drawing_guide_is_there(tmp_path):
    agent = _book(tmp_path)
    names = agent.inspect("demo.genko", "ops").data["ops"]
    assert "add_stroke" in names and "approve" not in names
    found = agent.inspect("demo.genko", "ops", op="add_stroke, fill").data
    assert [o["op"] for o in found["ops"]] == ["add_stroke", "fill"] or {o["op"] for o in found["ops"]} == {"add_stroke", "fill"}
    assert not agent.inspect("demo.genko", "ops", op="nothing").ok
    assert "emphasis_runs" in agent.inspect("demo.genko", "drawing").data["drawing"]


def test_a_failure_is_said_once_and_unknown_keys_are_warned_of(tmp_path):
    agent = _book(tmp_path)
    failed = agent.apply_ops("demo.genko", [{"op": "add_line", "page": 1, "txt": "x"}], commit=True).to_dict()
    text = json.dumps(failed, ensure_ascii=False)
    assert text.count("add_line の書き方") == 1 and "takes {" not in text
    done = agent.apply_ops("demo.genko", [{"op": "add_line", "page": 1, "text": "x", "nam": "y"}], commit=True)
    assert done.ok and any("nam" in w for w in done.data["warnings"])


def test_the_snapshot_of_one_page_and_a_missing_panel_names_the_panels(tmp_path):
    agent = _book(tmp_path)
    snap = agent.inspect("demo.genko", "snapshot", page=2).data["snapshot"]
    assert [p["index"] for p in snap["pages"]] == [2]
    missing = agent.candidates("demo.genko", page=1, frame_id="nope")
    assert not missing.ok and "1 ページのコマ" in missing.data["error"]


def test_adopt_finds_its_panel_and_mcp_writes_carry_the_next_item(tmp_path):
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    agent = _book(tmp_path)
    load = lambda name: json.loads((FIXTURES / name).read_text(encoding="utf-8"))  # noqa: E731
    assert not agent.adopt("demo.genko", "no-such-candidate").ok
    server = build_server(tmp_path, "ai:hermes")

    async def scenario():
        async with Client(server) as client:
            reply = json.loads((await client.call_tool("set_bible", {"project": "demo.genko", "bible": load("bible.json"),
                                                                      "commit": True})).content[0].text)
            assert reply["ok"] and reply["next"]["kind"] == "write_script"
            tools = (await client.list_tools()).tools
            schema = json.dumps([t.input_schema for t in tools])
            assert '"title": "' not in schema and '"type": "null"' not in schema
            create = next(t for t in tools if t.name == "create_project").input_schema
            assert "title" in create["properties"] and "title" in create["required"]  # (an argument named title stays)

    anyio.run(scenario)


def test_export_status_waits_for_the_job(tmp_path, monkeypatch):
    from genko.studio import jobs

    agent = _book(tmp_path)
    calls = iter([{"ok": True, "status": "running"}, {"ok": True, "status": "running"},
                  {"ok": True, "status": "done", "result": {"files": []}}])
    monkeypatch.setattr(jobs, "status", lambda project, job: next(calls))
    monkeypatch.setattr("time.sleep", lambda s: None)
    result = agent.export_status("demo.genko", "job1", wait_s=30)
    assert result.ok and result.data["status"] == "done"


def test_the_standing_notes_come_once_and_sheets_are_asked_for_together(tmp_path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    chars = [c["id"] for c in json.loads((FIXTURES / "bible.json").read_text(encoding="utf-8"))["characters"]]
    first = agent.generation_request("demo.genko", character_id=chars[0]).data["request"]
    again = agent.generation_request("demo.genko", character_id=chars[-1]).data["request"]
    assert "references_note" in first and "references_note" not in again and again["standing_notes"]
    assert not any(n in agent.STANDING_NOTES for n in again["notes_for_agent"])
    asked = agent.request_approval("demo.genko", "sheet", [], character_ids=chars)
    assert asked.ok and [r["character_id"] for r in asked.data["requests"]] == chars
    assert len({r["request"] for r in asked.data["requests"]}) == len(chars)
