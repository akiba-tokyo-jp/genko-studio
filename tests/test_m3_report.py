"""The Windows test report (M3): undo after CRLF, aimed focus lines, shout sizes, tickets the agent can close,
Japanese refusals, effects in the snapshot, lines outside the panels, the timelapse's frame count."""

import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.studio.service import HumanService, StudioService  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture
def agent(tmp_path: Path):
    service = StudioService(tmp_path, "ai:hermes")
    assert service.create_project("demo.genko", "demo", 2).ok
    return service


# --- 1. undo on a book saved with Windows line ends ----------------------------------------------------------------


def test_the_saved_file_has_the_same_bytes_everywhere(agent):
    path = agent.project_path("demo.genko")
    assert b"\r\n" not in (path / "project.json").read_bytes()


def test_undo_works_after_the_line_ends_turned_into_crlf(agent):
    path = agent.project_path("demo.genko")
    assert agent.apply_ops("demo.genko", [{"op": "add_line", "page": 1, "text": "やあ", "id": "a"}], commit=True).ok
    target = path / "project.json"
    target.write_bytes(target.read_bytes().replace(b"\n", b"\r\n"))  # (as git or an editor on Windows leaves it)
    result = agent.undo("demo.genko").to_dict()
    assert result["ok"], result
    assert not any(line.id == "a" for line in load_episode(path).story)


# --- 2. focus lines aimed at the faces ---------------------------------------------------------------------------


def _panel_with_fx(regions):
    from genko.models import PageSpec, new_episode

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    page = ep.pages[0]
    frame = page.leaf_frames()[0]
    frame.panel = {"status": "adopted", "fx": ["集中線"], "regions": regions}
    return ep, page, frame


def test_focus_lines_close_in_on_the_faces_and_leave_them_clear():
    from genko.studio.finish import plan

    ep, page, frame = _panel_with_fx([{"kind": "face", "char": "a", "rect_mm": [40, 60, 20, 20]},
                                      {"kind": "face", "char": "b", "rect_mm": [80, 60, 20, 20]}])
    ops, notes = plan(ep, page)
    effect = next(op for op in ops if op["op"] == "add_effect")
    assert effect["params"]["center"] == [70.0, 70.0]
    inner = effect["params"]["inner"]
    assert inner[0] >= 30 and inner[1] >= 10  # (half the faces' box, and more)
    apply_ops(ep, ops)  # (the effect takes these params)


def test_focus_lines_without_faces_say_why():
    from genko.studio.finish import plan

    ep, page, frame = _panel_with_fx([])
    ops, notes = plan(ep, page)
    assert next(op for op in ops if op["op"] == "add_effect")["params"] == {}
    assert any("report_regions" in n.get("why", "") for n in notes)


# --- 3. shout balloons big enough for their spikes -----------------------------------------------------------------


def test_a_shout_is_measured_for_its_valleys():
    from genko.balloons import _inner
    from genko.studio.letter import EM_MM, LEADING, PAD_MM, measure

    breaks = ["オー・イエス", "お詫びに"]
    speech = measure(breaks, "speech")
    shout = measure(breaks, "shout")
    assert shout[0] > speech[0] * 1.3 and shout[1] > speech[1] * 1.3
    text_w = EM_MM * 2 + EM_MM * LEADING
    text_h = EM_MM * 6
    room = _inner("shout", shout[0], shout[1], PAD_MM, 0.2)
    assert room[0] >= text_w - 0.01 and room[1] >= text_h - 0.01


# --- 4. tickets the agent can close ------------------------------------------------------------------------------


@pytest.fixture
def named(tmp_path: Path):
    """A book with its names approved (the M5 fixture)."""
    import test_m5 as m5

    return m5._project(tmp_path / "named")


def _approved_page_with_comment(named):
    agent, path, human = named
    ticket = human.comment(1, "3 コマ目の台詞を短く")["comment"]
    return path, human, ticket


def test_a_comment_on_an_approved_page_shows_in_next(named):
    agent = named[0]
    _path, _human, ticket = _approved_page_with_comment(named)
    items = agent.next("demo.genko", limit=50).to_dict()["items"]
    fix = next(i for i in items if i["kind"] == "fix_page")
    assert fix["tickets"] == [ticket] and "resolve_ticket" in fix["tools"]


def test_the_agent_closes_a_fix_and_a_person_can_reopen_it(named):
    agent = named[0]
    path, human, ticket = _approved_page_with_comment(named)
    assert not agent.resolve_ticket("demo.genko", ticket, "").ok  # (it says what it did)
    done = agent.resolve_ticket("demo.genko", ticket, "台詞を 2 行に縮めた")
    assert done.ok, done.to_dict()
    closed = next(t for t in load_episode(path).tickets if t["id"] == ticket)
    assert closed["status"] == "done" and closed["resolved_by"] == "ai:test" and closed["resolved_note"]
    assert not any(i["kind"] == "fix_page" for i in agent.next("demo.genko", limit=50).to_dict()["items"])
    assert human.reopen_ticket(ticket, "まだ長い")["ok"]
    assert next(t for t in load_episode(path).tickets if t["id"] == ticket)["status"] == "open"
    refused = agent.apply_ops("demo.genko", [{"op": "reopen_ticket", "id": ticket}], commit=True)
    assert not refused.ok


def test_the_agent_cannot_close_an_approval_request(agent):
    assert agent.request_approval("demo.genko", "name", [2]).ok
    path = agent.project_path("demo.genko")
    gate = next(t for t in load_episode(path).tickets if t.get("kind") == "gate")
    result = agent.resolve_ticket("demo.genko", gate["id"], "閉じたい").to_dict()
    assert not result["ok"] and "人" in result["error"]
    assert next(t for t in load_episode(path).tickets if t["id"] == gate["id"])["status"] == "open"


def test_resolving_a_panel_fix_takes_the_panel_out_of_fix_requested(named):
    import test_m5 as m5

    agent, path, human = named
    m5._sheets(agent, human)
    frame = load_episode(path).pages[0].leaf_frames()[0]
    ticket = human.comment(1, "効果線を足して", frame.id)["comment"]
    items = agent.next("demo.genko", limit=50).to_dict()["items"]
    assert any(i["kind"] == "fix_panel" and i.get("tickets") == [ticket] for i in items)
    assert agent.resolve_ticket("demo.genko", ticket, "集中線を足した").ok
    panel = next(f for f in load_episode(path).pages[0].leaf_frames() if f.id == frame.id).panel
    assert panel["status"] != "fix_requested"


# --- 5. refusals in Japanese -----------------------------------------------------------------------------------


def test_person_only_and_gate_refusals_come_back_in_japanese(agent):
    policy = agent.apply_ops("demo.genko", [{"op": "set_studio", "policy": {"pilot": False}}], commit=True).to_dict()
    assert not policy["ok"] and "人だけ" in policy["error"] and "only a person" in policy["detail"]
    stroke = agent.apply_ops("demo.genko", [{"op": "add_stroke", "page": 1, "layer": "ink",
                                              "points": [[50, 50], [60, 60]]}], commit=True).to_dict()
    assert not stroke["ok"] and "ネームの承認" in stroke["error"] and stroke["error"].startswith("ops[0] add_stroke: ")


# --- 6. effects in the snapshot, lines outside the panels ------------------------------------------------------


def test_effects_show_in_the_snapshot_with_their_ids(agent):
    path = agent.project_path("demo.genko")
    HumanService(path, "human:leaf").approve_name([1])
    frame = load_episode(path).pages[0].leaf_frames()[0]
    assert agent.apply_ops("demo.genko", [{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": frame.id,
                                            "id": "fx1"}], commit=True).ok
    page = agent.inspect("demo.genko", "snapshot").to_dict()["snapshot"]["pages"][0]
    assert page["effects"][0]["id"] == "fx1" and page["effects"][0]["kind"] == "focus"


def test_a_line_outside_every_panel_is_reported():
    from genko.models import PageSpec, new_episode

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ep.pages[0].name_ok = True
    frame = ep.pages[0].leaf_frames()[0].rect
    below = frame.y + frame.height + 3
    report = apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[frame.x + 5, below], [frame.x + 40, below]]},
                            {"op": "add_stroke", "page": 1, "layer": "ink",
                             "points": [[frame.x - 5, frame.y + 20], [frame.x + frame.width + 5, frame.y + 20]]}])
    results = report.get("results") or []
    warned = [r for r in results if r and r.get("warning") == "outside_panels"]
    assert len(warned) == 1  # (the second line crosses the panel: it shows)


# --- 7. the timelapse's frame count ------------------------------------------------------------------------------


def test_the_timelapse_counts_the_pictures_in_the_file(tmp_path):
    from genko import timelapse

    white = Image.new("RGB", (40, 30), "white")
    black = Image.new("RGB", (40, 30), "black")
    report: dict = {}
    dest = timelapse.write_movie([white, black, black, black, white], tmp_path / "t.gif", 10, "gif", hold=1, report=report)
    assert report["frames"] == 3
    with Image.open(dest) as movie:
        assert movie.n_frames == 3
        durations = []
        for i in range(movie.n_frames):
            movie.seek(i)
            durations.append(movie.info["duration"])
    assert durations == [100, 300, 1100]
