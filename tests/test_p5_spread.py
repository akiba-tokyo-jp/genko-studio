from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import render_page, render_spread


def test_spread_pages_meet_at_the_gutter_and_clip_false_crosses_gutter():
    from genko.render import mm_to_px

    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    one = render_page(ep.pages[0], 36, mode="print", episode=ep)
    img = render_spread(ep, 1, 2, dpi=36, mode="print")
    bleed = mm_to_px(ep.spec.bleed_mm, 36)
    assert abs(img.width - (2 * one.width - 2 * bleed)) <= 1  # the bleeds at the gutter lie under each other
