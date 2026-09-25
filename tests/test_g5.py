"""G5: what similar tools have — moving a whole layer, the navigator, all pages at a glance, more 3D
shapes, colour adjustments and gradients."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from PIL import Image  # noqa: E402

from genko import filters, prim3d  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode, stroke_points  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def test_a_gradient_runs_from_one_colour_to_the_other():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "g", "name": "グラデ"},
                   {"op": "gradient_fill", "page": 1, "layer_id": "g", "area": {"poly": [[50, 100], [200, 100], [200, 150], [50, 150]]},
                    "from": [50, 125], "to": [200, 125], "rgb_from": [200, 0, 0], "rgb_to": [0, 0, 200]}])
    image = render_page(ep.pages[0], 60, mode="proof", episode=ep)
    px = lambda x, y: image.getpixel((round(x / 25.4 * 60), round(y / 25.4 * 60)))  # noqa: E731
    left, right = px(55, 125), px(195, 125)
    assert left[0] > 150 and left[2] < 60 and right[2] > 150 and right[0] < 60
    assert px(30, 125)[:3] == (255, 255, 255)  # nothing outside the area
    # fading out, and circles
    apply_ops(ep, [{"op": "gradient_fill", "page": 1, "layer_id": "g", "from": [100, 250], "to": [100, 300], "rgb_from": [0, 0, 0],
                    "shape": "radial"}])
    image = render_page(ep.pages[0], 60, mode="proof", episode=ep)
    assert px(100, 250)[0] < 60 and px(100, 320)[0] > 240
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "gradient_fill", "page": 1, "layer_id": "g", "from": [1, 1], "to": [1, 1]}])


def test_colour_adjustments():
    image = Image.new("RGBA", (4, 1), (0, 0, 0, 255))
    image.putdata([(0, 0, 0, 255), (64, 64, 64, 255), (128, 128, 128, 255), (200, 50, 50, 255)])
    levels = filters.apply_filter(image, "levels", {"black": 64, "white": 128})
    assert levels.getpixel((1, 0))[0] == 0 and levels.getpixel((2, 0))[0] == 255
    darker = filters.apply_filter(image, "curve", {"gamma": 2.0})
    assert darker.getpixel((2, 0))[0] < 128 and darker.getpixel((0, 0))[0] == 0
    grey = filters.apply_filter(image, "hue", {"shift": 0, "saturation": 0, "value": 1})
    r, g, b, _ = grey.getpixel((3, 0))
    assert abs(r - g) < 3 and abs(g - b) < 3


def test_cylinders_stairs_and_floors():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    for kind in ("cylinder", "stairs", "floor"):
        apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": kind, "id": kind, "pos": [120, 180, 0]}])
        prim = next(p for p in ep.pages[0].prims if p["id"] == kind)
        lines = prim3d.edges(prim)
        assert len(lines) > 10 and all(seen for _a, _b, seen in lines)
        x, y, w, h = prim3d.bbox(prim)
        assert w > 5 and h > 1
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "trace_prims", "page": 1, "layer_id": ink.id}])
    assert len(next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK).strokes) > 30
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": "sphere"}])


# --- the window --------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 4, PageSpec.b4_comic())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[60, 100], [120, 140]], "width_mm": 1, "stabilize": 0,
                    "taper": False},
                   {"op": "fill_area", "page": 1, "layer_id": ink.id, "area": {"poly": [[150, 200], [180, 200], [180, 230], [150, 230]]},
                    "rgb": [0, 0, 0]}])
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def test_the_layer_move_tool_moves_everything_on_the_layer(window):
    from test_m13 import _drag

    ink = next(layer for layer in window.current_page().layers if layer.role == LayerRole.INK)
    window.set_target_layer(ink.id)
    window.act_move.trigger()
    assert window.canvas.tool == "move" and window.tool_settings.title.text().startswith("レイヤー移動")
    _drag(window.canvas, None, None, path=[(100, 150), (110, 155), (120, 160)])
    ink = next(layer for layer in window.current_page().layers if layer.role == LayerRole.INK)
    start = stroke_points(ink.strokes[0])[0]
    assert abs(start[0] - 80) < 0.1 and abs(start[1] - 110) < 0.1
    box = ink.patches[0]["box"]
    assert abs(box[0] - 170) < 1 and abs(box[1] - 210) < 1


def test_the_gradient_tool(window):
    from test_m13 import _drag

    window.act_layer_paint.trigger()
    window.act_gradient.trigger()
    window.gradient_mode.setCurrentIndex(window.gradient_mode.findData("bw"))
    _drag(window.canvas, None, None, path=[(40, 60), (120, 60), (200, 60)])
    layer = window.target_layer()
    assert layer.patches and layer.patches[-1]["mode"] == "image"


def test_the_navigator_and_the_page_overview(window, qapp):
    canvas = window.canvas
    canvas.zoom_by(3.0)
    nav = window.navigator
    assert window.navigator_dock.isVisible()
    rect = nav._page_rect()
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent

    target = QPointF(rect.x() + rect.width() * 0.8, rect.y() + rect.height() * 0.2)
    nav.mousePressEvent(QMouseEvent(QEvent.Type.MouseButtonPress, target, nav.mapToGlobal(target), Qt.MouseButton.LeftButton,
                                    Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier))
    x, y = canvas._to_mm(QPointF(canvas.width() / 2, canvas.height() / 2))
    assert abs(x - 257 * 0.8) < 2 and abs(y - 364 * 0.2) < 2
    sx, sy, sw, sh = nav.seen_mm()
    assert sx < x < sx + sw and sy < y < sy + sh
    # all pages at a glance
    window.act_overview.trigger()
    overview = window.overview
    assert overview.list.count() == 4 and not overview.list.item(0).icon().isNull()
    overview._open(overview.list.item(2))
    assert window.current_page().index == 3
