"""The AI's feedback after the Windows run: references in order of need (an image tool may take two), props
written by their name, and the enlargement's candidate id under the name adopt uses."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from genko.studio import genreq  # noqa: E402
from genko.studio.service import StudioService  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def test_references_come_most_needed_first():
    names = ["refs/loc_park_12345678.png", "refs/previous_panel.png", "refs/prop_cup_1234.png", "refs/style_catalog.png",
             "refs/hina_sheet.png", "refs/hina_face.png"]
    assert genreq._by_priority(names) == ["refs/hina_face.png", "refs/hina_sheet.png", "refs/style_catalog.png",
                                          "refs/previous_panel.png", "refs/prop_cup_1234.png", "refs/loc_park_12345678.png"]


def test_a_prop_written_by_its_name_is_read_as_its_id(tmp_path: Path):
    agent = StudioService(tmp_path, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    bible = _load("bible.json")
    bible["props"] = [{"id": "cup", "name": "青いマグカップ", "desc": "取っ手の欠けた青いマグ"}]
    assert agent.set_bible("demo.genko", bible, commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    plan = _load("p001.json")
    plan["panels"][0]["props"] = ["青いマグカップ"]
    result = agent.submit_name("demo.genko", plan)
    codes = {i.code for i in result.issues}
    assert result.ok and "prop_by_name" in codes and "unknown_prop" not in codes


def _approved(root: Path):
    from genko.studio.service import HumanService

    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    assert agent.submit_name("demo.genko", _load("p001.json"), commit=True).ok
    HumanService(root / "demo.genko", "human:leaf").approve_name([1])
    return agent


def test_one_call_takes_a_panels_picture_from_request_to_adopted(tmp_path: Path):
    from agents import images as fixture_images
    from genko.io import load_episode

    agent = _approved(tmp_path)
    frame = load_episode(tmp_path / "demo.genko").pages[0].leaf_frames()[0]
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    inbox = tmp_path / "demo.genko" / "studio" / "inbox" / request["id"]
    (inbox / "a.png").write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    origin = {"kind": "agent", "tool_id": "xai:grok-imagine", "model": "grok-imagine"}
    regions = [{"kind": "face", "char": f["char"], "box01": f["head01"]} for f in request["figures"]]
    done = agent.take_panel_art("demo.genko", request["id"], {"file": f"studio/inbox/{request['id']}/a.png", "origin": origin},
                                regions=regions)
    assert done.ok, done.issues
    steps = [s["step"] for s in done.data["steps"]]
    assert steps[:3] == ["import", "adopt", "upscale"] and (not regions or steps[-1] == "report_regions")
    panel = load_episode(tmp_path / "demo.genko").pages[0]._find(frame.id).panel
    assert panel["adopted"]["art"] == done.data["adopted"]
    assert done.data["upscaled"]  # (a ~1 MP picture is below 600 dpi for a whole panel: enlarged and adopted again)
    if done.data["upscaled"]:
        assert "adopt_upscaled" in steps and done.data["adopted"] != done.data["candidate_id"]
    # a wrong file stops at the import and says so
    bad = agent.take_panel_art("demo.genko", request["id"], {"file": "studio/inbox/none.png", "origin": origin})
    assert not bad.ok and bad.data["stopped_at"] == "import"
    # a sheet request is not for this tool
    sheet = agent.generation_request("demo.genko", character_id=_load("bible.json")["characters"][0]["id"]).data["request"]
    assert agent.take_panel_art("demo.genko", sheet["id"], {"file": "x", "origin": origin}).issues[0].code == "not_a_panel"
