"""M4 end to end, offline: a scripted agent over MCP and a scripted human with the CLI (§10.6)."""

import json
from pathlib import Path

import pytest

pytest.importorskip("mcp")

import anyio  # noqa: E402
from mcp import Client  # noqa: E402

from agents.fake_agent import FakeAgent  # noqa: E402
from agents.images import SENTINEL  # noqa: E402
from genko.__main__ import main  # noqa: E402
from genko.io import load_episode  # noqa: E402
from genko.mcp.server import build_server  # noqa: E402
from genko.render import render_page  # noqa: E402

PROJECT = "story.genko"


@pytest.fixture(autouse=True)
def _config(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


class ScriptedHuman:
    """Answers approval requests the way a person would, through `genko studio`."""

    def __init__(self, root: Path, out: Path, capsys) -> None:
        self.project = root / PROJECT
        self.out = out
        self.capsys = capsys
        self.exports: list[dict] = []
        self.refused: list[dict] = []

    def run(self, *args) -> dict:
        code = main(["studio", *map(str, args)])
        out = json.loads(self.capsys.readouterr().out.strip().splitlines()[-1])
        assert (code == 0) == out["ok"], out
        return out

    def __call__(self, waiting: list[dict]) -> None:
        episode = load_episode(self.project)
        asked = {t.get("gate") for t in episode.tickets if t.get("kind") == "gate" and t.get("status") == "open"}
        for gate in waiting:
            assert gate["gate"] in asked, f"the agent did not ask for {gate}"  # people answer requests, they are not polled
            if gate["gate"] in ("name", "art"):
                pages = ",".join(map(str, gate["pages"]))
                assert self.run("approve", self.project, gate["gate"], "--pages", pages, "--as", "human:test")["ok"]
            elif gate["gate"] == "sheet":
                for cid in gate["characters"]:
                    cand = episode.studio["character_candidates"][cid][0]["id"]
                    assert self.run("approve", self.project, "sheet", "--character", cid, "--candidate", cand, "--as", "human:test")["ok"]
            elif gate["gate"] == "export":
                refused = self.run("export", self.project, "--format", "pdf", "--out", self.out / "refused", "--dpi", 72, "--as", "human:test")
                self.refused.append(refused)
                for fmt in ("pdf", "tiff"):
                    self.exports.append(self.run("export", self.project, "--format", fmt, "--out", self.out / fmt, "--dpi", 72,
                                                 "--allow-fixture", "--force", "--as", "human:test"))


def _drive(root: Path, human, **agent_args) -> FakeAgent:
    server = build_server(root, "ai:test")
    holder: dict = {}

    async def scenario():
        async with Client(server) as client:
            agent = FakeAgent(client, root, PROJECT, human, **agent_args)
            created = await agent.tool("create_project", name=PROJECT, title="夏の午後の約束", pages=agent_args.get("pages", 8))
            assert created["ok"]
            holder["agent"] = agent
            await agent.run()

    anyio.run(scenario)
    return holder["agent"]


def test_eight_pages_from_bible_to_export_offline(tmp_path: Path, capsys, monkeypatch):
    root = tmp_path / "manga"
    root.mkdir()
    human = ScriptedHuman(root, tmp_path / "out", capsys)
    agent = _drive(root, human)

    episode = load_episode(root / PROJECT)
    assert len(episode.pages) == 8
    assert all(p.name_ok and p.art_ok and p.stage == "finish" for p in episode.pages)
    assert all(c.get("locked") and {r["kind"] for r in c["refs"]} >= {"sheet", "face"} for c in episode.bible.characters)
    # (d) the final export refuses test images, and says why
    assert human.refused and not human.refused[0]["ok"]
    assert "fixture_image" in {e["code"] for e in human.refused[0]["errors"]}
    # (c) with --allow-fixture the book comes out as PDF and TIFF
    pdf = human.exports[0]["files"]
    tiffs = human.exports[1]["files"]
    assert len(pdf) == 1 and Path(pdf[0]).read_bytes()[:4] == b"%PDF"
    assert len(tiffs) == 8 and all(Path(t).is_file() for t in tiffs)
    assert any(a["gate"] == "export" and a["by"] == "human:test" for a in episode.studio["approvals"])
    # nothing but adopted art prints: the magenta decoys stay candidates (colours checked before the mono finish)
    import genko.render

    monkeypatch.setattr(genko.render, "_finish_placed", lambda fitted, *args, **kwargs: fitted)
    for page in episode.pages:
        image = render_page(page, 40, mode="print", episode=episode).convert("RGB")
        assert SENTINEL not in {px for _, px in image.getcolors(1 << 20)}
    # every panel went through request → import → review → adopt, and faces were reported
    for page in episode.pages:
        for frame in page.leaf_frames():
            panel = frame.panel
            assert panel["status"] == "adopted" and panel["attempts"]["images"] == 2
            adopted = next(c for c in panel["candidates"] if c["id"] == panel["adopted"]["art"])
            assert adopted["origin"]["model"] == "fixture-good" and adopted["review"]["score"] == 0.9
            assert adopted["mapping"]["pad_mm"] == 3.0
    assert any(r.get("source") == "agent" for p in episode.pages for f in p.leaf_frames() for r in f.panel.get("regions", []))
    # the agent exported a watermarked proof before asking for the export
    assert list((root / PROJECT / "studio" / "proofs").rglob("proof.pdf"))
    # (b) the agent never approved anything
    assert all(a["by"].startswith("human:") for a in episode.studio["approvals"])
    assert len(agent.log) > 50
    # M5 acceptance on the offline run: only people changed approvals, and few images per panel
    from genko.studio import evaluate

    audit = evaluate.audit(root / PROJECT)
    assert audit["ok"] and set(audit["changes_by_actor"]) == {"human:test"}
    stats = evaluate.stats(root / PROJECT)
    assert stats["images_per_adopted_panel"]["median"] <= 8
    assert stats["tool_calls"]["total"] > 100 and stats["tool_calls"]["ms_p95"] < 2000
    # the pilot page was approved first and fixed the style for the rest
    locked = episode.studio["style"]["locked"]
    assert locked["page"] == 1 and locked["tool"] == "fixture"


def test_agent_cannot_approve_any_gate(tmp_path: Path):
    server = build_server(tmp_path, "ai:test")

    async def scenario():
        async with Client(server) as client:
            tools = {t.name for t in (await client.list_tools()).tools}
            assert not {"approve", "revoke", "export"} & tools
            agent = FakeAgent(client, tmp_path, PROJECT, None, pages=4)
            await agent.tool("create_project", name=PROJECT, title="t", pages=4)
            for op in ({"op": "approve", "gate": "name", "page": 1}, {"op": "approve", "gate": "export"},
                       {"op": "name_ok", "page": 1}, {"op": "revoke", "gate": "name", "page": 1}):
                result = await agent.tool("apply_ops", ops=[op], commit=True)
                assert not result["ok"]

    anyio.run(scenario)


def test_a_new_agent_resumes_where_the_last_one_stopped(tmp_path: Path, capsys):
    root = tmp_path / "manga"
    root.mkdir()
    human = ScriptedHuman(root, tmp_path / "out", capsys)
    server = build_server(root, "ai:test")

    async def scenario():
        async with Client(server) as client:
            first = FakeAgent(client, root, PROJECT, human, pages=4)
            await first.tool("create_project", name=PROJECT, title="t", pages=4)
            await first.run(stop_after=25)
            episode = load_episode(root / PROJECT)
            assert not all(p.art_ok for p in episode.pages)
            requests_before = set((episode.studio.get("requests") or {}))
            second = FakeAgent(client, root, PROJECT, human, pages=4)
            await second.run()
            # the second agent did not redo what the first had finished
            assert not set(first.seen_ids) & set(second.seen_ids) - {i for i in first.seen_ids if first.seen_ids.count(i) > 1}
            return requests_before

    before = anyio.run(scenario)
    episode = load_episode(root / PROJECT)
    assert all(p.art_ok and p.stage == "finish" for p in episode.pages)
    assert before <= set(episode.studio["requests"])
    # one request per panel: nothing was asked twice
    panels = sum(len(p.leaf_frames()) for p in episode.pages)
    sheets = len(episode.bible.characters)
    assert len(episode.studio["requests"]) == panels + sheets


def test_an_agent_that_keeps_failing_asks_a_person_and_moves_on(tmp_path: Path):
    agent = _drive(tmp_path, None, pages=4, broken_page=3)
    tries = [line for line in agent.log if line.startswith("plan_page {'page': 3}")]
    assert len(tries) == 3  # gave up after three tries instead of looping
    episode = load_episode(tmp_path / PROJECT)
    help_tickets = [t for t in episode.tickets if t.get("kind") == "help"]
    assert len(help_tickets) == 1 and help_tickets[0]["page_index"] == 3 and help_tickets[0]["assignee"] == "human"
    # the other pages went on to wait for their name approval
    assert all(episode.pages[i].plan for i in (0, 1, 3))
