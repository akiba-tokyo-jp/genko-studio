import base64
import io

from PIL import Image

from genko.models import LayerRole, PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.psd import export_psd
from genko.render import mm_to_px, render_page, render_spread


def _png_b64(color, size=(64, 64), mode="RGBA"):
    buf = io.BytesIO()
    Image.new(mode, size, color).save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def test_add_and_delete_user_layer():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    before = len(ep.pages[0].layers)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "重ね", "blend": "multiply"}])
    assert len(ep.pages[0].layers) == before + 1
    extra = ep.pages[0].layers[-1]
    assert extra.role == LayerRole.USER
    assert extra.blend == "multiply"
    assert extra.title == "重ね"
    apply_ops(ep, [{"op": "delete_layer", "page": 1, "id": extra.id}])
    assert len(ep.pages[0].layers) == before


def test_cannot_delete_ink_layer():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    try:
        apply_ops(ep, [{"op": "delete_layer", "page": 1, "id": ink.id}])
        raise AssertionError("expected ApplyError")
    except ApplyError:
        pass


def test_multiply_blend_darkens_gray():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "put_raster", "page": 1, "layer": "bg", "png_base64": _png_b64((180, 180, 180, 255))},
            {"op": "add_layer", "page": 1, "name": "赤", "blend": "multiply"},
        ],
    )
    extra = ep.pages[0].layers[-1]
    apply_ops(ep, [{"op": "put_raster", "page": 1, "id": extra.id, "png_base64": _png_b64((255, 0, 0, 255))}])
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    px = img.getpixel((8, 8))
    assert px[0] > 100
    assert px[1] < 50
    assert px[2] < 50


def test_clip_masks_to_layer_below():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    mask = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    for x in range(32):
        for y in range(64):
            mask.putpixel((x, y), (0, 0, 0, 255))
    buf = io.BytesIO()
    mask.save(buf, format="PNG")
    mask_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": mask_b64}])
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "クリップ", "clip": True, "blend": "normal"}])
    extra = ep.pages[0].layers[-1]
    apply_ops(ep, [{"op": "put_raster", "page": 1, "id": extra.id, "png_base64": _png_b64((0, 255, 0, 255))}])
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    left = img.getpixel((img.width // 8, img.height // 2))
    right = img.getpixel((img.width * 3 // 4, img.height // 2))
    assert left[1] > 200 and left[0] < 80
    assert right[0] > 200 and right[1] > 200


def test_filter_blur_spreads_ink():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 80], [80, 80]], "width_mm": 1.2}],
    )
    before = render_page(ep.pages[0], 72, mode="print", episode=ep)
    apply_ops(ep, [{"op": "filter_raster", "page": 1, "layer": "ink", "kind": "blur", "radius": 3}])
    after = render_page(ep.pages[0], 72, mode="print", episode=ep)
    assert before.tobytes() != after.tobytes()


def test_filter_kinds_change_pixels():
    src = Image.new("RGBA", (64, 64), (40, 80, 160, 255))
    for x in range(32):
        for y in range(64):
            src.putpixel((x, y), (200, 30, 30, 255))
    buf = io.BytesIO()
    src.save(buf, format="PNG")
    payload = base64.b64encode(buf.getvalue()).decode("ascii")
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": payload}])
    original = render_page(ep.pages[0], 72, mode="print", episode=ep).tobytes()
    for kind, params in (
        ("sharpen", {}),
        ("hue", {"shift": 40}),
        ("levels", {"black": 20, "white": 220}),
        ("curve", {"gamma": 1.8}),
        ("mosaic", {"block": 10}),
    ):
        ep2 = new_episode("t", 1, 1, PageSpec.a4_mono())
        apply_ops(ep2, [{"op": "put_raster", "page": 1, "layer": "bg", "png_base64": payload}])
        apply_ops(ep2, [{"op": "filter_raster", "page": 1, "layer": "bg", "kind": kind, **params}])
        assert render_page(ep2.pages[0], 72, mode="print", episode=ep2).tobytes() != original


def test_gpen_stabilize_pulls_spike():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "name",
                "stabilize": 5,
                "points": [[10, 40], [20, 40], [30, 90], [40, 40], [50, 40]],
            }
        ],
    )
    stroke = ep.pages[0].name_strokes[-1]
    ys = [pt[1] for pt in stroke]
    assert max(ys) < 85


def test_gpen_taper_lowers_end_pressure():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "name",
                "taper": True,
                "points": [[10, 40, 1], [20, 40, 1], [30, 40, 1], [40, 40, 1], [50, 40, 1]],
            }
        ],
    )
    from genko.models import coerce_stroke

    stroke = coerce_stroke(ep.pages[0]._layer(LayerRole.NAME).strokes[-1])
    assert stroke.pressure[0] < 0.5
    assert stroke.pressure[-1] < 0.5
    assert stroke.pressure[2] > 0.8


def test_lock_alpha_does_not_paint_empty():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    blank = _png_b64((0, 0, 0, 0))
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "ink", "png_base64": blank}])
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": ink.id, "lock_alpha": True}])
    apply_ops(
        ep,
        [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 80], [90, 80]], "width_mm": 3}],
    )
    img = Image.open(io.BytesIO(ink.raster_png)).convert("RGBA")
    extrema = img.split()[3].getextrema()
    assert extrema[1] == 0


def test_folder_layer_parent():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "フォルダ", "folder": True}])
    folder = ep.pages[0].layers[-1]
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "中", "parent": folder.id}])
    child = ep.pages[0].layers[-1]
    assert child.parent_id == folder.id


def test_spread_is_one_canvas():
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "set_spread", "page": 1, "with": 2}])
    img = render_spread(ep, 1, 2, dpi=72, mode="name")
    w = mm_to_px(ep.spec.width_mm, 72)
    assert img.width == w * 2


def test_psd_writes_named_layer_bytes(tmp_path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "ToneA"}])
    path = tmp_path / "p.psd"
    export_psd(ep, path, dpi=72)
    data = path.read_bytes()
    assert data.startswith(b"8BPS")
    # M6: layers carry Unicode names; an empty user layer has nothing to export
    assert "コマ枠".encode("utf-16-be") in data and b"ToneA" not in data
