"""Effect lines that keep clear of faces (avoid) or stay inside a selection (within), as hand-drawn lines stop at a
figure's outline and thin out there."""

import math
import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import effects  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def _page():
    return new_episode("t", 1, 1, PageSpec.b5_doujin()).pages[0]


def _inside(p, ellipse):
    cx, cy, rx, ry = ellipse
    return ((p[0] - cx) / rx) ** 2 + ((p[1] - cy) / ry) ** 2 < 0.98


@pytest.mark.parametrize("kind", ["speed", "focus", "uni_flash"])
def test_lines_stop_short_of_a_face_and_thin_out_there(kind):
    page = _page()
    face = [90.0, 120.0, 18.0, 22.0]
    plain = effects.geometry({"id": "e1", "kind": kind, "params": {"center": [60, 60]}}, page)
    clear = effects.geometry({"id": "e1", "kind": kind, "params": {"center": [60, 60], "avoid": [{"ellipse": face}]}}, page)
    assert any(_inside(p, face) for line in plain["lines"] for p in line["points"])  # (without it, lines cross the face)
    assert not any(_inside(p, face) for line in clear["lines"] for p in line["points"])
    # a line cut at the face thins out toward the cut
    cut = [line for line in clear["lines"] if min(math.hypot((p[0] - face[0]) / face[2], (p[1] - face[1]) / face[3])
                                                  for p in line["points"]) < 1.1]
    assert cut and all(min(p[2] for p in line["points"]) < 0.3 for line in cut)


def test_lines_stay_inside_a_selection_and_the_picture_follows():
    page = _page()
    box = [[20, 20], [80, 20], [80, 90], [20, 90]]
    geo = effects.geometry({"id": "e2", "kind": "speed", "params": {"angle": 0, "within": box}}, page)
    assert geo["lines"] and all(19.9 <= p[0] <= 80.1 and 19.9 <= p[1] <= 90.1 for line in geo["lines"] for p in line["points"])
    image = Image.new("RGB", (800, 1100), (255, 255, 255))
    drawn = effects.draw(image, {"id": "e3", "kind": "beta_flash", "params": {"center": [100, 130],
                                                                              "avoid": [{"ellipse": [100, 130, 20, 20]}]}}, page, 100)
    from genko.render import _xy

    x, y = _xy((100, 130), 100)
    assert drawn.getpixel((int(x), int(y))) == (255, 255, 255)  # (the fill is cut at the face too)
    with pytest.raises(ValueError):
        effects.validate("speed", {"avoid": [{"ellipse": [1, 2]}]})


def test_the_finish_keeps_every_effect_clear_of_the_reported_faces(tmp_path: Path):
    import test_battle_report as br
    from genko.io import load_episode
    from genko.studio import finish

    agent, frame, request, done = br._panel_with_art(tmp_path)
    regions = [{"kind": "face", "char": f["char"], "box01": f["head01"]} for f in request["figures"]]
    assert agent.report_regions("demo.genko", 1, frame.id, regions).ok
    # an effect put before: it is given the faces to keep clear of
    assert agent._ops("demo.genko", [{"op": "add_effect", "page": 1, "kind": "speed", "frame_id": frame.id, "id": "old", "params": {}}]).ok
    assert agent._ops("demo.genko", [{"op": "set_panel", "page": 1, "frame_id": frame.id, "set": {"fx": ["集中線"]}}]).ok
    episode = load_episode(tmp_path / "demo.genko")
    ops, notes = finish.plan(episode, episode.pages[0])
    edits = [op for op in ops if op["op"] == "edit_effect" and op["id"] == "old"]
    focus = [op for op in ops if op["op"] == "add_effect" and op["kind"] == "focus"]
    assert edits and edits[0]["params"]["avoid"] and focus and focus[0]["params"]["avoid"]
    assert any(n["kind"] == "clear_faces" for n in notes) and any("手前で線を止めた" in (n.get("why") or "") for n in notes)


def test_the_window_draws_an_effect_inside_or_clear_of_the_selection(qapp, tmp_path: Path):
    from genko.app.main import MainWindow
    from genko.io import save_episode

    project = tmp_path / "e.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b5_doujin()), project)
    window = MainWindow(project)
    try:
        window.show()
        qapp.processEvents()
        lasso = [[30.0, 40.0], [90.0, 40.0], [90.0, 100.0], [30.0, 100.0]]
        window.canvas.set_selection({"poly": lasso})
        window._effect_kind = "speed"
        window._effect_at(60, 70)
        effect = window._current().effects[-1]
        assert effect["params"]["within"] == lasso
        window._effect_clearing(None)
        assert not window._current().effects[-1]["params"].get("within")
        window._effect_clearing("avoid")
        assert window._current().effects[-1]["params"]["avoid"] == [{"path": lasso}]
    finally:
        window.close()
