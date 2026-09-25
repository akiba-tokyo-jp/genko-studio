import copy
import json
from pathlib import Path

import pytest

from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode
from genko.studio import layout, lint
from genko.studio.jsonschema_lite import conservative_problems, validate
from genko.studio.letter import EM_MM, measure, place_page
from genko.studio.schemas import SCHEMAS
from genko.tategaki import _columns

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _b4(pages: int = 4):
    return new_episode("t", 1, pages, PageSpec.b4_comic())


def test_schemas_are_conservative_and_fixtures_match():
    for name, schema in SCHEMAS.items():
        assert conservative_problems(schema) == [], name
    assert validate(_load("bible.json"), SCHEMAS["bible@1"]) == []
    assert validate(_load("script.json"), SCHEMAS["script@1"]) == []
    for n in range(1, 5):
        assert validate(_load(f"p00{n}.json"), SCHEMAS["name_plan@1"]) == []


def test_validator_reports_json_pointer_paths():
    plan = _load("p001.json")
    plan["panels"][1]["shot"] = "XL"
    del plan["panels"][0]["angle"]
    plan["extra"] = 1
    paths = {issue.path for issue in validate(plan, SCHEMAS["name_plan@1"])}
    assert {"/panels/1/shot", "/panels/0/angle", "/extra"} <= paths


def test_tategaki_explicit_newline_starts_column_and_plain_text_is_unchanged():
    assert _columns("…やっぱり\n来てくれたんだ", 20) == [list("…やっぱり"), list("来てくれたんだ")]
    assert _columns("あいうえおかきく", 3) == [list("あいう"), list("えおか"), list("きく")]


def test_layout_cols_run_right_to_left_and_tiers_top_to_bottom():
    ep = _b4()
    plan = {"page": 1, "tiers": [
        {"h": 0.4, "cols": [{"slot": "a", "w": 0.6, "rows": None}, {"slot": "b", "w": 0.4, "rows": None}]},
        {"h": 0.6, "cols": [{"slot": "c", "w": 0.5, "rows": [{"slot": "c1", "h": 0.5}, {"slot": "c2", "h": 0.5}]},
                            {"slot": "d", "w": 0.5, "rows": None}]},
    ], "panels": [{"slot": s} for s in ("a", "b", "c1", "c2", "d")]}
    out = layout.apply_layout(ep, 1, plan, agent="ai:test")
    assert out.reading_order == ["a", "b", "c1", "c2", "d"]
    r = out.leaf_rects_mm
    assert r["a"][0] > r["b"][0]  # a is the right column
    assert r["a"][1] < r["c1"][1]
    assert r["c1"][1] < r["c2"][1] and r["c1"][0] == r["c2"][0]
    inner = ep.pages[0].inner_rect_mm()
    assert abs(r["a"][2] / (r["a"][2] + r["b"][2]) - 0.6) < 0.01
    assert abs(r["a"][3] + r["c1"][3] + r["c2"][3] + 2 * layout.TIER_GUTTER_MM - inner.height) < 0.01
    assert abs((r["b"][0] + r["b"][2] + layout.COL_GUTTER_MM) - r["a"][0]) < 0.01


def test_layout_on_a_copy_leaves_the_original_untouched_and_saves(tmp_path: Path):
    ep = _b4()
    work = copy.deepcopy(ep)
    layout.apply_layout(work, 2, _load("p002.json") | {"page": 2}, agent="ai:test")
    assert len(ep.pages[1].leaf_frames()) == 1
    assert len(work.pages[1].leaf_frames()) == 3
    save_episode(work, tmp_path / "x.genko")
    assert len(load_episode(tmp_path / "x.genko").pages[1].leaf_frames()) == 3


def test_check_tiers_errors_point_into_the_plan():
    plan = _load("p001.json")
    plan["tiers"][1]["cols"][0]["w"] = 0.8
    plan["panels"].append(dict(plan["panels"][0], slot="zz"))
    codes = {(i.code, i.path) for i in layout.check_tiers(plan)}
    assert ("layout_ratio_sum", "/tiers/1/cols") in codes
    assert ("panel_unknown_slot", "/panels/3/slot") in codes


def test_lettering_stays_in_panel_in_reading_order_and_off_faces():
    ep = _b4()
    plan = _load("p003.json")
    compiled = layout.apply_layout(ep, 3, plan, agent="ai:test")
    bible, script = _load("bible.json"), _load("script.json")
    speakers = {bid: info["beat"].get("speaker_id") for bid, info in lint.script_index(script).items()}
    placements, issues = place_page(plan, compiled, bible, speakers)
    assert [i for i in issues if i.severity == "error"] == []
    assert len(placements) == 2
    for p in placements:
        x, y, w, h = compiled.leaf_rects_mm[p.slot]
        assert x <= p.x_mm and p.x_mm + p.w_mm <= x + w
        assert y <= p.y_mm and p.y_mm + p.h_mm <= y + h
        assert p.tail is not None  # the speaker is in the panel
    # the ellipse goes around the text block's corners (M6): 2 columns × √2 + the pad on both sides
    # (the two columns are 0.15 em apart)
    assert measure(["…やっぱり", "来てくれたんだ"], "speech")[0] == pytest.approx((2 * EM_MM + 0.15 * EM_MM) * 2 ** 0.5 + EM_MM / 2)
    assert measure(["…やっぱり", "来てくれたんだ"], "narration")[0] == pytest.approx(2 * EM_MM + 0.15 * EM_MM + EM_MM / 2)


def test_lettering_reports_overflow_with_the_line_path():
    ep = _b4()
    plan = _load("p003.json")
    plan["tiers"] = [{"h": 0.08, "cols": [{"slot": "p1", "w": 1.0, "rows": None}]}, {"h": 0.92, "cols": [{"slot": "p2", "w": 1.0, "rows": None}]}]
    compiled = layout.apply_layout(ep, 3, plan, agent="ai:test")
    _, issues = place_page(plan, compiled, _load("bible.json"), {})
    assert any(i.code == "balloon_overflow" and i.path == "/panels/0/lines/0" for i in issues)


def test_script_and_name_lint():
    bible, script = _load("bible.json"), _load("script.json")
    assert [i for i in lint.lint_script(script, bible, 4) if i.severity == "error"] == []
    bad = copy.deepcopy(script)
    bad["scenes"][0]["beats"][4]["speaker_id"] = "nobody"
    bad["scenes"][0]["beats"][0]["page"] = 9
    codes = {(i.code, i.path) for i in lint.lint_script(bad, bible, 4)}
    assert ("unknown_speaker", "/scenes/0/beats/4/speaker_id") in codes
    assert ("page_out_of_range", "/scenes/0/beats/0/page") in codes

    plan = _load("p001.json")
    assert [i for i in lint.lint_name_plan(plan, script, bible, 4) if i.severity == "error"] == []
    plan["panels"][2]["lines"] = []
    plan["panels"][0]["lines"][0]["breaks"] = ["あ" * 15]
    found = {(i.code, i.path) for i in lint.lint_name_plan(plan, script, bible, 4)}
    assert ("beat_not_placed", "/panels") in found
    assert ("column_too_long", "/panels/0/lines/0/breaks/0") in found


def test_turn_role_warnings_follow_page_parity():
    bible, script = _load("bible.json"), _load("script.json")
    plan = _load("p001.json") | {"turn_role": "reveal"}
    assert any(i.code == "reveal_on_odd_page" for i in lint.lint_name_plan(plan, script, bible, 4))
    plan2 = _load("p002.json")
    assert not any(i.code == "reveal_on_odd_page" for i in lint.lint_name_plan(plan2, script, bible, 4))
