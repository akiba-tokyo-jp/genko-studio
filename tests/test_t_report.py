"""The Windows test report (Hermes): slanted bleed panels, lines that follow moved gutters, trial lines before the
name is approved, the gaps between panels, the gutters handed back, and the error and warning wording."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.placement import bleed_poly  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402
from genko.studio import layout, lint  # noqa: E402
from genko.studio.issues import error, warning  # noqa: E402
from genko.studio.jsonschema_lite import validate  # noqa: E402
from genko.studio.schemas import SCHEMAS  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _grey(ep, x, y, dpi=100):
    return render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("L").getpixel((mm_to_px(x, dpi), mm_to_px(y, dpi)))


def test_a_slanted_bleed_panel_runs_out_along_its_outer_sides_only():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    layout.apply_layout(ep, 1, {"page": 1, "template": "2tier_wide", "panels": [{"slot": "p1", "bleed": True, "slant": 12}, {"slot": "p2"}]})
    page = ep.pages[0]
    first, second = page.leaf_frames()
    shape = bleed_poly(page, first)
    bleed = page.bleed_rect_mm()
    assert shape is not None and min(y for _, y in shape) == pytest.approx(bleed.y)
    assert bleed_poly(page, second) is None
    page.name_ok = True
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "width_mm": 4,
                    "points": [[bleed.x + 2, bleed.y + 2], [bleed.x + bleed.width - 2, bleed.y + 2]]}])
    inner = page.inner_rect_mm()
    assert _grey(ep, inner.x + inner.width / 2, bleed.y + 2) < 80  # (the ink reaches into the bleed above the panel)


def test_lines_follow_a_moved_gutter():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    top, bottom = ep.pages[0].frames[0].children
    r = bottom.rect
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "ふぁぁ…", "x_mm": r.x + 10, "y_mm": r.y + 10, "w_mm": 12, "h_mm": 30,
                    "tail": [r.x + 40, r.y + 60]}])
    apply_ops(ep, [{"op": "move_gutter", "page": 1, "frame_id": root.id, "delta_mm": 40}])
    _, bottom = ep.pages[0].frames[0].children
    nr = bottom.rect
    line = ep.story_for_page(1)[0]
    assert nr.y > r.y + 30
    assert nr.y <= line.y_mm and line.y_mm + line.h_mm <= nr.y + nr.height
    assert nr.y <= line.tail[1] <= nr.y + nr.height


def test_a_trial_layer_can_be_drawn_before_the_name_is_approved():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ep.strict_gates = True
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "try", "name": "はみ出し"}], agent="ai:hermes")
    with pytest.raises(ApplyError, match="name_ok"):
        apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "try", "points": [[20, 20], [60, 60]]}], agent="ai:hermes")
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "try", "exportable": False},
                   {"op": "add_stroke", "page": 1, "layer_id": "try", "points": [[20, 20], [60, 60]]}], agent="ai:hermes")
    layer = next(item for item in ep.pages[0].layers if item.id == "try")
    assert layer.strokes
    with pytest.raises(ApplyError, match="name_ok"):  # (drawn lines do not become printed before the approval)
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "try", "exportable": True}], agent="ai:hermes")
    ep.pages[0].name_ok = True
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "try", "exportable": True}], agent="ai:hermes")


def test_the_plan_sets_the_gaps_between_panels():
    ep = new_episode("t", 1, 1, PageSpec.webtoon())
    plan = {"page": 1, "template": None, "tier_gap_mm": 64, "col_gap_mm": 5,
            "tiers": [{"h": 0.5, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.5, "cols": [{"slot": "p2", "w": 0.5}, {"slot": "p3", "w": 0.5}]}],
            "panels": [{"slot": s} for s in ("p1", "p2", "p3")]}
    compiled = layout.apply_layout(ep, 1, plan)
    p1, p2, p3 = (compiled.leaf_rects_mm[s] for s in ("p1", "p2", "p3"))
    assert p2[1] - (p1[1] + p1[3]) == pytest.approx(64, abs=0.01)
    assert p2[0] - (p3[0] + p3[2]) == pytest.approx(5, abs=0.01)  # (read right to left)
    assert not [i for i in validate(plan, SCHEMAS["name_plan@1"]) if "gap" in i.path]  # (known to the schema)
    with pytest.raises(layout.LayoutError):
        layout.apply_layout(new_episode("t", 1, 1, PageSpec.webtoon()), 1, {**plan, "tier_gap_mm": -1})


def test_inspect_page_hands_back_the_gutters(tmp_path):
    from genko.io import save_episode
    from genko.studio.service import StudioService

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    root = ep.pages[0].frames[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    project = tmp_path / "book.genko"
    save_episode(ep, project)
    service = StudioService(tmp_path, actor="ai:hermes")
    result = service.inspect("book.genko", "page", page=1)
    assert result.ok
    gutters = result.data["gutters"]
    assert gutters[0]["frame_id"] == root.id and gutters[0]["index"] == 0 and gutters[0]["width_mm"] == pytest.approx(6)


def test_presets_keep_their_names():
    assert PageSpec.webtoon().preset == "webtoon" and PageSpec.a4_mono().preset == "a4-mono"


def test_an_unknown_key_names_the_keys_that_can_be_used():
    issues = validate({"page": 1, "tiers": None, "panels": [], "shade": 1}, SCHEMAS["name_plan@1"])
    unknown = next(i for i in issues if i.code == "schema_unknown_key")
    assert "panels" in unknown.hint and "tier_gap_mm" in unknown.hint


def test_the_known_effect_words_are_listed_once_and_errors_come_first():
    from genko.studio.service import ToolResult

    plan = {"page": 1, "template": "splash", "panels": [{"slot": "p1", "fx": ["ぴかぴかビーム", "ぐにゃぐにゃ", "もやもや光線"]}]}
    issues = [i for i in lint.lint_name_plan(plan, {"scenes": []}, {"characters": []}, 1) if i.code == "fx_unknown"]
    assert len(issues) == 3
    assert sum("・" in i.hint for i in issues) == 1
    out = ToolResult(False, {}, [warning("fx_unknown", "/a", "w"), error("balloon_overflow", "/b", "e")]).to_dict()
    assert [i["severity"] for i in out["issues"]] == ["error", "warning"]


def test_a_tall_strip_has_no_last_panel_rule():
    plan = {"page": 1, "tiers": [{"h": 0.8, "cols": [{"slot": "p1", "w": 1}]}, {"h": 0.2, "cols": [{"slot": "p2", "w": 1}]}],
            "panels": [{"slot": "p1"}, {"slot": "p2"}]}
    assert "finale_small" in {i.code for i in lint._size_hints(plan, 1, 1)}
    assert "finale_small" not in {i.code for i in lint._size_hints(plan, 1, 1, tall=True)}


def test_a_sound_effect_over_a_face_is_not_a_line_over_a_face():
    from genko.studio.letter import place_page

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    plan = {"page": 1, "template": "splash", "panels": [{
        "slot": "p1", "shot": "CU", "characters": [{"id": "hina", "pos": "center", "scale": 1.6}],
        "lines": [{"beat_id": "b1", "balloon": "sfx", "breaks": ["ぱしっ"]}], "sfx_at": [0.5, 0.3]}]}
    compiled = layout.apply_layout(ep, 1, plan)
    _, issues = place_page(plan, compiled, {"characters": [{"id": "hina", "name": "ひな"}]}, {"b1": None})
    assert "balloon_covers_face" not in {i.code for i in issues}


def test_the_graphics_board_is_off_until_chosen():
    from genko.app.canvas import gpu_available

    assert not gpu_available()
