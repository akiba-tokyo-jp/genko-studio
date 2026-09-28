"""For colour work and the rest: gradients of many colours (ellipses, repeating), a gradient map of one's own
colours, 光彩拡散 and 雨, taking pages from another book, backups, the obi, and brushes that mix with the
colour already there (下地混色・色延び)."""

from __future__ import annotations

import os
import zipfile
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PIL import Image  # noqa: E402

from genko import backup, covers, filters, merge, render  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def test_a_gradient_of_many_colours_an_ellipse_and_repeating():
    stops = [[0.0, [255, 0, 0], 1.0], [0.5, [0, 255, 0], 1.0], [1.0, [0, 0, 255], 1.0]]
    image = render.gradient_image((100, 10), 25.4, {"from": [0, 5], "to": [100, 5], "stops": stops})
    assert image.getpixel((1, 5))[0] > 240 and image.getpixel((50, 5))[1] > 240 and image.getpixel((99, 5))[2] > 240
    # repeating: the colours start again past the end; mirrored: they come back
    two = [[0.0, [0, 0, 0], 1.0], [1.0, [255, 255, 255], 1.0]]
    again = render.gradient_image((100, 4), 25.4, {"from": [0, 2], "to": [25, 2], "stops": two, "repeat": "repeat"})
    back = render.gradient_image((100, 4), 25.4, {"from": [0, 2], "to": [25, 2], "stops": two, "repeat": "mirror"})
    stays = render.gradient_image((100, 4), 25.4, {"from": [0, 2], "to": [25, 2], "stops": two})
    assert again.getpixel((27, 2))[0] < 40 and back.getpixel((27, 2))[0] > 200 and stays.getpixel((60, 2))[0] > 240
    # an ellipse twice as wide across as along: at the same distance, across is still nearer the middle's colour
    oval = render.gradient_image((100, 100), 25.4, {"from": [50, 50], "to": [70, 50], "stops": two, "shape": "ellipse", "ratio": 2})
    assert oval.getpixel((50, 65))[0] < oval.getpixel((65, 50))[0]


def test_gradient_fill_takes_stops_shape_and_repeat_and_refuses_nonsense():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "g"},
                   {"op": "gradient_fill", "page": 1, "layer_id": "g", "from": [50, 100], "to": [150, 100], "shape": "ellipse",
                    "ratio": 0.5, "repeat": "mirror",
                    "stops": [[0, [200, 0, 0], 1], [0.3, [0, 200, 0], 1], [1, [0, 0, 200], 0.5]]}])
    for bad in ({"stops": [[0, [0, 0, 0], 1]]}, {"repeat": "twice"}, {"shape": "star"}):
        with pytest.raises(ApplyError):
            apply_ops(ep, [{"op": "gradient_fill", "page": 1, "layer_id": "g", "from": [1, 1], "to": [50, 50], **bad}])


def test_a_gradient_map_of_ones_own_colours_glow_and_rain():
    grey = Image.new("RGBA", (3, 1))
    grey.putdata([(0, 0, 0, 255), (128, 128, 128, 255), (255, 255, 255, 255)])
    mapped = filters.apply_filter(grey, "gradient_map", {"stops": [[0, [0, 0, 128]], [0.5, [255, 0, 0]], [1, [255, 255, 0]]]})
    assert mapped.getpixel((0, 0))[:3] == (0, 0, 128) and mapped.getpixel((1, 0))[0] > 240 and mapped.getpixel((2, 0))[:3] == (255, 255, 0)
    dark = Image.new("RGBA", (60, 60), (20, 20, 20, 255))
    dark.paste((255, 255, 255, 255), (25, 25, 35, 35))
    glowing = filters.apply_filter(dark, "glow", {"radius": 6, "amount": 1.0})
    assert glowing.getpixel((22, 30))[0] > dark.getpixel((22, 30))[0]  # (the light spreads past the bright square)
    rainy = filters.apply_filter(Image.new("RGBA", (120, 120), (0, 0, 0, 255)), "rain", {"count": 80, "rgb": [255, 255, 255]})
    assert max(rainy.split()[0].getextrema()) > 100
    assert rainy.tobytes() == filters.apply_filter(Image.new("RGBA", (120, 120), (0, 0, 0, 255)), "rain",
                                                   {"count": 80, "rgb": [255, 255, 255]}).tobytes()  # (the same rain each time)


def _book(path: Path, lines: list[str]) -> Path:
    ep = new_episode("t", 1, len(lines), PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": i + 1, "text": text} for i, text in enumerate(lines)])
    save_episode(ep, path)
    return path


def test_pages_taken_from_another_book_with_their_lines_and_pictures(tmp_path):
    here = _book(tmp_path / "a.genko", ["いち", "に"])
    there = _book(tmp_path / "b.genko", ["さん", "よん", "ご"])
    (there / "assets" / "art").mkdir(parents=True)
    (there / "assets" / "art" / "x.png").write_bytes(b"png")
    assert merge.copy_assets(there, here) >= 1 and (here / "assets" / "art" / "x.png").exists()
    assert merge.copy_assets(there, here) == 0  # (already there: left)
    ep = load_episode(here)
    apply_ops(ep, [merge.import_op(there, pages=[2, 3], after=1)])
    assert len(ep.pages) == 4 and [p.index for p in ep.pages] == [1, 2, 3, 4]
    words = [[line.text for line in ep.story_for_page(i)] for i in range(1, 5)]
    assert words == [["いち"], ["よん"], ["ご"], ["に"]]
    assert len({p.id for p in ep.pages}) == 4
    with pytest.raises(ApplyError):
        apply_ops(ep, [merge.import_op(there, pages=[9])])
    with pytest.raises(ApplyError):
        apply_ops(ep, [merge.import_op(tmp_path / "none.genko")])


def test_backups_are_zipped_elsewhere_and_the_oldest_dropped(tmp_path):
    book = _book(tmp_path / "a.genko", ["いち"])
    folder = tmp_path / "backups"
    assert backup.due(folder, "a", 30)
    made = [backup.make(book, folder, keep=2, now=1_700_000_000 + i * 60) for i in range(3)]
    kept = backup.backups(folder, "a")
    assert kept == made[1:]
    assert any(name.endswith("project.json") for name in zipfile.ZipFile(kept[-1]).namelist())
    assert not backup.due(folder, "a", 30)
    with pytest.raises(ValueError):
        backup.make(book, book / "backups")


def test_an_obi_is_as_wide_as_the_jacket_and_as_tall_as_the_band():
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_cover", "kind": "obi", "spine_mm": 10, "flap_mm": 40, "height_mm": 60},
                   {"op": "add_cover", "kind": "jacket", "spine_mm": 10, "flap_mm": 40}])
    obi, jacket = ep.pages[-2], ep.pages[-1]
    assert obi.spec.width_mm == pytest.approx(jacket.spec.width_mm)
    assert obi.trim_rect_mm().height == pytest.approx(60, abs=0.01)
    assert [name for _, _, name in covers.folds(obi)] == [name for _, _, name in covers.folds(jacket)]
    with pytest.raises(ApplyError):
        apply_ops(new_episode("t", 1, 1, PageSpec.b4_comic()), [{"op": "add_cover", "kind": "obi", "spine_mm": 10, "height_mm": 5}])


def test_a_brush_that_mixes_with_the_colour_already_there():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "c"},
                   {"op": "add_stroke", "page": 1, "layer_id": "c", "points": [[40, 100], [40, 200]], "width_mm": 20,
                    "rgb": [255, 0, 0], "stabilize": 0, "taper": False}])
    across = [[20, 150], [60, 150], [90, 150], [120, 150]]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "c", "points": across, "width_mm": 3, "rgb": [0, 0, 255],
                    "mix": 0.8, "stretch": 0.9, "stabilize": 0, "taper": False}])
    layer = next(item for item in ep.pages[0].layers if item.id == "c")
    pieces = layer.strokes[1:]
    assert len(pieces) > 3
    colours = [piece.rgb for piece in pieces]
    reddest = max(range(len(colours)), key=lambda i: colours[i][0])
    assert colours[0][0] < 20 and colours[reddest][0] > 60  # (over the red: red is picked up)
    assert 0 < colours[-1][0] < colours[reddest][0]  # (carried on past it, fading)
    assert pieces[0].points[-1] == pieces[1].points[0]  # (the pieces meet)
    lower = [[x, 180] for x, _ in across]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "c", "points": lower, "width_mm": 3, "rgb": [0, 0, 255],
                    "mix": 0.8, "stretch": 0.0, "stabilize": 0, "taper": False}])
    layer = next(item for item in ep.pages[0].layers if item.id == "c")
    short = layer.strokes[len(pieces) + 1:]
    assert short[-1].rgb == (0, 0, 255)  # (nothing carried past the red)
    from genko import brushes

    mixer = brushes.from_dict("my_mix", {"label": "混ぜ", "mix": 0.5, "stretch": 0.7})
    assert mixer.mix == 0.5 and brushes.to_dict(mixer)["stretch"] == 0.7
    with pytest.raises(ValueError):
        brushes.from_dict("my_mix", {"label": "混ぜ", "mix": 2})


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def test_the_stops_editor_and_the_gradient_dialog(qapp, monkeypatch):
    from PySide6.QtWidgets import QDialog

    from genko.app import main
    from genko.app.gradient_editor import PRESETS, StopsEditor, stops_from

    editor = StopsEditor()
    editor.set_stops(PRESETS["虹"])
    assert len(editor.stops()) == 6
    editor._add()
    assert len(editor.stops()) == 7
    editor._remove(0)
    assert len(editor.stops()) == 6
    assert stops_from({"rgb_from": [1, 2, 3], "rgb_to": [4, 5, 6]})[1][1] == [4, 5, 6]

    def accept(dialog):
        editors = dialog.findChildren(StopsEditor)
        editors[0].set_stops(PRESETS["夕焼け"])
        return QDialog.DialogCode.Accepted

    monkeypatch.setattr(QDialog, "exec", accept)
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    out = main.gradient_dialog(None, ep.pages[0], {})
    assert out and len(out["stops"]) == 4
    mapped = main.gradient_map_dialog(None, {}, [[0, 0, 0], [255, 255, 255]])
    assert mapped and len(mapped["stops"]) == 4 and len(mapped["stops"][0]) == 2
