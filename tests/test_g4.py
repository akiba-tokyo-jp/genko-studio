"""G4: lettering materials — ruby in horizontal lines, parts of a line larger / smaller / bolder /
coloured, bold and italic lines, outline colours, and balloons with a hand-drawn wobble, a double line
and chosen spikes."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from PIL import ImageChops  # noqa: E402

from genko import balloons, fonts, tategaki  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _line(ep, **fields):
    op = {"op": "add_line", "page": 1, "id": "a", "x_mm": 30, "y_mm": 30, "w_mm": 70, "h_mm": 20, "balloon": "none"} | fields
    apply_ops(ep, [op])
    return ep.story[-1]


def _ink_rows(image, threshold=100):
    alpha = image.split()[3]
    return [y for y in range(image.height) if any(alpha.getpixel((x, y)) > threshold for x in range(image.width))]


def test_horizontal_ruby_sits_above_its_words():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    line = _line(ep, text="約束の日", wrap="horizontal", style={"size_mm": 6})
    plain, _ = balloons.text_image(line, 300)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "ruby_runs": [["約束", "やくそく"]]}])
    ruby, _ = balloons.text_image(ep.story[0], 300)
    assert ruby.height > plain.height
    band = round(balloons.px(6, 300) / 2)
    top = ruby.crop((0, 0, ruby.width, band))
    box = top.getbbox()
    assert box is not None and box[2] < ruby.width * 0.6  # over 約束 (the first half), not over の日


def test_part_of_a_line_larger_bolder_and_coloured():
    from genko.app.lettering import parse_marks

    text, _runs, _marks, styles = parse_marks("それは{大|本当}か{赤、太|！}")
    assert text == "それは本当か！" and styles == [["本当", {"scale": 1.4}], ["！", {"rgb": [210, 30, 30], "bold": True}]]
    em = 40
    face = fonts.face(None, fonts.DEFAULT_DIALOGUE)
    plain = tategaki.compose(text, face.font(em), em, 4000, face=face)
    styled = tategaki.compose(text, face.font(em), em, 4000, face=face, style_runs=styles)
    assert styled.width > plain.width and styled.height > plain.height  # the larger characters widen the column
    red = [p for p in styled.get_flattened_data() if p[3] > 200 and p[0] > 150 and p[1] < 80]
    assert red
    # bold is heavier
    bold = tategaki.compose(text, face.font(em), em, 4000, face=face, bold=True)
    assert sum(bold.split()[3].histogram()[200:]) > sum(plain.split()[3].histogram()[200:]) * 1.1
    # horizontal too, and saved with the book
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    line = _line(ep, text=text, wrap="horizontal", style={"size_mm": 6}, style_runs=styles)
    flat = new_episode("t", 1, 1, PageSpec.b4_comic())
    flat_line = _line(flat, text=text, wrap="horizontal", style={"size_mm": 6})
    assert balloons.text_image(line, 300)[0].height > balloons.text_image(flat_line, 300)[0].height
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "a", "style_runs": [["本当", {"scale": 9}]]}])


def test_italic_and_outline_colour():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    line = _line(ep, text="ドン", wrap="horizontal", balloon="sfx", w_mm=60, h_mm=30)
    flat, _ = balloons.text_image(line, 150)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"italic": True, "outline_rgb": [20, 20, 20], "outline_mm": 1}}])
    leaning, _ = balloons.text_image(ep.story[0], 150)
    assert leaning.width > flat.width
    # the halo is dark now: no white pixels around the letters
    assert not [p for p in leaning.get_flattened_data() if p[3] > 200 and min(p[:3]) > 230]


def _shape(style, kind="speech"):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "b", "text": "あ", "x_mm": 80, "y_mm": 120, "w_mm": 80, "h_mm": 60, "balloon": kind,
                    "wrap": "vertical", "style": style}])
    return render_page(ep.pages[0], 80, episode=ep).convert("L").crop((240, 360, 520, 560))


def test_balloon_wobble_double_line_and_spikes(tmp_path: Path):
    plain = _shape({})
    wobbly = _shape({"wobble": 0.8})
    assert ImageChops.difference(plain, wobbly).getbbox() is not None
    double = _shape({"double": True})
    dark = lambda im: sum(1 for p in im.get_flattened_data() if p < 100)  # noqa: E731
    assert dark(double) > dark(plain) * 1.5
    few = _shape({"spikes": 8, "spike_depth": 0.5}, "shout")
    many = _shape({"spikes": 40, "spike_depth": 0.1}, "shout")
    assert ImageChops.difference(few, many).getbbox() is not None
    with pytest.raises(ApplyError):
        _shape({"spikes": 3}, "shout")
    # saved with the book
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    _line(ep, text="やあ", style_runs=[["やあ", {"scale": 1.4}]], style={"wobble": 0.5, "double": True})
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko").story[0]
    assert again.style_runs == [["やあ", {"scale": 1.4}]] and again.style["double"] is True
