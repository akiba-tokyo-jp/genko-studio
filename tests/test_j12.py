"""J12: animation — a page as a short animation with animation folders and cels, an exposure sheet on a timeline,
onion skin and the light table, camera work, playing, and writing it out."""

import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import anim  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ball(x: float) -> dict:
    return {"op": "add_stroke", "page": 1, "layer_id": None, "points": [[x, 100], [x + 20, 100], [x + 20, 120], [x, 120], [x, 100]],
            "width_mm": 3}


def _animated(frames: int = 6):
    """A ball in three places: cels a, b, c shown two frames each, over a background line."""
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ep.pages[0].name_ok = True
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 200], [160, 200]]},
                   {"op": "set_animation", "page": 1, "fps": 8, "frames": frames},
                   {"op": "add_anim_folder", "page": 1, "id": "ball", "name": "ボール"},
                   {"op": "add_cel", "page": 1, "folder": "ball", "id": "a", "name": "A"},
                   {"op": "add_cel", "page": 1, "folder": "ball", "id": "b", "name": "B", "at": 3},
                   {"op": "add_cel", "page": 1, "folder": "ball", "id": "c", "name": "C", "at": 5}])
    for cel, x in (("a", 30), ("b", 80), ("c", 130)):
        apply_ops(ep, [{**_ball(x), "layer_id": cel}])
    return ep


def _dark_at(image, x_mm: float, y_mm: float, dpi: int) -> bool:
    from genko.render import mm_to_px

    px = image.convert("L").getpixel((mm_to_px(x_mm, dpi), mm_to_px(y_mm, dpi)))
    return px < 128


def test_exposures_choose_the_cel_of_each_frame():
    ep = _animated()
    page = ep.pages[0]
    assert [anim.cel_at(page, "ball", f) for f in range(1, 7)] == ["a", "a", "b", "b", "c", "c"]
    apply_ops(ep, [{"op": "set_exposure", "page": 1, "folder": "ball", "frame": 4, "cel": None}])
    assert anim.cel_at(ep.pages[0], "ball", 4) is None and anim.cel_at(ep.pages[0], "ball", 5) == "c"
    apply_ops(ep, [{"op": "set_exposure", "page": 1, "folder": "ball", "frame": 4, "clear": True}])
    assert anim.cel_at(ep.pages[0], "ball", 4) == "b"
    apply_ops(ep, [{"op": "set_exposures", "page": 1, "folder": "ball", "cels": [[1, "c"], [2, "b"], [3, "a"]]}])
    assert [anim.cel_at(ep.pages[0], "ball", f) for f in range(1, 5)] == ["c", "b", "a", "a"]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_exposure", "page": 1, "folder": "ball", "frame": 99, "cel": "a"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_exposure", "page": 1, "folder": "ball", "frame": 1, "cel": "ink"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_cel", "page": 1, "folder": "nope"}])


def test_each_frame_shows_its_cel_over_the_background():
    ep = _animated()
    page = ep.pages[0]
    dpi = 40
    first = anim.render_frame(page, 1, dpi, ep, area="paper")
    third = anim.render_frame(page, 3, dpi, ep, area="paper")
    assert _dark_at(first, 40, 100, dpi) and not _dark_at(first, 90, 100, dpi)
    assert _dark_at(third, 90, 100, dpi) and not _dark_at(third, 40, 100, dpi)
    assert _dark_at(first, 90, 200, dpi) and _dark_at(third, 90, 200, dpi)  # (the background is in every frame)
    from genko.render import render_page

    printed = render_page(page, dpi, mode="print", episode=ep)  # (printed, the page is its first frame)
    assert _dark_at(printed, 40, 100, dpi) and not _dark_at(printed, 140, 100, dpi)


def test_onion_skin_and_light_table():
    ep = _animated()
    page = ep.pages[0]
    ghost = anim.onion(page, 3, 30)
    from genko.render import mm_to_px

    before = ghost.getpixel((mm_to_px(40, 30), mm_to_px(100, 30)))  # (cel a, the frame before: red)
    after = ghost.getpixel((mm_to_px(140, 30), mm_to_px(100, 30)))  # (cel c, the frame after: blue)
    assert before[3] > 0 and before[0] > before[2]
    assert after[3] > 0 and after[2] > after[0]
    assert ghost.getpixel((mm_to_px(90, 30), mm_to_px(100, 30)))[3] == 0  # (not the cel shown now)
    apply_ops(ep, [{"op": "set_light_table", "page": 1, "cels": ["c"]}])
    lit = anim.onion(ep.pages[0], 1, 30, before=0, after=0)
    assert lit.getpixel((mm_to_px(140, 30), mm_to_px(100, 30)))[3] > 0


def test_camera_work_moves_between_keys():
    ep = _animated()
    apply_ops(ep, [{"op": "set_camera_key", "page": 1, "frame": 1, "rect": [0, 0, 100, 140]},
                   {"op": "set_camera_key", "page": 1, "frame": 5, "rect": [80, 80, 100, 140]}])
    page = ep.pages[0]
    assert anim.camera_at(page, 3) == pytest.approx([40, 40, 100, 140])
    assert anim.camera_at(page, 6) == pytest.approx([80, 80, 100, 140])
    one = anim.render_frame(page, 1, 20, ep)
    assert one.size == (round(100 / 25.4 * 20), round(140 / 25.4 * 20)) or abs(one.size[0] - 79) <= 1
    apply_ops(ep, [{"op": "set_camera_key", "page": 1, "frame": 5, "rect": None}])
    assert anim.camera_at(ep.pages[0], 5) == [0, 0, 100, 140]


def test_writing_it_out(tmp_path):
    ep = _animated()
    gif = anim.export(ep.pages[0], tmp_path / "a.gif", episode=ep, dpi=20)
    with Image.open(gif) as image:
        assert image.n_frames == 3 or image.n_frames == 6  # (GIF may merge the repeated frames)
        assert image.info.get("loop") == 0
    frames = anim.export(ep.pages[0], tmp_path / "frames", episode=ep, dpi=20, fmt="frames", width=100)
    assert len(frames) == 6 and Image.open(frames[0]).width == 100
    webp = anim.export(ep.pages[0], tmp_path / "a.webp", episode=ep, dpi=20)
    assert webp.is_file()
    plain = new_episode("t", 1, 1, PageSpec.b5_doujin())
    with pytest.raises(ValueError):
        anim.export(plain.pages[0], tmp_path / "x.gif")


def test_length_changes_and_saving(tmp_path):
    ep = _animated()
    apply_ops(ep, [{"op": "set_animation", "page": 1, "frames": 4, "loop": False}])
    assert anim.track(ep.pages[0], "ball")["cels"] == [[1, "a"], [3, "b"]]
    save_episode(ep, tmp_path / "a.genko")
    again = load_episode(tmp_path / "a.genko")
    assert anim.frames_of(again.pages[0]) == 4 and anim.cel_at(again.pages[0], "ball", 4) == "b"
    apply_ops(again, [{"op": "set_animation", "page": 1, "off": True}])
    assert not anim.is_animation(again.pages[0])
    with pytest.raises(ApplyError):
        apply_ops(again, [{"op": "set_animation", "page": 1, "fps": 0}])


def test_agents(tmp_path):
    from genko.studio.service import AGENT_OPS

    assert {"set_animation", "add_anim_folder", "add_cel", "set_exposure", "set_exposures", "set_camera_key",
            "set_light_table"} <= AGENT_OPS


def test_cli(tmp_path):
    from genko.__main__ import main

    ep = _animated()
    save_episode(ep, tmp_path / "a.genko")
    assert main(["export", str(tmp_path / "a.genko"), str(tmp_path / "out.gif"), "--format", "animation", "--dpi", "20"]) == 0
    assert (tmp_path / "out.gif").is_file()


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 2, PageSpec.b5_doujin())
    for page in ep.pages:
        page.name_ok = True
    project = tmp_path / "w.genko"
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_the_timeline_in_the_app(window, qapp, tmp_path):
    window.show_dock("タイムライン")
    panel = window.timeline
    assert panel.start.isVisible()
    panel._start()  # (this page becomes an animation with a folder and its first cel, which is the drawing target)
    page = window._current()
    assert anim.is_animation(page) and panel.table.rowCount() == 1 and panel.table.columnCount() == 24
    first = window._target_layer_id
    assert anim.cel_at(page, anim.spec(page)["tracks"][0]["folder"], 1) == first
    window.apply_ops([{**_ball(30), "layer_id": first}])
    panel.set_frame(5)
    panel._add_cel()  # (a new cel from frame 5, drawn on next)
    second = window._target_layer_id
    assert second != first and anim.cel_at(window._current(), panel.current_folder(), 5) == second
    window.apply_ops([{**_ball(90), "layer_id": second}])
    panel.set_frame(2)
    assert window._target_layer_id == first  # (the frame's cel becomes the drawing target)
    assert window.current_frame() == 2
    assert window._render_current(30) is not None  # (with the onion skin)
    assert panel.table.item(0, 4).text() == "2" and panel.table.item(0, 0).text() == "1"
    panel.play_button.setChecked(True)
    for _ in range(3):
        panel._tick()
    assert panel.frame == 5
    panel.play_button.setChecked(False)
    out = anim.export(window._current(), tmp_path / "w.gif", episode=window.episode, dpi=20)
    assert out.is_file()
    page_menu = next(a.menu() for a in window.menuBar().actions() if a.text() == "ページ")
    assert window.act_timeline in page_menu.actions()
