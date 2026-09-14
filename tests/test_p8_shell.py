from genko.models import LayerRole, PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import render_page


def test_set_brush_tints_ink_stroke():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_brush", "rgb": [255, 0, 0], "width_mm": 4}])
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 80], [90, 80]]}])
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    reds = [
        img.getpixel((x, y))
        for x in range(img.width)
        for y in range(img.height)
        if img.getpixel((x, y))[0] > 180 and img.getpixel((x, y))[1] < 40
    ]
    assert reds


def test_gpen_curve_lowers_mid_pressure():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_brush", "curve": "gpen"}])
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[10, 40, 0.5], [20, 40, 0.5], [30, 40, 0.5]]}],
    )
    from genko.models import coerce_stroke

    stroke = coerce_stroke(ep.pages[0]._layer(LayerRole.NAME).strokes[-1])
    assert stroke.pressure[1] < 0.45


def test_spread_stroke_crosses_to_paired_page():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    width = ep.spec.width_mm
    apply_ops(
        ep,
        [
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "name",
                "points": [[width + 20, 40], [width + 40, 40]],
            }
        ],
    )
    assert ep.pages[0].name_strokes == [] or all(pt[0] < width for stroke in ep.pages[0].name_strokes for pt in stroke)
    assert ep.pages[1].name_strokes
    xs = [pt[0] for pt in ep.pages[1].name_strokes[-1]]
    assert max(xs) < width
    assert min(xs) > 10
