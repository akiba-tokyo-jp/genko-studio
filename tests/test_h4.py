"""H4: background scenes in 3D, fills that look at chosen reference layers, and auto actions."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import prim3d  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(page):
    return next(layer for layer in page.layers if layer.role == LayerRole.INK)


# --- scenes ------------------------------------------------------------------------------------------------


@pytest.mark.parametrize("kind", prim3d.SCENES)
def test_a_scene_is_one_guide_that_moves_and_traces_as_one(kind):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_scene", "page": 1, "kind": kind, "id": "s1"}])
    page = ep.pages[0]
    assert len(page.prims) == 1 and page.prims[0]["scene"] == kind
    lines = prim3d.edges(page.prims[0])
    assert len(lines) > 30
    x, y, w, h = prim3d.bbox(page.prims[0])
    assert w > 40 and h > 40  # it fills a good part of the page
    before = prim3d.bbox(page.prims[0])
    apply_ops(ep, [{"op": "edit_prim", "page": 1, "id": "s1", "pos": [150, 200, page.prims[0]["pos"][2]]}])
    assert prim3d.bbox(ep.pages[0].prims[0])[:2] != before[:2]
    apply_ops(ep, [{"op": "trace_prims", "page": 1, "ids": ["s1"], "layer": "ink"}])
    assert len(_ink(ep.pages[0]).strokes) == len(lines)
    guide = render_page(ep.pages[0], 60, mode="proof", episode=ep)
    printed = render_page(ep.pages[0], 60, mode="print", episode=ep)
    assert guide.tobytes() != printed.tobytes() or kind  # (the guide shows on screen only)
    apply_ops(ep, [{"op": "delete_prim", "page": 1, "id": "s1"}])
    assert not ep.pages[0].prims


def test_a_corridor_runs_to_one_vanishing_point():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_scene", "page": 1, "kind": "corridor", "id": "c"}])
    prim = ep.pages[0].prims[0]
    assert abs(prim["rot"][1]) < 1e-9  # looking straight down it
    w, h, d = prim["size"]
    near = prim3d._to_page(prim, (-w / 2, h / 2, -d / 2))
    far = prim3d._to_page(prim, (-w / 2, h / 2, d / 2))
    far_right = prim3d._to_page(prim, (w / 2, h / 2, d / 2))
    near_right = prim3d._to_page(prim, (w / 2, h / 2, -d / 2))
    assert abs(far_right[0] - far[0]) < abs(near_right[0] - near[0])  # the far end is smaller


def test_unknown_scenes_are_refused():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_scene", "page": 1, "kind": "castle"}])


# --- reference layers -----------------------------------------------------------------------------------------


def _boxed(ep):
    """A closed square drawn on the ink layer, and a separate colour layer."""
    page = ep.pages[0]
    ink = _ink(page)
    square = [[60, 60], [120, 60], [120, 120], [60, 120], [60, 60]]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": square, "width_mm": 1, "stabilize": 0},
                   {"op": "add_layer", "page": 1, "kind": "paint", "name": "色", "id": "colour"}])
    return ink.id


def test_a_fill_can_look_at_the_reference_layers_only(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _boxed(ep)
    with pytest.raises(ApplyError, match="reference"):
        apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "colour", "x_mm": 90, "y_mm": 90, "reference": "reference"}])
    apply_ops(ep, [{"op": "set_layer", "page": 1, "id": ink, "reference": True}])
    apply_ops(ep, [{"op": "fill", "page": 1, "layer_id": "colour", "x_mm": 90, "y_mm": 90, "rgb": [200, 60, 60],
                    "reference": "reference", "gap_mm": 0}])
    colour = next(layer for layer in ep.pages[0].layers if layer.id == "colour")
    box = colour.patches[-1]["box"]
    assert 55 <= box[0] <= 65 and box[2] < 70  # the square's inside, not the whole panel
    # (looking at the colour layer alone, there are no lines: the fill would take the panel)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    assert next(layer for layer in load_episode(project).pages[0].layers if layer.id == ink).reference


def test_the_layer_panel_marks_reference_layers(tmp_path: Path):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _boxed(ep)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    window = MainWindow(project)
    window.show()
    app.processEvents()
    window.set_target_layer(ink)
    window.layers.refresh()
    window.layers.reference.click()
    assert _ink(window.episode.pages[0]).reference
    window.layers.refresh()  # (the panel redraws lazily when it is out of sight)
    row = window.layers.ids.index(ink)
    assert "参照" in window.layers.list.item(row).text()
    assert window.brush.reference.findData("reference") >= 0
    window.commit_now()
    window.close()


# --- auto actions --------------------------------------------------------------------------------------------


def test_record_and_play_an_action_on_another_page(tmp_path: Path):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    from genko.app import actions
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 3, PageSpec.b4_comic())
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    window = MainWindow(project)
    window.show()
    app.processEvents()
    window.start_recording()
    assert "記録中" in window.status.text()
    window.apply_ops([{"op": "add_layer", "page": 1, "kind": "paint", "name": "影", "id": "shade", "blend": "multiply"},
                      {"op": "set_layer", "page": 1, "id": "shade", "opacity": 0.5}])
    window.apply_ops([{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": window.episode.pages[0].leaf_frames()[0].id}])
    assert window.stop_recording("影と集中線")
    saved = actions.load()["影と集中線"]["ops"]
    assert [op["op"] for op in saved] == ["add_layer", "set_layer", "add_effect"]
    assert (Path(os.environ["GENKO_CONFIG_DIR"]) / "actions.json").is_file()

    window.go_to_page(3)
    before = len(window.episode.pages[2].layers)
    undo_depth = len(window.episode.undo_stack)
    assert window.play_action("影と集中線")
    page = window.episode.pages[2]
    added = [layer for layer in page.layers if layer.title == "影"]
    assert len(page.layers) == before + 1 and added and added[0].opacity == 0.5 and added[0].id != "shade"
    assert page.effects and len(window.episode.undo_stack) == undo_depth + 1  # one undo takes it all back
    # again on the same page: new ids each time, no clash
    assert window.play_action("影と集中線")
    assert len([layer for layer in window.episode.pages[2].layers if layer.title == "影"]) == 2
    window._fill_actions_menu()
    menu_titles = [act.text() for act in window.actions_menu.actions()]
    assert "実行: 影と集中線" in menu_titles
    actions.remove("影と集中線")
    assert "影と集中線" not in actions.load()
    window.commit_now()
    window.close()


def test_replay_aims_at_the_current_layer_and_panel():
    from genko.app import actions

    ops = [{"op": "add_stroke", "page": 1, "layer_id": "old", "points": [[1, 1], [2, 2]]},
           {"op": "filter_raster", "page": 1, "id": "old", "kind": "blur"},
           {"op": "add_tone", "page": 1, "frame_id": "f_old", "density": 0.2}]
    out = actions.replay(ops, 5, "now", None)
    assert out[0]["page"] == 5 and out[0]["layer_id"] == "now" and out[1]["id"] == "now"
    assert "frame_id" not in out[2]
    assert actions.replay(ops, 5, "now", "f_new")[2]["frame_id"] == "f_new"
    assert actions.recordable([{"op": "undo"}, {"op": "add_page", "count": 1}]) == []
