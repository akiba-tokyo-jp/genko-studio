"""M3: imported images placed into panels, and the agent's state in project.json."""

import base64
import io
import json
import shutil
from pathlib import Path

import pytest
from PIL import Image

from genko.__main__ import main
from genko.io import load_episode, save_episode
from genko.models import LayerKind, PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.render import render_page
from genko.studio.service import HumanService, StudioService

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"
RED, BLUE, GREEN, YELLOW = (220, 30, 30), (30, 30, 220), (30, 200, 30), (230, 220, 20)


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _png(size: tuple[int, int], color=None, halves=None) -> str:
    image = Image.new("RGB", size, color or (0, 0, 0))
    if halves:
        image.paste(halves[0], (0, 0, size[0] // 2, size[1]))
        image.paste(halves[1], (size[0] // 2, 0, size[0], size[1]))
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _named(root: Path, approve: bool = True) -> tuple[StudioService, Path]:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    for n in range(1, 5):
        assert agent.submit_name("demo.genko", _load(f"p00{n}.json"), commit=True).ok
    project = root / "demo.genko"
    if approve:
        HumanService(project, "human:leaf").approve_name([1, 2, 3, 4])
    return agent, project


def _commit(agent: StudioService, ops: list[dict]):
    result = agent.apply_ops("demo.genko", ops, commit=True)
    assert result.ok, result.to_dict()
    return result


def _import(agent: StudioService, page: int, frame_id: str, b64: str) -> tuple[str, list[int]]:
    imported = agent.import_image("demo.genko", png_base64=b64)
    assert imported.ok, imported.to_dict()
    asset, px = imported.data["asset"], imported.data["px"]
    _commit(agent, [{"op": "import_candidates", "page": page, "frame_id": frame_id,
                     "candidates": [{"id": f"c_{frame_id}_{asset[7:15]}", "asset": asset, "px": px, "origin": {"kind": "agent"}}]}])
    return f"c_{frame_id}_{asset[7:15]}", px


def _share(image: Image.Image, rect, dpi: int, color, x_range=(0.1, 0.9)) -> float:
    """Share of a grid of points inside `rect` (mm) whose pixel is close to `color`."""
    hits = total = 0
    for i in range(7):
        for j in range(7):
            x = rect.x + rect.width * (x_range[0] + (x_range[1] - x_range[0]) * i / 6)
            y = rect.y + rect.height * (0.1 + 0.8 * j / 6)
            px = image.getpixel((int(x / 25.4 * dpi), int(y / 25.4 * dpi)))
            total += 1
            hits += all(abs(a - b) < 40 for a, b in zip(px[:3], color))
    return hits / total


def _gutter_point(page) -> tuple[float, float]:
    leaves = page.leaf_frames()
    for a in leaves:
        for b in leaves:
            ra, rb = a.rect, b.rect
            if ra.y + ra.height < rb.y - 1 and ra.x < rb.x + rb.width and rb.x < ra.x + ra.width:
                x0, x1 = max(ra.x, rb.x), min(ra.x + ra.width, rb.x + rb.width)
                return (x0 + x1) / 2, (ra.y + ra.height + rb.y) / 2
            if ra.x + ra.width < rb.x - 1 and ra.y < rb.y + rb.height and rb.y < ra.y + ra.height:
                y0, y1 = max(ra.y, rb.y), min(ra.y + ra.height, rb.y + rb.height)
                return (ra.x + ra.width + rb.x) / 2, (y0 + y1) / 2
    raise AssertionError("no gutter")


def test_three_images_of_different_shapes_fill_three_panels_and_print_clipped(tmp_path: Path):
    agent, project = _named(tmp_path)
    page = load_episode(project).pages[0]
    leaves = page.leaf_frames()
    assert len(leaves) == 3
    images = [_png((2000, 500), halves=(RED, BLUE)), _png((400, 900), GREEN), _png((700, 700), YELLOW)]
    for frame, b64 in zip(leaves, images):
        cand, _ = _import(agent, 1, frame.id, b64)
        _commit(agent, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand}])

    episode = load_episode(project)
    page = episode.pages[0]
    placed = [layer for layer in page.layers if layer.kind == LayerKind.PLACED]
    assert len(placed) == 3 and all(layer.exportable for layer in placed)
    for layer in placed:  # "cover": the image covers its panel
        frame = page._find(layer.frame_id)
        r, f = layer.placement_mm, frame.rect
        assert r.x <= f.x + 0.01 and r.y <= f.y + 0.01
        assert r.x + r.width >= f.x + f.width - 0.01 and r.y + r.height >= f.y + f.height - 0.01

    dpi = 100
    image = render_page(page, dpi, mode="print", episode=episode)
    first, second, third = page.leaf_frames()
    assert _share(image, first.rect, dpi, RED, (0.05, 0.3)) > 0.5
    assert _share(image, first.rect, dpi, BLUE, (0.7, 0.95)) > 0.5
    assert _share(image, second.rect, dpi, GREEN) > 0.5
    assert _share(image, third.rect, dpi, YELLOW) > 0.5
    gx, gy = _gutter_point(page)
    assert image.getpixel((int(gx / 25.4 * dpi), int(gy / 25.4 * dpi)))[:3] == (255, 255, 255)
    # nothing is drawn outside the panels (top-left paper corner is white)
    assert image.getpixel((2, 2))[:3] == (255, 255, 255)

    text = (project / "project.json").read_text(encoding="utf-8")
    assert "base64" not in text and "iVBOR" not in text  # images live in assets/, never inline
    assert len(text) < 400_000


def test_art_is_resampled_from_the_source_at_print_resolution(tmp_path: Path):
    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[2].leaf_frames()[0]
    # a 1-pixel checkerboard: resampled from the source it averages to grey; nearest-neighbour would not
    checker = Image.new("L", (1200, 1200))
    checker.putdata([255 * ((x + y) % 2) for y in range(1200) for x in range(1200)])
    buf = io.BytesIO()
    checker.save(buf, format="PNG")
    cand, _ = _import(agent, 3, frame.id, base64.b64encode(buf.getvalue()).decode())
    _commit(agent, [{"op": "adopt_candidate", "page": 3, "frame_id": frame.id, "candidate_id": cand, "fit": "contain"}])
    episode = load_episode(project)
    page = episode.pages[2]
    layer = next(layer for layer in page.layers if layer.kind == LayerKind.PLACED)
    r = layer.placement_mm
    dpi = 72
    image = render_page(page, dpi, mode="print", episode=episode).convert("L")
    cx, cy = int((r.x + r.width / 2) / 25.4 * dpi), int((r.y + r.height / 2) / 25.4 * dpi)
    values = [image.getpixel((cx + dx, cy + dy)) for dx in range(-3, 4) for dy in range(-3, 4)]
    assert all(90 < v < 170 for v in values)


def test_undo_restores_the_adoption(tmp_path: Path):
    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[0].leaf_frames()[0]
    cand, _ = _import(agent, 1, frame.id, _png((300, 200), RED))
    episode = load_episode(project)
    apply_ops(episode, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand}], agent="ai:test")
    assert any(layer.kind == LayerKind.PLACED for layer in episode.pages[0].layers)
    apply_ops(episode, [{"op": "undo"}], agent="ai:test")
    assert not any(layer.kind == LayerKind.PLACED for layer in episode.pages[0].layers)
    assert episode.pages[0]._find(frame.id).panel["status"] == "candidates"

    # across processes: the journal restores the saved state
    _commit(agent, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand}])
    assert main(["undo", str(project), "--as", "ai:test"]) == 0
    page = load_episode(project).pages[0]
    assert not any(layer.kind == LayerKind.PLACED for layer in page.layers)
    assert page._find(frame.id).panel["status"] == "candidates"
    assert main(["redo", str(project), "--as", "ai:test"]) == 0
    assert any(layer.kind == LayerKind.PLACED for layer in load_episode(project).pages[0].layers)


def test_duplicate_page_keeps_art_on_the_new_panels(tmp_path: Path):
    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[0].leaf_frames()[1]
    cand, _ = _import(agent, 1, frame.id, _png((300, 600), GREEN))
    _commit(agent, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand}])
    episode = load_episode(project)
    apply_ops(episode, [{"op": "duplicate_page", "page": 1}], agent="human:leaf")
    clone = episode.pages[-1]
    layer = next(layer for layer in clone.layers if layer.kind == LayerKind.PLACED)
    assert layer.frame_id != frame.id
    new_frame = clone._find(layer.frame_id)
    assert new_frame.rect == frame.rect and new_frame.panel["adopted"]["art"] == cand
    image = render_page(clone, 60, mode="print", episode=episode)
    assert _share(image, new_frame.rect, 60, GREEN) > 0.5


def test_name_draft_and_candidates_do_not_print(tmp_path: Path):
    agent, project = _named(tmp_path, approve=False)
    frame = load_episode(project).pages[0].leaf_frames()[0]
    cand, _ = _import(agent, 1, frame.id, _png((400, 300), RED))
    # before the name is approved, art cannot be adopted, but a draft can
    blocked = agent.apply_ops("demo.genko", [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand}], commit=True)
    assert not blocked.ok and "name" in blocked.data["error"]
    _commit(agent, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": cand, "to": "draft"}])
    other = load_episode(project).pages[0].leaf_frames()[1]
    _import(agent, 1, other.id, _png((400, 300), BLUE))  # a candidate only
    episode = load_episode(project)
    page = episode.pages[0]
    printed = render_page(page, 60, mode="print", episode=episode)
    proof = render_page(page, 60, mode="proof", episode=episode)
    assert _share(printed, frame.rect, 60, RED) == 0
    assert _share(proof, frame.rect, 60, RED) > 0.3  # the draft shows while working
    assert _share(printed, other.rect, 60, BLUE) == 0 and _share(proof, other.rect, 60, BLUE) == 0


def test_panel_crop_render_over_service_and_http(tmp_path: Path):
    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[0].leaf_frames()[0]
    result = agent.render("demo.genko", 1, "proof", 400, frame_id=frame.id)
    assert result.ok and Path(result.files[0]).is_file()
    crop = Image.open(io.BytesIO(result.images[0]))
    assert abs(crop.width / crop.height - frame.rect.width / frame.rect.height) < 0.05
    assert max(crop.size) <= 420

    from genko.server import handle_request

    status, body = handle_request("GET", f"/v1/pages/1/frames/{frame.id}.png?path=demo.genko&dpi=100", b"",
                                  {"root": tmp_path, "actor": "human:leaf"})
    assert status == 200 and body.startswith(b"\x89PNG")
    status, _ = handle_request("GET", "/v1/pages/1/frames/nope.png?path=demo.genko", b"", {"root": tmp_path, "actor": "human:leaf"})
    assert status == 404


def test_split_and_merge_keep_briefs_and_protect_placed_art(tmp_path: Path):
    agent, project = _named(tmp_path, approve=False)
    episode = load_episode(project)
    page = episode.pages[2]
    slots = {f.panel["slot"]: f for f in page.leaf_frames()}
    target = slots["p1"]
    # the name is not approved: layout can change, and the brief moves to the panel read first
    apply_ops(episode, [{"op": "split_frame", "page": 3, "frame_id": target.id, "axis": "vertical"}], agent="ai:test")
    parent = episode.pages[2]._find(target.id)
    left, right = parent.children
    assert right.panel and right.panel["slot"] == "p1" and left.panel is None  # right-bound: the right column reads first
    apply_ops(episode, [{"op": "merge_frame", "page": 3, "frame_id": left.id}], agent="ai:test")
    assert episode.pages[2]._find(target.id).panel["slot"] == "p1"

    # with placed art, a split needs force and the art goes to studio.orphans
    save_episode(episode, project, actor="ai:test")
    cand, _ = _import(agent, 3, target.id, _png((200, 200), RED))
    _commit(agent, [{"op": "adopt_candidate", "page": 3, "frame_id": target.id, "candidate_id": cand, "to": "draft"}])
    episode = load_episode(project)
    with pytest.raises(ApplyError, match="force"):
        apply_ops(episode, [{"op": "split_frame", "page": 3, "frame_id": target.id, "axis": "horizontal"}], agent="ai:test")
    apply_ops(episode, [{"op": "split_frame", "page": 3, "frame_id": target.id, "axis": "horizontal", "force": True}], agent="ai:test")
    assert not any(layer.kind == LayerKind.PLACED for layer in episode.pages[2].layers)
    orphan = episode.studio["orphans"][-1]
    assert orphan["kind"] == "layers" and orphan["layers"][0]["asset"].startswith("sha256:")
    top = episode.pages[2]._find(target.id).children[0]
    assert top.panel["slot"] == "p1"  # horizontal split: the top panel reads first


def test_strict_gates_freeze_the_approved_layout_and_gate_finish(tmp_path: Path):
    agent, project = _named(tmp_path)
    episode = load_episode(project)
    frame = episode.pages[0].leaf_frames()[0]
    with pytest.raises(ApplyError, match="revoke"):
        apply_ops(episode, [{"op": "split_frame", "page": 1, "frame_id": frame.id, "axis": "vertical"}], agent="ai:test")
    with pytest.raises(ApplyError, match="art"):
        apply_ops(episode, [{"op": "advance", "page": 1, "to": "finish"}], agent="ai:test")
    # approving the art needs every panel adopted (or skipped by a person)
    with pytest.raises(ApplyError, match="adopted"):
        apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent="human:leaf")
    for leaf in episode.pages[0].leaf_frames():
        apply_ops(episode, [{"op": "set_panel", "page": 1, "frame_id": leaf.id, "set": {"status": "skip"}}], agent="human:leaf")
    with pytest.raises(ApplyError, match="person"):
        apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent="ai:test")
    apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent="human:leaf")
    assert episode.pages[0].art_ok and episode.studio["style"]["locked_from_page"] == episode.pages[0].id
    apply_ops(episode, [{"op": "advance", "page": 1, "to": "finish"}], agent="ai:test")
    with pytest.raises(ApplyError, match="revoke needs a person"):
        apply_ops(episode, [{"op": "revoke", "gate": "name", "page": 1}], agent="ai:test")
    apply_ops(episode, [{"op": "revoke", "gate": "name", "page": 1, "reason": "やり直し"}], agent="human:leaf")
    assert not episode.pages[0].name_ok and not episode.pages[0].art_ok


def test_pins_overrides_and_regions_belong_to_the_person(tmp_path: Path):
    agent, project = _named(tmp_path)
    episode = load_episode(project)
    fid = episode.pages[0].leaf_frames()[0].id

    def op(agent_name, **fields):
        apply_ops(episode, [{"op": "set_panel", "page": 1, "frame_id": fid, **fields}], agent=agent_name)

    op("human:leaf", set={"shot": "close"}, pin=["shot"])
    with pytest.raises(ApplyError, match="pinned"):
        op("ai:test", set={"shot": "long"})
    with pytest.raises(ApplyError, match="unpin"):
        op("ai:test", unpin=["shot"])
    with pytest.raises(ApplyError, match="override"):
        op("ai:test", set={"gen": {"prompt_override": "x"}})
    with pytest.raises(ApplyError, match="skip"):
        op("ai:test", set={"status": "skip"})
    op("ai:test", set={"action": "走る"})
    panel = episode.pages[0]._find(fid).panel
    assert panel["shot"] == "close" and panel["action"] == "走る"
    before = panel["brief_hash"]

    apply_ops(episode, [{"op": "add_region", "page": 1, "frame_id": fid, "region": {"id": "r1", "kind": "face", "rect_mm": [1, 2, 3, 4]}}],
              agent="human:leaf")
    with pytest.raises(ApplyError, match="person drew"):
        apply_ops(episode, [{"op": "delete_region", "page": 1, "frame_id": fid, "id": "r1"}], agent="ai:test")
    apply_ops(episode, [{"op": "replace_regions", "page": 1, "frame_id": fid, "source": "agent",
                         "regions": [{"kind": "balloon", "rect_mm": [0, 0, 5, 5]}]}], agent="ai:test")
    panel = episode.pages[0]._find(fid).panel
    assert [r["source"] for r in panel["regions"]] == ["user", "agent"]
    assert panel["brief_hash"] != before


def test_sheet_approval_locks_the_character(tmp_path: Path):
    agent, project = _named(tmp_path)
    imported = agent.import_image("demo.genko", png_base64=_png((600, 900), GREEN))
    _commit(agent, [{"op": "import_candidates", "character_id": "hina",
                     "candidates": [{"id": "sheet1", "asset": imported.data["asset"], "px": imported.data["px"], "origin": {"kind": "agent"}}]}])
    assert agent.request_approval("demo.genko", "sheet", [], "設定画です", character_id="hina").ok
    nxt = agent.next("demo.genko", limit=50).data
    assert {"gate": "sheet", "characters": ["hina"]} in nxt["waiting_for"]
    HumanService(project, "human:leaf").approve_sheet("hina", "sheet1")
    episode = load_episode(project)
    hina = next(c for c in episode.bible.characters if c["id"] == "hina")
    assert hina["locked"] and hina["refs"][-1] == {"asset": imported.data["asset"], "kind": "sheet", "approved_by": "human:leaf"}
    assert all(t["status"] == "done" for t in episode.tickets if t.get("gate") == "sheet")
    with pytest.raises(ApplyError, match="locked"):
        apply_ops(episode, [{"op": "upsert_character", "character": {"id": "hina", "name": "別人"}}], agent="ai:test")
    # a new bible keeps the approved sheet
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    hina = next(c for c in load_episode(project).bible.characters if c["id"] == "hina")
    assert hina["locked"] and hina["refs"][-1]["kind"] == "sheet"
    # panels with hina no longer wait for a sheet
    kinds = {(i["kind"], i["target"].get("character_id")) for i in agent.next("demo.genko", limit=50).data["items"]}
    assert ("make_sheet", "hina") not in kinds


def test_panel_fix_request_and_adoption_close_the_ticket(tmp_path: Path):
    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[1].leaf_frames()[0]  # a panel without characters
    cand, _ = _import(agent, 2, frame.id, _png((500, 300), RED))
    _commit(agent, [{"op": "adopt_candidate", "page": 2, "frame_id": frame.id, "candidate_id": cand}])
    HumanService(project, "human:leaf").comment(2, "空をもっと暗く", frame.id)
    items = agent.next("demo.genko", limit=50).data["items"]
    fix = next(i for i in items if i["kind"] == "fix_art")
    assert fix["target"] == {"page": 2, "frame_id": frame.id} and fix["comments"] == ["空をもっと暗く"]
    cand2, _ = _import(agent, 2, frame.id, _png((500, 300), BLUE))
    _commit(agent, [{"op": "adopt_candidate", "page": 2, "frame_id": frame.id, "candidate_id": cand2}])
    episode = load_episode(project)
    assert all(t["status"] == "done" for t in episode.tickets if t.get("kind") == "fix")
    panel = episode.pages[1]._find(frame.id).panel
    assert panel["adopted"]["art"] == cand2 and panel["adopt_history"][-1]["candidate"] == cand
    _commit(agent, [{"op": "unadopt", "page": 2, "frame_id": frame.id}])
    assert load_episode(project).pages[1]._find(frame.id).panel["adopted"]["art"] == cand


def test_gc_keeps_candidates_and_orphans(tmp_path: Path):
    import os
    import time

    from genko.maintenance import gc

    agent, project = _named(tmp_path)
    frame = load_episode(project).pages[0].leaf_frames()[0]
    _import(agent, 1, frame.id, _png((100, 100), RED))
    stray = agent.import_image("demo.genko", png_base64=_png((50, 50), BLUE)).data["asset"]
    old = time.time() - 3 * 24 * 3600
    for path in (project / "assets").rglob("*"):
        if path.is_file():
            os.utime(path, (old, old))
    result = gc(project, dry_run=True)
    removed = " ".join(result["removed"])
    assert stray[7:] in removed
    candidate = load_episode(project).pages[0]._find(frame.id).panel["candidates"][0]["asset"]
    assert candidate[7:] not in removed


def test_place_asset_needs_an_asset_ref_and_the_name(tmp_path: Path):
    agent, project = _named(tmp_path, approve=False)
    asset = agent.import_image("demo.genko", png_base64=_png((300, 300), RED)).data["asset"]
    frame = load_episode(project).pages[0].leaf_frames()[0]
    bad = agent.apply_ops("demo.genko", [{"op": "place_asset", "page": 1, "asset": "/etc/hosts"}], commit=True)
    assert not bad.ok
    early = agent.apply_ops("demo.genko", [{"op": "place_asset", "page": 1, "asset": asset, "frame_id": frame.id}], commit=True)
    assert not early.ok
    _commit(agent, [{"op": "place_asset", "page": 1, "asset": asset, "frame_id": frame.id, "to": "draft"}])
    missing = agent.apply_ops("demo.genko", [{"op": "place_asset", "page": 1, "asset": "sha256:" + "0" * 64, "to": "draft"}], commit=True)
    assert not missing.ok and "assets" in missing.data["error"]


def test_import_image_stays_inside_root_and_checks_the_bytes(tmp_path: Path):
    agent, _ = _named(tmp_path / "root", approve=False)
    outside = tmp_path / "x.png"
    Image.new("RGB", (10, 10)).save(outside)
    assert not agent.import_image("demo.genko", path=str(outside)).ok
    inside = tmp_path / "root" / "in.jpg"
    Image.new("RGB", (30, 20), RED).save(inside)
    result = agent.import_image("demo.genko", path="in.jpg")
    assert result.ok and result.data["px"] == [30, 20]  # stored as PNG
    with pytest.raises(ApplyError):
        agent.import_image("demo.genko", png_base64=base64.b64encode(b"not an image").decode())


def test_adopt_drafts_moves_m0_sidecars_into_the_project(tmp_path: Path):
    agent, project = _named(tmp_path / "a", approve=False)
    for n in range(1, 5):
        agent.record_review("demo.genko", n, 0.7, "ok")
    HumanService(project, "human:leaf").comment(3, "もっと間を")
    episode = load_episode(project)
    # build the M0 layout: sidecars under studio/drafts, nothing agent-side in project.json
    drafts = project / "studio" / "drafts"
    (drafts / "name").mkdir(parents=True)
    (drafts / "bible.json").write_text(json.dumps(_load("bible.json"), ensure_ascii=False), encoding="utf-8")
    (drafts / "script.json").write_text(json.dumps(_load("script.json"), ensure_ascii=False), encoding="utf-8")
    reviews = {}
    for page in episode.pages:
        plan = page.plan
        (drafts / "name" / f"p{page.index:03d}.json").write_text(json.dumps({
            "plan": plan["name"], "input_hash": plan["input_hash"], "seq": page.index,
            "slot_to_frame": plan["slot_to_frame"], "reading_order": plan["reading_order"], "by": "ai:test"}), encoding="utf-8")
        reviews[str(page.index)] = {"by": "ai:test", "score": 0.7, "notes": "ok", "input_hash": plan["input_hash"]}
    (drafts / "reviews.json").write_text(json.dumps(reviews), encoding="utf-8")
    (drafts / "requests.json").write_text(json.dumps([
        {"id": "c9", "kind": "comment", "page": 3, "text": "もっと間を", "by": "human:leaf", "status": "open", "seq": 9},
        {"id": "rq10", "kind": "approval", "gate": "name", "pages": [1, 2], "by": "ai:test", "status": "open", "seq": 10},
    ]), encoding="utf-8")
    expected = {p.index: {f.id: f.panel["slot"] for f in p.leaf_frames()} for p in episode.pages}
    episode.studio = {}
    episode.tickets = []
    for page in episode.pages:
        page.plan = None
        for frame in page.leaf_frames():
            frame.panel = None
    save_episode(episode, project)

    assert main(["studio", "adopt-drafts", str(project)]) == 0
    adopted = load_episode(project)
    assert adopted.studio["script"] == _load("script.json")
    assert adopted.studio["bible_doc"]["title"] == _load("bible.json")["title"]
    for page in adopted.pages:
        assert {f.id: f.panel["slot"] for f in page.leaf_frames()} == expected[page.index]
        assert page.plan["reviews"]["name"]["score"] == 0.7
    kinds = sorted((t["kind"], t.get("gate") or t.get("text")) for t in adopted.tickets)
    assert kinds == [("fix", "もっと間を"), ("gate", "name")]
    assert not drafts.exists() and (project / "studio" / "drafts.adopted").is_dir()
    items = StudioService(tmp_path / "a", "ai:test").next("demo.genko", limit=10).data["items"]
    assert any(i["kind"] == "revise_page" and i["target"] == {"page": 3} for i in items)


def test_plain_projects_render_placed_layers_without_a_studio(tmp_path: Path):
    project = tmp_path / "plain.genko"
    episode = new_episode("t", 1, 1, PageSpec.a4_mono())
    save_episode(episode, project)
    from genko.assets import AssetStore

    ref = AssetStore(project).put_bytes(base64.b64decode(_png((100, 100), RED)), ".png")
    episode = load_episode(project)
    frame = episode.pages[0].leaf_frames()[0]
    apply_ops(episode, [{"op": "place_asset", "page": 1, "asset": ref, "frame_id": frame.id}])
    save_episode(episode, project)
    episode = load_episode(project)
    image = render_page(episode.pages[0], 40, mode="print", episode=episode)
    assert _share(image, frame.rect, 40, RED) > 0.5
    shutil.rmtree(project / "assets")
    assert render_page(load_episode(project).pages[0], 40, mode="print", episode=episode)  # a missing asset does not crash


def test_bleed_panel_art_runs_to_the_paper_edge(tmp_path: Path):
    agent, project = _named(tmp_path)
    page = load_episode(project).pages[0]
    top = min(page.leaf_frames(), key=lambda f: f.rect.y)
    _commit(agent, [{"op": "set_frame", "page": 1, "frame_id": top.id, "bleed": True}])
    cand, _ = _import(agent, 1, top.id, _png((800, 400), RED))
    _commit(agent, [{"op": "adopt_candidate", "page": 1, "frame_id": top.id, "candidate_id": cand}])
    episode = load_episode(project)
    layer = next(layer for layer in episode.pages[0].layers if layer.kind == LayerKind.PLACED)
    assert layer.clip_to == "bleed"
    image = render_page(episode.pages[0], 60, mode="print", episode=episode)
    assert image.getpixel((1, 1))[:3] == RED  # the top-left paper corner
    assert image.getpixel((1, image.height - 2))[:3] == (255, 255, 255)  # below the panel: paper
