"""J6: curved panels, border styles and panel numbers; lettering stretched (長体・平体), in a gradient, along a
path, with variation selectors and paired punctuation set tight; picture balloons, uneven spikes, cloud bumps
and tail shapes; speed lines along a curve and focus lines around any shape; pattern tones, layers turned into
halftone and the moiré check; parallel / multi / radial curve rulers, rulers kept with a layer, the ruler pen
and a fixed eye level."""

import base64
import io
import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402

DPI = 60


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _px(mm: float, dpi: int = DPI) -> int:
    return round(mm / 25.4 * dpi)


def _two_panels():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": ep.pages[0].frames[0].id, "axis": "horizontal", "ratio": 0.5}])
    return ep, [f.id for f in ep.pages[0].leaf_frames()]


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


# --- panels ------------------------------------------------------------------------------------------------------


def test_an_edge_bows_and_the_panel_follows(tmp_path):
    from genko import frames

    ep, (top, bottom) = _two_panels()
    frame = ep.pages[0]._find(top)
    r = frame.rect
    below = (r.x + r.width / 2, r.y + r.height + 5)  # just under the bottom edge
    assert not frames.contains(frame, *below)
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "bow": {"edge": 2, "mm": 10}}])
    frame = ep.pages[0]._find(top)
    assert frame.curves == [0.0, 0.0, 10.0, 0.0] and frames.contains(frame, *below)
    image = render_page(ep.pages[0], DPI, "print", ep)
    apex = image.getpixel((_px(r.x + r.width / 2), _px(r.y + r.height + 10)))
    assert max(apex) < 100  # the border runs through the bowed-out middle
    save_episode(ep, tmp_path / "b.genko")
    assert load_episode(tmp_path / "b.genko").pages[0]._find(top).curves == [0.0, 0.0, 10.0, 0.0]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "curves": [1, 2]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "bow": {"edge": 0, "mm": 500}}])
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "curves": None}])
    assert ep.pages[0]._find(top).curves is None


def test_border_styles_draw_differently():
    ep, (top, _bottom) = _two_panels()
    frame = ep.pages[0]._find(top)
    y = _px(frame.rect.y)
    xs = range(_px(frame.rect.x + 10), _px(frame.rect.x + frame.rect.width - 10))

    def inked(image):
        return sum(1 for x in xs if max(image.getpixel((x, y))) < 128)

    solid = inked(render_page(ep.pages[0], DPI, "print", ep))
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "line": {"kind": "dashed"}}])
    dashed = inked(render_page(ep.pages[0], DPI, "print", ep))
    assert 0.3 * solid < dashed < 0.85 * solid
    apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "line": {"kind": "double", "rgb": [200, 0, 0]}}])
    image = render_page(ep.pages[0], 150, "print", ep, finish=False)
    column = [image.getpixel((_px(frame.rect.x + 40, 150), yy)) for yy in range(_px(frame.rect.y, 150) - 4, _px(frame.rect.y, 150) + 12)]
    reds = [p for p in column if p[0] > 150 and p[1] < 80]
    assert reds and len([i for i in range(1, len(column)) if (column[i][0] > 150 and column[i][1] < 80)
                         != (column[i - 1][0] > 150 and column[i - 1][1] < 80)]) >= 4  # two red lines
    for kind in ("dotted", "rough", "solid"):
        apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "line": {"kind": kind}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_frame", "page": 1, "frame_id": top, "line": {"kind": "sparkly"}}])


# --- lettering ---------------------------------------------------------------------------------------------------


def _line(ep, line_id):
    return next(line for line in ep.story if line.id == line_id)


def test_letters_narrow_wide_and_in_a_gradient():
    from genko.balloons import text_image

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "ドドド", "balloon": "none", "x_mm": 40, "y_mm": 40, "w_mm": 60, "h_mm": 20,
                    "wrap": "horizontal", "id": "a", "style": {"size_mm": 10}}])
    plain, _ = text_image(_line(ep, "a"), 150)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"scale_x": 0.5}}])
    narrow, _ = text_image(_line(ep, "a"), 150)
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"scale_x": 1.5}}])
    wide, _ = text_image(_line(ep, "a"), 150)
    assert narrow.width < plain.width * 0.6 and wide.width > plain.width * 1.4 and narrow.height == plain.height
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"scale_x": None, "gradient": {"rgb_from": [255, 0, 0], "rgb_to": [0, 0, 255]}}}])
    coloured, _ = text_image(_line(ep, "a"), 150)
    pixels = [coloured.getpixel((x, y)) for y in range(coloured.height) for x in range(coloured.width)
              if coloured.getpixel((x, y))[3] > 250]
    assert any(p[0] > 200 and p[2] < 80 for p in pixels) and any(p[2] > 200 and p[0] < 80 for p in pixels)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"scale_x": 9}}])


def test_letters_follow_a_path():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "ぐるぐる", "balloon": "none", "x_mm": 60, "y_mm": 60, "w_mm": 80, "h_mm": 40,
                    "id": "p", "style": {"size_mm": 6, "text_path": [[0, 40], [80, 0]]}}])
    image = render_page(ep.pages[0], DPI, "print", ep)
    dark = [(x, y) for y in range(_px(55), _px(105)) for x in range(_px(55), _px(145)) if max(image.getpixel((x, y))) < 100]
    assert dark
    middle = sorted(x for x, _y in dark)[len(dark) // 2]
    left = [y for x, y in dark if x < middle]
    right = [y for x, y in dark if x > middle]
    assert left and right and sum(left) / len(left) > sum(right) / len(right) + 3  # rising to the right, as the path does
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "p", "style": {"text_path": [[0, 0]]}}])


def test_variation_selectors_and_tight_punctuation():
    from genko.balloons import text_image
    from genko.tategaki import cells

    assert cells("辻\U000e0100辻") == ["辻\U000e0100", "辻"]
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "辻\U000e0100のこと", "x_mm": 40, "y_mm": 40, "w_mm": 20, "h_mm": 40, "id": "v"},
                   {"op": "add_line", "page": 1, "text": "「あ」「い」", "balloon": "none", "wrap": "horizontal", "x_mm": 40,
                    "y_mm": 100, "w_mm": 90, "h_mm": 15, "id": "k", "style": {"size_mm": 8}}])
    render_page(ep.pages[0], DPI, "print", ep)  # (no crash on the selector)
    tight, _ = text_image(_line(ep, "k"), 150)
    apply_ops(ep, [{"op": "edit_line", "id": "k", "style": {"yakumono": False}}])
    loose, _ = text_image(_line(ep, "k"), 150)
    assert tight.width < loose.width - 20
    apply_ops(ep, [{"op": "edit_line", "id": "k", "style": {"features": ["jp78", "hwid"]}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "k", "style": {"features": ["zzzz"]}}])


# --- balloons ----------------------------------------------------------------------------------------------------


def _png(colour=(30, 160, 30, 255), size=(40, 30)) -> str:
    buf = io.BytesIO()
    Image.new("RGBA", size, colour).save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def test_a_picture_is_a_balloon(tmp_path, monkeypatch):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "やあ", "x_mm": 50, "y_mm": 50, "w_mm": 40, "h_mm": 30, "id": "b",
                    "balloon": "picture", "style": {"picture": _png()}}])
    image = render_page(ep.pages[0], DPI, "print", ep, finish=False)
    assert image.getpixel((_px(53), _px(53))) == (30, 160, 30)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "b", "style": {"picture": "not a picture"}}])
    from genko import materials

    picture = tmp_path / "p.png"
    Image.new("RGBA", (40, 30), (200, 40, 40, 255)).save(picture)
    item = materials.import_image(picture, "赤いフキダシ")
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "ね", "x_mm": 120, "y_mm": 50, "w_mm": 40, "h_mm": 30, "id": "c"},
                   {"op": "stamp_material", "page": 1, "material_id": item["id"], "line_id": "c"}])
    assert _line(ep, "c").balloon == "picture"
    assert render_page(ep.pages[0], DPI, "print", ep, finish=False).getpixel((_px(123), _px(53))) == (200, 40, 40)


def test_spikes_bumps_and_tail_shapes():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "わ", "balloon": "shout", "x_mm": 40, "y_mm": 40, "w_mm": 50, "h_mm": 40,
                    "id": "s", "style": {"spike_jitter": 1}},
                   {"op": "add_line", "page": 1, "text": "ふ", "balloon": "cloud", "x_mm": 120, "y_mm": 40, "w_mm": 50, "h_mm": 40,
                    "id": "c", "style": {"bumps": 6}, "tails": [{"to": [180, 110], "kind": "fade"}]},
                   {"op": "add_line", "page": 1, "text": "ぎ", "x_mm": 40, "y_mm": 150, "w_mm": 40, "h_mm": 30, "id": "z",
                    "tails": [{"to": [60, 220], "kind": "zigzag"}]}])
    even = render_page(ep.pages[0], DPI, "print", ep)
    apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"spike_jitter": None}}])
    assert render_page(ep.pages[0], DPI, "print", ep).tobytes() != even.tobytes()
    image = render_page(ep.pages[0], 150, "print", ep)
    tip = image.getpixel((_px(179, 150), _px(109, 150)))
    assert min(tip) > 200  # the fading tail is gone at its tip
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "move_line", "id": "z", "tails": [{"to": [60, 220], "kind": "spiral"}]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "c", "style": {"bumps": 2}}])


# --- effects and tones -------------------------------------------------------------------------------------------


def test_speed_lines_along_a_curve_and_focus_lines_around_a_shape():
    from genko.effects import geometry

    ep, (top, bottom) = _two_panels()
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "speed", "frame_id": top, "id": "s",
                    "params": {"path": [[40, 100], [120, 60], [200, 100]], "spread_mm": 20, "count": 20}},
                   {"op": "add_effect", "page": 1, "kind": "focus", "frame_id": bottom, "id": "f",
                    "params": {"center": [130, 280], "inner_path": [[110, 260], [150, 260], [150, 300], [110, 300]], "twist": 30}}])
    speed = geometry(ep.pages[0].effects[0], ep.pages[0])
    mids = [line["points"][len(line["points"]) // 2] for line in speed["lines"]]
    assert all(30 <= p[0] <= 210 and 40 <= p[1] <= 120 for p in mids)
    focus = geometry(ep.pages[0].effects[1], ep.pages[0])
    ends = [line["points"][-1] for line in focus["lines"]]
    assert all(max(abs(p[0] - 130), abs(p[1] - 280)) >= 19 for p in ends)  # they stop at the square, not inside it


def test_pattern_tones_and_a_picture_tile():
    from genko.tones import MOTIFS

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    for i, pattern in enumerate(m for m in MOTIFS if m != "image"):
        apply_ops(ep, [{"op": "add_tone", "page": 1, "area": {"rect": [30 + 25 * i, 60, 20, 20]}, "pattern": pattern, "density": 0.3,
                        "scale_mm": 2}])
    image = render_page(ep.pages[0], 150, "print", ep)
    for i in range(len(MOTIFS) - 1):
        crop = image.crop((_px(32 + 25 * i, 150), _px(62, 150), _px(48 + 25 * i, 150), _px(78, 150))).convert("L")
        dark = sum(1 for v in crop.getdata() if v < 128) / (crop.width * crop.height)
        assert 0.02 < dark < 0.9, MOTIFS[i]
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": {"rect": [40, 150, 40, 40]}, "pattern": "image", "id": "pic",
                    "tile_png": _png((0, 0, 0, 255), (8, 8)), "scale_mm": 2}])
    assert render_page(ep.pages[0], 150, "print", ep).getpixel((_px(60, 150), _px(170, 150)))[0] < 60
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_tone", "page": 1, "area": {"rect": [40, 150, 40, 40]}, "pattern": "image"}])


def test_a_grey_layer_prints_as_dots_and_overlaps_are_flagged():
    from genko import checks

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "g"},
                   {"op": "fill_area", "page": 1, "layer_id": "g", "rgb": [128, 128, 128], "area": {"rect": [50, 50, 60, 60]}},
                   {"op": "set_layer", "page": 1, "id": "g", "screen": {"lpi": 40}}])
    image = render_page(ep.pages[0], 300, "print", ep, finish=False)
    crop = image.crop((_px(60, 300), _px(60, 300), _px(90, 300), _px(90, 300))).convert("L")
    values = set(crop.getdata())
    assert min(values) < 40 and max(values) > 215 and not any(80 < v < 170 for v in values)  # black dots on white, no grey
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": {"rect": [30, 150, 80, 60]}, "density": 0.3, "lpi": 60, "name": "A"},
                   {"op": "add_tone", "page": 1, "area": {"rect": [70, 170, 80, 60]}, "density": 0.3, "lpi": 55, "angle": 30, "name": "B"}])
    codes = [issue["code"] for issue in checks.page_issues(ep, ep.pages[0])]
    assert "tone_moire" in codes
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "g", "screen": {"lpi": 500}}])


# --- rulers ------------------------------------------------------------------------------------------------------


def test_curve_rulers():
    from genko import rulers

    curve = [[0, 0], [50, 20], [100, 0]]
    parallel = {"id": "p", "kind": "parallel_curve", "points": curve}
    snapped = rulers.snap([[10, 40], [50, 60], [90, 40]], [parallel])
    mid = min(snapped, key=lambda p: abs(p[0] - 50))
    assert mid[1] > 40 + 3 and abs(snapped[0][1] - 40) < 8  # the curve's bow, moved down to where the line started
    radial = {"id": "r", "kind": "radial_curve", "points": curve, "center": [50, -100]}
    snapped = rulers.snap([[50, 70], [80, 60]], [radial])
    assert abs(snapped[0][1] - 70) < 2 and len(snapped) > 2
    multi = {"id": "m", "kind": "multi_curve", "points": [[0, 0], [100, 0]], "points2": [[0, 100], [50, 140], [100, 100]]}
    snapped = rulers.snap([[0, 50], [50, 55], [100, 50]], [multi])
    mid = min(snapped, key=lambda p: abs(p[0] - 50))
    assert 60 < mid[1] < 80  # half way between a straight line and a bowed one
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "multi_curve", "points": curve}])
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "radial_curve", "points": curve, "center": [50, -100], "id": "r"}])


def test_a_ruler_kept_with_a_layer_and_the_ruler_pen():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _ink(ep)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "other"},
                   {"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 100]], "id": "r", "layer_id": ink.id}])
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 103], [150, 108]], "snap_ruler": True, "stabilize": 0},
                   {"op": "add_stroke", "page": 1, "layer_id": "other", "points": [[40, 103], [150, 108]], "snap_ruler": True, "stabilize": 0}])
    assert all(abs(p[1] - 100) < 0.01 for p in _ink(ep).strokes[-1].points)
    other = next(layer for layer in ep.pages[0].layers if layer.id == "other")
    assert other.strokes[-1].points[-1][1] > 107  # (not this layer's ruler)
    apply_ops(ep, [{"op": "ruler_to_layer", "page": 1, "id": "r", "layer_id": "other", "width_mm": 0.3}])
    drawn = next(layer for layer in ep.pages[0].layers if layer.id == "other").strokes[-1]
    assert drawn.points[0] == (20.0, 100.0) and drawn.points[-1] == (200.0, 100.0)
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "parallel", "angle": 30, "id": "d"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "ruler_to_layer", "page": 1, "id": "d", "layer_id": "other"}])


def test_the_eye_level_can_be_kept_and_moved():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_ruler", "page": 1, "kind": "perspective", "points": [[20, 150], [220, 150]], "id": "p", "lock_horizon": True}])
    apply_ops(ep, [{"op": "edit_ruler", "page": 1, "id": "p", "points": [[40, 170], [220, 150]]}])
    ruler = ep.pages[0].rulers[0]
    assert ruler["points"][0] == [40.0, 150.0]  # slid along the eye level
    apply_ops(ep, [{"op": "edit_ruler", "page": 1, "id": "p", "horizon_y": 120}])
    assert [p[1] for p in ep.pages[0].rulers[0]["points"]] == [120.0, 120.0]
    apply_ops(ep, [{"op": "edit_ruler", "page": 1, "id": "p", "fixed": True}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_ruler", "page": 1, "id": "p", "points": [[0, 0], [10, 0]]}])


def test_agents_have_it_all():
    from genko.studio.service import AGENT_OPS

    assert "ruler_to_layer" in AGENT_OPS


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep, _ids = _two_panels()
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "やあ", "x_mm": 60, "y_mm": 60, "w_mm": 30, "h_mm": 30, "id": "l"}])
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_bowing_a_panel_on_the_canvas(window, qapp):
    canvas = window.canvas
    page = window.episode.pages[0]
    frame = page.leaf_frames()[0]
    window._tool("frame")
    window._current().selected_frame_id = frame.id
    canvas.page.selected_frame_id = frame.id
    handles = canvas._bow_handles()
    assert len(handles) == 4
    _i, (bx, by) = handles[2]
    canvas._frame_press(canvas._pt(bx, by))
    assert canvas._frame_drag["kind"] == "bow"
    canvas._frame_move(canvas._pt(bx, by + 8))
    canvas._frame_release()
    assert window.episode.pages[0]._find(frame.id).curves[2] == pytest.approx(8, abs=0.5)
    window.act_frame_numbers.setChecked(True)
    window._toggle_frame_numbers(True)
    canvas.repaint()
    window._current().selected_frame_id = frame.id
    window.border_kind_actions[1].trigger()
    assert window.episode.pages[0]._find(frame.id).line == {"kind": "double"}


def test_the_lettering_fields(window, qapp):
    window.story.refresh()
    window.story.select("l")
    panel = window.story
    panel.scale_x.setValue(0.7)
    panel._style_changed()
    line = next(ln for ln in window.episode.story if ln.id == "l")
    assert line.style["scale_x"] == 0.7
    panel.yakumono.setChecked(False)
    panel._style_changed()
    assert next(ln for ln in window.episode.story if ln.id == "l").style["yakumono"] is False


def test_the_ruler_menu_and_a_layer_ruler(window, qapp):
    kinds = [kind for _title, kind, _opts, _tip in window.ruler_kinds]
    assert {"parallel_curve", "multi_curve", "radial_curve"} <= set(kinds)
    window.apply_ops([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 100]], "id": "r"}])
    window.canvas.selected_ruler_id = "r"
    window.set_target_layer(_ink(window.episode).id)
    window._ruler_to_target_layer()
    assert window.episode.pages[0].rulers[0]["layer_id"] == _ink(window.episode).id
    window.apply_ops([{"op": "add_layer", "page": 1, "kind": "pen", "id": "x"}])
    window.set_target_layer("x")
    assert window.canvas._page_rulers() == []
    window.set_target_layer(_ink(window.episode).id)
    assert len(window.canvas._page_rulers()) == 1
    window._ruler_pen()
    assert _ink(window.episode).strokes[-1].points[0] == (20.0, 100.0)


def test_lettering_materials_warp_and_picture_fill():
    from genko import materials
    from genko.balloons import text_image

    items = [m for m in materials.all_materials() if m["kind"] == "lettering"]
    assert len(items) >= 10 and materials.thumbnail(items[0]).getbbox()
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": items[0]["id"], "x_mm": 100, "y_mm": 100, "id": "s"}])
    line = _line(ep, "s")
    assert line.text == items[0]["text"] and abs(line.x_mm + line.w_mm / 2 - 100) < 0.1
    apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"warp": [[0.3, 0], [0.7, 0], [1, 1], [0, 1]], "fill_png": _png((0, 150, 0, 255))}}])
    picture, _ = text_image(_line(ep, "s"), 150)
    top = [x for x in range(picture.width) if picture.getpixel((x, 3))[3] > 200]
    bottom = [x for x in range(picture.width) if picture.getpixel((x, picture.height - 4))[3] > 200]
    assert not top or not bottom or (max(top) - min(top)) < (max(bottom) - min(bottom))  # narrower at the top
    greens = [picture.getpixel((x, y)) for y in range(picture.height) for x in range(picture.width)
              if picture.getpixel((x, y))[3] > 250 and picture.getpixel((x, y))[1] > 120 and picture.getpixel((x, y))[0] < 60]
    assert greens
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_line", "id": "s", "style": {"warp": [[0, 0], [1, 0]]}}])
