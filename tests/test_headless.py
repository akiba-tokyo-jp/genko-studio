from genko.headless import ApplyError, apply_ops, snapshot
from genko.models import PageSpec, new_episode


def test_snapshot_is_compact_json_for_agents():
    ep = new_episode("試作", 1, 2, PageSpec.a4_mono())
    ep.add_line(1, "始めよう。", speaker="主人公")
    data = snapshot(ep)
    assert data["title"] == "試作"
    assert data["pages"][0]["index"] == 1
    assert data["pages"][0]["stage"] == "name"
    assert data["pages"][0]["name_ok"] is False
    assert data["pages"][0]["leaf_count"] == 1
    assert data["pages"][0]["story"][0]["text"] == "始めよう。"
    assert "name_strokes" not in data["pages"][0]


def test_apply_split_name_ok_and_line():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    result = apply_ops(
        ep,
        [
            {"op": "split_frame", "page": 1, "axis": "vertical", "ratio": 0.5, "gutter_mm": 4},
            {"op": "add_line", "page": 1, "text": "待て。", "speaker": "相手"},
            {"op": "name_ok", "page": 1},
        ],
    )
    assert result["ok"] is True
    page = result["snapshot"]["pages"][0]
    assert page["leaf_count"] == 2
    assert page["name_ok"] is True
    assert page["stage"] == "ink"
    assert page["story"][0]["speaker"] == "相手"


def test_apply_rejects_ink_before_name_ok():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    try:
        apply_ops(ep, [{"op": "advance", "page": 1, "to": "ink"}])
        raise AssertionError("expected ApplyError")
    except ApplyError as exc:
        assert "name" in str(exc).lower()


def test_unknown_op_is_apply_error():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    try:
        apply_ops(ep, [{"op": "explode"}])
        raise AssertionError("expected ApplyError")
    except ApplyError:
        pass


def test_add_stroke_goes_to_name_until_gate():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[10, 10], [20, 20]]}],
    )
    assert len(ep.pages[0].name_strokes) == 1
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 30], [40, 40]]}],
    )
    assert len(ep.pages[0].ink_strokes) == 1
