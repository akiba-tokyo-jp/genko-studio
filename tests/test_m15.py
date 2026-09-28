"""M15: tones (dots, lines, cross, sand, gradients, scraping, layering), effect lines (focus, speed,
flashes) and materials (built-in and the person's own: pictures and drawn parts, folders)."""

import os
import sys
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import effects, materials, tones  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerKind, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page  # noqa: E402

DPI = 150


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "name_ok", "page": 1}])
    return ep


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


def _layer(ep, layer_id):
    return next(layer for layer in ep.pages[0].layers if layer.id == layer_id)


def _black_share(ep, box, mode="print", dpi=DPI):
    x, y, w, h = box
    image = render_page(ep.pages[0], dpi, mode=mode, episode=ep).convert("L")
    crop = np.asarray(image.crop((mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi))))
    return float((crop < 128).mean()), float(crop.mean())


SQUARE = {"poly": [[60, 80], [120, 80], [120, 140], [60, 140]]}


# --- tones -----------------------------------------------------------------------------------------------


@pytest.mark.parametrize("pattern", ["dot", "line", "cross", "noise"])
def test_tone_patterns_print_the_asked_black_share(pattern):
    ep = _book()
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": SQUARE, "pattern": pattern, "density": 0.3, "lpi": 50, "id": "t"}])
    share, _ = _black_share(ep, (65, 85, 50, 50))
    assert share == pytest.approx(0.3, abs=0.05), pattern
    outside, _ = _black_share(ep, (130, 85, 20, 20))
    assert outside < 0.01
    # proofs show the grey it reads as
    _, grey = _black_share(ep, (65, 85, 50, 50), mode="proof")
    assert 160 < grey < 200


def test_gradient_tone_gets_lighter_along_its_direction(tmp_path: Path):
    ep = _book()
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": SQUARE, "density": 0.5, "id": "g",
                    "gradient": {"shape": "linear", "angle": 90, "start": 0.6, "end": 0.0}}])
    top, _ = _black_share(ep, (62, 82, 56, 10))
    bottom, _ = _black_share(ep, (62, 128, 56, 10))
    assert top > 0.4 and bottom < 0.1
    apply_ops(ep, [{"op": "set_tone", "page": 1, "id": "g", "gradient": {"shape": "radial", "start": 0.0, "end": 0.6}}])
    middle, _ = _black_share(ep, (85, 105, 10, 10))
    corner, _ = _black_share(ep, (61, 81, 6, 6))
    assert middle < 0.15 and corner > 0.3
    project = tmp_path / "t.genko"
    save_episode(ep, project)
    again = _layer(load_episode(project), "g")
    assert again.kind == LayerKind.TONE and again.tone["gradient"]["shape"] == "radial" and again.patches
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_tone", "page": 1, "id": "g", "pattern": "stripes"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_tone", "page": 1, "id": "g", "density": 1.5}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_tone", "page": 1, "id": _ink(ep).id, "density": 0.5}])


def test_scrape_paint_and_fill_a_tone_layer():
    ep = _book()
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": SQUARE, "density": 0.4, "id": "t"}])
    # the eraser scrapes a band out of the tone
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": "t", "points": [[60, 110], [120, 110]], "width_mm": 8}])
    scraped, _ = _black_share(ep, (65, 108, 50, 4))
    kept, _ = _black_share(ep, (65, 85, 50, 15))
    assert scraped < 0.02 and kept > 0.3
    assert _layer(ep, "t").strokes[-1].kind == "scrape"
    # a soft scrape fades it
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": "t", "points": [[60, 90], [120, 90]], "width_mm": 8, "soft": True}])
    assert _layer(ep, "t").strokes[-1].kind == "scrape_soft"
    # the pen paints more tone outside the square
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": "t", "points": [[140, 110], [180, 110]], "width_mm": 6, "kind": "mili",
                    "stabilize": 0, "taper": False}])
    painted, _ = _black_share(ep, (145, 109, 30, 2))
    assert painted > 0.2
    # tone by the region a fill would take
    ep2 = _book()
    ink = _ink(ep2).id
    apply_ops(ep2, [{"op": "add_stroke", "page": 1, "layer_id": ink, "points": pts, "width_mm": 0.6, "kind": "mili", "stabilize": 0,
                     "taper": False} for pts in ([[60, 80], [120, 80]], [[120, 80], [120, 140]], [[120, 140], [60, 140]], [[60, 140], [60, 80]])])
    apply_ops(ep2, [{"op": "add_tone", "page": 1, "at": {"x_mm": 90, "y_mm": 110}, "density": 0.3, "id": "f"}])
    inside, _ = _black_share(ep2, (70, 90, 40, 40))
    outside, _ = _black_share(ep2, (130, 90, 20, 20))
    assert inside == pytest.approx(0.3, abs=0.05) and outside < 0.01


def test_a_layer_above_covers_the_tone_and_old_tones_still_work():
    ep = _book()
    apply_ops(ep, [{"op": "add_tone", "page": 1, "area": SQUARE, "density": 0.5, "id": "t"},
                   {"op": "add_layer", "page": 1, "kind": "pen", "id": "top", "name": "上"},
                   {"op": "fill_area", "page": 1, "layer_id": "top", "area": {"poly": [[60, 80], [90, 80], [90, 140], [60, 140]]},
                    "rgb": [255, 255, 255]}])
    covered, _ = _black_share(ep, (62, 85, 25, 50))
    shown, _ = _black_share(ep, (95, 85, 20, 50))
    assert covered < 0.01 and shown > 0.4
    old = _book()
    apply_ops(old, [{"op": "add_tone", "page": 1, "density": 0.3}])  # no region: every panel
    share, _ = _black_share(old, (40, 60, 100, 100))
    assert share == pytest.approx(0.3, abs=0.05)


# --- effect lines ---------------------------------------------------------------------------------------


def _panel(ep):
    return ep.pages[0].leaf_frames()[0]


@pytest.mark.parametrize("kind", ["focus", "speed", "uni_flash", "beta_flash"])
def test_effects_draw_inside_their_panel_and_repeat_exactly(kind):
    ep = _book()
    root = _panel(ep)
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    top, bottom = sorted(ep.pages[0].leaf_frames(), key=lambda f: f.rect.y)
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": kind, "frame_id": top.id, "id": "e", "params": {"count": 120}}])
    r = top.rect
    ink, _ = _black_share(ep, (r.x, r.y, r.width, r.height))
    below, _ = _black_share(ep, (bottom.rect.x + 5, bottom.rect.y + 5, bottom.rect.width - 10, bottom.rect.height - 10))
    assert ink > 0.03 and below < 0.005
    first = effects.geometry(ep.pages[0].effects[0], ep.pages[0])
    assert effects.geometry(ep.pages[0].effects[0], ep.pages[0]) == first


def test_focus_lines_leave_the_middle_clear_and_follow_the_centre():
    ep = _book()
    frame = _panel(ep)
    r = frame.rect
    centre = [r.x + r.width * 0.3, r.y + r.height * 0.4]
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": frame.id, "id": "f",
                    "params": {"center": centre, "inner": [25, 25], "count": 150, "jitter": 0.1}}])
    middle, _ = _black_share(ep, (centre[0] - 10, centre[1] - 10, 20, 20))
    ring, _ = _black_share(ep, (centre[0] + 60, centre[1] - 10, 20, 20))
    assert middle < 0.01 and ring > 0.05
    apply_ops(ep, [{"op": "edit_effect", "page": 1, "id": "f", "params": {"center": [r.x + r.width * 0.7, centre[1]]}}])
    now, _ = _black_share(ep, (centre[0] - 10, centre[1] - 10, 20, 20))
    assert now > 0.03
    apply_ops(ep, [{"op": "edit_effect", "page": 1, "id": "f", "visible": False}])
    hidden, _ = _black_share(ep, (r.x + 3, r.y + 3, r.width - 6, r.height - 6))
    assert hidden < 0.01
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "sparkle"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "edit_effect", "page": 1, "id": "f", "params": {"jitter": 3}}])


def test_effect_becomes_pen_lines_to_finish_by_hand(tmp_path: Path):
    ep = _book()
    frame = _panel(ep)
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "speed", "frame_id": frame.id, "id": "s", "params": {"count": 30}},
                   {"op": "add_effect", "page": 1, "kind": "beta_flash", "frame_id": frame.id, "id": "b"}])
    before, _ = _black_share(ep, (frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height))
    apply_ops(ep, [{"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": _ink(ep).id},
                   {"op": "effect_to_layer", "page": 1, "id": "b", "layer_id": _ink(ep).id}])
    assert not ep.pages[0].effects
    ink = _ink(ep)
    assert len([s for s in ink.strokes if s.kind == "fx"]) == 30 and len(ink.patches) == 2
    after, _ = _black_share(ep, (frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height))
    assert after == pytest.approx(before, abs=0.08)
    # the lines can be erased like any pen line
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[frame.rect.x, 150], [frame.rect.x + frame.rect.width, 150]],
                    "width_mm": 4}])
    project = tmp_path / "e.genko"
    save_episode(ep, project)
    assert len(_ink(load_episode(project)).patches) == 2


# --- materials ---------------------------------------------------------------------------------------------


def test_builtin_materials_stamp_tones_and_effects():
    catalog = materials.all_materials()
    assert {"トーン", "グラデーション", "効果線"} <= set(materials.folders())
    ep = _book()
    frame = _panel(ep)
    for item in catalog:
        if item["kind"] == "tone":
            apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": item["id"], "frame_id": frame.id}])
        else:
            apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": item["id"], "frame_id": frame.id, "x_mm": 120, "y_mm": 150}])
    tone_layers = [layer for layer in ep.pages[0].layers if layer.kind == LayerKind.TONE]
    assert len(tone_layers) == sum(1 for i in catalog if i["kind"] == "tone")
    assert all(layer.title for layer in tone_layers)
    assert len(ep.pages[0].effects) == sum(1 for i in catalog if i["kind"] == "effect")
    assert any(e["params"].get("center") == [120.0, 150.0] for e in ep.pages[0].effects)
    for item in catalog:
        assert materials.thumbnail(item, 48).size == (48, 48)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "nope"}])


def test_own_materials_pictures_and_drawn_parts(tmp_path: Path):
    from PIL import Image

    from genko import selection

    picture = tmp_path / "leaf.png"
    Image.new("RGBA", (200, 100), (0, 0, 0, 255)).save(picture)
    item = materials.import_image(picture, "葉っぱ", "背景", width_mm=40)
    assert item["kind"] == "image" and item["aspect"] == pytest.approx(0.5) and item["folder"] == "背景"
    assert "背景" in materials.folders()
    ep = _book()
    ink = _ink(ep).id
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": item["id"], "layer_id": ink, "x_mm": 100, "y_mm": 120}])
    share, _ = _black_share(ep, (82, 111, 36, 18))
    assert share > 0.9
    # a drawn part: register lines, put them somewhere else
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[20, 200], [40, 220]], "kind": "mili", "stabilize": 0,
                    "taper": False}])
    import copy

    lifted = selection.lift(copy.deepcopy(_ink(ep)), {"poly": [[15, 195], [45, 195], [45, 225], [15, 225]]}, ep.pages[0])
    part = materials.add_material("斜線", "lines", "パーツ", items=selection.items_to_json(lifted))
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": part["id"], "layer_id": ink, "x_mm": 150, "y_mm": 60}])
    placed = _ink(ep).strokes[-1]
    assert placed.points[0] == pytest.approx((140, 50)) and placed.points[-1] == pytest.approx((160, 70))
    # the library keeps them across sessions; folders, rename, delete
    assert {m["id"] for m in materials.user_materials()} == {item["id"], part["id"]}
    materials.update_material(part["id"], name="斜線 2", folder="マイ素材")
    assert materials.get_material(part["id"])["name"] == "斜線 2"
    materials.add_folder("空のフォルダ")
    assert "空のフォルダ" in materials.folders()
    materials.delete_material(item["id"])
    assert not (materials.library_dir() / f"{item['id']}.png").exists()
    with pytest.raises(ValueError):
        materials.delete_material("focus")
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": part["id"], "layer_id": "nope"}])


def test_agents_have_the_tone_and_effect_tools():
    from genko.ops import OPS_SCHEMA
    from genko.studio.service import AGENT_OPS

    tools = {"set_tone", "edit_effect", "delete_effect", "effect_to_layer", "add_tone", "add_effect", "stamp_material"}
    assert tools <= AGENT_OPS and tools <= {entry["op"] for entry in OPS_SCHEMA}
    assert tones.swatch({"pattern": "line", "density": 0.3}).size == (96, 96)


# --- the window ---------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QInputDialog, QMessageBox

    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    monkeypatch.setattr(QInputDialog, "getText", lambda *a, **k: ("マイパーツ", True))
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(_book(), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    win.set_target_layer(_ink(win.episode).id)
    yield win
    win.close()


def _mouse(canvas, kind, at):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QMouseEvent

    left, none = Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton
    p = canvas._pt(*at)
    t, button, buttons = {"press": (QEvent.Type.MouseButtonPress, left, left), "move": (QEvent.Type.MouseMove, left, left),
                          "release": (QEvent.Type.MouseButtonRelease, left, none)}[kind]
    event = QMouseEvent(t, p, canvas.mapToGlobal(p), button, buttons, Qt.KeyboardModifier.NoModifier)
    {"press": canvas.mousePressEvent, "move": canvas.mouseMoveEvent, "release": canvas.mouseReleaseEvent}[kind](event)


def _drag(canvas, path):
    _mouse(canvas, "press", path[0])
    for pt in path[1:]:
        _mouse(canvas, "move", pt)
    _mouse(canvas, "release", path[-1])


def _click(canvas, at):
    _drag(canvas, [at])


def _tones(win):
    return [layer for layer in win.current_page().layers if layer.kind == LayerKind.TONE]


def test_tone_on_a_selection_then_scrape_and_change_it(window):
    window.act_marquee.trigger()
    _drag(window.canvas, [(60, 80), (90, 110), (120, 140)])
    window.show_dock("素材")
    window.materials.select_material("dot-60-40")
    window.act_tone_here.trigger()
    tone = _tones(window)[-1]
    assert tone.title == "網点 60 線 40%" and window.target_layer().id == tone.id
    assert window.materials.tone_box.isEnabled() and window.materials.density.value() == 40
    # the eraser scrapes (and softly when asked)
    window.canvas.set_selection(None)
    window.materials.soft.setChecked(True)
    window.act_eraser.trigger()
    _drag(window.canvas, [(60, 110), (90, 110), (120, 110)])
    assert _tones(window)[-1].strokes[-1].kind == "scrape_soft"
    # settings from the panel
    window.materials.density.setValue(20)
    window.materials.density.editingFinished.emit()
    window.materials.gradient.setCurrentIndex(window.materials.gradient.findData("linear"))
    window.materials._gradient()
    tone = _tones(window)[-1]
    assert tone.density == pytest.approx(0.2) and tone.tone["gradient"]["shape"] == "linear"
    window.materials.pattern.setCurrentIndex(window.materials.pattern.findData("noise"))
    window.materials.pattern.activated.emit(window.materials.pattern.currentIndex())
    assert _tones(window)[-1].tone["pattern"] == "noise"


def test_tone_by_clicking_a_closed_area(window):
    ink = _ink(window.episode).id
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": pts, "width_mm": 0.6, "kind": "mili", "stabilize": 0,
                       "taper": False} for pts in ([[60, 80], [120, 80]], [[120, 80], [120, 140]], [[120, 140], [60, 140]], [[60, 140], [60, 80]])])
    window.act_tone_click.trigger()
    assert window.canvas.tool == "stamp"
    _click(window.canvas, (90, 110))
    inside, _ = _black_share(window.episode, (70, 90, 40, 40))
    outside, _ = _black_share(window.episode, (130, 90, 20, 20))
    assert inside == pytest.approx(0.3, abs=0.05) and outside < 0.01


def test_effect_tool_puts_and_moves_focus_lines(window):
    window.effect_actions[0].trigger()  # 集中線
    assert window.canvas.tool == "effect"
    _click(window.canvas, (100, 150))
    effect = window.current_page().effects[-1]
    assert effect["kind"] == "focus" and effect["params"]["center"] == [100, 150]
    assert window.materials.effects.count() == 1 and window.materials.effect_fields
    _drag(window.canvas, [(100, 150), (120, 160), (140, 170)])
    assert window.current_page().effects[-1]["params"]["center"] == [140, 170]
    # the panel's settings change it
    window.materials.effect_fields["count"].setValue(200)
    window.materials.effect_fields["count"].editingFinished.emit()
    assert window.current_page().effects[-1]["params"]["count"] == 200
    # into pen lines on the ink layer
    window.materials.effect_to_layer()
    assert not window.current_page().effects and any(s.kind == "fx" for s in _ink(window.episode).strokes)
    window.effect_actions[3].trigger()  # ベタフラッシュ
    _click(window.canvas, (100, 150))
    window.materials.delete_effect()
    assert not window.current_page().effects


def test_materials_panel_registers_and_places_parts(window, tmp_path: Path, monkeypatch):
    from PIL import Image

    ink = _ink(window.episode).id
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[20, 200], [40, 220]], "kind": "mili", "stabilize": 0,
                       "taper": False}])
    window.act_marquee.trigger()
    _drag(window.canvas, [(15, 195), (30, 210), (45, 225)])
    window.show_dock("素材")
    window.materials.register_selection()
    item = window.materials.current_material()
    assert item["name"] == "マイパーツ" and item["kind"] == "lines"
    window.canvas.set_selection(None)
    window.materials.use()
    assert window.canvas.tool == "stamp"
    _click(window.canvas, (150, 60))
    assert _ink(window.episode).strokes[-1].points[0] == pytest.approx((140, 50))
    # a picture from a file
    picture = tmp_path / "p.png"
    Image.new("RGB", (100, 100), "black").save(picture)
    from PySide6.QtWidgets import QFileDialog

    monkeypatch.setattr(QFileDialog, "getOpenFileName", staticmethod(lambda *a, **k: (str(picture), "")))
    window.materials.import_image()
    assert window.materials.current_material()["kind"] == "image"
    window.materials.use()
    _click(window.canvas, (100, 250))
    assert _ink(window.episode).patches[-1]["mode"] == "image"
    window.materials.delete()
    assert all(m["kind"] != "image" for m in materials.user_materials())
    # built-in effects go into the selected panel
    frame = window.current_page().leaf_frames()[0]
    window._on_frame_selected(frame.id)
    window.materials.select_material("speed-v")
    window.materials.use()
    assert window.current_page().effects[-1]["kind"] == "speed"
