from genko.headless import inspect_stroke, snapshot
from genko.models import LayerRole, PageSpec, new_episode
from genko.ops import apply_ops


def test_compact_inspect_still_omits_points():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[1, 1], [2, 2]]}])
    data = snapshot(ep)
    assert "name_strokes" not in data["pages"][0]


def test_inspect_stroke_returns_one_id():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[1, 1], [9, 9]]}])
    stroke = ep.pages[0]._layer(LayerRole.NAME).strokes[0]
    data = inspect_stroke(ep, stroke.id)
    assert data["id"] == stroke.id
    assert len(data["points"]) >= 2
