"""M7 service layer (no Qt): the GUI session and the approval box model."""

import json
import sys
from pathlib import Path

import pytest

from genko.app import review_model
from genko.app.session import Session, default_actor, disk_revision
from genko.io import load_episode
from genko.ops import ApplyError
from genko.studio import evaluate
from genko.studio.service import StudioService

sys.path.insert(0, str(Path(__file__).parent))

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


@pytest.fixture(autouse=True)
def _config(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _named(root: Path) -> tuple[StudioService, Path]:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    for n in range(1, 5):
        assert agent.submit_name("demo.genko", _load(f"p00{n}.json"), commit=True).ok
    return agent, root / "demo.genko"


def test_actor_is_a_person():
    assert default_actor() == "human:leaf"


def test_gui_edits_stay_in_memory_until_commit_and_never_erase_an_agents_commit(tmp_path: Path):
    agent, project = _named(tmp_path)
    gui = Session.open(project)
    line = gui.episode.story[0]
    gui.apply([{"op": "move_line", "id": line.id, "x_mm": 33, "y_mm": 44}])
    gui.apply([{"op": "set_note", "page": 2, "note": "人間のメモ"}])
    assert gui.dirty and disk_revision(project) == gui.base_revision  # nothing written yet
    # meanwhile the agent commits (a review and a new request)
    assert agent.record_review("demo.genko", 1, 0.9, "点検した").ok
    assert agent.request_approval("demo.genko", "name", [1], "確認を").ok
    assert gui.outside_change()
    result = gui.commit()
    assert result.ok and result.rebased and not result.conflicts
    episode = load_episode(project)
    moved = next(ln for ln in episode.story if ln.id == line.id)
    assert (moved.x_mm, moved.y_mm) == (33, 44) and episode.pages[1].note == "人間のメモ"  # ours
    assert episode.pages[0].plan["reviews"]["name"]["notes"] == "点検した"  # theirs
    assert any(t.get("gate") == "name" for t in episode.tickets)  # theirs
    assert gui.base_revision == episode.revision and not gui.dirty


def test_ops_that_no_longer_apply_are_reported_not_forced(tmp_path: Path):
    agent, project = _named(tmp_path)
    gui = Session.open(project)
    line = gui.episode.story_for_page(3)[0]
    gui.apply([{"op": "set_note", "page": 1, "note": "残る"}])
    gui.apply([{"op": "move_line", "id": line.id, "x_mm": 20, "y_mm": 20}])  # (the note is saved before this)
    # the agent rebuilt page 3 in the meantime: that line is gone
    assert agent.submit_name("demo.genko", _load("p003.json"), commit=True, replace=True).ok
    result = gui.commit()
    assert result.rebased and len(result.conflicts) == 1 and "move_line" in json.dumps(result.conflicts[0]["ops"])
    episode = load_episode(project)
    assert episode.pages[0].note == "残る"
    assert all(ln.id != line.id for ln in episode.story)


def test_sync_reloads_when_idle_and_undo_works_in_memory_then_on_disk(tmp_path: Path):
    agent, project = _named(tmp_path)
    gui = Session.open(project)
    agent.record_review("demo.genko", 2, 0.5, "x")
    assert gui.sync().rebased and gui.episode.pages[1].plan["reviews"]["name"]["score"] == 0.5
    gui.apply([{"op": "set_note", "page": 1, "note": "a"}])
    gui.undo()
    assert gui.episode.pages[0].note != "a" and not gui.dirty
    gui.apply([{"op": "set_note", "page": 1, "note": "b"}])
    gui.commit()
    assert load_episode(project).pages[0].note == "b"
    gui.undo()  # our own last commit, through the journal
    assert load_episode(project).pages[0].note != "b"
    agent.record_review("demo.genko", 3, 0.5, "y")
    with pytest.raises(ApplyError):
        gui.undo()  # the latest change is the agent's


def test_approval_box_lists_requests_and_approves_one_at_a_time(tmp_path: Path):
    agent, project = _named(tmp_path)
    for n in (1, 2):
        agent.record_review("demo.genko", n, 0.8, "ok")
    agent.request_approval("demo.genko", "name", [1, 2], "確認を")
    agent.ask_human("demo.genko", "3 ページの見せ場が弱い", page=3, item="plan_page")
    gui = Session.open(project)
    items = review_model.inbox(gui.episode)
    assert [i.kind for i in items] == ["gate", "help"]
    assert items[0].title == "ネームの承認: 1, 2 ページ" and items[0].by == "ai:test"
    progress = dict(review_model.progress(gui.episode))
    assert progress["ネーム承認待ち"] == 4 and progress["設定画未承認"] == 2
    gui.apply(review_model.approve_ops(gui.episode, items[0]))
    gui.commit()
    episode = load_episode(project)
    assert episode.pages[0].name_ok and episode.pages[1].name_ok and not episode.pages[2].name_ok
    assert [i.kind for i in review_model.inbox(episode)] == ["help"]
    # answering the question sends the reply to the agent as an instruction
    gui.apply(review_model.send_back_ops(gui.episode, review_model.inbox(gui.episode)[0], "めくりで見開きにする"))
    gui.commit()
    episode = load_episode(project)
    assert review_model.inbox(episode) == []
    items = agent.next("demo.genko", limit=20).data["items"]
    assert any(i["kind"] == "revise_page" and i["target"] == {"page": 3} and "めくりで見開きにする" in i["comments"] for i in items)
    assert evaluate.audit(project)["changes_by_actor"] == {"human:leaf": 2}


def test_send_back_art_and_sheets_reaches_the_agent(tmp_path: Path):
    from agents import images as fixture_images
    from genko.studio.service import HumanService

    agent, project = _named(tmp_path)
    HumanService(project, "human:leaf").approve_name([1, 2, 3, 4])
    req = agent.generation_request("demo.genko", character_id="hina").data
    (Path(req["inbox"]) / "s.png").write_bytes(fixture_images.sheet(req["request"]["size"]["suggested_px"]))
    agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s.png", "origin": {"tool_id": "t"}}])
    agent.request_approval("demo.genko", "sheet", [], "設定画", character_id="hina")
    gui = Session.open(project)
    [sheet_item] = [i for i in review_model.inbox(gui.episode) if i.gate == "sheet"]
    with pytest.raises(ValueError):
        review_model.approve_ops(gui.episode, sheet_item, project=project)  # a candidate must be chosen
    gui.apply(review_model.send_back_ops(gui.episode, sheet_item, "髪をもっと長く"))
    gui.commit()
    items = agent.next("demo.genko", limit=20).data["items"]
    sheet_work = next(i for i in items if i["kind"] == "make_sheet" and i["target"] == {"character_id": "hina"})
    assert sheet_work["comments"] == ["髪をもっと長く"]
    # a new sheet answers the send-back and can be approved from the box, face cut out
    req = agent.generation_request("demo.genko", character_id="hina", instruction="髪をもっと長く").data
    (Path(req["inbox"]) / "s2.png").write_bytes(fixture_images.sheet([900, 1300]))
    cand = agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s2.png",
                                                                     "origin": {"tool_id": "t"}}]).data["candidates"][0]
    agent.request_approval("demo.genko", "sheet", [], "描き直した", character_id="hina")
    gui.sync()
    [sheet_item] = [i for i in review_model.inbox(gui.episode) if i.gate == "sheet"]
    gui.apply(review_model.approve_ops(gui.episode, sheet_item, project=project, candidate_id=cand))
    gui.commit()
    hina = next(c for c in load_episode(project).bible.characters if c["id"] == "hina")
    assert hina["locked"] and {r["kind"] for r in hina["refs"]} == {"sheet", "face"}
    assert not [t for t in load_episode(project).tickets if t.get("kind") == "fix" and t.get("status") == "open"]


def test_agents_cannot_use_the_person_only_ops(tmp_path: Path):
    agent, project = _named(tmp_path)
    result = agent.apply_ops("demo.genko", [{"op": "reject_sheet", "character_id": "hina", "note": "x"}], commit=True)
    assert not result.ok
