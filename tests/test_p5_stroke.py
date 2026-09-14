from genko.models import LayerRole, PageSpec, Stroke, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page
from genko.stroke import pack_point, stamp_polyline


def test_legacy_points_become_stroke_with_pressure():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[10, 10, 0.2], [40, 40, 1.0]]}],
    )
    stroke = ep.pages[0]._layer(LayerRole.NAME).strokes[0]
    assert isinstance(stroke, Stroke)
    assert stroke.points[0] == (10.0, 10.0)
    assert stroke.pressure[0] == 0.2
    assert stroke.kind == "gpen"


def test_pack_point_defaults_mouse_pressure():
    assert pack_point(1, 2) == [1, 2, 0.7]
    assert pack_point(1, 2, 1.5)[2] == 1.0


def test_gpen_thick_point_is_darker_blob_than_thin():
    from PIL import Image, ImageDraw

    image = Image.new("L", (80, 40), 255)
    draw = ImageDraw.Draw(image)
    stamp_polyline(draw, [(10, 20, 0.15), (30, 20, 0.15)], dpi=150, width_mm=0.8, fill=0)
    stamp_polyline(draw, [(50, 20, 1.0), (70, 20, 1.0)], dpi=150, width_mm=0.8, fill=0)
    thin = sum(1 for x in range(8, 32) for y in range(16, 24) if image.getpixel((x, y)) < 40)
    thick = sum(1 for x in range(48, 72) for y in range(16, 24) if image.getpixel((x, y)) < 40)
    assert thick > thin
