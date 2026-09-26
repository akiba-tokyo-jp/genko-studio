"""The quality report on the test story: print files, balloons by their speakers, the book's lettering, effect words,
props and the panel before, lighter faces in the tones, bold panel shapes, and the title page."""

import json
import os
import sys
import zlib
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.studio import layout, lint  # noqa: E402
from genko.studio.letter import place_page, placements_to_ops  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _book(pages=2):
    ep = new_episode("t", 1, pages, PageSpec.b4_comic())
    for page in ep.pages:
        page.name_ok = True
    return ep


# --- 1. print files --------------------------------------------------------------------------------------------


def _pdf_images(data: bytes) -> list[dict]:
    """The picture objects of a PDF written by Genko: their dictionaries and inflated bytes."""
    out = []
    pos = 0
    while True:
        at = data.find(b"/Subtype /Image", pos)
        if at < 0:
            return out
        head_start = data.rfind(b"<<", 0, at)
        head_end = data.find(b">>", at)
        head = data[head_start:head_end].decode("latin-1")
        start = data.find(b"stream\n", head_end) + len(b"stream\n")
        length = int(head.split("/Length ")[1].split()[0])
        out.append({"head": head, "data": zlib.decompress(data[start:start + length])})
        pos = start + length


def test_a_monochrome_pdf_is_lossless_grey_with_its_trim_box(tmp_path):
    from genko.export import export_print

    ep = _book(1)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "テスト", "x_mm": 60, "y_mm": 60}])
    pdf = export_print(ep, tmp_path / "a", fmt="pdf", dpi=40, area="bleed")[0].read_bytes()
    assert b"/DCTDecode" not in pdf and b"/FlateDecode" in pdf
    assert b"/TrimBox" in pdf and b"/BleedBox" in pdf
    image = _pdf_images(pdf)[0]
    assert "/DeviceGray" in image["head"] and "/BitsPerComponent 8" in image["head"]
    width = int(image["head"].split("/Width ")[1].split()[0])
    height = int(image["head"].split("/Height ")[1].split()[0])
    assert len(image["data"]) == width * height  # (one grey byte a pixel: nothing lost)
    two = export_print(ep, tmp_path / "b", fmt="pdf", dpi=40, color="bitonal")[0].read_bytes()
    assert "/BitsPerComponent 1" in _pdf_images(two)[0]["head"]


def test_preflight_stops_a_monochrome_book_written_in_colour(tmp_path):
    from genko.studio.preflight import check

    ep = _book(1)
    report = check(ep, tmp_path, color="rgb")
    assert any(e["code"] == "mono_as_colour" for e in report["errors"])
    assert not any(e["code"] == "mono_as_colour" for e in check(ep, tmp_path, color="auto")["errors"])


# --- 2 and 3. balloons by their speakers, the book's lettering -------------------------------------------------


BIBLE = {"title": "雨の日", "author": "山田", "characters": [{"id": "hina", "name": "ひな"}, {"id": "sota", "name": "そうた"}]}


def _two_person_page(lines, template="splash", extra=None, bible=None):
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    plan = {"page": 1, "template": template, "panels": [{
        "slot": "p1", "shot": "MS", "characters": [{"id": "hina", "pos": "left", "scale": 0.8},
                                                   {"id": "sota", "pos": "right", "scale": 0.8}],
        "lines": lines, **(extra or {})}]}
    compiled = layout.apply_layout(ep, 1, plan)
    speakers = {"b1": "hina", "b2": "sota", "b3": None}
    placements, issues = place_page(plan, compiled, bible or BIBLE, speakers)
    return ep, plan, compiled, placements, issues


def test_a_balloon_sits_by_its_speaker_and_its_tail_reaches_them():
    from genko.studio.blocking import figures

    ep, plan, compiled, placements, issues = _two_person_page([{"beat_id": "b1", "balloon": "speech", "breaks": ["ちっちゃ!"]}])
    rect = compiled.leaf_rects_mm["p1"]
    hina, sota = figures(plan["panels"][0], rect)
    p = placements[0]
    centre = p.x_mm + p.w_mm / 2
    hina_x = hina.head[0] + hina.head[2] / 2
    sota_x = sota.head[0] + sota.head[2] / 2
    assert abs(centre - hina_x) < abs(centre - sota_x)  # (on the speaker's side, not the right corner)
    tx, ty = p.tail
    hx, hy, hw, hh = hina.head
    gap = max(hx - tx, tx - (hx + hw), hy - ty, ty - (hy + hh), 0)
    assert gap < 4.0  # (the point ends by the face, not 7 mm out of the balloon)
    assert not (hx < tx < hx + hw and hy < ty < hy + hh)  # (and never on it)
    op = placements_to_ops(1, placements, compiled)[0]
    assert op["style"]["speaker_id"] == "hina"


def test_a_thought_points_its_bubbles_at_the_thinker():
    ep, plan, compiled, placements, issues = _two_person_page([{"beat_id": "b2", "balloon": "thought", "breaks": ["同じクラス", "だけど"]}])
    assert placements[0].tail is not None


def test_shouts_are_big_and_whispers_small_and_the_book_can_change_them():
    ep, plan, compiled, placements, issues = _two_person_page([
        {"beat_id": "b1", "balloon": "shout", "breaks": ["よくない!"]},
        {"beat_id": "b2", "balloon": "whisper", "breaks": ["うん"]},
        {"beat_id": "b3", "balloon": "narration", "breaks": ["雨だった"]}])
    shout, whisper, narration = placements
    assert shout.style["size_mm"] > 6 and shout.style["weight"] == "bold"
    assert whisper.style["size_mm"] < 4.5
    assert not (narration.style or {}).get("font")  # (no font forced on the book)
    bible = {**BIBLE, "lettering": {"narration": {"font": "maru", "scale": None, "weight": None}, "shout": {"scale": 1.0}}}
    ep, plan, compiled, placements, issues = _two_person_page([
        {"beat_id": "b1", "balloon": "shout", "breaks": ["よくない!"]},
        {"beat_id": "b3", "balloon": "narration", "breaks": ["雨だった"]}], bible=bible)
    assert "size_mm" not in placements[0].style and placements[1].style["font"] == "maru"
    assert lint.lint_bible({**bible, "lettering": {"narration": {"font": "nothing-like-it"}}})[-1].code == "unknown_font"


# --- 4. effect words ---------------------------------------------------------------------------------------------


def test_effect_words_are_known_warned_and_written_into_the_art_request():
    from genko.studio import fxwords

    assert fxwords.resolve("雨")["kind"] == "rain" and fxwords.resolve("水しぶき")["kind"] == "art"
    assert fxwords.resolve("汗")["kind"] == "mark" and fxwords.resolve("集中線")["kind"] == "effect"
    plan = _load("p001.json")
    plan["panels"][0]["fx"] = ["雨", "ぴかぴかビーム"]
    issues = lint.lint_name_plan(plan, _load("script.json"), _load("bible.json"), 4)
    assert [i.path for i in issues if i.code == "fx_unknown"] == ["/panels/0/fx/1"]
    ja, en = fxwords.art_words(["雨", "ぴかぴかビーム"])
    assert "rain" in en[0] and "(in Japanese)" in en[1]
    assert fxwords.emphasis_words(0.9) and fxwords.emphasis_words(0.5) is None


def test_the_finish_draws_marks_by_the_face_rain_and_lines():
    from genko.studio.finish import plan as finish_plan

    ep = _book(1)
    page = ep.pages[0]
    frame = page.leaf_frames()[0]
    r = frame.rect
    frame.panel = {"status": "adopted", "fx": ["汗", "雨", "集中線", "水しぶき", "なぞの音"],
                   "regions": [{"kind": "face", "char": "hina", "rect_mm": [r.x + 40, r.y + 40, 30, 30]}]}
    ops, notes = finish_plan(ep, page)
    kinds = [op["op"] for op in ops]
    assert "add_effect" in kinds and "stamp_material" in kinds and kinds.count("add_stroke") >= 12
    stamp = next(op for op in ops if op["op"] == "stamp_material")
    assert stamp["material_id"] == "mark-汗" and stamp["x_mm"] > r.x + 70  # (beside the face)
    assert {n["kind"] for n in notes} >= {"fx_in_art", "fx_unknown", "add_mark", "add_rain"}
    apply_ops(ep, ops)
    page = ep.pages[0]  # (apply_ops puts new objects in place)
    assert any(layer.title == "効果（仕上げ）" and len(layer.strokes) >= 12 for layer in page.layers)
    again, _ = finish_plan(ep, page)
    assert not any(op["op"] in ("stamp_material", "add_stroke") for op in again)  # (once)


def test_a_sound_effect_goes_to_its_source():
    ep, plan, compiled, placements, issues = _two_person_page(
        [{"beat_id": "b3", "balloon": "sfx", "breaks": ["バシャッ"]}], extra={"sfx_at": [0.3, 0.85]})
    x, y, w, h = compiled.leaf_rects_mm["p1"]
    p = placements[0]
    assert abs(p.x_mm + p.w_mm / 2 - (x + w * 0.3)) < 25 and p.y_mm + p.h_mm / 2 > y + h * 0.6


# --- 5. props and the panel before ---------------------------------------------------------------------------------


def test_props_and_the_panel_before_go_with_the_art_request(tmp_path):
    import test_m5 as m5

    from genko.assets import AssetStore

    agent, project, human = m5._project(tmp_path / "b")
    m5._sheets(agent, human)
    ep = load_episode(project)
    umbrella = AssetStore(project).put_bytes(m5.fixture_images.sheet([256, 256]), ".png")
    assert human._apply([{"op": "upsert_prop", "prop": {"id": "kasa", "name": "傘", "desc": "うさぎ柄の透明傘"}},
                         {"op": "attach_reference", "target": {"prop_id": "kasa"}, "asset": umbrella}])
    frames = ep.pages[0].leaf_frames()
    m5._art(agent, project, 1, frames[0].id)
    human._apply([{"op": "set_panel", "page": 1, "frame_id": frames[1].id, "set": {"props": ["kasa"]}}])
    req = agent.generation_request("demo.genko", page=1, frame_id=frames[1].id)
    assert req.ok, req.to_dict()
    refs = req.data["request"]["files"]["references"]
    assert any(r.startswith("refs/prop_kasa_") for r in refs) and "refs/previous_panel.png" in refs
    assert "傘" in req.data["request"]["prompt"]["ja"]


def test_the_sheet_request_carries_the_written_look(tmp_path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path / "b")
    req = agent.generation_request("demo.genko", character_id="hina").data
    path = Path(req["inbox"]) / "s.png"
    path.write_bytes(m5.fixture_images.sheet(req["request"]["size"]["suggested_px"]))
    agent.import_images("demo.genko", req["request"]["id"], [{"file": f"studio/inbox/{req['request']['id']}/s.png",
                                                             "origin": {"tool_id": m5.TOOL}}])
    asked = agent.request_approval("demo.genko", "sheet", [], "", "hina").to_dict()
    assert asked["written"] and "髪" in asked["written"]


# --- 6. lighter faces ---------------------------------------------------------------------------------------------


def test_faces_keep_lighter_in_the_tones():
    from genko import screentone

    grey = Image.new("L", (200, 200), 150)
    faces = Image.new("L", (200, 200), 0)
    faces.paste(255, (50, 50, 150, 150))
    finish = screentone.Finish()
    plain = screentone.finish_gray(grey, finish, 300, screen=True)
    lit = screentone.finish_gray(grey, finish, 300, screen=True, faces=faces)
    inside = (70, 70, 130, 130)
    ink = lambda im: sum(1 for v in im.crop(inside).getdata() if v < 128)  # noqa: E731
    assert ink(lit) < ink(plain) * 0.7
    outside = (0, 0, 40, 40)
    assert list(lit.crop(outside).getdata()) == list(plain.crop(outside).getdata())


# --- 7. panel shapes --------------------------------------------------------------------------------------------


def test_bleed_slant_and_spread_in_the_plan():
    ep = new_episode("t", 1, 4, PageSpec.b4_comic())
    plan = {"page": 2, "template": "2tier_wide", "spread": True,
            "panels": [{"slot": "p1", "bleed": True, "slant": 12}, {"slot": "p2"}]}
    compiled = layout.apply_layout(ep, 2, plan)
    page = ep.pages[1]
    first, second = page.leaf_frames()
    assert first.bleed and first.poly and page.spread_with == 3
    assert any(op.get("tilt_mm") == 12 for op in compiled.ops)
    for name in ("reveal_top", "finale_bleed", "action_slant", "reveal_bleed", "title_top"):
        assert name in layout.templates()


def test_small_big_moments_are_pointed_out():
    plan = _load("p001.json")  # (tiers 0.35 / 0.65 split 0.55 and 0.45: the last panel is under 3 tenths)
    plan["page"] = 4
    plan["panels"][2]["emphasis"] = 0.9
    codes = {i.code for i in lint._size_hints(plan, 4, 4)}
    assert {"finale_small", "emphasis_small"} <= codes
    plan["panels"][2]["bleed"] = True
    assert "finale_small" not in {i.code for i in lint._size_hints(plan, 4, 4)}
    plan["turn_role"] = "reveal"
    plan["tiers"][0]["h"] = 0.2
    assert "reveal_small" in {i.code for i in lint._size_hints(plan, 2, 4)}


# --- 8. the title page ------------------------------------------------------------------------------------------


def test_a_title_page_sets_the_title_and_author():
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    plan = {"page": 1, "template": "reveal_bleed", "title": True, "panels": [{"slot": "p1", "shot": "LS", "characters": [], "lines": [
        {"beat_id": "b1", "balloon": "speech", "breaks": ["……あ。"]}]}]}
    compiled = layout.apply_layout(ep, 1, plan)
    placements, issues = place_page(plan, compiled, BIBLE, {"b1": "hina"})
    title, author, line = placements
    assert title.text == "雨の日" and author.text == "山田" and title.style["size_mm"] > author.style["size_mm"]
    assert title.x_mm > line.x_mm  # (the title down the right edge, the line after it)
    ops = placements_to_ops(1, placements, compiled)
    apply_ops(ep, ops)
    assert [line.text for line in ep.story_for_page(1)][:2] == ["雨の日", "山田"]
    missing = lint.lint_name_plan({**_load("p001.json"), "title": True}, _load("script.json"), {**_load("bible.json"), "author": None}, 4)
    assert any(i.code == "author_missing" for i in missing)
