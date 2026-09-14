from genko.models import LayerRole, PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_flood_fill_paints_frame_containing_point():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "name_ok", "page": 1},
            {"op": "flood_fill", "page": 1, "layer": "ink", "x_mm": 40, "y_mm": 40, "rgb": [0, 0, 0], "gap_mm": 0.5},
        ],
    )
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    px, py = mm_to_px(40, 72), mm_to_px(40, 72)
    pixel = img.getpixel((min(px, img.width - 1), min(py, img.height - 1)))
    assert pixel[0] < 40


def test_add_tone_draws_halftone_dots():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame_id = ep.pages[0].frames[0].id
    apply_ops(ep, [{"op": "add_tone", "page": 1, "frame_id": frame_id, "lpi": 40, "density": 0.8}])
    tone = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.TONE)
    assert tone.lpi == 40
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    # some interior pixel of the inner frame should not be pure paper
    inner = ep.pages[0].inner_rect_mm()
    px, py = mm_to_px(inner.x + 8, 72), mm_to_px(inner.y + 8, 72)
    extrema = img.convert("L").getextrema()
    assert extrema[0] < 200
    apply_ops(ep, [{"op": "delete_tone", "page": 1, "id": tone.id}])
    assert all(layer.role != LayerRole.TONE for layer in ep.pages[0].layers)


def test_focus_and_speed_effects():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame_id = ep.pages[0].frames[0].id
    apply_ops(
        ep,
        [
            {"op": "add_effect", "page": 1, "kind": "focus", "frame_id": frame_id, "params": {"count": 24}},
            {"op": "add_effect", "page": 1, "kind": "speed", "frame_id": frame_id, "params": {"count": 12}},
        ],
    )
    assert [item["kind"] for item in ep.pages[0].effects] == ["focus", "speed"]
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    assert img.convert("L").getextrema()[0] < 50
