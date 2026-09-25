"""J3: brushes as CLIP STUDIO has them — tips (flat, picture), stamps, scatter, patterns, speed,
post-correction, anti-aliasing, watercolour edges; blending colours; .abr and brush files; the whole-line
eraser."""

import base64
import io
import json
import math
import os
import struct
import sys
from pathlib import Path

import pytest
from PIL import Image, ImageDraw

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import abr, brushes  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")
    yield
    brushes.CUSTOM.clear()


def _cover(kind, points=None, width=2.0, dpi=150):
    points = points or [(10 + t, 20 + 4 * math.sin(t / 6), 0.8) for t in range(0, 60)]
    out = brushes.draw((600, 300), points, dpi, width, kind, seed="t")
    assert out is not None, kind
    return out[0]


NEW = ("calligraphy", "water", "spray", "stipple", "dotline", "dashline", "lace", "grass", "leaves", "hearts", "stars")


@pytest.mark.parametrize("kind", NEW)
def test_every_new_brush_draws(kind):
    cover = _cover(kind)
    assert cover.getbbox() is not None and sum(1 for v in cover.getdata() if v > 128) > 20


def test_dotted_lines_have_gaps_and_solid_lines_do_not():
    straight = [(10 + t, 20, 1.0) for t in range(0, 60)]
    dots = _cover("dotline", straight, width=1.0)
    pen = _cover("mili", straight, width=1.0)
    row = lambda img: [img.getpixel((x, img.height // 2)) for x in range(img.width)]  # noqa: E731
    gaps = sum(1 for a, b in zip(row(dots), row(dots)[1:]) if a > 128 >= b)
    assert gaps >= 5 and sum(1 for a, b in zip(row(pen), row(pen)[1:]) if a > 128 >= b) <= 1


def test_a_flat_tip_is_wide_one_way_and_thin_the_other():
    across = _cover("calligraphy", [(10 + t, 20, 1.0) for t in range(0, 40)], width=3)
    down = _cover("calligraphy", [(20, 10 + t, 1.0) for t in range(0, 40)], width=3)
    heights = [img.getbbox()[3] - img.getbbox()[1] for img in (across,)]
    widths = [img.getbbox()[2] - img.getbbox()[0] for img in (down,)]
    assert heights[0] != widths[0]  # (the chisel at 35°: a horizontal and a vertical line differ in weight)


def test_speed_thins_quick_parts():
    b = brushes.from_dict("my_fast", {"label": "速さ", "base": "gpen", "speed": 1.0, "min_pressure": 0.1})
    brushes.CUSTOM["my_fast"] = b
    slow = [(10 + t * 0.5, 20, 0.8) for t in range(40)]
    quick = [(10 + t * 3.0, 40, 0.8) for t in range(10)]
    pressured = brushes._pressured(slow + quick, b)
    assert pressured[-3][2] < pressured[10][2]


def test_post_correction_smooths_a_shaky_line():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    shaky = [[40 + t, 100 + (2 if t % 2 else -2), 0.7] for t in range(40)]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": shaky, "stabilize": 0, "post_smooth": 4},
                   {"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": shaky, "stabilize": 0}])
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    wobble = lambda s: max(p[1] for p in s.points[5:-5]) - min(p[1] for p in s.points[5:-5])  # noqa: E731
    assert wobble(ink.strokes[0]) < wobble(ink.strokes[1]) / 2


def test_anti_aliasing_none_is_hard_edged():
    brushes.CUSTOM["my_hard"] = brushes.from_dict("my_hard", {"label": "硬い", "base": "mili", "aa": "none"})
    cover = _cover("my_hard")
    assert set(cover.getdata()) <= {0, 255}


def test_watercolour_gathers_at_the_rim():
    cover = _cover("water", [(10 + t, 30, 1.0) for t in range(0, 60)], width=8)
    mid_x = cover.width // 2
    column = [cover.getpixel((mid_x, y)) for y in range(cover.height)]
    inked = [v for v in column if v > 10]
    assert max(inked) > inked[len(inked) // 2] + 20  # the edge darker than the middle


def _png(image) -> str:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def test_a_tip_made_from_a_picture(tmp_path: Path):
    picture = Image.new("RGB", (40, 40), "white")
    ImageDraw.Draw(picture).polygon([(20, 2), (38, 38), (2, 38)], fill="black")  # a dark triangle on paper
    path = tmp_path / "tri.png"
    picture.save(path)
    tip = abr.tip_from_picture(str(path))
    b = brushes.from_dict("my_tri", {"label": "三角", "tip": "image", "tip_png": tip, "spacing": 1.2})
    brushes.CUSTOM["my_tri"] = b
    cover = _cover("my_tri", width=4)
    assert cover.getbbox() is not None
    with pytest.raises(ValueError):
        brushes.from_dict("my_bad", {"label": "x", "tip": "image"})
    with pytest.raises(ValueError):
        brushes.from_dict("my_bad", {"label": "x", "pattern": "clouds"})


def test_define_brush_keeps_the_new_settings(tmp_path: Path):
    from genko.io import load_episode

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "define_brush", "key": "my_lace", "label": "自分のレース", "base": "lace", "spacing": 0.8,
                    "pattern": "lace", "stamp_size": 1.2}])
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    again = load_episode(project)
    assert again.brush_custom["my_lace"]["pattern"] == "lace" and again.brush_custom["my_lace"]["stamp_size"] == 1.2


# --- .abr -------------------------------------------------------------------------------------------------


def _tip_image() -> Image.Image:
    image = Image.new("L", (12, 10), 0)
    ImageDraw.Draw(image).ellipse((1, 1, 10, 8), fill=255)
    return image


def _abr_v2(image: Image.Image, name: str = "丸") -> bytes:
    w, h = image.size
    body = struct.pack(">i", 0) + struct.pack(">h", 30)
    utf = (name + "\0").encode("utf-16-be")
    body += struct.pack(">I", len(utf) // 2) + utf
    body += b"\x01" + struct.pack(">4h", 0, 0, h, w) + struct.pack(">4i", 0, 0, h, w) + struct.pack(">h", 8) + b"\x00"
    body += image.tobytes()
    return struct.pack(">hh", 2, 1) + struct.pack(">h", 2) + struct.pack(">i", len(body)) + body


def _packbits_row(row: bytes) -> bytes:
    out = b""
    for i in range(0, len(row), 128):
        chunk = row[i:i + 128]
        out += bytes([len(chunk) - 1]) + chunk
    return out


def _abr_v6(image: Image.Image) -> bytes:
    w, h = image.size
    rows = [_packbits_row(image.tobytes()[y * w:(y + 1) * w]) for y in range(h)]
    tip = b"\0" * 47 + struct.pack(">4i", 0, 0, h, w) + struct.pack(">h", 8) + b"\x01"
    tip += b"".join(struct.pack(">H", len(r)) for r in rows) + b"".join(rows)
    tip_block = struct.pack(">I", len(tip)) + tip + b"\0" * (-len(tip) % 4)
    samp = b"8BIM" + b"samp" + struct.pack(">I", len(tip_block)) + tip_block
    other = b"8BIM" + b"desc" + struct.pack(">I", 4) + b"\0\0\0\0"
    return struct.pack(">hh", 6, 1) + samp + other


def test_abr_files_old_and_new():
    image = _tip_image()
    old = abr.read(_abr_v2(image))
    assert old[0]["name"] == "丸" and old[0]["image"].tobytes() == image.tobytes() and old[0]["spacing"] == 30
    new = abr.read(_abr_v6(image))
    assert new[0]["image"].tobytes() == image.tobytes()
    made = abr.brushes_from(_abr_v6(image), prefix="テスト ")
    b = brushes.from_dict("my_abr", made[0])
    assert b.tip == "image" and b.tip_png
    with pytest.raises(abr.AbrError):
        abr.read(struct.pack(">h", 3) + b"\0" * 10)


# --- blending and erasing -------------------------------------------------------------------------------------


def _paint_book():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "p", "name": "色"},
                   {"op": "fill_area", "page": 1, "layer_id": "p", "rgb": [200, 0, 0], "area": {"rect": [60, 80, 30, 60]}},
                   {"op": "fill_area", "page": 1, "layer_id": "p", "rgb": [0, 0, 200], "area": {"rect": [90, 80, 30, 60]}}])
    return ep


@pytest.mark.parametrize("mode", ["blur", "smudge", "blend"])
def test_blending_changes_the_colours_where_the_brush_passes(mode):
    from genko.render import layer_image

    ep = _paint_book()
    layer = next(item for item in ep.pages[0].layers if item.id == "p")
    strip = lambda img: [img.getpixel((round(x / 25.4 * 100), round(110 / 25.4 * 100))) for x in range(78, 104)]  # noqa: E731
    before = strip(layer_image(ep.pages[0], layer, 100, ep))
    apply_ops(ep, [{"op": "smudge", "page": 1, "layer_id": "p", "points": [[75, 110], [105, 110]], "width_mm": 10,
                    "strength": 0.9, "mode": mode}])
    layer = next(item for item in ep.pages[0].layers if item.id == "p")
    after = strip(layer_image(ep.pages[0], layer, 100, ep))
    assert after != before and layer.patches[-1]["mode"] == "image"
    far = layer_image(ep.pages[0], layer, 100, ep).getpixel((round(65 / 25.4 * 100), round(135 / 25.4 * 100)))
    assert far[:3] == (200, 0, 0)  # untouched away from the brush


def test_blending_nothing_is_said():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "p"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "smudge", "page": 1, "layer_id": "p", "points": [[75, 110], [105, 110]]}])


def test_the_whole_line_eraser():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 100], [140, 100]], "stabilize": 0},
                   {"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 150], [140, 150]], "stabilize": 0}])
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[90, 95], [90, 105]], "width_mm": 2, "mode": "whole"}])
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    assert len(ink.strokes) == 1 and ink.strokes[0].points[0][1] == 150
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[1, 1], [2, 2]], "mode": "zap"}])


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(_paint_book(), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_the_blend_tool_and_the_eraser_choice(window, qapp):
    window.set_target_layer("p")
    window._tool("blend")
    assert window.canvas.tool == "blend"
    patches = len(next(item for item in window.current_page().layers if item.id == "p").patches)
    window._on_stroke([[75, 110, 0.8], [105, 110, 0.8]])
    assert len(next(item for item in window.current_page().layers if item.id == "p").patches) == patches + 1
    assert window.eraser_mode.findData("whole") >= 0


def test_brushes_go_to_a_file_and_come_back(window, qapp, tmp_path: Path):
    window.brush.reload_kinds(select="lace")
    out = tmp_path / "lace.genkobrush"
    assert window._export_brush(str(out))
    data = json.loads(out.read_text(encoding="utf-8"))
    assert list(data["brushes"].values())[0]["pattern"] == "lace"
    keys = window.import_brushes(str(out))
    assert keys and brushes.brush(keys[0]).pattern == "lace"
    abr_file = tmp_path / "tips.abr"
    abr_file.write_bytes(_abr_v2(_tip_image(), "ぼかし丸"))
    keys = window.import_brushes(str(abr_file))
    assert keys and brushes.brush(keys[0]).tip == "image" and "ぼかし丸" in brushes.brush(keys[0]).label


def test_the_brush_dialog_has_the_tip_settings(window, qapp):
    from genko.app.brush_panel import BrushDialog

    dialog = BrushDialog(window, "stars")
    data = dialog.data()
    assert data["pattern"] == "stars" and data["turn_jitter"] is True
    dialog.pattern.setCurrentIndex(dialog.pattern.findData("hearts"))
    dialog.speed.setValue(40)
    data = dialog.data()
    assert data["pattern"] == "hearts" and data["speed"] == 0.4
    assert brushes.from_dict("my_x", data).pattern == "hearts"
