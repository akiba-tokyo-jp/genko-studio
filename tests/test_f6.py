"""F6: the missing features — layers (duplicate, merge down, draft, mask, colour, small pictures), free
transform, brushes of one's own, export ranges and checks, history, help and preferences."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from PIL import ImageChops, ImageStat  # noqa: E402

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerKind, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402

DPI = 60


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 100 + i * 8], [220, 110 + i * 8]],
                    "width_mm": 2, "stabilize": 0, "taper": False} for i in range(5)])
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "name": "影", "id": "u", "after": ink.id},
                   {"op": "add_stroke", "page": 1, "layer_id": "u", "points": [[60, 60], [60, 300]], "width_mm": 6, "stabilize": 0,
                    "taper": False, "rgb": [200, 30, 30]}])
    return ep, ink


def _diff(a, b) -> float:
    return max(ImageStat.Stat(ImageChops.difference(a.convert("RGB"), b.convert("RGB"))).mean)


def _layer(ep, layer_id):
    return next(layer for layer in ep.pages[0].layers if layer.id == layer_id)


def test_duplicate_and_merge_down_keep_the_picture():
    ep, ink = _book()
    before = render_page(ep.pages[0], DPI, episode=ep)
    apply_ops(ep, [{"op": "duplicate_layer", "page": 1, "id": "u", "new_id": "u2"}])
    twin = _layer(ep, "u2")
    assert twin.title == "影 のコピー" and len(twin.strokes) == 1
    assert twin.strokes[0].id != _layer(ep, "u").strokes[0].id
    assert [layer.id for layer in ep.pages[0].layers].index("u2") == [layer.id for layer in ep.pages[0].layers].index("u") + 1
    # pen onto pen stays lines
    apply_ops(ep, [{"op": "merge_down", "page": 1, "id": "u2"}])
    assert "u2" not in [layer.id for layer in ep.pages[0].layers] and len(_layer(ep, "u").strokes) == 2
    assert _layer(ep, "u").kind == LayerKind.STROKES
    assert _diff(before, render_page(ep.pages[0], DPI, episode=ep)) < 1.0
    # a half-see-through layer merged into the ink becomes pixels, and looks the same
    from genko.raster import WORKING_DPI

    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "u", "opacity": 0.5}])
    before = render_page(ep.pages[0], WORKING_DPI, episode=ep)  # (pixels are kept at the working resolution)
    apply_ops(ep, [{"op": "merge_down", "page": 1, "id": "u"}])
    merged = _layer(ep, ink.id)
    assert merged.kind == LayerKind.RASTER and merged.raster_png and not merged.strokes
    assert _diff(before, render_page(ep.pages[0], WORKING_DPI, episode=ep)) < 0.5
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "merge_down", "page": 1, "id": ep.pages[0].layers[0].id}])  # nothing below


def test_a_draft_layer_is_seen_but_never_exported():
    ep, _ink = _book()
    shown = render_page(ep.pages[0], DPI, mode="proof", episode=ep)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "u", "exportable": False}])
    printed = render_page(ep.pages[0], DPI, episode=ep)
    x = round(60 / 25.4 * DPI)
    y = round(200 / 25.4 * DPI)
    assert printed.getpixel((x, y))[0] > 200 and printed.getpixel((x, y))[1] > 200  # no red in print
    assert shown.getpixel((x, y))[1] < 120  # but on screen
    assert _diff(shown, render_page(ep.pages[0], DPI, mode="proof", episode=ep)) < 0.01


def test_a_mask_hides_and_the_pen_shows_again(tmp_path: Path):
    ep, _ink = _book()
    x, y_hidden, y_shown = round(60 / 25.4 * DPI), round(250 / 25.4 * DPI), round(80 / 25.4 * DPI)
    apply_ops(ep, [{"op": "set_layer_mask", "page": 1, "id": "u", "area": {"poly": [[0, 0], [257, 0], [257, 150], [0, 150]]}}])
    image = render_page(ep.pages[0], DPI, episode=ep)
    assert image.getpixel((x, y_shown))[1] < 120  # red where the mask shows
    assert image.getpixel((x, y_hidden))[1] > 200  # hidden below
    apply_ops(ep, [{"op": "paint_mask", "page": 1, "id": "u", "points": [[50, 240], [70, 260]], "width_mm": 20, "show": True}])
    assert render_page(ep.pages[0], DPI, episode=ep).getpixel((x, y_hidden))[1] < 120
    apply_ops(ep, [{"op": "paint_mask", "page": 1, "id": "u", "points": [[60, 70], [60, 90]], "width_mm": 20, "show": False}])
    assert render_page(ep.pages[0], DPI, episode=ep).getpixel((x, y_shown))[1] > 200
    # off, inverted, kept on disk, gone
    apply_ops(ep, [{"op": "set_layer_mask", "page": 1, "id": "u", "enabled": False}])
    assert render_page(ep.pages[0], DPI, episode=ep).getpixel((x, y_shown))[1] < 120
    apply_ops(ep, [{"op": "set_layer_mask", "page": 1, "id": "u", "enabled": True, "invert": True}])
    assert render_page(ep.pages[0], DPI, episode=ep).getpixel((x, y_shown))[1] < 120
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "u", "color": [40, 110, 230]}])
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko")
    layer = _layer(again, "u")
    assert layer.mask and layer.mask["enabled"] and layer.mask["png"] and layer.color == (40, 110, 230)
    assert _diff(render_page(ep.pages[0], DPI, episode=ep), render_page(again.pages[0], DPI, episode=again)) < 0.01
    apply_ops(again, [{"op": "set_layer_mask", "page": 1, "id": "u", "delete": True}])
    assert _layer(again, "u").mask is None


def test_a_layer_colour_is_for_the_screen_only():
    ep, _ink = _book()
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "u", "color": [40, 110, 230]}])
    x, y = round(60 / 25.4 * DPI), round(200 / 25.4 * DPI)
    r, g, b = render_page(ep.pages[0], DPI, mode="proof", episode=ep).getpixel((x, y))[:3]
    assert b > 180 and r < 100
    r, g, b = render_page(ep.pages[0], DPI, episode=ep).getpixel((x, y))[:3]
    assert r > 150 and b < 100
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "u", "color": None}])
    assert _layer(ep, "u").color is None


# --- the window --------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    ep, _ink = _book()
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def test_the_layer_panel_duplicates_merges_masks_and_shows_pictures(window):
    from test_m13 import _drag

    panel = window.layers
    window.set_target_layer("u")
    panel.refresh()
    assert panel.list.item(panel.ids.index("u")).icon().isNull() is False
    panel._duplicate()
    page = window.current_page()
    assert len([layer for layer in page.layers if layer.title == "影 のコピー"]) == 1
    panel._merge_down()
    assert len(window.current_page().layers) == len(page.layers) - 1
    # draft and colour
    window.set_target_layer("u")
    panel.refresh()
    panel.draft.click()
    panel.refresh()
    assert not _layer(window.episode, "u").exportable and "下描き" in panel.list.item(panel.ids.index("u")).text()
    panel.tint.setCurrentIndex(1)
    panel.tint.activated.emit(1)
    assert _layer(window.episode, "u").color == (40, 110, 230)
    # a mask from the selection, then the pen shows more of it
    window.canvas.set_selection({"poly": [[0, 0], [257, 0], [257, 150], [0, 150]]})
    panel._mask_from_selection()
    assert _layer(window.episode, "u").mask
    panel.act_mask_edit.setChecked(True)
    assert window.mask_edit
    before = _layer(window.episode, "u").mask["png"]
    strokes = len(_layer(window.episode, "u").strokes)
    window.act_pen.trigger()
    _drag(window.canvas, None, None, path=[(60, 240), (62, 250), (64, 260)])
    after = _layer(window.episode, "u")
    assert after.mask["png"] != before and len(after.strokes) == strokes  # the mask changed, not the lines
    panel.act_mask_edit.setChecked(False)
    assert not window.mask_edit
