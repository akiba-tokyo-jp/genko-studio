"""J8: the 3D figure with a body to shape and joints to turn (hands too), heads and hands alone, OBJ models,
surfaces lit by a light, the page's camera, and 3D turned into pen lines (hidden parts left out) and shaded
surfaces printed as tone."""

import math
import os
import sys
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402

CUBE = "\n".join(["v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0", "v 0 0 1", "v 1 0 1", "v 1 1 1", "v 0 1 1",
                  "f 1 4 3 2", "f 5 6 7 8", "f 1 2 6 5", "f 2 3 7 6", "f 3 4 8 7", "f 4 1 5 8"])


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book():
    return new_episode("t", 1, 1, PageSpec.b4_comic())


def _prim(ep, prim_id):
    return next(p for p in ep.pages[0].prims if p.get("id") == prim_id)


def test_the_figure_has_a_body_to_shape(tmp_path):
    from genko import mesh3d, prim3d

    ep = _book()
    apply_ops(ep, [{"op": "add_figure", "page": 1, "pos": [100, 180, 0], "height_mm": 120, "id": "f"}])
    tall = prim3d.prim_bbox(_prim(ep, "f"))
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"heads": 4.5, "build": 1.6, "shoulders": 1.4}}])
    figure = _prim(ep, "f")
    chibi = mesh3d.figure_skeleton(figure)
    assert chibi["unit"] == pytest.approx(120 / 4.5)
    assert prim3d.prim_bbox(figure)[2] > tall[2]  # (broader)
    save_episode(ep, tmp_path / "b.genko")
    assert _prim(load_episode(tmp_path / "b.genko"), "f")["body"]["heads"] == 4.5
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"heads": 20}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"tail": 1}}])


def test_joints_turn_and_presets_and_hands():
    from genko import mesh3d

    ep = _book()
    apply_ops(ep, [{"op": "add_figure", "page": 1, "pos": [100, 180, 0], "height_mm": 100, "id": "f"}])
    before = mesh3d.figure_skeleton(_prim(ep, "f"))["points"]["r_wrist"].copy()
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "joints": {"r_arm": {"z": -1.5}}}])
    after = mesh3d.figure_skeleton(_prim(ep, "f"))["points"]["r_wrist"]
    assert after[1] < before[1] - 10  # the arm raised sideways
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "preset": "peace"}])
    figure = _prim(ep, "f")
    assert figure["preset"] == "peace" and figure["hands"]["r"] == "peace"
    for preset in mesh3d.FIGURE_PRESETS:
        apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "preset": preset}])
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "hands": {"l": "fist", "r": "point"}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "hands": {"r": "wave"}}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "joints": {"tail": {"x": 1}}}])


def test_dragging_a_hand_points_the_arm_at_the_pen():
    from genko import threeops

    ep = _book()
    apply_ops(ep, [{"op": "add_figure", "page": 1, "pos": [100, 180, 0], "height_mm": 100, "id": "f", "rot": [0, 0.5, 0]}])
    handles = dict(threeops.figure_handles(_prim(ep, "f")))
    shoulder_to = (handles["r_elbow"][0] - 30, handles["r_elbow"][1] - 40)
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "drag": {"handle": "r_elbow", "to": list(shoulder_to)}}])
    moved = dict(threeops.figure_handles(_prim(ep, "f")))
    from genko import mesh3d

    shoulder = mesh3d.to_page(_prim(ep, "f"), np.array([mesh3d.figure_skeleton(_prim(ep, "f"))["points"]["r_shoulder"]]))[0][0]
    got = math.atan2(moved["r_elbow"][1] - shoulder[1], moved["r_elbow"][0] - shoulder[0])
    want = math.atan2(shoulder_to[1] - shoulder[1], shoulder_to[0] - shoulder[0])
    assert math.cos(got - want) > 0.98
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "f", "drag": {"handle": "pelvis", "to": [60, 150]}}])
    assert _prim(ep, "f")["pos"][:2] == [60.0, 150.0]


def test_heads_hands_and_models():
    from genko import prim3d

    ep = _book()
    apply_ops(ep, [{"op": "add_head", "page": 1, "pos": [60, 60, 0], "size_mm": 30, "rot": [0.2, 0.6, 0], "id": "h"},
                   {"op": "add_hand", "page": 1, "pos": [120, 60, 0], "size_mm": 30, "pose": "peace", "side": "l", "id": "k"},
                   {"op": "import_model", "page": 1, "obj": CUBE, "size_mm": 40, "pos": [180, 60, 0], "id": "m", "name": "箱"}])
    for prim_id in ("h", "k", "m"):
        assert len(prim3d.trace(_prim(ep, prim_id))) >= 3
    apply_ops(ep, [{"op": "pose_figure", "page": 1, "id": "k", "pose": "fist"}])
    assert _prim(ep, "k")["pose"] == "fist"
    cube_lines = prim3d.trace(_prim(ep, "m"))
    assert 6 <= sum(len(line) - 1 for line in cube_lines)  # (the seen edges of a turned cube)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_model", "page": 1, "obj": "v 0 0 0\nv 1 0 0"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_model", "page": 1, "obj": "v 0 0 0\nf 1 2 3"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_hand", "page": 1, "pos": [0, 0, 0], "side": "x"}])


def test_hidden_lines_are_left_out():
    from genko import mesh3d

    front = {"kind": "mesh", "pos": [100, 100, 0], "size": [40, 40, 40], "rot": [0, 0, 0], "mesh": mesh3d.read_obj(CUBE)}
    behind = {**front, "pos": [100, 100, 60], "size": [20, 20, 20]}
    alone = mesh3d.lines([behind])
    together = mesh3d.lines([front, behind])
    ink_alone = sum(math.dist(a, b) for line in alone for a, b in zip(line, line[1:]))
    ink_front = sum(math.dist(a, b) for line in mesh3d.lines([front]) for a, b in zip(line, line[1:]))
    ink_together = sum(math.dist(a, b) for line in together for a, b in zip(line, line[1:]))
    assert ink_alone > 20 and ink_together < ink_front + ink_alone * 0.3  # the small cube behind is hidden


def test_the_camera_turns_every_3d_and_the_light_shades():
    from genko import mesh3d, prim3d

    ep = _book()
    apply_ops(ep, [{"op": "add_prim3d", "page": 1, "kind": "box", "pos": [80, 120, 0], "size": 40, "rot": [0, 0, 0], "id": "b"},
                   {"op": "add_figure", "page": 1, "pos": [160, 200, 0], "height_mm": 90, "id": "f"}])
    plain = prim3d.bbox(mesh3d.with_camera(_prim(ep, "b"), ep.pages[0]))
    apply_ops(ep, [{"op": "set_camera", "page": 1, "turn": 0.8, "tip": 0.3, "focal_mm": 250}])
    assert ep.pages[0].extra["camera"]["focal_mm"] == 250
    turned = prim3d.bbox(mesh3d.with_camera(_prim(ep, "b"), ep.pages[0]))
    assert abs(turned[2] - plain[2]) > 3 or abs(turned[0] - plain[0]) > 3
    apply_ops(ep, [{"op": "set_light", "page": 1, "dir": [1, 0, -0.2], "ambient": 0.2}])
    shade, _z, alpha = mesh3d.raster([_prim(ep, "f")], (200, 300), 30, None, ep.pages[0].extra["light"]["dir"], 0.2,
                                     box=(110.0, 120.0))
    lit = shade[alpha]
    assert lit.max() > 0.8 and lit.min() < 0.4  # one side lit, the other in shadow
    image = render_page(ep.pages[0], 60, "proof", ep)
    assert image.getbbox()
    apply_ops(ep, [{"op": "set_camera", "page": 1, "off": True}])
    assert "camera" not in ep.pages[0].extra
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_camera", "page": 1, "focal_mm": 1}])


def test_3d_into_lines_and_toned_surfaces():
    ep = _book()
    apply_ops(ep, [{"op": "add_figure", "page": 1, "pos": [120, 200, 0], "height_mm": 120, "id": "f", "preset": "walk"},
                   {"op": "add_layer", "page": 1, "kind": "paint", "id": "lt"},
                   {"op": "render_prims", "page": 1, "layer_id": "lt", "ids": ["f"], "tone": {"lpi": 50}}])
    layer = next(item for item in ep.pages[0].layers if item.id == "lt")
    assert len(layer.strokes) > 20 and layer.raster_png and layer.screen["lpi"] == 50
    printed = render_page(ep.pages[0], 300, "print", ep, finish=False).convert("L")
    crop = np.asarray(printed.crop((round(110 / 25.4 * 300), round(120 / 25.4 * 300), round(130 / 25.4 * 300), round(140 / 25.4 * 300))))
    assert (crop < 60).any() and (crop > 200).any() and not ((crop > 90) & (crop < 170)).mean() > 0.2  # dots, not grey
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "lines"},
                   {"op": "render_prims", "page": 1, "layer_id": "lines", "surfaces": False}])
    assert next(item for item in ep.pages[0].layers if item.id == "lines").strokes


def test_materials_and_agents():
    from genko.studio.service import AGENT_OPS

    assert {"add_figure", "pose_figure", "add_head", "add_hand", "import_model", "set_camera", "set_light", "render_prims"} <= AGENT_OPS
    ep = _book()
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "3d-手（ピース）", "x_mm": 80, "y_mm": 80, "id": "k"},
                   {"op": "stamp_material", "page": 1, "material_id": "3d-頭部", "x_mm": 150, "y_mm": 80, "id": "h"}])
    assert _prim(ep, "k")["kind"] == "hand" and _prim(ep, "k")["pose"] == "peace" and _prim(ep, "h")["kind"] == "head"


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


def test_placing_and_posing_the_3d_figure(window, qapp):
    from genko import threeops

    window.act_add_figure.trigger()
    figure = window.current_page().prims[0]
    assert figure["kind"] == "figure" and window.canvas.tool == "3d"
    handles = dict(window.canvas._prim_handles(figure))
    assert "r_elbow" in handles and "pelvis" in handles and "turn" in handles
    elbow = handles["r_elbow"]
    window.canvas._prim_drag = {"id": figure["id"], "handle": "r_elbow", "prim": dict(figure), "orig": dict(figure),
                                "start": elbow, "moved": False}
    window.canvas._prim_move(window.canvas._pt(elbow[0] - 15, elbow[1] - 20))
    assert window.canvas._prim_lines(window.canvas._prim_drag["prim"], quick=True)
    window.canvas._prim_release()
    moved = dict(threeops.figure_handles(window.current_page().prims[0]))
    assert moved["r_elbow"] != pytest.approx(elbow, abs=1)
    window.pose_actions[[a.text() for a in window.pose_actions].index("ポーズ: 考える")].trigger()
    assert window.current_page().prims[0]["preset"] == "think"
    window.act_add_head.trigger()
    window.act_add_hand.trigger()
    assert [p["kind"] for p in window.current_page().prims] == ["figure", "head", "hand"]
    window.canvas.repaint()


def _glb() -> bytes:
    """A tetrahedron as a binary glTF (what VRM files are)."""
    import json
    import struct

    positions = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype="<f4").tobytes()
    indices = np.array([0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3], dtype="<u2").tobytes()
    blob = positions + indices
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"mesh": 0, "translation": [5, 0, 0]}], "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
           "buffers": [{"byteLength": len(blob)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions)},
                           {"buffer": 0, "byteOffset": len(positions), "byteLength": len(indices)}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
                         {"bufferView": 1, "componentType": 5123, "count": 12, "type": "SCALAR"}]}
    text = json.dumps(doc).encode("utf-8")
    text += b" " * (-len(text) % 4)
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob
    return struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body


def test_gltf_and_vrm_models():
    import base64

    from genko import mesh3d, prim3d

    mesh = mesh3d.read_gltf(_glb())
    assert len(mesh["f"]) == 4 and len(mesh["v"]) == 12
    ep = _book()
    apply_ops(ep, [{"op": "import_model", "page": 1, "glb": base64.b64encode(_glb()).decode("ascii"), "size_mm": 40, "pos": [100, 100, 0],
                    "id": "t"}])
    assert _prim(ep, "t")["kind"] == "mesh" and prim3d.trace(_prim(ep, "t"))
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_model", "page": 1, "glb": base64.b64encode(b"glTF" + b"\0" * 8).decode("ascii")}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_model", "page": 1, "gltf": "{\"buffers\": [{\"uri\": \"model.bin\"}]}"}])
