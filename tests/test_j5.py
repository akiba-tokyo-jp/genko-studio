"""J5: fill, gradient and correction layers; the layer colour printed; border and watercolour edges; the other
blend modes; several layers merged, grouped, moved and set at once; the visible layers merged; pen ⇄ paint;
the paper's colour; liquify; skew handles and the transform's resampling; the added filters."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerKind, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import layer_image, render_page  # noqa: E402

DPI = 60


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _px(mm: float) -> int:
    return round(mm / 25.4 * DPI)


def _layer(ep, layer_id):
    return next(layer for layer in ep.pages[0].layers if layer.id == layer_id)


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


def _book():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "red"},
                   {"op": "fill_area", "page": 1, "layer_id": "red", "rgb": [200, 30, 30], "area": {"rect": [50, 50, 40, 40]}},
                   {"op": "add_layer", "page": 1, "kind": "pen", "id": "blue"},
                   {"op": "add_stroke", "page": 1, "layer_id": "blue", "stabilize": 0, "points": [[40, 150], [140, 170]],
                    "width_mm": 1.5, "rgb": [0, 0, 200]}])
    return ep


def _at(ep, x, y, mode="print"):
    return render_page(ep.pages[0], DPI, mode, ep).getpixel((_px(x), _px(y)))


# --- fill, gradient and correction layers --------------------------------------------------------------------


def test_a_fill_layer_is_a_colour_that_can_be_changed(tmp_path):
    ep = _book()
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "fill", "id": "f", "rgb": [240, 220, 100], "after": "red"}])
    assert _layer(ep, "f").kind == LayerKind.FILL and _layer(ep, "f").title == "ベタ塗り"
    assert _at(ep, 120, 120)[:3] == (240, 220, 100)
    assert _at(ep, 70, 70)[:3] == (240, 220, 100)  # (over the red: it is in front)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "f", "fill": {"rgb": [10, 200, 10]}, "blend": "multiply"}])
    assert _at(ep, 120, 120)[:3] == (10, 200, 10)
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko")
    assert _layer(again, "f").fill == {"rgb": [10, 200, 10]} and _layer(again, "f").blend == "multiply"


def test_a_gradient_layer_runs_from_one_colour_to_the_other():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    h = ep.pages[0].spec.height_mm
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "gradient", "id": "g",
                    "gradient": {"from": [0, 0], "to": [0, h], "rgb_from": [255, 0, 0], "rgb_to": [0, 0, 255]}},
                   {"op": "set_layer", "page": 1, "id": "g", "panel_clip": False}])
    top, bottom = _at(ep, 100, 2), _at(ep, 100, h - 2)
    assert top[0] > 240 and top[2] < 20 and bottom[2] > 240 and bottom[0] < 20
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "g", "fill": {"gradient": {"shape": "star"}}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "g", "adjust": {"kind": "invert"}}])  # (not a correction layer)


def test_a_correction_layer_changes_what_is_under_it_and_can_be_changed_again(tmp_path):
    ep = _book()
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "adjust", "id": "adj", "adjust": {"kind": "invert"}}])
    assert _layer(ep, "adj").kind == LayerKind.ADJUST
    assert _at(ep, 20, 20)[:3] == (0, 0, 0)  # the white paper, inverted
    red = _at(ep, 70, 70)
    assert red[0] < 80 and red[1] > 200  # (200, 30, 30) → (55, 225, 225)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "adj", "adjust": {"kind": "threshold", "threshold": 128}}])
    assert _at(ep, 70, 70)[:3] == (0, 0, 0) and _at(ep, 20, 20)[:3] == (255, 255, 255)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "adj", "opacity": 0}])
    assert _at(ep, 70, 70)[:3] == (200, 30, 30)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "adj", "opacity": 1,
                    "adjust": {"kind": "gradient_map", "colors": [[0, 0, 120], [255, 240, 200]]}}])
    assert _at(ep, 20, 20)[:3] == (255, 240, 200)
    save_episode(ep, tmp_path / "b.genko")
    assert _layer(load_episode(tmp_path / "b.genko"), "adj").adjust["kind"] == "gradient_map"
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "adjust", "adjust": {"kind": "twirl"}}])  # (shapes: not a correction)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "adjust", "adjust": {"kind": "posterize", "levels": "many"}}])


def test_the_layer_colour_prints_only_when_asked():
    ep = _book()
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": _ink(ep).id, "stabilize": 0, "points": [[40, 200], [140, 200]],
                    "width_mm": 2, "rgb": [20, 20, 20]},
                   {"op": "set_layer", "page": 1, "id": _ink(ep).id, "color": [40, 110, 230]}])
    ep.pages[0].spec = PageSpec.b4_comic()
    assert max(_at(ep, 90, 200)[:3]) < 60  # printed in its own ink
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": _ink(ep).id, "color_prints": True}])
    shown = render_page(ep.pages[0], DPI, "print", ep, finish=False).getpixel((_px(90), _px(200)))
    assert shown[2] > shown[0] + 60


def test_border_and_watercolour_edges():
    ep = _book()
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "effect": {"border": {"width_mm": 2, "rgb": [0, 200, 0]}}}])
    picture = layer_image(ep.pages[0], _layer(ep, "red"), DPI, ep)
    outside = picture.getpixel((_px(49), _px(70)))
    assert outside[3] > 200 and outside[1] > 150 and outside[0] < 60  # the border around the red
    assert picture.getpixel((_px(70), _px(70)))[:3] == (200, 30, 30)
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "effect": {"water_edge": {"width_mm": 1.5, "strength": 1}}}])
    picture = layer_image(ep.pages[0], _layer(ep, "red"), DPI, ep)
    assert sum(picture.getpixel((_px(50.5), _px(70)))[:3]) < sum(picture.getpixel((_px(70), _px(70)))[:3])  # darker at the rim
    assert picture.getpixel((_px(48), _px(70)))[3] == 0
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "effect": None}])
    assert _layer(ep, "red").effect is None
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "effect": {"glow": {}}}])


def test_the_other_blend_modes():
    from PIL import Image

    from genko.render import BLEND_MODES, _blend_over

    base = Image.new("RGBA", (4, 4), (100, 150, 200, 255))
    over = Image.new("RGBA", (4, 4), (200, 100, 50, 255))
    results = {mode: _blend_over(base, over, mode, 1.0).getpixel((0, 0))[:3] for mode in BLEND_MODES}
    assert results["darken"] == (100, 100, 50) and results["lighten"] == (200, 150, 200)
    assert results["difference"] == (100, 50, 150) and results["subtract"] == (0, 50, 150)
    assert len(set(results.values())) >= 12  # (they differ)
    ep = _book()
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "blend": "luminosity"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "red", "blend": "sparkle"}])


# --- several layers ----------------------------------------------------------------------------------------------


def test_chosen_layers_merge_into_the_lowest_as_they_show():
    ep = _book()
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": "blue", "opacity": 0.5}])
    apply_ops(ep, [{"op": "merge_layers", "page": 1, "ids": ["blue", "red"], "name": "まとめ"}])
    ids = [layer.id for layer in ep.pages[0].layers]
    assert "blue" not in ids and _layer(ep, "red").title == "まとめ" and _layer(ep, "red").kind == LayerKind.RASTER
    assert _at(ep, 70, 70)[:3] == (200, 30, 30)
    line = _at(ep, 90, 160)
    assert line[2] > 150 and line[0] > 60  # half see-through, as it showed
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "merge_layers", "page": 1, "ids": ["red"]}])


def test_the_visible_layers_merge_or_are_copied():
    ep = _book()
    before = [layer.id for layer in ep.pages[0].layers]
    apply_ops(ep, [{"op": "merge_visible", "page": 1, "id": "all"}])
    assert [layer.id for layer in ep.pages[0].layers] == before + ["all"]
    copied = layer_image(ep.pages[0], _layer(ep, "all"), DPI, ep)
    assert copied.getpixel((_px(70), _px(70)))[:3] == (200, 30, 30) and copied.getpixel((_px(90), _px(160)))[2] > 150
    apply_ops(ep, [{"op": "delete_layer", "page": 1, "id": "all"}, {"op": "set_layer", "page": 1, "id": "blue", "visible": False}])
    apply_ops(ep, [{"op": "merge_visible", "page": 1, "copy": False}])
    ids = [layer.id for layer in ep.pages[0].layers]
    assert "red" not in ids and "blue" in ids  # (the hidden one stays)
    assert _at(ep, 70, 70)[:3] == (200, 30, 30)


def test_layers_go_into_a_folder_and_move_together():
    ep = _book()
    apply_ops(ep, [{"op": "group_layers", "page": 1, "ids": ["red", "blue"], "id": "folder", "name": "人物"}])
    layers = ep.pages[0].layers
    assert _layer(ep, "folder").kind == LayerKind.FOLDER and _layer(ep, "folder").title == "人物"
    assert _layer(ep, "red").parent_id == _layer(ep, "blue").parent_id == "folder"
    assert [layer.id for layer in layers][-3:] == ["red", "blue", "folder"]
    apply_ops(ep, [{"op": "move_layers", "page": 1, "ids": ["red", "blue"], "parent": None, "after": "bottom"}])
    assert [layer.id for layer in ep.pages[0].layers][:2] == ["red", "blue"] and _layer(ep, "red").parent_id is None
    apply_ops(ep, [{"op": "move_layers", "page": 1, "ids": ["blue"], "parent": "folder"}])
    assert _layer(ep, "blue").parent_id == "folder"
    order = [layer.id for layer in ep.pages[0].layers]
    assert order.index("blue") == order.index("folder") - 1
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "move_layers", "page": 1, "ids": ["folder"], "parent": "folder"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "move_layers", "page": 1, "ids": ["red"], "parent": "blue"}])


def test_many_layers_are_set_at_once():
    ep = _book()
    apply_ops(ep, [{"op": "set_layers", "page": 1, "ids": ["red", "blue"], "visible": False, "opacity": 0.4}])
    assert not _layer(ep, "red").visible and _layer(ep, "blue").opacity == 0.4
    apply_ops(ep, [{"op": "set_layers", "page": 1, "all": True, "visible": True}])
    assert all(layer.visible for layer in ep.pages[0].layers)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layers", "page": 1, "ids": ["red"]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_layers", "page": 1, "ids": ["nothing"], "visible": True}])


def test_pen_lines_become_pixels_and_pixels_become_pen_lines():
    ep = _book()
    apply_ops(ep, [{"op": "convert_layer", "page": 1, "id": "blue", "to": "paint"}])
    blue = _layer(ep, "blue")
    assert blue.kind == LayerKind.RASTER and not blue.strokes and blue.raster_png
    apply_ops(ep, [{"op": "convert_layer", "page": 1, "id": "blue", "to": "pen"}])
    blue = _layer(ep, "blue")
    assert blue.kind == LayerKind.STROKES and len(blue.strokes) == 1 and blue.raster_png is None
    stroke = blue.strokes[0]
    ends = sorted([stroke.points[0], stroke.points[-1]])
    assert abs(ends[0][0] - 40) < 2 and abs(ends[1][0] - 140) < 2 and abs(ends[1][1] - 170) < 2
    assert 1.0 < stroke.width_mm < 2.2 and stroke.rgb[2] > 150
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "empty"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "convert_layer", "page": 1, "id": "empty", "to": "pen"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "convert_layer", "page": 1, "id": "blue", "to": "sticker"}])


def test_the_paper_has_a_colour():
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "set_paper", "rgb": [250, 240, 220]}])
    assert all(render_page(page, DPI, "print", ep).getpixel((3, 3)) == (250, 240, 220) for page in ep.pages)
    apply_ops(ep, [{"op": "set_paper", "page": 2, "rgb": None}])
    assert render_page(ep.pages[1], DPI, "print", ep).getpixel((3, 3)) == (255, 255, 255)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_paper", "rgb": [300, 0, 0]}])


# --- transforms --------------------------------------------------------------------------------------------------


def test_liquify_pushes_pixels_and_lines():
    ep = _book()
    def alpha_at(x, y):
        return layer_image(ep.pages[0], _layer(ep, "red"), DPI, ep).getpixel((_px(x), _px(y)))[3]

    assert alpha_at(58, 70) == 255
    apply_ops(ep, [{"op": "liquify", "page": 1, "layer_id": "red", "points": [[44, 70], [66, 70]], "width_mm": 16,
                    "strength": 1, "mode": "push"}])
    assert alpha_at(58, 70) == 0 and alpha_at(58, 52) == 255  # the empty paper was pushed in along the drag only
    ink = _ink(ep)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "stabilize": 0, "points": [[40, 230], [140, 230]]},
                   {"op": "liquify", "page": 1, "layer_id": ink.id, "points": [[90, 228]], "width_mm": 20, "mode": "bloat"}])
    line = _ink(ep).strokes[-1]
    assert len(line.points) > 2 and max(p[1] for p in line.points) > 230.3
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "liquify", "page": 1, "layer_id": "red", "points": [[70, 70]], "mode": "melt"}])


def test_the_transform_can_keep_hard_pixels():
    import base64
    import io

    import numpy as np
    from PIL import Image

    ep = _book()
    apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": "red", "area": {"rect": [40, 40, 60, 60]},
                    "matrix": [1.37, 0, 0.21, 1.13, -3.1, 2.7], "interp": "nearest"}])
    patch = _layer(ep, "red").patches[-1]
    image = Image.open(io.BytesIO(patch["png"] if isinstance(patch["png"], bytes) else base64.b64decode(patch["png"])))
    alphas = set(np.unique(np.asarray(image.convert("RGBA").split()[3])).tolist())
    assert alphas <= {0, 255}  # no soft edge: nothing was blended
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "transform_area", "page": 1, "layer_id": "red", "area": {"rect": [40, 40, 60, 60]},
                        "matrix": [1, 0, 0, 1, 1, 1], "interp": "soft"}])


def test_every_filter_keeps_the_size():
    from PIL import Image

    from genko.filters import KINDS, apply_filter

    picture = Image.new("RGBA", (40, 30), (0, 0, 0, 0))
    picture.paste((200, 60, 20, 255), (10, 8, 30, 22))
    for kind in KINDS:
        out = apply_filter(picture, kind, {})
        assert out.size == picture.size, kind
    moved = apply_filter(picture, "motion_blur", {"distance": 10})
    assert moved.getpixel((33, 15))[3] > 0  # (smeared past the edge)
    assert apply_filter(picture, "invert", {}).getpixel((20, 15))[:3] == (55, 195, 235)
    assert apply_filter(picture, "posterize", {"levels": 2}).getpixel((20, 15))[:3] == (255, 0, 0)


# --- agents --------------------------------------------------------------------------------------------------------


def test_agents_have_the_same_layer_work(tmp_path):
    from genko.studio.service import AGENT_OPS

    assert {"merge_layers", "merge_visible", "group_layers", "move_layers", "convert_layer", "set_layers", "set_paper",
            "liquify"} <= AGENT_OPS
    ep = _book()
    ep.strict_gates = True
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "merge_layers", "page": 1, "ids": ["red", "blue"]}], agent="ai:hermes")
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "liquify", "page": 1, "layer_id": "red", "points": [[70, 70]]}], agent="ai:hermes")


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(_book(), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def _choose(panel, ids):
    panel.list.clearSelection()
    for layer_id in ids:
        panel.list.item(panel.ids.index(layer_id)).setSelected(True)


def test_the_layer_panel_works_on_several_layers(window, qapp):
    panel = window.layers
    panel.refresh()
    _choose(panel, ["red", "blue"])
    assert set(panel.selected_ids()) == {"red", "blue"}
    panel._group_selected()
    panel.refresh()
    folder = next(layer for layer in window.episode.pages[0].layers if layer.kind == LayerKind.FOLDER)
    assert _layer(window.episode, "red").parent_id == folder.id
    _choose(panel, ["red", "blue"])
    panel._merge_selected()
    assert "blue" not in [layer.id for layer in window.episode.pages[0].layers]
    panel.search.setText("まとめ")
    assert all(panel.list.item(row).isHidden() for row in range(panel.list.count()))
    panel.search.setText("")
    assert not any(panel.list.item(row).isHidden() for row in range(panel.list.count()))
    panel._merge_visible(True)
    assert window.episode.pages[0].layers[-1].title == "表示レイヤーのコピー"


def test_fill_and_correction_layers_from_the_panel(window, qapp):
    panel = window.layers
    panel.refresh()
    panel._add_special("fill", "ベタ塗り", {"rgb": [1, 2, 3]})
    target = window.target_layer()
    assert target.kind == LayerKind.FILL and target.fill == {"rgb": [1, 2, 3]}
    params = panel._adjust_fields("gradient_map")
    assert len(params["colors"]) == 2
    panel._add_special("adjust", "反転", {"adjust": {"kind": "invert"}})
    assert window.target_layer().kind == LayerKind.ADJUST
    panel._set("color_prints", True)
    assert window.target_layer().color_prints
    labels = [panel.filter.itemData(i) for i in range(panel.filter.count())]
    assert {"motion_blur", "lineart", "gradient_map", "posterize"} <= set(labels)
    blends = [panel.blend.itemData(i) for i in range(panel.blend.count())]
    assert len(blends) == 20 and "color_dodge" in blends


def test_skew_handles_and_the_resampling_choice(window, qapp):
    canvas = window.canvas
    window._tool("rect")
    canvas.set_selection({"poly": [[50, 50], [90, 50], [90, 90], [50, 90]]}, [[50, 50], [90, 50], [90, 90], [50, 90]])
    skews = [h for h in canvas._sel_handles() if h[0] == "skew"]
    assert {key for _kind, key, _pos in skews} == {"n", "s", "e", "w"}
    canvas._sel_drag = {"kind": "skew", "key": "n", "start": (60, 50), "box": (50, 50, 90, 90)}
    a, b, c, d, e, f = canvas._sel_matrix((70, 50))
    assert (a, b, d) == (1, 0, 1) and abs(c * 50 + e - 10) < 1e-6 and abs(c * 90 + e) < 1e-6  # top slides, bottom stays
    window.transform_interp = "nearest"
    window.set_target_layer("red")
    window._transform_selection([1, 0, 0, 1, 5, 0])
    assert window.episode.pages[0].layers  # (the transform went through with the choice)


def test_the_liquify_tool(window, qapp):
    window.set_target_layer(_ink(window.episode).id)
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": _ink(window.episode).id, "stabilize": 0,
                       "points": [[40, 230], [140, 230]]}])
    window._tool("liquify")
    assert window.tool_actions["liquify"].isChecked()
    window.liquify_mode.setCurrentIndex(window.liquify_mode.findData("push"))
    window.canvas.blend_mm = 20
    window._on_stroke([[80, 225, 0.5], [80, 240, 0.5]])
    line = _ink(window.episode).strokes[-1]
    assert max(p[1] for p in line.points) > 231
