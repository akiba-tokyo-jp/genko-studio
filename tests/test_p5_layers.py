from genko.models import PageSpec, new_episode
from genko.ops import apply_ops


def test_reorder_and_hide_layer():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    ids = [layer.id for layer in ep.pages[0].layers]
    apply_ops(
        ep,
        [
            {"op": "set_layer", "page": 1, "id": ids[0], "visible": False, "opacity": 0.5},
            {"op": "reorder_layers", "page": 1, "order": list(reversed(ids))},
        ],
    )
    assert ep.pages[0].layers[0].id == ids[-1]
    hidden = next(layer for layer in ep.pages[0].layers if layer.id == ids[0])
    assert hidden.visible is False
    assert hidden.opacity == 0.5
