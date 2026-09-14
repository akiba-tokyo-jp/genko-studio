from pathlib import Path

from PIL import Image

from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.render import mm_to_px, render_page


def test_move_line_sets_balloon_geometry():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_line",
                "page": 1,
                "text": "待て。",
                "speaker": "相手",
                "balloon": "speech",
                "x_mm": 40,
                "y_mm": 50,
                "w_mm": 30,
                "h_mm": 18,
            }
        ],
    )
    line = ep.story[0]
    apply_ops(
        ep,
        [{"op": "move_line", "id": line.id, "x_mm": 80, "y_mm": 90, "tail": [70, 120]}],
    )
    line = ep.story[0]
    assert line.x_mm == 80
    assert line.y_mm == 90
    assert line.tail == (70.0, 120.0)
    assert line.balloon == "speech"


def test_merge_frame_collapses_siblings():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5}])
    leaves = ep.pages[0].leaf_frames()
    assert len(leaves) == 2
    apply_ops(ep, [{"op": "merge_frame", "page": 1, "frame_id": leaves[0].id}])
    assert len(ep.pages[0].leaf_frames()) == 1


def test_resize_and_bleed_frame():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame_id = ep.pages[0].frames[0].id
    apply_ops(
        ep,
        [
            {
                "op": "resize_frame",
                "page": 1,
                "frame_id": frame_id,
                "rect": {"x": 10, "y": 12, "width": 80, "height": 100},
            },
            {"op": "set_frame", "page": 1, "frame_id": frame_id, "bleed": True, "clip": False, "border_mm": 0.5},
        ],
    )
    frame = ep.pages[0].frames[0]
    assert frame.bleed is True
    assert frame.clip is False
    assert frame.border_mm == 0.5
    assert frame.rect.width == 80


def test_put_raster_lands_on_layer_and_roundtrips(tmp_path: Path):
    from genko.io import load_episode, save_episode

    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    img = Image.new("RGB", (8, 8), (12, 34, 56))
    png = tmp_path / "in.png"
    img.save(png)
    apply_ops(ep, [{"op": "put_raster", "page": 1, "layer": "bg", "path": str(png)}])
    bg = next(layer for layer in ep.pages[0].layers if layer.role.value == "bg")
    assert bg.raster_relpath == "pages/001/bg.png"
    assert bg.raster_png
    dest = tmp_path / "work.genko"
    save_episode(ep, dest)
    assert (dest / "pages" / "001" / "bg.png").is_file()
    loaded = load_episode(dest)
    loaded_bg = next(layer for layer in loaded.pages[0].layers if layer.role.value == "bg")
    assert loaded_bg.raster_png


def test_print_render_draws_balloon_not_plain_dump():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_line",
                "page": 1,
                "text": "今だ",
                "x_mm": 40,
                "y_mm": 40,
                "w_mm": 50,
                "h_mm": 30,
                "balloon": "speech",
            }
        ],
    )
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    # top-center of the speech ellipse sits on the outline
    px, py = mm_to_px(65, 72), mm_to_px(40, 72)
    pixel = img.getpixel((min(px, img.width - 1), min(py, img.height - 1)))
    assert pixel[0] < 80


def test_ops_schema_file_exists_and_lists_p1_ops():
    from pathlib import Path
    import json

    path = Path("docs/ops.schema.json")
    assert path.is_file()
    data = json.loads(path.read_text(encoding="utf-8"))
    names = {item["op"] for item in data["ops"]}
    for name in ("move_line", "merge_frame", "resize_frame", "set_frame", "put_raster"):
        assert name in names
