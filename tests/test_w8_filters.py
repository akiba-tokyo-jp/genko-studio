"""Filters and corrections as CLIP STUDIO has them: brightness and contrast, the finer levels and a curve of
points, the despeckle, the filter inside the selection only."""

from __future__ import annotations

import numpy as np
import pytest
from PIL import Image

from genko import filters
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def _grey(value: int = 128, size=(40, 40)) -> Image.Image:
    return Image.new("RGBA", size, (value, value, value, 255))


def test_brightness_and_contrast():
    brighter = filters.apply_filter(_grey(), "brightness_contrast", {"brightness": 40})
    assert brighter.getpixel((1, 1))[0] > 170
    grey = Image.new("RGBA", (2, 1))
    grey.putpixel((0, 0), (100, 100, 100, 255))
    grey.putpixel((1, 0), (160, 160, 160, 255))
    stronger = filters.apply_filter(grey, "brightness_contrast", {"contrast": 60})
    assert stronger.getpixel((0, 0))[0] < 100 and stronger.getpixel((1, 0))[0] > 160
    assert "brightness_contrast" in filters.ADJUSTMENTS


def test_levels_middle_output_and_one_colour():
    table = filters.levels_table(0, 255, 2.0)
    assert table[0] == 0 and table[255] == 255 and table[128] > 160  # (the middle lighter)
    assert filters.levels_table(0, 255, 1.0, 40, 200)[0] == 40
    red_only = filters.apply_filter(_grey(), "levels", {"black": 0, "white": 255, "gamma": 2.0, "channel": "r"})
    r, g, b, _ = red_only.getpixel((1, 1))
    assert r > 160 and g == 128 and b == 128


def test_a_curve_through_points_never_turns_back():
    table = filters.curve_table([[0, 0], [64, 120], [192, 200], [255, 255]])
    assert table[64] == 120 and table[192] == 200
    assert all(b >= a for a, b in zip(table, table[1:]))
    out = filters.apply_filter(_grey(64), "curve", {"points": [[0, 0], [64, 120], [255, 255]]})
    assert out.getpixel((0, 0))[0] == 120
    with pytest.raises(ValueError):
        filters.curve_table([[10, 10]])


def test_despeckle_takes_small_dots_and_fills_small_holes():
    arr = np.zeros((120, 120, 4), dtype=np.uint8)
    arr[5:7, 5:7] = (0, 0, 0, 255)          # a speck
    arr[20:100, 40:60] = (0, 0, 0, 255)     # a line
    arr[50:52, 48:50] = (255, 255, 255, 255)  # a hole in it
    out = np.asarray(filters.apply_filter(Image.fromarray(arr, "RGBA"), "despeckle", {"size_px": 4, "what": "both"}))
    assert out[5, 5, 3] == 0 and out[50, 48, 3] == 255 and out[50, 48, 0] == 0 and out[60, 50, 3] == 255
    scan = np.full((60, 60, 4), 255, dtype=np.uint8)
    scan[10:12, 10:12] = (0, 0, 0, 255)
    out = np.asarray(filters.apply_filter(Image.fromarray(scan, "RGBA"), "despeckle", {"size_px": 4}))
    assert tuple(out[10, 10]) == (255, 255, 255, 255)  # (a scan: the speck becomes paper)


def test_a_filter_inside_the_selection_only():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "p"},
                        {"op": "fill_area", "page": 1, "layer_id": "p", "area": {"poly": [[20, 20], [120, 20], [120, 120], [20, 120]]},
                         "rgb": [100, 100, 100]}])
    apply_ops(episode, [{"op": "filter_raster", "page": 1, "id": "p", "kind": "invert",
                         "area": {"poly": [[20, 20], [60, 20], [60, 120], [20, 120]]}}])
    layer = next(item for item in episode.pages[0].layers if item.id == "p")
    from genko.raster import ensure_raster

    image = ensure_raster(episode.pages[0], layer)
    px = lambda x_mm, y_mm: image.getpixel((round(x_mm / 25.4 * 200), round(y_mm / 25.4 * 200)))  # noqa: E731
    assert px(40, 70)[0] > 140 and px(100, 70)[0] < 110
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "filter_raster", "page": 1, "id": "p", "kind": "curve", "points": [[3, 3]]}])


def test_the_filter_preview_shows_on_the_page_and_goes(qapp, tmp_path):
    from genko.app.main import MainWindow

    window = MainWindow()
    try:
        page = window._current()
        window.apply_ops([{"op": "add_layer", "page": page.index, "kind": "paint", "id": "pv"},
                          {"op": "fill_area", "page": page.index, "layer_id": "pv", "area": {"poly": [[20, 20], [120, 20], [120, 120], [20, 120]]},
                           "rgb": [0, 0, 0]}])
        page = window._current()
        layer = next(item for item in page.layers if item.id == "pv")
        window.preview_filter(page, layer, "invert", {})
        shown = window._frame_page(page)
        assert any(item.id == "pv~preview" for item in shown.layers)
        assert next(item for item in page.layers if item.id == "pv").patches  # (the layer itself is untouched)
        window.preview_filter(page, layer, "invert", None)
        assert window._frame_page(page) is page
    finally:
        window.close()


def test_the_filter_dialog_curve_and_choices(qapp):
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication

    from genko.app import filter_dialog
    from genko.app.main import FILTER_FIELDS

    seen = []

    def accept():
        dialog = QApplication.activeModalWidget()
        dialog.curve.points.insert(1, [128, 190])
        dialog.curve.changed.emit()
        dialog.accept()

    QTimer.singleShot(50, accept)
    values = filter_dialog.ask(None, "curve", FILTER_FIELDS["curve"], {}, preview=seen.append)
    assert values["points"] == [[0, 0], [128, 190], [255, 255]] and values["channel"] == "rgb"
    assert seen[-1] is None  # (the preview taken away when the dialog closes)


def test_the_levels_dialog_shows_the_layers_histogram(qapp):
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication, QLabel

    from genko.app import filter_dialog
    from genko.app.main import FILTER_FIELDS

    found = []

    def look():
        dialog = QApplication.activeModalWidget()
        found.extend(label for label in dialog.findChildren(QLabel) if label.pixmap() is not None and not label.pixmap().isNull())
        dialog.reject()

    QTimer.singleShot(50, look)
    counts = [0] * 256
    counts[30], counts[200] = 500, 900
    assert filter_dialog.ask(None, "levels", FILTER_FIELDS["levels"], {}, histogram=counts) is None
    assert found
