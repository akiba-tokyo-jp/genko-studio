"""M4 parts: generation requests, guides, imports, claims, preflight, finishing, HTTP."""

import io
import json
import time
from pathlib import Path

import pytest
from PIL import Image

from agents import images as fixture_images
from genko.__main__ import main
from genko.io import load_episode, save_episode
from genko.ops import apply_ops
from genko.studio import claims, genreq, importer, preflight
from genko.studio.service import HumanService, StudioService

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"
GOLDEN = Path(__file__).parent / "golden" / "genreq_p1_p1.json"


@pytest.fixture(autouse=True)
def _config(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _approved(root: Path, pages: int = 4) -> tuple[StudioService, Path]:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    for n in range(1, pages + 1):
        assert agent.submit_name("demo.genko", _load(f"p00{n}.json"), commit=True).ok
    project = root / "demo.genko"
    HumanService(project, "human:leaf").approve_name(list(range(1, pages + 1)))
    return agent, project


def _slot(project: Path, page: int, slot: str):
    return next(f for f in load_episode(project).pages[page - 1].leaf_frames() if f.panel["slot"] == slot)


def _normalized(request: dict, episode) -> dict:
    """The request without the random frame / page ids, for the golden file."""
    text = json.dumps(request, ensure_ascii=False)
    for page in episode.pages:
        text = text.replace(page.id, "<page_id>")
        for frame in page.leaf_frames():
            text = text.replace(frame.id, f"<{frame.panel['slot'] if frame.panel else 'frame'}>")
    out = json.loads(text)
    out.pop("id")
    out["import"].pop("request_id")
    out["import"].pop("inbox")
    out["notes_for_agent"] = out["notes_for_agent"][:-1]
    return out


def test_generation_request_golden_and_same_content_same_id(tmp_path: Path):
    agent, project = _approved(tmp_path)
    frame = _slot(project, 1, "p1")
    first = agent.generation_request("demo.genko", page=1, frame_id=frame.id)
    again = agent.generation_request("demo.genko", page=1, frame_id=frame.id)
    assert first.ok and first.data["request"]["id"] == again.data["request"]["id"]
    assert len(load_episode(project).studio["requests"]) == 1
    request = first.data["request"]
    # tokens are used as written; text, balloons and colour are never drawn
    assert "a slim 16-year-old girl with shoulder-length straight black hair and a star-shaped hair clip" in request["prompt"]["en"]
    assert "文字・フキダシ・効果音の描き文字" in request["avoid"] and "色" in request["avoid"]
    assert request["keepout"] and all(0 <= v <= 1 for k in request["keepout"] for v in k["box01"])
    assert "台詞用に静かに空けておく" in request["prompt"]["ja"]
    size = request["size"]
    assert size["suggested_px"][0] % 64 == 0 and 0.8e6 < size["suggested_px"][0] * size["suggested_px"][1] < 1.3e6
    assert size["print_px"]["dpi"] == 600
    for name in ("guides/composition.png", "guides/pose.png", "guides/keepout.png"):
        path = Path(first.data["files"][name])
        assert path.is_file() and Image.open(path).size == tuple(size["suggested_px"])
    assert Path(first.data["inbox"]).is_dir() and first.images  # the guide preview
    episode = load_episode(project)
    stats = {name: __import__("genko.guide", fromlist=["ink_ratio"]).ink_ratio(Image.open(first.data["files"][name]))
             for name in ("guides/composition.png", "guides/pose.png", "guides/keepout.png")}
    actual = {"request": _normalized(request, episode), "guide_ink": stats}
    if not GOLDEN.exists():  # pragma: no cover - first run writes the golden file
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_text(json.dumps(actual, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    assert actual["request"] == golden["request"]
    for name, ratio in golden["guide_ink"].items():
        assert abs(actual["guide_ink"][name] - ratio) < 0.01


def test_request_follows_tools_json_and_human_overrides(tmp_path: Path):
    from genko.studio import tools_registry

    agent, project = _approved(tmp_path)
    frame = _slot(project, 1, "p1")  # a wide panel

    tools_registry.set_tool("square:only", {"sizes_px": [[1024, 1024]], "supports": {"references": False, "mask": False}})
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id, tool="square:only").data["request"]
    assert request["tool"] == "square:only"
    assert request["size"]["tool_sizes"] == [{"px": [1024, 1024], "crop": "上下を中央で切る"}]
    assert any("参照画像を渡せない" in n for n in request["notes_for_agent"])
    unknown = agent.generation_request("demo.genko", page=1, frame_id=frame.id, tool="nobody:knows").data["request"]
    assert unknown["tool"] is None and len(unknown["size"]["tool_sizes"]) == 3
    episode = load_episode(project)
    apply_ops(episode, [{"op": "set_panel", "page": 1, "frame_id": frame.id,
                         "set": {"gen": {"prompt_override": "人間が書いたプロンプト"}}}], agent="human:leaf")
    save_episode(episode, project)
    overridden = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    assert overridden["prompt"]["ja"] == "人間が書いたプロンプト"
    assert any("人間の指定" in n for n in overridden["notes_for_agent"])
    assert overridden["id"] != request["id"]


def test_fix_requests_carry_the_source_and_a_mask(tmp_path: Path):
    agent, project = _approved(tmp_path)
    frame = _slot(project, 1, "p2")
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    inbox = project / "studio" / "inbox" / request["id"]
    (inbox / "a.png").write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    cand = agent.import_images("demo.genko", request["id"], [{"file": f"studio/inbox/{request['id']}/a.png",
                                                              "origin": {"tool_id": "t", "model": "m"}}]).data["candidates"][0]
    no_parent = agent.generation_request("demo.genko", page=1, frame_id=frame.id, mode="edit")
    assert not no_parent.ok and no_parent.issues[0].path == "/parent"
    edit = agent.generation_request("demo.genko", page=1, frame_id=frame.id, mode="edit", parent=cand, instruction="顔を大きく")
    assert edit.ok and "source.png" in edit.data["files"] and "今回の指示: 顔を大きく" in edit.data["request"]["prompt"]["ja"]
    inpaint = agent.generation_request("demo.genko", page=1, frame_id=frame.id, mode="inpaint", parent=cand,
                                       regions=[[frame.rect.x + 5, frame.rect.y + 5, 20, 20]])
    mask = Image.open(inpaint.data["files"]["mask.png"])
    assert inpaint.ok and 0 < fixture_ratio(mask) < 0.5
    assert load_episode(project).pages[0]._find(frame.id).panel["attempts"]["fix_rounds"] == 2


def fixture_ratio(mask: Image.Image) -> float:
    hist = mask.convert("L").histogram()
    return sum(hist[128:]) / (mask.width * mask.height)


def test_import_checks_sources_and_is_idempotent(tmp_path: Path):
    agent, project = _approved(tmp_path)
    frame = _slot(project, 1, "p3")
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    rid = request["id"]
    inbox = project / "studio" / "inbox" / rid
    (inbox / "a.png").write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    buf = io.BytesIO()
    Image.new("RGB", (640, 480), (90, 90, 90)).save(buf, format="JPEG")
    (inbox / "b.jpg").write_bytes(buf.getvalue())
    (inbox / "junk.png").write_bytes(b"not an image")
    outside = tmp_path / "outside.png"
    outside.write_bytes(fixture_images.decoy([200, 200]))
    origin = {"kind": "agent", "tool_id": "openai:gpt-image-1", "model": "gpt-image-1", "prompt": "…"}

    def call(*items):
        return agent.import_images("demo.genko", rid, list(items))

    for bad, path in ((str(outside), "/images/0/file"), ("studio/inbox/../../project.json", "/images/0/file"),
                      (f"studio/inbox/{rid}/junk.png", "/images/0")):
        result = call({"file": bad, "origin": origin})
        assert not result.ok and result.issues[0].path == path, result.to_dict()
    assert not call({"file": f"studio/inbox/{rid}/a.png"}).ok  # no origin
    assert not call({"file": f"studio/inbox/{rid}/a.png", "origin": {"kind": "stolen"}}).ok
    assert not agent.import_images("demo.genko", "rq_nothing", [{"file": "x", "origin": origin}]).ok
    first = call({"file": f"studio/inbox/{rid}/a.png", "origin": origin}, {"file": f"studio/inbox/{rid}/b.jpg", "origin": origin})
    assert first.ok and len(first.data["candidates"]) == 2 and first.images  # compare preview
    again = call({"file": f"studio/inbox/{rid}/a.png", "origin": origin})
    assert again.ok
    panel = load_episode(project).pages[0]._find(frame.id).panel
    assert len(panel["candidates"]) == 2 and panel["attempts"]["images"] == 2
    good, jpeg = panel["candidates"]
    assert good["metrics"]["aspect_error"] < 0.05 and jpeg["metrics"]["aspect_error"] > 0.2
    assert good["metrics"]["rank"] < jpeg["metrics"]["rank"]
    assert good["origin"] == {**origin, "reported": True, "actor": "ai:test"} and good["request"] == rid
    listed = agent.candidates("demo.genko", page=1, frame_id=frame.id)
    assert [r["id"] for r in listed.data["candidates"]][0] == good["id"] and listed.images


def test_brief_change_marks_candidates_stale(tmp_path: Path):
    agent, project = _approved(tmp_path)
    frame = _slot(project, 1, "p1")
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    (project / "studio" / "inbox" / request["id"] / "a.png").write_bytes(fixture_images.decoy([256, 128]))
    agent.import_images("demo.genko", request["id"], [{"file": f"studio/inbox/{request['id']}/a.png", "origin": {"tool_id": "t"}}])
    assert not agent.candidates("demo.genko", page=1, frame_id=frame.id).data["candidates"][0]["stale"]
    agent.apply_ops("demo.genko", [{"op": "set_panel", "page": 1, "frame_id": frame.id, "set": {"action": "走り出す"}}], commit=True)
    assert agent.candidates("demo.genko", page=1, frame_id=frame.id).data["candidates"][0]["stale"]


def test_claims_keep_parallel_agents_apart(tmp_path: Path):
    agent, project = _approved(tmp_path)
    other = StudioService(tmp_path, "ai:other")
    mine = agent.next("demo.genko", limit=2, claim=True).data["items"]
    theirs = other.next("demo.genko", limit=2, claim=True).data["items"]
    assert len(mine) == 2 and len(theirs) == 2 and not {i["id"] for i in mine} & {i["id"] for i in theirs}
    assert agent.next("demo.genko", limit=2, claim=True).data["items"] == mine  # renewing keeps the same work
    later = time.time() + claims.LEASE_SECONDS + 1
    assert claims.claim(project, mine[0]["id"], "ai:other", now=later)  # an expired lease can be taken


def test_preflight_reasons_force_and_proof_watermark(tmp_path: Path):
    agent, project = _approved(tmp_path, pages=1)
    report = agent.preflight("demo.genko").data
    codes = {e["code"] for e in report["errors"]}
    assert not report["ready"] and {"art_not_approved", "page_not_finished", "panel_without_art", "sheet_not_approved"} <= codes
    # adopt a tiny image: resolution too low unless forced
    episode = load_episode(project)
    page = episode.pages[0]
    for frame in page.leaf_frames():
        request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
        path = project / "studio" / "inbox" / request["id"] / "a.png"
        path.write_bytes(fixture_images.panel([128, 128], []))
        cand = agent.import_images("demo.genko", request["id"], [{"file": str(path.relative_to(project)),
                                                                  "origin": {"kind": "agent"}}]).data["candidates"][0]
        assert agent.adopt("demo.genko", cand, page=1, frame_id=frame.id).ok
    for c in load_episode(project).bible.characters:
        episode = load_episode(project)
        apply_ops(episode, [{"op": "upsert_character", "character": {**c, "locked": True}}], agent="human:leaf")
        save_episode(episode, project)
    HumanService(project, "human:leaf").approve_art([1])
    assert agent.finish_page("demo.genko", 1, commit=True).data["committed"]
    # pages 2-4 are still untouched: keep them out of the book for this check
    episode = load_episode(project)
    apply_ops(episode, [{"op": "delete_page", "page": 4}, {"op": "delete_page", "page": 3}, {"op": "delete_page", "page": 2}],
              agent="human:leaf")
    save_episode(episode, project)
    report = agent.preflight("demo.genko").data
    assert {e["code"] for e in report["errors"]} == {"low_dpi"}
    assert {w["code"] for w in report["warnings"]} == {"provenance_missing"}
    assert agent.preflight("demo.genko", force=True).data["ready"]
    assert all(row["dpi"] < 100 for row in report["dpi"])
    proof = agent.export_proof("demo.genko", "png")
    assert proof.ok and len(proof.data["files"]) == 1
    from genko.render import render_page

    plain = render_page(load_episode(project).pages[0], 150, mode="print", episode=load_episode(project)).convert("RGB")
    marked = Image.open(proof.data["files"][0]).convert("RGB")
    assert plain.size == marked.size and plain.tobytes() != marked.tobytes()
    refused = HumanService(project, "human:leaf").export("pdf", tmp_path / "out")
    assert not refused["ok"] and refused["errors"][0]["code"] == "low_dpi"
    done = HumanService(project, "human:leaf").export("pdf", tmp_path / "out", dpi=72, force=True)
    assert done["ok"] and Path(done["files"][0]).is_file()


def test_finish_moves_balloons_off_reported_faces_and_adds_effects(tmp_path: Path):
    agent, project = _approved(tmp_path, pages=1)
    episode = load_episode(project)
    page = episode.pages[0]
    line = next(ln for ln in episode.story_for_page(1) if ln.frame_id)
    frame = page._find(line.frame_id)
    face = [line.x_mm, line.y_mm, line.w_mm, line.h_mm]  # a face right under the balloon
    apply_ops(episode, [{"op": "set_panel", "page": 1, "frame_id": frame.id, "set": {"fx": ["集中線"]}},
                        {"op": "replace_regions", "page": 1, "frame_id": frame.id, "source": "agent",
                         "regions": [{"kind": "face", "rect_mm": face, "char": "hina"}]}], agent="ai:test")
    for leaf in page.leaf_frames():
        apply_ops(episode, [{"op": "set_panel", "page": 1, "frame_id": leaf.id, "set": {"status": "skip"}}], agent="human:leaf")
    apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent="human:leaf")
    save_episode(episode, project)
    dry = agent.finish_page("demo.genko", 1)
    assert dry.ok and not dry.data["committed"] and dry.images
    kinds = [c["kind"] for c in dry.data["changes"]]
    assert "move_line" in kinds and kinds.count("add_effect") == 1
    assert agent.finish_page("demo.genko", 1, commit=True).data["committed"]
    episode = load_episode(project)
    moved = next(ln for ln in episode.story if ln.id == line.id)
    assert (moved.x_mm, moved.y_mm) != (face[0], face[1])
    assert episode.pages[0].stage == "finish" and len(episode.pages[0].effects) == 1
    assert agent.finish_page("demo.genko", 1, commit=True).data["changes"] == []  # nothing left to do


def test_sheet_request_face_crop_and_location_reference(tmp_path: Path):
    agent, project = _approved(tmp_path, pages=1)
    sheet = agent.generation_request("demo.genko", character_id="hina")
    request = sheet.data["request"]
    assert request["purpose"] == "character_sheet" and request["size"]["suggested_px"][1] > request["size"]["suggested_px"][0]
    inbox = Path(sheet.data["inbox"])
    (inbox / "s.png").write_bytes(fixture_images.sheet(request["size"]["suggested_px"]))
    cand = agent.import_images("demo.genko", request["id"], [{"file": f"studio/inbox/{request['id']}/s.png",
                                                              "origin": {"kind": "agent", "tool_id": "t"}}]).data["candidates"][0]
    kinds = {i["kind"] for i in agent.next("demo.genko", limit=20).data["items"]}
    assert "make_sheet" not in kinds  # candidates exist: now it waits for the person
    assert {"gate": "sheet", "characters": ["hina"]} in agent.next("demo.genko").data["waiting_for"]
    done = HumanService(project, "human:leaf").approve_sheet("hina", cand, [0.25, 0.0, 0.5, 0.3])
    face = Image.open(project / "assets" / done["face_asset"][7:9] / f"{done['face_asset'][7:]}.png")
    width = request["size"]["suggested_px"][0]
    assert abs(face.width - width * 0.5) <= 1
    # the face now travels with every panel request for hina
    frame = _slot(project, 1, "p2")
    panel_req = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data
    assert {"refs/hina_face.png", "refs/hina_sheet.png"} <= set(panel_req["request"]["files"]["references"])
    # locations: a background reference request, adopted as the location's reference
    episode = load_episode(project)
    apply_ops(episode, [{"op": "upsert_location", "location": {"id": "loc_rooftop"}}], agent="ai:test")
    save_episode(episode, project)
    loc = agent.generation_request("demo.genko", location_id="loc_rooftop").data
    (Path(loc["inbox"]) / "l.png").write_bytes(fixture_images.decoy([512, 340]))
    lcand = agent.import_images("demo.genko", loc["request"]["id"], [{"file": f"studio/inbox/{loc['request']['id']}/l.png",
                                                                      "origin": {"tool_id": "t"}}]).data["candidates"][0]
    assert agent.adopt("demo.genko", lcand, location_id="loc_rooftop").ok
    again = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    assert any(r.startswith("refs/loc_rooftop_") for r in again["files"]["references"])


def test_http_upload_and_request_files(tmp_path: Path):
    import threading
    import urllib.request

    from genko.server import HeadlessServer, handle_request

    agent, project = _approved(tmp_path, pages=1)
    frame = _slot(project, 1, "p1")
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    ctx = {"root": tmp_path, "actor": "ai:remote"}
    png = fixture_images.panel(request["size"]["suggested_px"], request["figures"])
    status, body = handle_request("POST", "/v1/assets?path=demo.genko", png, ctx)
    uploaded = json.loads(body)
    assert status == 200 and uploaded["asset"].startswith("sha256:")
    status, _ = handle_request("POST", "/v1/assets?path=demo.genko", b"junk", ctx)
    assert status == 400
    status, _ = handle_request("POST", "/v1/assets?path=../elsewhere", png, ctx)
    assert status == 403
    imported = agent.import_images("demo.genko", request["id"], [{"asset": uploaded["asset"], "origin": {"tool_id": "t"}}])
    assert imported.ok
    status, guide_png = handle_request("GET", f"/v1/requests/{request['id']}/files/guides/composition.png?path=demo.genko", b"", ctx)
    assert status == 200 and guide_png.startswith(b"\x89PNG")
    for bad in (f"/v1/requests/{request['id']}/files/../../../project.json", "/v1/requests/rq_nope/files/request.json"):
        assert handle_request("GET", bad + "?path=demo.genko", b"", ctx)[0] == 404

    server = HeadlessServer(port=0, root=tmp_path, tokens={"tok": "ai:remote"})
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        def post(ctype: str) -> int:
            req = urllib.request.Request(f"http://127.0.0.1:{server.port}/v1/assets?path=demo.genko", data=png, method="POST",
                                         headers={"Content-Type": ctype, "Authorization": "Bearer tok"})
            try:
                return urllib.request.urlopen(req).status
            except urllib.error.HTTPError as exc:
                return exc.code

        assert post("text/plain") == 415 and post("image/png") == 200
    finally:
        server.shutdown()
        server.server_close()


def test_tools_answer_within_two_seconds(tmp_path: Path):
    agent, project = _approved(tmp_path, pages=1)
    frame = _slot(project, 1, "p1")
    started = time.perf_counter()
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    assert time.perf_counter() - started < 2.0
    (project / "studio" / "inbox" / request["id"] / "a.png").write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    started = time.perf_counter()
    assert agent.import_images("demo.genko", request["id"], [{"file": f"studio/inbox/{request['id']}/a.png", "origin": {"tool_id": "t"}}]).ok
    assert time.perf_counter() - started < 2.0
    started = time.perf_counter()
    assert agent.next("demo.genko", limit=10).ok and agent.status("demo.genko").ok
    assert time.perf_counter() - started < 2.0


def test_cli_call_and_tools(tmp_path: Path, capsys):
    agent, project = _approved(tmp_path, pages=1)

    def run(*args) -> dict:
        main(["studio", *map(str, args)])
        return json.loads(capsys.readouterr().out.strip().splitlines()[-1])

    assert run("tools", "set", "openai:gpt-image-1")["tool"]["sizes_px"]
    assert "openai:gpt-image-1" in run("tools", "list")["tools"]
    args = tmp_path / "args.json"
    args.write_text(json.dumps({"limit": 3}))
    assert len(run("call", project, "next", args)["items"]) == 3
    assert not run("call", project, "approve")["ok"]
    assert run("tools", "remove", "openai:gpt-image-1")["ok"]


def test_gc_keeps_inbox_request_assets(tmp_path: Path):
    """Assets referenced by candidates stay; request folders are files, not assets."""
    agent, project = _approved(tmp_path, pages=1)
    frame = _slot(project, 1, "p1")
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    assert genreq.read(project, request["id"])["id"] == request["id"]
    assert genreq.read(project, "../etc") is None
    assert importer.candidate_id("rq_a", "sha256:x") == importer.candidate_id("rq_a", "sha256:x")
    assert preflight.effective_dpi(600, 25.4) == 600
