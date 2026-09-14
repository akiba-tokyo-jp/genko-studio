from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_erase_clears_ink_pixels_not_stroke_index():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "name_ok", "page": 1},
            {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 80], [120, 80]]},
            {
                "op": "erase_raster",
                "page": 1,
                "layer": "ink",
                "points": [[70, 80], [80, 80]],
                "width_mm": 4,
            },
        ],
    )
    assert len(ep.pages[0]._layer(__import__("genko.models", fromlist=["LayerRole"]).LayerRole.INK).strokes) == 1
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    mid = img.getpixel((mm_to_px(75, 72), mm_to_px(80, 72)))
    end = img.getpixel((mm_to_px(35, 72), mm_to_px(80, 72)))
    assert mid[0] > 200
    assert end[0] < 80
