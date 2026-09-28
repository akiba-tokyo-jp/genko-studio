"""The 3D figure as CLIP STUDIO has it: IK, a man's and a woman's build, each finger bent, own poses kept,
and spheres, cones and props to place one by one."""

from __future__ import annotations

import math

import pytest

from genko import mesh3d, prim3d, threeops
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


def _figure_page():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_figure", "page": 1, "id": "f", "pos": [100, 160, 0], "height_mm": 90}])
    return episode


def _prim(episode, prim_id="f"):
    return next(p for p in episode.pages[0].prims if p["id"] == prim_id)


def test_pulling_the_hand_brings_the_whole_arm():
    episode = _figure_page()
    wrist = dict(threeops.figure_handles(_prim(episode)))["l_wrist"]
    target = [wrist[0] + 8, wrist[1] - 28]
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "drag": {"handle": "l_wrist", "to": target, "ik": True}}])
    prim = _prim(episode)
    now = threeops._page_of_joint(prim, "l_wrist", None)
    assert math.dist(now, target) < 1.0
    assert prim["joints"]["l_arm"] and prim["joints"]["l_elbow"]["x"] > 0.05  # (the shoulder and the elbow both moved)
    ankle = dict(threeops.figure_handles(prim))["r_ankle"]
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "drag": {"handle": "r_ankle", "to": [ankle[0] + 5, ankle[1] - 20], "ik": True}}])
    assert _prim(episode)["joints"]["r_knee"]["x"] <= 0.01  # (a knee bends backward only)
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "drag": {"handle": "head", "to": [0, 0], "ik": True}}])


def test_a_womans_and_a_mans_build():
    episode = _figure_page()
    plain = mesh3d.figure_skeleton(_prim(episode))
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"sex": "female"}}])
    woman = mesh3d.figure_skeleton(_prim(episode))
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"sex": "male", "hips": 1.3}}])
    man = mesh3d.figure_skeleton(_prim(episode))

    def width(sk, a, b):
        return abs(sk["points"][a][0] - sk["points"][b][0])

    assert width(woman, "l_shoulder", "r_shoulder") < width(plain, "l_shoulder", "r_shoulder") < width(man, "l_shoulder", "r_shoulder")
    assert width(woman, "l_hip", "r_hip") > width(plain, "l_hip", "r_hip")
    assert man["body"]["hips"] == 1.3  # (the person's own number wins over the build)
    faces_woman = len(mesh3d._figure({**_prim(episode), "body": {"sex": "female"}})[0][1])
    faces_plain = len(mesh3d._figure({**_prim(episode), "body": {}})[0][1])
    assert faces_woman > faces_plain
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "body": {"sex": "robot"}}])


def test_each_finger_bends_on_its_own():
    episode = _figure_page()
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "hands": {"r": {"pose": "open", "curls": {"index": 1.0}}}}])
    hand = _prim(episode)["hands"]["r"]
    assert hand["curls"] == [0.0, 1.0, 0.0, 0.0, 0.0]
    assert mesh3d.hand_curls(hand) == (0.0, 1.0, 0.0, 0.0, 0.0)
    apply_ops(episode, [{"op": "add_hand", "page": 1, "id": "h", "pose": "fist"},
                        {"op": "pose_figure", "page": 1, "id": "h", "curls": [0, 0, 1, 1, 1]}])
    assert _prim(episode, "h")["curls"] == [0.0, 0.0, 1.0, 1.0, 1.0]
    assert mesh3d.prim_lines(_prim(episode, "h"))
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "hands": {"l": {"pose": "open", "curls": {"sixth": 1}}}}])


def test_own_poses_are_kept_and_brought_back(tmp_path, monkeypatch):
    from genko import poses

    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path))
    episode = _figure_page()
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "preset": "run"}])
    poses.save_pose("走り込み", _prim(episode))
    assert [p["name"] for p in poses.user_poses()] == ["走り込み"]
    kept = poses.find("走り込み")
    apply_ops(episode, [{"op": "pose_figure", "page": 1, "id": "f", "preset": "stand"},
                        {"op": "pose_figure", "page": 1, "id": "f", "set_joints": kept["joints"], "hands": kept["hands"]}])
    assert _prim(episode)["joints"]["l_knee"]["x"] == pytest.approx(-1.3)
    poses.delete_pose("走り込み")
    assert poses.user_poses() == []


def test_spheres_cones_and_props():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_prim3d", "page": 1, "kind": "sphere", "id": "s", "size": [40, 40, 40]},
                        {"op": "add_prim3d", "page": 1, "kind": "cone", "id": "c"},
                        {"op": "add_prim3d", "page": 1, "kind": "prop", "prop": "chair", "id": "ch"}])
    s = _prim(episode, "s")
    x, y, w, h = prim3d.bbox(s)
    assert w == pytest.approx(h, rel=0.15)
    assert len(prim3d.trace(_prim(episode, "c"))) >= 9 and len(prim3d.edges(_prim(episode, "ch"))) == 6 * 12
    verts, faces, _parts = mesh3d._solid(_prim(episode, "ch"))
    assert len(faces) == 6 * 6
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "add_prim3d", "page": 1, "kind": "prop", "prop": "spaceship"}])


def test_the_3d_panel_keeps_a_pose_and_offers_it(tmp_path, monkeypatch):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path))
    from genko import poses
    from genko.app.main import MainWindow

    window = MainWindow()
    try:
        window._add_prim("figure")
        prim_id = window.canvas.selected_prim_id
        prim = next(p for p in window._current().prims if p["id"] == prim_id)
        poses.save_pose("手を振る", {**prim, "joints": {"r_arm": {"z": -2.0}}})
        window.guides.select_prim(prim_id)
        window.guides._show_prim()
        labels = [window.guides.preset.itemText(i) for i in range(window.guides.preset.count())]
        assert "自分: 手を振る" in labels
        window.guides.preset.setCurrentIndex(labels.index("自分: 手を振る"))
        window.guides._preset()
        prim = next(p for p in window._current().prims if p["id"] == prim_id)
        assert prim["joints"]["r_arm"]["z"] == -2.0
        window._add_prim("prop", prop="desk")
        assert window._current().prims[-1]["prop"] == "desk"
    finally:
        window.close()
