"""The Windows run that drew a 4-page battle manga from the day's AI news: a flash that painted a panel white,
an export job that died with its server, faces left behind when a picture moved, check's pages, a picture
short of the book's resolution, cutting a white margin off, references for a tool that takes two, a page
number over a picture."""

import json
import sys
import time
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode  # noqa: E402
from genko.studio import fxwords, genreq, jobs  # noqa: E402

import test_ai_feedback as fb  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


def test_a_flash_is_rays_over_the_picture_and_white_is_only_asked_for():
    assert fxwords.resolve("フラッシュ")["key"] == "uni_flash" and fxwords.resolve("flash")["key"] == "uni_flash"
    assert fxwords.resolve("白で塗る")["key"] == "white"


def _panel_with_art(tmp_path: Path, **take):
    from agents import images as fixture_images

    agent = fb._approved(tmp_path)
    frame = load_episode(tmp_path / "demo.genko").pages[0].leaf_frames()[0]
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
    inbox = tmp_path / "demo.genko" / "studio" / "inbox" / request["id"]
    (inbox / "a.png").write_bytes(fixture_images.panel(request["size"]["suggested_px"], request["figures"]))
    origin = {"kind": "agent", "tool_id": "xai:grok-imagine", "model": "grok-imagine"}
    done = agent.take_panel_art("demo.genko", request["id"], {"file": f"studio/inbox/{request['id']}/a.png", "origin": origin}, **take)
    return agent, frame, request, done


def test_white_is_never_painted_over_an_adopted_picture(tmp_path: Path):
    from genko.studio import finish

    agent, frame, _request, done = _panel_with_art(tmp_path)
    assert done.ok
    assert agent._ops("demo.genko", [{"op": "set_panel", "page": 1, "frame_id": frame.id, "set": {"fx": ["白で塗る", "フラッシュ"]}}]).ok
    episode = load_episode(tmp_path / "demo.genko")
    ops, notes = finish.plan(episode, episode.pages[0])
    kinds = [op.get("kind") for op in ops if op["op"] == "add_effect" and op.get("frame_id") == frame.id]
    assert "white" not in kinds and "uni_flash" in kinds
    skipped = [n for n in notes if n["kind"] == "effect_skipped"]
    assert skipped and "白" in skipped[0]["why"]
    assert any(n.get("label") == "ウニフラッシュ" for n in notes if n["kind"] == "add_effect")


def test_a_job_whose_server_stopped_is_lost_not_running(tmp_path: Path):
    folder = tmp_path / "studio" / "jobs"
    folder.mkdir(parents=True)
    old = time.time() - 400
    (folder / "job_x.json").write_text(json.dumps({"id": "job_x", "kind": "export", "status": "running", "started": old, "beat": old}))
    state = jobs.status(tmp_path, "job_x")
    assert state["status"] == "lost" and "export" in state["hint"]
    # a job still at work writes its beat and stays "running"
    reply = jobs.start(tmp_path, "export", lambda: (time.sleep(0.3), {"ok": True})[1], actor="ai:x", wait=0.01)
    assert jobs.status(tmp_path, reply["job"])["status"] == "running"


def test_a_moved_picture_takes_its_faces_along_and_check_keeps_to_the_pages(tmp_path: Path):
    agent, frame, request, done = _panel_with_art(tmp_path)
    regions = [{"kind": "face", "char": f["char"], "box01": f["head01"]} for f in request["figures"]]
    assert agent.report_regions("demo.genko", 1, frame.id, regions).ok
    before = load_episode(tmp_path / "demo.genko").pages[0]._find(frame.id).panel["regions"][0]["rect_mm"]
    assert agent._ops("demo.genko", [{"op": "set_placement", "page": 1, "frame_id": frame.id, "offset_mm": [6, 0]}]).ok
    after = load_episode(tmp_path / "demo.genko").pages[0]._find(frame.id).panel["regions"][0]["rect_mm"]
    assert after[0] == pytest.approx(before[0] + 6, abs=0.05) and after[1:] == pytest.approx(before[1:], abs=0.05)
    only = agent.check("demo.genko", pages=[2])
    assert only.ok and all(c.get("page") in (None, 2) for c in only.data["checks"]) and only.data["pages"] == [2]


def test_the_picture_can_be_cut_and_a_short_resolution_is_said(tmp_path: Path):
    agent, frame, request, done = _panel_with_art(tmp_path, upscale=False, crop01=[0.0, 0.0, 0.8, 1.0])
    assert done.ok, done.issues
    steps = [s["step"] for s in done.data["steps"]]
    assert steps[0] == "crop" and done.data["steps"][0]["file"].endswith("_crop.png")
    assert done.data["dpi_before"] and done.data["dpi_wanted"] == 600 and not done.data["upscaled"]
    assert "dpi_short" in {i.code for i in done.issues}
    bad = agent.take_panel_art("demo.genko", request["id"], {"file": f"studio/inbox/{request['id']}/a.png"}, crop01=[0.5, 0, 0.9, 1])
    assert not bad.ok and bad.data["stopped_at"] == "crop" and bad.issues[0].code == "bad_crop"


def test_one_sheet_for_a_tool_that_takes_two_references():
    def png(colour, size=(100, 160)):
        import io

        out = io.BytesIO()
        Image.new("RGB", size, colour).save(out, "PNG")
        return out.getvalue()

    files = {"refs/a_face.png": png((200, 0, 0)), "refs/a_sheet.png": png((0, 200, 0), (300, 200)), "refs/style_pilot.png": png((0, 0, 200))}
    names = genreq._by_priority(files)
    sheet = genreq._reference_sheet(files, names, [1536, 640])
    import io

    image = Image.open(io.BytesIO(sheet))
    assert abs(image.width / image.height - 1536 / 640) < 0.02
    colours = {image.getpixel((x, image.height // 2)) for x in range(0, image.width, 8)}
    assert (200, 0, 0) in colours and (0, 200, 0) in colours and (0, 0, 200) in colours
    assert genreq._reference_sheet({"refs/a_face.png": files["refs/a_face.png"]}, ["refs/a_face.png"], [512, 512]) is None


def test_a_page_number_over_a_picture_gets_a_white_edge():
    from genko import nombre
    from genko.models import PageSpec, new_episode

    episode = new_episode("t", 1, 2, PageSpec.b5_doujin())
    page = episode.pages[0]
    dark = Image.new("RGB", (800, 1100), (40, 40, 40))
    nombre.draw(dark, episode, page, 100)
    plain = Image.new("RGB", (800, 1100), (255, 255, 255))
    nombre.draw(plain, episode, page, 100)
    assert dark.convert("L").getextrema()[1] > 240  # (the white edge: nothing else on the dark page is white)
    assert plain.convert("L").getextrema()[0] < 100
