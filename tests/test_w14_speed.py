"""Working quickly as CLIP STUDIO allows: a tool only while its key is held, each tool's own modifier keys,
a brush's details opened again, brush size presets of one's own, Alt+click to show one layer alone."""

from __future__ import annotations

import pytest

QtWidgets = pytest.importorskip("PySide6.QtWidgets")
from PySide6.QtCore import Qt  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    from PySide6.QtCore import QSettings

    from genko.app.main import MainWindow

    settings = QSettings("Genko", "Genko Studio")
    kept = {k: settings.value(k) for k in ("keys/hold_swap", "brush/sizes", "keys/pen/shift", "keys/pen/alt")}
    win = MainWindow()
    yield win
    win.close()
    for key, value in kept.items():
        if value is None:
            settings.remove(key)
        else:
            settings.setValue(key, value)


def test_a_tools_key_held_is_that_tool_only_while_held(window):
    import time

    window._tool("pen")
    window._tool("eraser")  # (as E pressed)
    start = window._tool_switch[2]
    assert not window.hold_key_released(Qt.Key.Key_E, now=start + 0.1)  # (a tap: the eraser stays)
    assert window.canvas.tool == "eraser"
    window._tool("pen")
    window._tool("eraser")
    assert window.hold_key_released(Qt.Key.Key_E, now=time.monotonic() + 1.0)  # (held: back to the pen)
    assert window.canvas.tool == "pen"
    window._tool("eraser")
    assert not window.hold_key_released(Qt.Key.Key_B, now=time.monotonic() + 1.0)  # (another key: nothing)
    from PySide6.QtCore import QSettings

    QSettings("Genko", "Genko Studio").setValue("keys/hold_swap", False)
    window._tool("pen")
    window._tool("eraser")
    assert not window.hold_key_released(Qt.Key.Key_E, now=time.monotonic() + 1.0)


def test_each_tool_has_its_own_modifier_keys(window):
    from genko.app import workspace

    workspace.set_tool_modifier("pen", "shift", "eraser")
    workspace.set_tool_modifier("pen", "alt", "none")
    window.canvas.tool_modifiers = workspace.tool_modifiers()
    canvas = window.canvas
    canvas.set_tool("pen")
    canvas.hold_modifier("shift", True)
    assert canvas.tool == "eraser"
    canvas.hold_modifier("alt", False)  # (another key let go: the eraser stays)
    assert canvas.tool == "eraser"
    canvas.hold_modifier("shift", False)
    assert canvas.tool == "pen"
    canvas.hold_modifier("alt", True)  # (the pen says Alt does nothing)
    assert canvas.tool == "pen"
    canvas.hold_modifier("alt", False)
    canvas.set_tool("fill")
    canvas.hold_modifier("alt", True)  # (the fill has none of its own: the common スポイト)
    assert canvas.tool == "picker"
    canvas.hold_modifier("alt", False)
    canvas.hold_modifier("shift", True)  # (no Shift of its own: no switch)
    assert canvas.tool == "fill"
    canvas.hold_modifier("shift", False)
    workspace.set_tool_modifier("pen", "shift", "")
    workspace.set_tool_modifier("pen", "alt", "")


def test_brush_size_presets_of_ones_own(window):
    panel = window.brush
    panel.size.setValue(1.7)
    panel.set_sizes([*panel.sizes(), panel.size.value()])
    assert 1.7 in panel.sizes()
    panel.set_sizes([v for v in panel.sizes() if v != 10.0])
    assert 10.0 not in panel.sizes()
    labels = [panel.size_grid.itemAt(i).widget().text() for i in range(panel.size_grid.count())]
    assert "1.7" in labels and labels[-1] == "＋"
    panel.set_sizes([])
    from genko.app.brush_panel import SIZES

    assert panel.sizes() == SIZES


def test_a_brushs_details_opened_again(window, monkeypatch):
    from genko import brushes
    from genko.app import brush_panel

    key = "my_test_edit"
    brushes.CUSTOM[key] = brushes.from_dict(key, {"label": "試し", "base": "gpen", "width_mm": 0.6, "opacity": 1.0})
    brushes.save_to_library(key, brushes.to_dict(brushes.CUSTOM[key]))
    window.brush.reload_kinds(select=key)

    def exec_(dialog):
        assert dialog.editing and dialog.width.value() == pytest.approx(0.6)
        dialog.width.setValue(1.4)
        dialog.opacity.setValue(60)
        return True

    monkeypatch.setattr(brush_panel.BrushDialog, "exec", exec_)
    window._edit_brush()
    edited = brushes.load_library()[key]
    assert edited["width_mm"] == pytest.approx(1.4) and edited["opacity"] == pytest.approx(0.6) and edited["label"] == "試し"
    assert brushes.brush(key).width_mm == pytest.approx(1.4)


def test_alt_click_shows_one_layer_alone_and_again_brings_the_rest_back(window):
    page = window._current()
    window.apply_ops([{"op": "add_layer", "page": page.index, "kind": "paint", "id": "a"},
                      {"op": "add_layer", "page": page.index, "kind": "paint", "id": "b"}])
    shown_before = {layer.id for layer in window._current().layers if layer.visible}
    window.layers.solo("a")
    page = window._current()
    assert [layer.id for layer in page.layers if layer.visible] == ["a"]
    window.layers.solo("a")
    assert {layer.id for layer in window._current().layers if layer.visible} == shown_before
