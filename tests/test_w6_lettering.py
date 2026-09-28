"""Lettering: any colour or size for part of a line, 縦中横 chosen by hand, justified columns, ruby size
and モノルビ, and a line set under a layer."""

from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko import fonts  # noqa: E402
from genko.app.lettering import parse_marks, with_marks  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402
from genko.tategaki import cells, compose, mark_tcy, mono_runs  # noqa: E402


def test_any_colour_or_size_and_tcy_by_hand():
    text, _ruby, _dots, styles = parse_marks("{#3060c0|青い}と{×1.3|大}と{縦中横|ABC}")
    assert text == "青いと大とABC"
    assert styles == [["青い", {"rgb": [48, 96, 192]}], ["大", {"scale": 1.3}], ["ABC", {"tcy": True}]]

    class Line:
        pass

    line = Line()
    line.text, line.ruby_runs, line.emphasis_runs, line.style_runs = text, [], [], styles
    assert with_marks(line) == "{#3060c0|青い}と{×1.3|大}と{縦中横|ABC}"
    marked = mark_tcy("いまABCだ", [["ABC", {"tcy": True}]])
    assert "ABC" in cells(marked, tcy=False)


def test_justified_columns_fill_the_height_and_ruby_sizes():
    face = fonts.face(None)
    short = compose("あいう", face.font(40), 40, 400, face=face)
    spread = compose("あいう", face.font(40), 40, 400, face=face, align="justify")
    assert spread.height >= 400 > short.height
    small = compose("約束", face.font(40), 40, 400, face=face, ruby_runs=[["約束", "やくそく"]], ruby_scale=0.3)
    large = compose("約束", face.font(40), 40, 400, face=face, ruby_runs=[["約束", "やくそく"]], ruby_scale=0.7)
    assert large.width > small.width
    assert mono_runs([["約束", "やくそく"]]) == [["約", "やく"], ["束", "そく"]]
    assert mono_runs([["明日", "あした"]]) == [["明日", "あした"]]  # (三字は二字に割れない)


def test_a_line_under_a_layer():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "c"},
                   {"op": "fill_area", "page": 1, "layer_id": "c", "area": {"poly": [[50, 50], [120, 50], [120, 120], [50, 120]]},
                    "rgb": [0, 0, 200]},
                   {"op": "add_line", "page": 1, "id": "a", "text": "あ", "x_mm": 70, "y_mm": 70, "w_mm": 30, "h_mm": 30,
                    "balloon": "box"}])

    def middle():
        image = render_page(ep.pages[0], 100, mode="proof", episode=ep).convert("RGB")
        return image.getpixel((round(75 / 25.4 * 100), round(95 / 25.4 * 100)))

    assert middle() == (255, 255, 255)  # (the balloon over the paint)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"below_layer": "c"}}])
    assert middle() == (0, 0, 200)  # (the paint over the balloon)
