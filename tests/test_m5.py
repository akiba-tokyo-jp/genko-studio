"""M5: pilot page and style lock, multi-character fixes, review page, run records, audit, D5 kit."""

import csv
import json
from pathlib import Path

import pytest

from agents import images as fixture_images
from genko.__main__ import main
from genko.io import load_episode, save_episode
from genko.ops import ApplyError, apply_ops
from genko.studio import evaluate
from genko.studio.service import HumanService, StudioService

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"
TOOL = "openai:gpt-image-1"


@pytest.fixture(autouse=True)
def _config(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _project(root: Path) -> tuple[StudioService, Path, HumanService]:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    for n in range(1, 5):
        assert agent.submit_name("demo.genko", _load(f"p00{n}.json"), commit=True).ok
    project = root / "demo.genko"
    human = HumanService(project, "human:leaf")
    human.approve_name([1, 2, 3, 4])
    return agent, project, human


def _sheets(agent: StudioService, human: HumanService) -> None:
    for cid in ("hina", "sora"):
        req = agent.generation_request("demo.genko", character_id=cid).data
        path = Path(req["inbox"]) / "s.png"
        path.write_bytes(fixture_images.sheet(req["request"]["size"]["suggested_px"]))
        cand = agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s.png",
                                                                        "origin": {"tool_id": TOOL}}]).data["candidates"][0]
        human.approve_sheet(cid, cand)


def _art(agent: StudioService, project: Path, page: int, frame_id: str, report: bool = True) -> str:
    req = agent.generation_request("demo.genko", page=page, frame_id=frame_id)
    assert req.ok, req.to_dict()
    request = req.data["request"]
    path = Path(req.data["inbox"]) / "a.png"
    path.write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    cand = agent.import_images("demo.genko", request["id"], [{"file": f"studio/inbox/{request['id']}/a.png",
                                                              "origin": {"tool_id": TOOL, "model": "gpt-image-1", "prompt": "x"}}])
    cand_id = cand.data["candidates"][0]
    assert agent.adopt("demo.genko", cand_id, page=page, frame_id=frame_id).ok
    if report:
        figures = [f for f in agent.inspect("demo.genko", "panel", page, frame_id).data["panels"][0]["figures"]]
        regions = [{"kind": k, "char": f["char"], "rect_mm": f[m]} for f in figures for k, m in (("face", "head_mm"), ("person", "body_mm"))]
        if regions:
            assert agent.report_regions("demo.genko", page, frame_id, regions).ok
    return cand_id


def test_pilot_page_first_then_the_style_is_fixed(tmp_path: Path):
    agent, project, human = _project(tmp_path)
    _sheets(agent, human)
    items = agent.next("demo.genko", limit=50).data["items"]
    assert items and {i["target"]["page"] for i in items} == {1}  # only the pilot page's art is offered
    page2 = load_episode(project).pages[1].leaf_frames()[0]
    before = agent.generation_request("demo.genko", page=2, frame_id=page2.id).data["request"]
    assert "refs/style_pilot.png" not in before["files"]["references"] and before["tool"] is None
    for frame in load_episode(project).pages[0].leaf_frames():
        _art(agent, project, 1, frame.id)
    human.approve_art([1])
    locked = load_episode(project).studio["style"]["locked"]
    assert locked["page"] == 1 and locked["tool"] == TOOL and locked["reference"].startswith("sha256:")
    assert locked["by"] == "human:leaf"
    kinds = {(i["kind"], i["target"].get("page")) for i in agent.next("demo.genko", limit=50).data["items"]}
    assert ("gen_panel", 2) in kinds
    after = agent.generation_request("demo.genko", page=2, frame_id=page2.id).data["request"]
    assert "refs/style_pilot.png" in after["files"]["references"] and after["tool"] == TOOL
    assert any("1 ページで固定" in n for n in after["notes_for_agent"])
    # re-opening the pilot page opens the style again
    human.revoke("art", [1], reason="線を太く")
    assert "locked" not in load_episode(project).studio["style"]


def test_multi_character_panel_is_fixed_one_person_at_a_time(tmp_path: Path):
    agent, project, human = _project(tmp_path)
    _sheets(agent, human)
    human._apply([{"op": "set_studio", "policy": {"pilot": False}}])
    frame = next(f for f in load_episode(project).pages[3].leaf_frames() if len(f.panel["characters"]) == 2)
    new = agent.generation_request("demo.genko", page=4, frame_id=frame.id).data["request"]
    assert new["steps"] and "inpaint" in new["steps"][2]
    wrong = agent.generation_request("demo.genko", page=4, frame_id=frame.id, focus_character="hina")
    assert not wrong.ok and wrong.issues[0].path == "/focus_character"  # only for fixes
    cand = _art(agent, project, 4, frame.id, report=False)
    unreported = agent.generation_request("demo.genko", page=4, frame_id=frame.id, mode="inpaint", parent=cand, focus_character="hina")
    assert not unreported.ok and "report_regions" in unreported.data["error"]
    figures = agent.inspect("demo.genko", "panel", 4, frame.id).data["panels"][0]["figures"]
    agent.report_regions("demo.genko", 4, frame.id, [{"kind": "person", "char": f["char"], "rect_mm": f["body_mm"]} for f in figures]
                         + [{"kind": "face", "char": f["char"], "rect_mm": f["head_mm"]} for f in figures])
    fix = agent.generation_request("demo.genko", page=4, frame_id=frame.id, mode="inpaint", parent=cand, focus_character="hina")
    assert fix.ok, fix.to_dict()
    request = fix.data["request"]
    assert request["focus_character"] == "hina" and request["prompt"]["ja"].startswith("直すのは日向ひなだけ")
    assert {"refs/hina_face.png", "refs/hina_sheet.png"} <= set(request["files"]["references"])
    assert not any(r.startswith("refs/sora_") for r in request["files"]["references"])
    assert request["files"]["mask"] == "mask.png" and request["files"]["source"] == "source.png"
    face_only = agent.generation_request("demo.genko", page=4, frame_id=frame.id, mode="inpaint", parent=cand, regions=["face:sora"])
    assert face_only.ok and face_only.data["request"]["id"] != request["id"]
    with pytest.raises(Exception):
        agent.generation_request("demo.genko", page=4, frame_id=frame.id, mode="inpaint", parent=cand, focus_character="nobody").data["request"]["id"]


def test_audit_proves_only_people_approved_and_redo_cannot_sneak_one_in(tmp_path: Path):
    agent, project, human = _project(tmp_path)
    report = evaluate.audit(project)
    assert report["ok"] and report["changes"] == 4 and report["changes_by_actor"] == {"human:leaf": 4}
    # a person takes an approval back with undo; an agent may not redo it
    assert main(["undo", str(project), "--as", "human:leaf"]) == 0
    assert not load_episode(project).pages[3].name_ok
    with pytest.raises(ApplyError, match="only a person"):
        from genko.journal import restore

        restore(project, actor="ai:test", redo=True)
    assert main(["redo", str(project), "--as", "human:leaf"]) == 0
    assert load_episode(project).pages[3].name_ok
    assert evaluate.audit(project)["ok"]
    # an approval written around the ops (a bug, or a hand-edited file) shows up as a violation
    episode = load_episode(project)
    episode.pages[0].art_ok = True
    save_episode(episode, project, actor="ai:rogue")
    report = evaluate.audit(project)
    assert not report["ok"] and report["violations"][0]["actor"] == "ai:rogue"


def test_tool_calls_are_logged_and_stats_summarise_the_run(tmp_path: Path):
    pytest.importorskip("mcp")
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    agent, project, human = _project(tmp_path)
    server = build_server(tmp_path, "ai:hermes")

    async def scenario():
        async with Client(server) as client:
            await client.call_tool("status", {"project": "demo.genko"})
            await client.call_tool("submit_name", {"project": "demo.genko", "plan": _load("p001.json"), "commit": True})  # approved: refused
            await client.call_tool("import_images", {"project": "demo.genko", "request_id": "rq_missing", "images": []})
            await client.call_tool("inspect", {"project": "demo.genko", "target": "nonsense"})
            skill = await client.read_resource("genko://guide/skill")
            assert "パイロットページ" in skill.contents[0].text

    anyio.run(scenario)
    stats = evaluate.stats(project)
    calls = stats["tool_calls"]
    assert calls["total"] == 4 and calls["failed"] == 3
    assert calls["failure_reasons"]["name_approved"] == 1
    assert calls["by_tool"]["status"] == {"calls": 1, "failed": 0}
    assert all(p["name_submits"] == 1 for p in stats["pages"])
    log = (project / "studio" / "logs" / "tools.jsonl").read_text(encoding="utf-8")
    assert "ai:hermes" in log and "tiers" not in log  # documents are not copied into the log


def test_review_page_shows_candidates_requests_and_commands(tmp_path: Path, capsys):
    agent, project, human = _project(tmp_path)
    req = agent.generation_request("demo.genko", character_id="hina").data
    (Path(req["inbox"]) / "s.png").write_bytes(fixture_images.sheet(req["request"]["size"]["suggested_px"]))
    sheet = agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s.png",
                                                                     "origin": {"kind": "fixture"}}]).data["candidates"][0]
    agent.request_approval("demo.genko", "sheet", [], "設定画です", character_id="hina")
    frame = load_episode(project).pages[0].leaf_frames()[0]
    cand = _art(agent, project, 1, frame.id)
    asked = agent.ask_human("demo.genko", "扉の向きが分からない", page=1, frame_id=frame.id, item="gen_panel")
    ticket = asked.data["tickets"][0]
    result = agent.review_page("demo.genko")
    page = Path(result.data["path"]).read_text(encoding="utf-8")
    assert result.ok and {"kind": "gate", "gate": "sheet", "pages": [], "character_id": "hina"} in result.data["open_requests"]
    assert f"approve {project} sheet --character hina --candidate {sheet}" in page
    assert cand in page and "✔採用" in page and "試験用の画像" in page
    assert f"close-ticket {project} {ticket}" in page and "扉の向きが分からない" in page
    assert "書き出しを止めている理由" in page
    # the person answers the question with an instruction: the agent gets it as a fix
    main(["studio", "close-ticket", str(project), ticket, "--reply", "扉は右開き", "--as", "human:leaf"])
    assert json.loads(capsys.readouterr().out)["ok"]
    episode = load_episode(project)
    assert next(t for t in episode.tickets if t["id"] == ticket)["status"] == "done"
    fix = [t for t in episode.tickets if t.get("kind") == "fix" and t.get("status") == "open"]
    assert fix and fix[0]["text"] == "扉は右開き" and fix[0]["assignee"] == "agent"


def test_d5_blind_character_samples_and_score(tmp_path: Path, capsys):
    agent, project, human = _project(tmp_path)
    _sheets(agent, human)
    human._apply([{"op": "set_studio", "policy": {"pilot": False}}])
    singles = [(p.index, f.id) for p in load_episode(project).pages for f in p.leaf_frames() if len(f.panel["characters"]) == 1]
    for page, frame_id in singles:
        _art(agent, project, page, frame_id, report=False)
    out = tmp_path / "d5"
    assert main(["studio", "eval-sample", str(project), "--out", str(out), "--per-character", "2", "--seed", "3"]) == 0
    made = json.loads(capsys.readouterr().out)
    assert made["ok"] and made["samples"] == 3 and made["characters"] == 2  # hina ×2, sora ×1 (only one single-sora panel)
    key = json.loads((out / "key.json").read_text(encoding="utf-8"))
    assert set(key["letters"].values()) == {"hina", "sora"}
    assert all((out / "samples" / f"{name}.png").is_file() for name in key["samples"])
    assert all((out / "sheets" / f"{letter}.png").is_file() for letter in key["letters"])
    form = (out / "form.html").read_text(encoding="utf-8")
    assert "hina" not in form and "sora" not in form  # blind: letters only
    answers = tmp_path / "answers.csv"
    with answers.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["evaluator", "sample", "choice"])
        names = sorted(key["samples"])
        for name in names:
            writer.writerow(["a", name, key["samples"][name]["answer"]])
        for i, name in enumerate(names):
            wrong = next(letter for letter in key["letters"] if letter != key["samples"][name]["answer"])
            writer.writerow(["b", name, key["samples"][name]["answer"] if i == 0 else wrong])
    assert main(["studio", "eval-score", str(out), str(answers)]) == 0
    score = json.loads(capsys.readouterr().out)
    assert score["by_evaluator"] == {"a": 1.0, "b": round(1 / 3, 3)} and score["answers"] == 6
    assert score["accuracy"] == round(4 / 6, 3) and not score["passed"]


def test_skill_copy_matches_the_packaged_one():
    root = Path(__file__).resolve().parents[1]
    packaged = (root / "src" / "genko" / "studio" / "guide" / "SKILL.md").read_text(encoding="utf-8")
    shipped = (root / "integrations" / "hermes" / "genko-manga" / "SKILL.md").read_text(encoding="utf-8")
    assert packaged == shipped
    # every MCP tool the skill names exists
    import re

    pytest.importorskip("mcp")
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    async def names():
        async with Client(build_server(root, "ai:x")) as client:
            return {t.name for t in (await client.list_tools()).tools}

    tools = anyio.run(names)
    named = set(re.findall(r"mcp_genko_(\w+)", packaged))
    assert named <= tools, named - tools


def test_art_approval_by_an_agent_is_still_refused(tmp_path: Path):
    agent, project, human = _project(tmp_path)
    episode = load_episode(project)
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent="ai:test")
