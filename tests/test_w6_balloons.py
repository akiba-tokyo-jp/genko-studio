"""Balloons: their own line and fill colours, words moved inside, a bent tail, the balloon eraser, a
hand-drawn outline as a curve."""

from __future__ import annotations

import os

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402

DPI = 100


def _book(**style):
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "a", "text": "あ", "x_mm": 60, "y_mm": 60, "w_mm": 40, "h_mm": 30,
                    "balloon": "box", "style": style}])
    return ep


def _px(ep, x, y):
    image = render_page(ep.pages[0], DPI, mode="proof", episode=ep).convert("RGB")
    return image.getpixel((round(x / 25.4 * DPI), round(y / 25.4 * DPI)))


def test_the_balloons_line_and_fill_have_their_own_colours():
    ep = _book(line_rgb=[200, 0, 0], fill_rgb=[0, 0, 200], border_mm=1.0)
    assert _px(ep, 65, 75) == (0, 0, 200)
    edge = _px(ep, 60.3, 75)
    assert edge[0] > 150 and edge[2] < 80
    ep = _book(fill_rgb=[0, 0, 0], fill_opacity=0.5)
    assert 100 < _px(ep, 65, 75)[0] < 160  # (half covering the white paper)


def test_the_words_move_inside_the_balloon():
    plain = render_page(_book().pages[0], DPI, mode="proof", episode=_book()).convert("L")
    moved_ep = _book(text_dx_mm=10)
    moved = render_page(moved_ep.pages[0], DPI, mode="proof", episode=moved_ep).convert("L")

    def ink_x(image):
        from PIL import ImageOps

        box = ImageOps.invert(image.crop((round(62 / 25.4 * DPI), round(64 / 25.4 * DPI),
                                          round(98 / 25.4 * DPI), round(86 / 25.4 * DPI)))).point(lambda v: 255 if v > 128 else 0).getbbox()
        return box[0]

    assert ink_x(moved) - ink_x(plain) == pytest.approx(10 / 25.4 * DPI, abs=3)


def test_a_bent_tail_and_the_balloon_eraser():
    ep = _book()
    apply_ops(ep, [{"op": "move_line", "id": "a", "tails": [{"to": [130, 60], "vias": [[110, 100]]}]}])
    line = ep.story[0] if hasattr(ep, "story") else ep.story_for_page(1)[0]
    assert line.tails[0]["vias"] == [[110.0, 100.0]]
    image = render_page(ep.pages[0], DPI, mode="proof", episode=ep).convert("L")
    near_bend = image.crop(tuple(round(v / 25.4 * DPI) for v in (105, 95, 115, 105)))
    assert near_bend.getextrema()[0] < 100  # (the tail's outline goes down through the bend)
    ep = _book(border_mm=0.8)
    apply_ops(ep, [{"op": "cut_balloon", "id": "a", "points": [[60, 75], [70, 75]], "width_mm": 6}])
    assert ep.story_for_page(1)[0].style["cuts"][0]["points"][0] == [0.0, 15.0]
    assert _px(ep, 60.2, 75) == (255, 255, 255)  # (the line is cut away there)
    assert _px(ep, 60.2, 62) != (255, 255, 255)  # (and kept elsewhere)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"cuts": None}}])
    assert _px(ep, 60.2, 75) != (255, 255, 255)


def test_a_hand_drawn_outline_becomes_a_curve():
    from genko.balloons import _smooth_closed

    square = [(0, 0), (10, 0), (10, 10), (0, 10)]
    smooth = _smooth_closed(square)
    assert len(smooth) > len(square) * 4
    ep = _book(path_curve=True)
    assert ep.story_for_page(1)[0].style["path_curve"] is True


def test_the_text_panel_and_the_bend(tmp_path):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")
    from genko.app.main import MainWindow
    from genko.io import save_episode

    ep = _book()
    apply_ops(ep, [{"op": "move_line", "id": "a", "tails": [{"to": [130, 60]}]}])
    save_episode(ep, tmp_path / "b.genko")
    win = MainWindow(tmp_path / "b.genko")
    line = win.episode.story_for_page(1)[0]
    bent = win._with_bend(line, line.tails[0])
    assert len(bent["vias"]) == 1 and "via" not in bent
    again = win._with_bend(line, bent)
    assert len(again["vias"]) == 2
    win.close()
