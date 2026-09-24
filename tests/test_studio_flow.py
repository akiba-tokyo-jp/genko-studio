"""The M0 flow as a scripted agent would run it: bible → script → name → review → human approval."""

import json
from pathlib import Path

import pytest

from genko.__main__ import main
from genko.io import load_episode, save_episode
from genko.studio.service import HumanService, StudioService

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _run_agent(root: Path) -> StudioService:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.next("demo.genko").data["items"][0]["kind"] == "write_bible"
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.next("demo.genko").data["items"][0]["kind"] == "write_script"
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    kinds = [i["kind"] for i in agent.next("demo.genko", limit=10).data["items"]]
    assert kinds == ["plan_page"] * 4
    for n in range(1, 5):
        dry = agent.submit_name("demo.genko", _load(f"p00{n}.json"))
        assert dry.ok and dry.data["committed"] is False and dry.images
        result = agent.submit_name("demo.genko", _load(f"p00{n}.json"), commit=True)
        assert result.ok and result.data["committed"], result.to_dict()
    return agent


def test_scripted_agent_builds_a_name_and_waits_for_a_human(tmp_path: Path):
    agent = _run_agent(tmp_path)
    kinds = [i["kind"] for i in agent.next("demo.genko", limit=10).data["items"]]
    assert kinds == ["review_name"] * 4
    for n in range(1, 5):
        assert agent.record_review("demo.genko", n, 0.8, "読み順よし").ok
    nxt = agent.next("demo.genko")
    assert nxt.data["items"] == [] and nxt.data["waiting_for"] == [{"gate": "name", "pages": [1, 2, 3, 4]}]
    assert agent.request_approval("demo.genko", "name", [1, 2, 3, 4], "確認お願いします").ok

    project = tmp_path / "demo.genko"
    with pytest.raises(Exception):
        HumanService(project, "ai:test")
    HumanService(project, "human:leaf").approve_name([1, 2, 3, 4])
    episode = load_episode(project)
    assert all(p.name_ok for p in episode.pages)
    after = agent.next("demo.genko", limit=50).data
    assert after["waiting_for"] == [] and after["blocked"] == 0
    # with the name approved, the art starts from the character sheets (panels with unapproved characters wait)
    assert {i["kind"] for i in after["items"]} <= {"make_sheet", "make_art"}
    assert any(i["kind"] == "make_sheet" for i in after["items"])
    episode = load_episode(project)
    assert all(f.panel and f.panel.get("status") == "briefed" for p in episode.pages for f in p.leaf_frames())
    assert all(p.plan and p.plan["input_hash"] and p.plan["reviews"]["name"]["score"] == 0.8 for p in episode.pages)
    assert not (project / "studio" / "drafts").exists()  # M3: everything lives in project.json
    assert all(t["status"] == "done" for t in agent.tickets("demo.genko", "all").data["tickets"])

    again = agent.submit_name("demo.genko", _load("p001.json"), commit=True, replace=True)
    assert not again.ok and again.issues[0].code == "name_approved"


def test_name_is_deterministic_and_keeps_the_project_format(tmp_path: Path):
    _run_agent(tmp_path / "a")
    _run_agent(tmp_path / "b")

    def lines(root: Path) -> list:
        ep = load_episode(root / "demo.genko")
        return [(l.page_index, l.text, l.balloon, l.x_mm, l.y_mm, l.w_mm, l.h_mm, l.tail) for l in ep.story]

    def rects(root: Path) -> list:
        ep = load_episode(root / "demo.genko")
        return [[(f.rect.x, f.rect.y, f.rect.width, f.rect.height) for f in p.leaf_frames()] for p in ep.pages]

    assert lines(tmp_path / "a") == lines(tmp_path / "b")
    assert rects(tmp_path / "a") == rects(tmp_path / "b")
    project = tmp_path / "a" / "demo.genko"
    before = json.loads((project / "project.json").read_text(encoding="utf-8"))
    save_episode(load_episode(project), project)
    after = json.loads((project / "project.json").read_text(encoding="utf-8"))
    before.pop("revision"), after.pop("revision")
    assert before == after  # no keys the loader would drop
    fresh = tmp_path / "fresh.genko"
    StudioService(tmp_path, "ai:test").create_project("fresh.genko", "demo", 4)
    assert set(before) | {"revision"} == set(json.loads((fresh / "project.json").read_text(encoding="utf-8")))


def test_errors_block_commit_and_point_to_the_input(tmp_path: Path):
    agent = _run_agent(tmp_path)
    plan = _load("p003.json")
    plan["tiers"][0]["h"] = 0.9
    result = agent.submit_name("demo.genko", plan, commit=True, replace=True)
    assert not result.ok and result.data["committed"] is False
    assert any(i.path == "/tiers" and i.code == "layout_ratio_sum" for i in result.issues)
    page = load_episode(tmp_path / "demo.genko").pages[2]
    assert len(page.leaf_frames()) == 2  # unchanged

    blocked = agent.submit_name("demo.genko", _load("p003.json"), commit=True)
    assert not blocked.ok and blocked.issues[0].code == "page_not_blank"


def test_human_comment_reopens_a_page_until_it_is_resubmitted(tmp_path: Path):
    agent = _run_agent(tmp_path)
    project = tmp_path / "demo.genko"
    HumanService(project, "human:leaf").comment(2, "2コマ目の台詞を減らして")
    items = agent.next("demo.genko", limit=10).data["items"]
    revise = [i for i in items if i["kind"] == "revise_page"]
    assert revise and revise[0]["target"] == {"page": 2} and revise[0]["comments"] == ["2コマ目の台詞を減らして"]
    assert agent.submit_name("demo.genko", _load("p002.json"), commit=True, replace=True).ok
    kinds = {(i["kind"], i["target"].get("page")) for i in agent.next("demo.genko", limit=10).data["items"]}
    assert ("revise_page", 2) not in kinds
    assert agent.tickets("demo.genko").data["tickets"] == []


def test_apply_ops_rejects_gates_and_local_file_reads(tmp_path: Path):
    agent = _run_agent(tmp_path)
    for op in ({"op": "name_ok", "page": 1}, {"op": "put_raster", "page": 1, "layer": "bg", "path": "/etc/hosts"},
               {"op": "set_meta", "font_path": "/etc/passwd"}, {"op": "unlock_page", "page": 1}):
        result = agent.apply_ops("demo.genko", [op], commit=True)
        assert not result.ok and result.issues[0].code == "op_not_allowed"
    line = load_episode(tmp_path / "demo.genko").story[0]
    moved = agent.apply_ops("demo.genko", [{"op": "move_line", "id": line.id, "x_mm": 30}], commit=True)
    assert moved.ok
    assert load_episode(tmp_path / "demo.genko").story[0].x_mm == 30


def test_projects_stay_inside_root(tmp_path: Path):
    agent = StudioService(tmp_path, "ai:test")
    for bad in ("../x.genko", "/etc", ""):
        with pytest.raises(Exception):
            agent.project_path(bad, must_exist=False)


def test_cli_flow_and_human_commands(tmp_path: Path, capsys):
    project = tmp_path / "demo.genko"

    def run(*args) -> dict:
        code = main(["studio", *map(str, args)])
        out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
        assert (code == 0) == out["ok"]
        return out

    assert run("init", project, "--pages", 4)["ok"]
    assert run("set-bible", project, FIXTURES / "bible.json", "--commit")["committed"]
    assert run("set-script", project, FIXTURES / "script.json", "--commit")["committed"]
    assert run("submit-name", project, FIXTURES / "p001.json", "--commit")["committed"]
    assert not run("approve", project, "name", "--pages", "1", "--as", "ai:x")["ok"]
    assert run("approve", project, "name", "--pages", "1", "--as", "human:leaf")["approved"] == [1]
    out = tmp_path / "review.html"
    assert run("review", project, "--out", out)["ok"]
    html = out.read_text(encoding="utf-8")
    assert "承認済み" in html and "data:image/png;base64," in html
