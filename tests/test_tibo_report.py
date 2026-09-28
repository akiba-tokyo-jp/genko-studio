"""The run that drew 「点が六つだけ」 twice, in two styles: the style fixed by the pilot page stayed the old one,
a finish made for replaced art stayed, old sheet candidates came first in the approval box, two styles of one
name, balloons moved onto a crowd, marks that could only go all together, a body cut like any framing warned
about, a balloon touching the hair called a covered face, candidates an agent could not take back, and errors
that did not say the right way to write the op."""

from __future__ import annotations

import pytest

from genko import checks, stylecat
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.studio import finish
from genko.studio.service import wording_error

PERSON = "human:leaf"
AGENT = "ai:test"
ASSET = "sha256:" + "1" * 64
OTHER = "sha256:" + "2" * 64


def _cand(cid: str, asset: str = ASSET) -> dict:
    return {"id": cid, "asset": asset, "px": [800, 1200], "origin": {"kind": "agent", "tool_id": "xai:grok-imagine"},
            "status": "candidate"}


def _book(pages: int = 2):
    episode = new_episode("t", 1, pages, PageSpec.b5_doujin())
    for page in episode.pages:
        page.name_ok = True
        frame = page.leaf_frames()[0]
        frame.panel = {"status": "adopted", "candidates": [_cand("c1"), _cand("c2", OTHER)], "adopted": {"art": "c1"},
                       "characters": [{"id": "tibo"}]}
    return episode


def _catalog(sid: str) -> dict:
    return {"id": sid, "title": "ゆるい手描きの落書き風", "prompt": {"ja": "ゆるい線"}, "version": 6}


def test_the_style_lock_is_made_again_when_the_pilot_is_approved_again_or_the_style_changes():
    episode = _book()
    apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent=PERSON)
    lock = episode.studio["style"]["locked"]
    assert lock["reference"] == ASSET and lock["from_page"] == episode.pages[0].id
    # the pilot page's art replaced and approved again: the lock takes the new picture
    apply_ops(episode, [{"op": "adopt_candidate", "page": 1, "frame_id": episode.pages[0].leaf_frames()[0].id,
                         "candidate_id": "c2"}], agent=AGENT)
    apply_ops(episode, [{"op": "approve", "gate": "art", "page": 1}], agent=PERSON)
    assert episode.studio["style"]["locked"]["reference"] == OTHER
    # a person changes the style: the old picture is no model any more, and the next approved page fixes it again
    apply_ops(episode, [{"op": "set_style_catalog", "catalog": _catalog("gag-surreal-doodle-casual")}], agent=PERSON)
    stale = episode.studio["style"]["locked"]
    assert stale["stale"] and stale["reference"] is None and "changed_rev" in episode.studio["style"]
    apply_ops(episode, [{"op": "approve", "gate": "art", "page": 2}], agent=PERSON)
    fresh = episode.studio["style"]["locked"]
    assert not fresh.get("stale") and fresh["from_page"] == episode.pages[1].id and fresh["reference"] == ASSET
    # an agent still cannot change it
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "set_style_catalog", "catalog": _catalog("essay-food-doodle-casual")}], agent=AGENT)


def test_replaced_art_sends_the_page_back_to_finishing_and_its_faces_are_old():
    episode = _book(1)
    page = episode.pages[0]
    frame = page.leaf_frames()[0]
    apply_ops(episode, [{"op": "replace_regions", "page": 1, "frame_id": frame.id, "source": "agent",
                         "regions": [{"kind": "face", "char": "tibo", "rect_mm": [60, 60, 20, 24]}]}], agent=AGENT)
    assert episode.pages[0].leaf_frames()[0].panel["regions_for"] == "c1"
    episode.pages[0].stage = "finish"
    apply_ops(episode, [{"op": "adopt_candidate", "page": 1, "frame_id": frame.id, "candidate_id": "c2"}], agent=AGENT)
    page = episode.pages[0]
    frame = page.leaf_frames()[0]
    assert page.stage == "ink" and finish.regions_stale(frame.panel)
    assert any(i["code"] == "regions_stale" for i in checks.page_issues(episode, page))
    ops, notes = finish.plan(episode, page)
    assert any(n["kind"] == "regions_stale" for n in notes)
    # faces reported again for the new art: fresh
    apply_ops(episode, [{"op": "replace_regions", "page": 1, "frame_id": frame.id, "source": "agent",
                         "regions": [{"kind": "face", "char": "tibo", "rect_mm": [90, 60, 20, 24]}]}], agent=AGENT)
    assert not finish.regions_stale(episode.pages[0].leaf_frames()[0].panel)


def _lettered(line_box, faces):
    episode = _book(1)
    page = episode.pages[0]
    frame = page.leaf_frames()[0]
    frame.panel["regions"] = [{"kind": "face", "char": c, "rect_mm": r, "source": "agent"} for c, r in faces]
    line = episode.add_line(1, "リセット来るぞ！", x_mm=line_box[0], y_mm=line_box[1], w_mm=line_box[2], h_mm=line_box[3])
    line.frame_id = frame.id
    line.style = {"speaker_id": "tibo"}
    return episode, page, line


def test_a_balloon_nearer_someone_else_is_only_suggested_and_hair_is_not_the_face():
    # nearer the other face than the speaker's: a suggestion, not a move
    episode, page, line = _lettered((100, 40, 12, 30), [("tibo", [40, 150, 24, 28]), ("crowd", [110, 80, 24, 28])])
    ops, notes = finish.plan(episode, page)
    assert not any(op["op"] == "move_line" and "x_mm" in op for op in ops)
    assert any(n["kind"] == "suggest_move" and n["line_id"] == line.id for n in notes)
    # touching the top of the head box (the hair) is not covering the face
    episode, page, line = _lettered((70, 40, 12, 22), [("tibo", [65, 58, 24, 28])])
    ops, notes = finish.plan(episode, page)
    assert not any(n["kind"] in ("move_line", "face_covered") for n in notes)
    # over the eyes and mouth it is
    episode, page, line = _lettered((70, 70, 12, 22), [("tibo", [65, 58, 24, 28])])
    ops, notes = finish.plan(episode, page)
    assert any(n["kind"] in ("move_line", "face_covered") for n in notes)


def test_each_mark_has_its_own_layer():
    episode = _book(1)
    page = episode.pages[0]
    frame = page.leaf_frames()[0]
    frame.panel["regions"] = [{"kind": "face", "char": "tibo", "rect_mm": [60, 60, 20, 24], "source": "agent"}]
    frame.panel["fx"] = ["汗", "はてな"]
    ops, notes = finish.plan(episode, page)
    layers = [op["id"] for op in ops if op["op"] == "add_layer"]
    marks = [n for n in notes if n["kind"] == "add_mark"]
    assert len(layers) == len(marks) == 2 and len(set(layers)) == 2
    assert all(n["layer_id"] in layers and "delete_layer" in n["why"] for n in marks)
    apply_ops(episode, ops, agent=AGENT)
    apply_ops(episode, [{"op": "delete_layer", "page": 1, "id": marks[0]["layer_id"]}], agent=AGENT)
    assert any(layer.id == marks[1]["layer_id"] for layer in episode.pages[0].layers)


def test_a_body_cut_by_the_edge_with_its_face_inside_is_framing():
    episode = _book(1)
    page = episode.pages[0]
    frame = page.leaf_frames()[0]
    r = frame.rect
    body = [r.x + 20, r.y + 40, 40, r.height]  # (legs out of the bottom)
    frame.panel["regions"] = [{"kind": "face", "char": "tibo", "rect_mm": [r.x + 30, r.y + 45, 18, 20]},
                              {"kind": "person", "char": "tibo", "rect_mm": body}]
    assert not [i for i in checks.page_issues(episode, page) if i["code"] == "cut_by_panel"]
    frame.panel["regions"][0]["rect_mm"] = [r.x - 8, r.y + 45, 18, 20]  # the face itself cut: said
    cut = [i for i in checks.page_issues(episode, page) if i["code"] == "cut_by_panel"]
    assert cut and "顔" in cut[0]["message"]


def test_an_agent_takes_back_its_own_sheet_candidates():
    episode = _book(1)
    episode.bible.characters.append({"id": "tibo", "name": "ティボ"})
    episode.studio["character_candidates"] = {"tibo": [
        {**_cand("s1"), "origin": {"kind": "agent", "actor": AGENT}},
        {**_cand("s2", OTHER), "origin": {"kind": "agent", "actor": "ai:other"}}]}
    apply_ops(episode, [{"op": "withdraw_candidates", "character_id": "tibo", "candidate_ids": ["s1"]}], agent=AGENT)
    assert episode.studio["character_candidates"]["tibo"][0]["status"] == "withdrawn"
    with pytest.raises(ApplyError):  # another agent's
        apply_ops(episode, [{"op": "withdraw_candidates", "character_id": "tibo", "candidate_ids": ["s2"]}], agent=AGENT)
    with pytest.raises(ApplyError, match="candidates: s1, s2"):
        apply_ops(episode, [{"op": "withdraw_candidates", "character_id": "tibo", "candidate_ids": ["nope"]}], agent=AGENT)


def test_the_same_picture_imported_again_updates_its_candidate():
    from genko.studio.studio_ops import _import_candidates

    episode = _book(1)
    episode.bible.characters.append({"id": "tibo", "name": "ティボ"})
    episode.studio.setdefault("assets", {})
    import genko.studio.studio_ops as so

    keep = so._require_asset
    so._require_asset = lambda *_: None
    try:
        item = {"asset": ASSET, "px": [600, 900], "origin": {"kind": "agent"}}
        _import_candidates(episode, {"character_id": "tibo", "candidates": [item]}, AGENT)
        _import_candidates(episode, {"character_id": "tibo", "candidates": [{**item, "face_box01": [0.3, 0.1, 0.3, 0.2]}]}, AGENT)
    finally:
        so._require_asset = keep
    cands = episode.studio["character_candidates"]["tibo"]
    assert len(cands) == 1 and cands[0]["face_box01"] == [0.3, 0.1, 0.3, 0.2] and "rev" in cands[0]


def test_same_named_styles_show_their_genres():
    nodes = [{"id": "essay", "parent": None, "title": "エッセイ漫画"}, {"id": "essay-food", "parent": "essay", "title": "食べ歩き"},
             {"id": "essay-food-doodle-casual", "parent": "essay-food", "title": "ゆるい手描きの落書き風"},
             {"id": "gag", "parent": None, "title": "ギャグ漫画"}, {"id": "gag-surreal", "parent": "gag", "title": "シュール"},
             {"id": "gag-surreal-doodle-casual", "parent": "gag-surreal", "title": "ゆるい手描きの落書き風"}]
    found = stylecat.find("落書き", nodes)
    assert {tuple(f["path"]) for f in found} == {("エッセイ漫画", "食べ歩き", "ゆるい手描きの落書き風"),
                                                ("ギャグ漫画", "シュール", "ゆるい手描きの落書き風")}
    same = stylecat.namesakes("gag-surreal-doodle-casual", "ゆるい手描きの落書き風", nodes)
    assert same == [{"id": "essay-food-doodle-casual", "path": ["エッセイ漫画", "食べ歩き", "ゆるい手描きの落書き風"]}]


def test_a_wrong_key_or_type_says_how_the_op_is_written():
    episode = _book(1)
    line = episode.add_line(1, "あ", x_mm=10, y_mm=10, w_mm=10, h_mm=20)
    with pytest.raises(ApplyError) as wrong_type:
        apply_ops(episode, [{"op": "move_line", "id": line.id, "tail": [{"to": [1, 2]}]}])
    shown = wording_error(str(wrong_type.value))
    assert "値の型が違います" in shown and "move_line の書き方" in shown and "tails" in shown
    with pytest.raises(ApplyError) as wrong_key:
        apply_ops(episode, [{"op": "delete_layer", "page": 1, "layer_id": "x"}])
    shown = wording_error(str(wrong_key.value))
    assert "知らない鍵: layer_id" in shown and "id: str" in shown
