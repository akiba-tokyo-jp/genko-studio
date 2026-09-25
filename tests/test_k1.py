"""K1: the last rows of the comparison — the pen's barrel turn (アートペン), effect lines edited one by one, and filter
plugins."""

import os
import sys
from pathlib import Path

import pytest
from PIL import Image, ImageChops

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ep.pages[0].name_ok = True
    return ep


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


LINE = [[40, 60, 0.8], [70, 60, 0.8], [100, 60, 0.8], [130, 60, 0.8]]


def test_the_barrel_turn_turns_a_flat_nib(tmp_path):
    from genko.render import render_page

    plain, turned = _book(), _book()
    op = {"op": "add_stroke", "page": 1, "layer": "ink", "points": LINE, "kind": "calligraphy", "width_mm": 3,
          "stabilize": 0, "post_smooth": 0}
    apply_ops(plain, [op])
    apply_ops(turned, [{**op, "rotation": [0, 30, 60, 90]}])
    stroke = _ink(turned).strokes[-1]
    assert len(stroke.rotation) == len(stroke.points) and stroke.rotation[0] == 0 and stroke.rotation[-1] == 90
    assert _ink(plain).strokes[-1].rotation == []
    a = render_page(plain.pages[0], 100, mode="print", episode=plain)
    b = render_page(turned.pages[0], 100, mode="print", episode=turned)
    assert ImageChops.difference(a.convert("L"), b.convert("L")).getbbox() is not None  # (the nib turned along the line)
    save_episode(turned, tmp_path / "t.genko")
    again = load_episode(tmp_path / "t.genko")
    assert _ink(again).strokes[-1].rotation == stroke.rotation
    save_episode(plain, tmp_path / "p.genko")
    assert '"r"' not in (tmp_path / "p.genko" / "project.json").read_text(encoding="utf-8")  # (older lines stay as they were)


def test_a_brush_can_follow_the_barrel():
    from genko import brushes

    assert brushes.brush("calligraphy").tip_rotation
    ep = _book()
    apply_ops(ep, [{"op": "define_brush", "key": "my_art", "label": "アート", "base": "calligraphy", "tip_rotation": False}])
    assert ep.brush_custom["my_art"].get("tip_rotation", False) is False  # (only what differs from a plain pen is kept)
    apply_ops(ep, [{"op": "define_brush", "key": "my_art2", "label": "アート2", "base": "calligraphy", "tip": "flat",
                    "tip_rotation": True}])
    assert ep.brush_custom["my_art2"]["tip_rotation"] is True


def test_effect_lines_become_lines_to_edit_one_by_one():
    ep = _book()
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "speed", "id": "s", "params": {"count": 12}},
                   {"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": _ink(ep).id}])
    strokes = _ink(ep).strokes
    assert len(strokes) >= 10 and all(s.kind == "fx" for s in strokes)
    first = strokes[0]
    apply_ops(ep, [{"op": "delete_stroke", "page": 1, "layer": "ink", "index": 0}])  # (one line goes; the rest stay)
    assert len(_ink(ep).strokes) == len(strokes) - 1 and first not in _ink(ep).strokes


SEPIA = '''
NAME = "セピア"
PARAMS = {"amount": {"label": "強さ", "min": 0, "max": 1, "default": 1}}


def run(image, amount=1.0):
    from PIL import Image, ImageOps

    toned = ImageOps.colorize(image.convert("L"), (40, 20, 0), (255, 240, 200)).convert("RGB")
    return Image.blend(image.convert("RGB"), toned, float(amount))
'''


def _plugin(tmp_path: Path, name: str, text: str) -> Path:
    folder = tmp_path / "config" / "plugins"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{name}.py"
    path.write_text(text, encoding="utf-8")
    return path


def test_filter_plugins(tmp_path):
    from genko import plugins

    _plugin(tmp_path, "sepia", SEPIA)
    _plugin(tmp_path, "broken", "def nothing(:\n")
    found = plugins.available()
    assert [p["key"] for p in found] == ["sepia"] and found[0]["name"] == "セピア"
    assert plugins.fields("plugin:sepia") == [("amount", "強さ", 0.0, 1.0, 1.0)]
    ep = _book()
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "p"}])
    import base64
    import io

    picture = Image.new("RGBA", (100, 140), (0, 0, 0, 0))
    picture.paste((30, 120, 220, 255), (10, 10, 60, 60))
    buf = io.BytesIO()
    picture.save(buf, format="PNG")
    apply_ops(ep, [{"op": "put_raster", "page": 1, "id": "p", "png_base64": base64.b64encode(buf.getvalue()).decode()},
                   {"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:sepia", "amount": 1}])
    layer = next(item for item in ep.pages[0].layers if item.id == "p")
    out = Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
    r, g, b, a = out.getpixel((30, 30))
    assert r > b and a == 255  # (blue became sepia)
    assert out.getpixel((90, 130))[3] == 0  # (the clear part stays clear)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:none"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:../evil"}])


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def test_plugins_and_the_art_pen_in_the_app(qapp, tmp_path):
    from genko.app.main import MainWindow

    _plugin(tmp_path, "sepia", SEPIA)
    ep = _book()
    save_episode(ep, tmp_path / "w.genko")
    win = MainWindow(tmp_path / "w.genko")
    win.show()
    try:
        assert win.layers.filter.findData("plugin:sepia") >= 0
        assert win.act_plugins.text() == "プラグインのフォルダーを開く"
        win.canvas.last_rotation = [0.0, 45.0]
        win._on_stroke([[40.0, 60.0, 0.7], [90.0, 60.0, 0.7]])
        stroke = win.target_layer().strokes[-1]
        assert stroke.rotation and stroke.rotation[-1] == 45.0
    finally:
        win.commit_now()
        win.close()
