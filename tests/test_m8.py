"""M8: hand-drawn names — detection, import, proposals, a person's confirmation, D8 evaluation."""

import io
import json
import os
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))

from agents import atari as synth  # noqa: E402
from genko.__main__ import main  # noqa: E402
from genko.io import load_episode  # noqa: E402
from genko.models import LayerKind, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.render import render_page  # noqa: E402
from genko.studio import xycut  # noqa: E402
from genko.studio.evaluate import _edge_error  # noqa: E402
from genko.studio.service import HumanService, StudioService  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _matched(found: list[dict], truth: list[list[float]], tol: float = 5.0) -> int:
    return sum(1 for t in truth if any(_edge_error(d["rect_mm"], t) <= tol for d in found))


# --- detection --------------------------------------------------------------------------------


@pytest.mark.parametrize("dpi", [150, 300])
def test_xycut_recovers_panels_of_hand_drawn_pages(dpi: int):
    hits = total = 0
    for i, layout in enumerate(synth.LAYOUTS):
        for seed in (1, 2):
            png, truth = synth.draw_page(layout, dpi, seed * 100 + i)
            found = xycut.detect(Image.open(io.BytesIO(png)), (0, 0, *synth.PAGE_MM))
            hits += _matched(found.leaves_mm, truth)
            total += len(truth)
    assert hits / total >= 0.9, (hits, total)


def test_detection_gives_tiers_and_reading_order():
    png, truth = synth.draw_page(synth.LAYOUTS[0], 150, 7)
    found = xycut.detect(Image.open(io.BytesIO(png)), (0, 0, *synth.PAGE_MM))
    assert [len(t["cols"]) for t in found.tiers] == [1, 2, 1]
    middle = found.tiers[1]["cols"]
    assert abs(middle[0]["w"] - 0.55) < 0.03  # columns listed right to left, as in the name DSL
    # reading order: top panel, then the right column before the left one
    xs = [panel["rect_mm"][0] for panel in found.leaves_mm]
    assert xs[1] > xs[2]
    assert all(0 < panel["confidence"] <= 1 for panel in found.leaves_mm) and found.confidence > 0.6


def test_auto_alignment_puts_the_drawing_on_the_live_area():
    from genko.studio.atari import placement_for

    # the synthetic scans are drawn on the whole B4 sheet with 13 mm margins (the old layout), so the page matches
    page = new_episode("t", 1, 1, PageSpec(257, 364, 600, 3, 10, "mono")).pages[0]
    png, truth = synth.draw_page(synth.LAYOUTS[1], 150, 3, margin_mm=15)  # a scan with extra paper around the page
    image = Image.open(io.BytesIO(png))
    small = image.resize((round(image.width * 0.8), round(image.height * 0.8)))  # and scanned at another scale
    placement, used = placement_for(small.convert("L"), page, "auto")
    assert used == "live"
    found = xycut.detect(small, tuple(placement))
    assert _matched(found.leaves_mm, truth) == len(truth)
    assert placement_for(small.convert("L"), page, "page")[0] == [0.0, 0.0, page.spec.width_mm, page.spec.height_mm]


# --- import, proposals, confirmation ---------------------------------------------------------------------


def _project(root: Path, pages: int = 2) -> tuple[StudioService, Path, list[Path], list[list[list[float]]]]:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", pages).ok
    bible = json.loads((Path(__file__).parent / "fixtures" / "studio" / "demo4" / "bible.json").read_text(encoding="utf-8"))
    assert agent.set_bible("demo.genko", bible, commit=True).ok
    scans, truths = [], []
    for n in range(pages):
        png, truth = synth.draw_page(synth.LAYOUTS[n], 150, 40 + n)
        path = root / "scans" / f"page{n + 1}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(png)
        scans.append(path)
        truths.append(truth)
    return agent, root / "demo.genko", scans, truths


def _run(capsys, *args) -> dict:
    main(["studio", *map(str, args)])
    return json.loads(capsys.readouterr().out.strip().splitlines()[-1])


def test_import_places_the_scan_as_a_draft_and_only_proposes(tmp_path: Path, capsys):
    agent, project, scans, truths = _project(tmp_path)
    before = load_episode(project)
    result = _run(capsys, "import-name", project, *scans, "--align", "page")
    assert result["ok"] and [i["page"] for i in result["imported"]] == [1, 2]
    episode = load_episode(project)
    for page, truth in zip(episode.pages, truths):
        atari = page.plan["atari"]
        assert atari["align"] == "page" and atari["by"] == "human:leaf"
        assert episode.studio["assets"][atari["asset"]]["origin"] == "self"
        layer = next(layer for layer in page.layers if layer.kind == LayerKind.PLACED)
        assert layer.role == LayerRole.DRAFT and not layer.exportable and layer.asset == atari["asset"]
        # the page itself did not change: one panel, no lines
        assert len(page.leaf_frames()) == 1 and not episode.story_for_page(page.index)
        proposal = next(p for p in episode.studio["proposals"].values() if p["page_id"] == page.id)
        assert proposal["kind"] == "layout" and proposal["status"] == "open" and proposal["source"] == "genko:xycut"
        assert _matched(proposal["panels"], truth) == len(truth)
        analysis = project / proposal["analysis"]
        assert analysis.is_file() and analysis.with_suffix(".png").is_file()
    assert [p.frames[0].rect for p in before.pages] == [p.frames[0].rect for p in episode.pages]
    # the scan never prints; the name view shows it
    printed = render_page(episode.pages[0], 40, mode="print", episode=episode).convert("L")
    name = render_page(episode.pages[0], 40, mode="name", episode=episode).convert("L")

    def dark(image):
        return sum(image.histogram()[:128])

    assert dark(name) > dark(printed) + 1000  # the scan's lines show in the name view only
    # the agent sees what to do and waits for the person
    nxt = agent.next("demo.genko", limit=20).data
    assert {"gate": "proposal", "pages": [1, 2]} in nxt["waiting_for"]
    assert {i["kind"] for i in nxt["items"]} == {"read_atari"}


def test_a_person_accepts_the_layout_and_the_lines(tmp_path: Path, capsys):
    agent, project, scans, truths = _project(tmp_path)
    agent_import = StudioService(tmp_path, "ai:test").import_name("demo.genko", [str(scans[0])], align="page", confine=False)
    assert agent_import.ok  # an agent may import too (the proposal still needs a person)
    episode = load_episode(project)
    layout = next(p for p in episode.studio["proposals"].values() if p["kind"] == "layout")
    # the agent reads the handwriting and proposes lines
    first = layout["panels"][0]["rect_mm"]
    lines = [
        {"text": "5年前の今日\nここで約束した", "balloon": "narration", "x_mm": first[0] + first[2] - 20, "y_mm": first[1] + 4},
        {"text": "…来るわけないよね", "box01": [0.5, 0.45, 0.04, 0.1]},
    ]
    bad = agent.propose_lines("demo.genko", 1, [{"text": ""}])
    assert not bad.ok and bad.issues[0].path == "/lines/0"
    proposed = agent.propose_lines("demo.genko", 1, lines)
    assert proposed.ok and proposed.images
    assert not load_episode(project).story_for_page(1)  # nothing on the page yet
    # the agent cannot confirm its own proposals
    for op in ({"op": "resolve_proposal", "id": layout["id"], "status": "accepted"},
               {"op": "set_layout", "page": 1, "tree": layout["tree"]}):
        assert not agent.apply_ops("demo.genko", [op], commit=True).ok
    assert load_episode(project).studio["proposals"][layout["id"]]["status"] == "open"
    # the person accepts the layout, then the lines
    assert _run(capsys, "accept", project, layout["id"], "--as", "human:leaf")["ok"]
    assert _run(capsys, "accept", project, proposed.data["proposal"], "--as", "human:leaf")["ok"]
    episode = load_episode(project)
    page = episode.pages[0]
    leaves = page.leaf_frames()
    assert len(leaves) == len(truths[0])
    assert all(any(_edge_error([f.rect.x, f.rect.y, f.rect.width, f.rect.height], t) <= 5 for f in leaves) for t in truths[0])
    added = episode.story_for_page(1)
    assert [ln.text for ln in added] == ["5年前の今日\nここで約束した", "…来るわけないよね"]
    assert all(ln.frame_id in {f.id for f in leaves} for ln in added)  # each line landed in the panel it sits in
    assert {p["status"] for p in episode.studio["proposals"].values() if p["page_id"] == page.id} == {"accepted"}
    # next: briefs for the panels, then the name approval
    kinds = {i["kind"] for i in agent.next("demo.genko", limit=20).data["items"] if i["target"].get("page") == 1}
    assert kinds == {"brief_panels"}
    ops = [{"op": "set_panel", "page": 1, "frame_id": f.id, "set": {"shot": "MS", "angle": "eye"}} for f in leaves]
    assert agent.apply_ops("demo.genko", ops, commit=True).ok
    assert {"gate": "name", "pages": [1]} in agent.next("demo.genko", limit=20).data["waiting_for"]


def test_rejecting_reanalysing_and_user_regions(tmp_path: Path, capsys):
    agent, project, scans, truths = _project(tmp_path, pages=1)
    _run(capsys, "import-name", project, scans[0], "--align", "page")
    episode = load_episode(project)
    first = next(iter(episode.studio["proposals"].values()))
    assert _run(capsys, "reject", project, first["id"], "--note", "コマが多すぎる", "--as", "human:leaf")["ok"]
    kinds = {i["kind"] for i in agent.next("demo.genko", limit=20).data["items"]}
    assert "atari_layout" in kinds  # the agent is told the layout still needs a proposal
    again = agent.analyze_name("demo.genko", 1, {"min_panel_mm": 20})
    assert again.ok and again.images
    assert load_episode(project).studio["proposals"][again.data["proposal"]]["status"] == "open"  # reopened if identical
    HumanService(project, "human:leaf").accept_proposal(again.data["proposal"])
    page = load_episode(project).pages[0]
    frame = page.leaf_frames()[0]
    HumanService(project, "human:leaf")._apply([{"op": "add_region", "page": 1, "frame_id": frame.id,
                                                "region": {"kind": "keep", "rect_mm": [frame.rect.x + 5, frame.rect.y + 5, 20, 20]}}])
    # analysing the scan again only proposes: the person's regions stay
    third = agent.analyze_name("demo.genko", 1)
    assert third.ok
    page = load_episode(project).pages[0]
    assert [r["source"] for r in page._find(frame.id).panel["regions"]] == ["user"]
    # the same analysis again adds nothing new; a different layout over a page that has one needs force
    import copy

    accepted = load_episode(project).studio["proposals"][again.data["proposal"]]
    other = copy.deepcopy({k: accepted[k] for k in ("kind", "tree", "panels")})
    other["tree"]["rect_mm"][3] -= 1.0
    other.update({"id": "pr_other", "page": 1})
    HumanService(project, "human:leaf")._apply([{"op": "propose", "proposal": other}])
    result = HumanService(project, "human:leaf").accept_proposal("pr_other")
    assert not result["ok"] and "force" in result["error"]
    assert HumanService(project, "human:leaf").accept_proposal("pr_other", force=True)["ok"]


def test_d8_evaluation_command(tmp_path: Path, capsys):
    pages = []
    for i, layout in enumerate(synth.LAYOUTS):
        png, truth = synth.draw_page(layout, 150, 500 + i)
        (tmp_path / f"s{i}.png").write_bytes(png)
        pages.append({"scan": f"s{i}.png", "panels": truth})
    truth_file = tmp_path / "truth.json"
    truth_file.write_text(json.dumps({"align": "page", "pages": pages}), encoding="utf-8")
    result = _run(capsys, "eval-atari", truth_file)
    assert result["ok"] and result["passed"] and result["rate"] >= 0.9 and len(result["pages"]) == len(synth.LAYOUTS)


def test_mcp_import_and_proposal_tools(tmp_path: Path):
    pytest.importorskip("mcp")
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    agent, project, scans, truths = _project(tmp_path, pages=1)
    inbox = project / "studio" / "inbox" / "scans"
    inbox.mkdir(parents=True)
    (inbox / "p1.png").write_bytes(scans[0].read_bytes())

    async def scenario():
        async with Client(build_server(tmp_path, "ai:hermes")) as client:
            def data(result):
                return json.loads(result.content[0].text)

            outside = data(await client.call_tool("import_name", {"project": "demo.genko", "files": ["/etc/hosts"]}))
            assert not outside["ok"]
            imported = data(await client.call_tool("import_name", {"project": "demo.genko", "files": ["demo.genko/studio/inbox/scans/p1.png"],
                                                                   "align": "page"}))
            assert imported["ok"] and imported["proposals"][0]["panels"] == len(truths[0])
            listed = data(await client.call_tool("proposals", {"project": "demo.genko"}))
            assert listed["proposals"][0]["kind"] == "layout"
            shown = await client.call_tool("render", {"project": "demo.genko", "page": 1, "kind": "atari", "max_px": 500})
            assert shown.content[1].type == "image"
            proposed = await client.call_tool("propose_lines", {"project": "demo.genko", "page": 1,
                                                                "lines": [{"text": "待って！", "balloon": "shout", "box01": [0.6, 0.1, 0.05, 0.1]}]})
            assert data(proposed)["ok"] and proposed.content[1].type == "image"

    anyio.run(scenario)


def test_approval_box_confirms_proposals(tmp_path: Path, capsys):
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")
    from genko.app import review_model
    from genko.app.main import MainWindow

    agent, project, scans, truths = _project(tmp_path, pages=1)
    _run(capsys, "import-name", project, scans[0], "--align", "page")
    window = MainWindow(project)
    window.show()
    app.processEvents()
    items = window.approvals.items
    assert [i.kind for i in items] == ["proposal"] and items[0].title == "アタリからの提案: 1 ページのコマ割り"
    window.approvals.list.setCurrentRow(0)
    app.processEvents()
    assert window.approvals.approve_button.text() == "確定"
    window.approvals.approve()
    app.processEvents()
    page = load_episode(project).pages[0]
    assert len(page.leaf_frames()) == len(truths[0])
    assert review_model.inbox(load_episode(project)) == []
    window.close()
