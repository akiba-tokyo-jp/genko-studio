"""C: the words outside the balloons: 呟き (aside: hand-lettered, small, tilted, no balloon), 名札 (a character's
label, set across in a small rounded box), and 飾り枠 (a dotted box, a box laid with a light tone, a double line
with its corners marked) — all in the name plan, so an agent can ask for them."""

from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import render_page
from genko.studio import layout
from genko.studio.letter import measure, place_page, placements_to_ops
from genko.studio.jsonschema_lite import validate
from genko.studio.schemas import SCHEMAS, fill_nulls


def _plan():
    return {"page": 1, "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": None}]}],
            "panels": [{"slot": "p1", "shot": "MS", "characters": [{"id": "a", "pos": "right", "tag": "司会 サワっち"}],
                        "lines": [{"beat_id": "b1", "balloon": "fancy_box", "breaks": ["今月の", "テーマ"]},
                                  {"beat_id": "b2", "balloon": "aside", "breaks": ["あぁあ…", "やってもた"]},
                                  {"beat_id": "b3", "balloon": "rounded", "breaks": ["つまり", "こういうこと"]}]}]}


def _placed():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    plan = _plan()
    compiled = layout.apply_layout(ep, 1, plan, agent="ai:test")
    placements, issues = place_page(plan, compiled, {"characters": [{"id": "a", "name": "A"}]}, {"b1": "a", "b2": "a", "b3": "a"})
    return ep, compiled, placements, issues


def test_the_plan_takes_the_new_kinds_and_a_tag():
    import json
    from pathlib import Path

    plan = json.loads((Path(__file__).parent / "fixtures" / "studio" / "demo4" / "p001.json").read_text(encoding="utf-8"))
    schema = SCHEMAS["name_plan@1"]
    panel = next(p for p in plan["panels"] if p.get("lines") and p.get("characters"))
    panel["characters"][0]["tag"] = "司会 サワっち"
    for line, kind in zip(panel["lines"], ("aside", "fancy_box", "tone_box")):
        line["balloon"] = kind
    assert validate(fill_nulls(plan, schema), schema) == []


def test_an_aside_is_bare_hand_lettering_that_leans():
    _ep, _compiled, placements, issues = _placed()
    assert [i for i in issues if i.severity == "error"] == []
    aside = next(p for p in placements if p.text.startswith("あぁあ"))
    assert aside.balloon == "none" and aside.tail is None
    assert aside.style["font"] == "hand" and abs(aside.style["rotate_deg"]) == 6.0


def test_a_tag_is_set_across_by_its_character_and_frames_have_no_tail():
    ep, compiled, placements, _issues = _placed()
    tag = next(p for p in placements if p.text == "司会 サワっち")
    assert tag.balloon == "rounded" and tag.wrap == "horizontal" and tag.w_mm > tag.h_mm
    frame = next(p for p in placements if p.balloon == "fancy_box")
    assert frame.tail is None
    apply_ops(ep, placements_to_ops(1, placements, compiled))
    assert any(line.wrap == "horizontal" for line in ep.story)


def test_frames_draw_differently_and_keep_their_words_further_in():
    assert measure(["テーマ"], "fancy_box")[0] > measure(["テーマ"], "box")[0]
    looks = {}
    for kind in ("box", "dotted_box", "tone_box", "fancy_box"):
        ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
        apply_ops(ep, [{"op": "add_line", "page": 1, "text": "見出し", "balloon": kind, "wrap": "vertical", "x_mm": 60,
                        "y_mm": 60, "w_mm": 20, "h_mm": 30}])
        looks[kind] = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L").crop((200, 200, 360, 380)).tobytes()
    assert len(set(looks.values())) == 4
