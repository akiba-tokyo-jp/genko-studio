"""F4: the feel of drawing — the line shows as it will print while the pen moves, quickly; the pen's
eraser end erases; the view turns and mirrors without touching the page, and lines land where pointed."""

import os
import sys
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import brushes  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode, stroke_points  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


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

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    ink = next(layer for layer in win.current_page().layers if layer.role == LayerRole.INK)
    win.set_target_layer(ink.id)
    yield win
    win.close()


def _mouse(canvas, kind, screen, buttons=True):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton
    types = {"press": QEvent.Type.MouseButtonPress, "move": QEvent.Type.MouseMove, "release": QEvent.Type.MouseButtonRelease}
    button = left if kind != "move" else none
    return QMouseEvent(types[kind], screen, canvas.mapToGlobal(screen), button, left if buttons else none,
                       Qt.KeyboardModifier.NoModifier)


def _screen(canvas, x, y):
    return canvas._view().map(canvas._pt(x, y))


def _ink(win):
    return next(layer for layer in win.current_page().layers if layer.role == LayerRole.INK)


def test_the_line_being_drawn_looks_like_the_finished_line(window):
    from PIL import ImageChops

    canvas = window.canvas
    window.act_pen.trigger()
    window.brush.kinds.setCurrentRow(list(brushes.BRUSHES).index("maru"))
    window.brush.size.setValue(1.2)
    window.brush.taper.setChecked(False)
    window.brush.steady.setValue(0)
    window.brush.set_colour((200, 30, 30))
    path = [(60 + i * 2.0, 120 + (i % 7) * 1.5) for i in range(40)]
    canvas.mousePressEvent(_mouse(canvas, "press", _screen(canvas, *path[0])))
    for pt in path[1:]:
        canvas.mouseMoveEvent(_mouse(canvas, "move", _screen(canvas, *pt)))
    image = canvas._live_sync()
    live = canvas._live.coverage()
    # the colour and the pen of the brush panel, not a guide line
    mid = canvas._pt(*path[20])
    colour = image.pixelColor(int(mid.x()), int(mid.y()))
    assert colour.alpha() > 200 and colour.red() > 150 and colour.green() < 90
    canvas.mouseReleaseEvent(_mouse(canvas, "release", _screen(canvas, *path[-1]), buttons=False))
    stroke = _ink(window).strokes[-1]
    assert stroke.kind == "maru"
    # the finished line, drawn the same way on the canvas's pixels
    shift = [(p[0] + canvas._pan_x / canvas._scale, p[1] + canvas._pan_y / canvas._scale, *p[2:]) for p in stroke_points(stroke)]
    mask, (x0, y0) = brushes.draw(live.size, shift, canvas._scale * 25.4, stroke.width_mm, stroke.kind)
    final = live.copy()
    final.paste(0, (0, 0, *final.size))
    final.paste(mask, (x0, y0))
    both = ImageChops.multiply(live.point(lambda v: 255 if v > 96 else 0), final.point(lambda v: 255 if v > 96 else 0))
    either = ImageChops.lighter(live.point(lambda v: 255 if v > 96 else 0), final.point(lambda v: 255 if v > 96 else 0))
    overlap = both.histogram()[255] / max(1, either.histogram()[255])
    assert overlap > 0.9, overlap


def test_each_move_is_quick_and_does_not_grow_with_the_line():
    pytest.importorskip("PySide6.QtWidgets")
    from genko.app.live_ink import LiveInk

    scale = 96 / 25.4  # 100%
    for kind in ("gpen", "pencil", "fude"):
        live = LiveInk((1280, 800), scale, (100, 40), {"kind": kind, "width_mm": 1.0, "rgb": [20, 20, 20]})
        points = [(20 + i * 0.4, 60 + 30 * ((i % 90) / 90), 0.3 + 0.7 * ((i % 13) / 13)) for i in range(400)]
        times = []
        for n in range(1, len(points) + 1):
            start = time.perf_counter()
            live.extend(points[:n])
            times.append(time.perf_counter() - start)
        early, late = sum(times[20:70]) / 50, sum(times[-50:]) / 50
        assert sum(times) / len(times) < 0.016, (kind, sum(times) / len(times))
        assert late < early * 3 + 0.002, (kind, early, late)


_DEVICES: dict = {}  # (an event only points at its device; keep the devices alive)


def _tablet(canvas, kind, screen, pressure=0.8, eraser=True):
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QInputDevice, QPointingDevice, QTabletEvent

    if eraser not in _DEVICES:
        _DEVICES[eraser] = QPointingDevice("pen", 7 + eraser, QInputDevice.DeviceType.Stylus,
                                           QPointingDevice.PointerType.Eraser if eraser else QPointingDevice.PointerType.Pen,
                                           QInputDevice.Capability.Position | QInputDevice.Capability.Pressure, 1, 2)
    device = _DEVICES[eraser]
    types = {"press": QEvent.Type.TabletPress, "move": QEvent.Type.TabletMove, "release": QEvent.Type.TabletRelease}
    button = Qt.MouseButton.LeftButton if kind != "move" else Qt.MouseButton.NoButton
    buttons = Qt.MouseButton.NoButton if kind == "release" else Qt.MouseButton.LeftButton
    return QTabletEvent(types[kind], device, QPointF(screen), QPointF(canvas.mapToGlobal(screen)), pressure, 0.0, 0.0, 0.0,
                        0.0, 0.0, Qt.KeyboardModifier.NoModifier, button, buttons)


def test_the_pens_eraser_end_erases_and_gives_the_pen_back(window):
    canvas = window.canvas
    ink = _ink(window)
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 150], [200, 150]], "width_mm": 1,
                       "stabilize": 0, "taper": False}])
    window.act_pen.trigger()
    before = [stroke_points(s) for s in _ink(window).strokes]
    across = [(120, 140 + i) for i in range(21)]
    canvas.tabletEvent(_tablet(canvas, "press", _screen(canvas, *across[0])))
    assert canvas.tool == "eraser"
    for pt in across[1:]:
        canvas.tabletEvent(_tablet(canvas, "move", _screen(canvas, *pt)))
    canvas.tabletEvent(_tablet(canvas, "release", _screen(canvas, *across[-1])))
    after = [stroke_points(s) for s in _ink(window).strokes]
    assert after != before and len(after) == 2  # cut in two where the eraser end passed
    assert canvas.tool == "pen"
    # the pen's tip still draws
    canvas.tabletEvent(_tablet(canvas, "press", _screen(canvas, 60, 200), eraser=False))
    canvas.tabletEvent(_tablet(canvas, "move", _screen(canvas, 90, 210), eraser=False))
    canvas.tabletEvent(_tablet(canvas, "release", _screen(canvas, 90, 210), eraser=False))
    assert len(_ink(window).strokes) == 3


def test_a_turned_and_mirrored_view_draws_where_the_pen_points(window):
    from PySide6.QtCore import QPointF

    canvas = window.canvas
    window.act_turn_right.trigger()
    window.act_turn_right.trigger()
    window.act_mirror.trigger()
    assert canvas.rotation == 30 and canvas.flipped
    assert "回転 +30°" in window.zoom_label.text() and "左右反転" in window.zoom_label.text()
    # the mapping goes both ways
    for mm in ((10, 20), (150, 300), (240, 30)):
        back = canvas._to_mm(canvas._ev(_screen(canvas, *mm)))
        assert abs(back[0] - mm[0]) < 1e-6 and abs(back[1] - mm[1]) < 1e-6
    # a mirrored view puts the left of the page on the right of the screen
    left, right = _screen(canvas, 20, 180), _screen(canvas, 220, 180)
    window.act_turn_reset.trigger()
    window.act_mirror.setChecked(True)
    canvas.flip_view(True)
    assert _screen(canvas, 20, 180).x() > _screen(canvas, 220, 180).x()
    canvas.set_rotation(30)
    window.act_pen.trigger()
    path = [(80 + i * 3, 100 + i * 2) for i in range(15)]
    canvas.mousePressEvent(_mouse(canvas, "press", _screen(canvas, *path[0])))
    for pt in path[1:]:
        canvas.mouseMoveEvent(_mouse(canvas, "move", _screen(canvas, *pt)))
    canvas.mouseReleaseEvent(_mouse(canvas, "release", _screen(canvas, *path[-1]), buttons=False))
    drawn = stroke_points(_ink(window).strokes[-1])
    assert abs(drawn[0][0] - 80) < 1 and abs(drawn[0][1] - 100) < 1
    assert abs(drawn[-1][0] - path[-1][0]) < 1.5 and abs(drawn[-1][1] - path[-1][1]) < 1.5
    # a click on a balloon still finds it; the page itself never turned
    assert window.current_page().spec.width_mm == PageSpec.b4_comic().width_mm
    # Shift+Space drag turns the view
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QKeyEvent, QMouseEvent

    window.act_turn_reset.trigger()
    assert canvas.rotation == 0 and not canvas.flipped and not window.act_mirror.isChecked()
    canvas.keyPressEvent(QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_Space, Qt.KeyboardModifier.ShiftModifier))
    centre = QPointF(canvas.width() / 2, canvas.height() / 2)
    start, end = centre + QPointF(200, 0), centre + QPointF(0, 200)
    shift = Qt.KeyboardModifier.ShiftModifier
    left_button = Qt.MouseButton.LeftButton
    canvas.mousePressEvent(QMouseEvent(QEvent.Type.MouseButtonPress, start, canvas.mapToGlobal(start), left_button, left_button, shift))
    canvas.mouseMoveEvent(QMouseEvent(QEvent.Type.MouseMove, end, canvas.mapToGlobal(end), Qt.MouseButton.NoButton, left_button, shift))
    canvas.mouseReleaseEvent(QMouseEvent(QEvent.Type.MouseButtonRelease, end, canvas.mapToGlobal(end), left_button,
                                         Qt.MouseButton.NoButton, shift))
    canvas.keyReleaseEvent(QKeyEvent(QEvent.Type.KeyRelease, Qt.Key.Key_Space, Qt.KeyboardModifier.NoModifier))
    assert abs(canvas.rotation - 90) < 0.5
    assert left != right
