from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_print_mode_clips_ink_to_frames_and_skips_name():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 8},
            {"op": "name_ok", "page": 1},
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "ink",
                "points": [[20, 20], [20, 280]],
            },
        ],
    )
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    # gutter around y=148.5mm should stay paper-white
    gx, gy = mm_to_px(20, 72), mm_to_px(148.5, 72)
    pixel = img.getpixel((min(gx, img.width - 1), min(gy, img.height - 1)))
    assert pixel[0] > 200


def test_name_mode_includes_name_strokes():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[40, 40], [80, 40]]}],
    )
    proof = render_page(ep.pages[0], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[0], 72, mode="print", episode=ep)
    px, py = mm_to_px(60, 72), mm_to_px(40, 72)
    assert proof.getpixel((px, py)) != printed.getpixel((px, py))
