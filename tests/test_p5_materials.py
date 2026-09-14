from genko.models import LayerRole, PageSpec, new_episode
from genko.ops import apply_ops


def test_stamp_material_adds_tone_from_catalog():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame_id = ep.pages[0].frames[0].id
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "dot-60-30", "frame_id": frame_id}])
    tone = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.TONE)
    assert tone.lpi == 60
    assert tone.density == 0.3
