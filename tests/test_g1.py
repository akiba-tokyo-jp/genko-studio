"""G1: a new book's first lines print, undo takes back one change at a time, and a finished page stays
quick to draw on."""

import os
import random
import sys
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import checks  # noqa: E402
from genko import render as renders  # noqa: E402
from genko.app.session import Session  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(ep, page=0):
    return next(layer for layer in ep.pages[page].layers if layer.role == LayerRole.INK)


def test_lines_only_on_the_name_are_named_by_the_check():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    name = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.NAME)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": name.id, "points": [[40, 150], [200, 150]], "width_mm": 3}])
    issues = checks.book(ep)["issues"]
    hit = next(i for i in issues if i["code"] == "art_not_printed")
    assert hit["level"] == "error" and "ネーム" in hit["message"] and "出ない" in hit["message"]
    assert not any(i["code"] == "empty_page" for i in issues)


def test_undo_takes_back_one_change_at_a_time(tmp_path: Path):
    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, PageSpec.b4_comic()), project)
    session = Session.open(project)
    for i in range(3):
        session.apply([{"op": "add_stroke", "page": 1, "layer_id": _ink(session.episode).id, "points": [[10, 10 + i], [50, 10 + i]]}])
    session.commit()  # (the save that used to gather all three)
    session.undo()
    assert len(_ink(session.episode).strokes) == 2
    session.undo()
    assert len(_ink(session.episode).strokes) == 1
    session.redo()
    session.redo()
    assert len(_ink(session.episode).strokes) == 3
    # an undo of a change not yet saved can be redone after the next undo, too
    session.apply([{"op": "add_stroke", "page": 1, "layer_id": _ink(session.episode).id, "points": [[10, 90], [50, 90]]}])
    session.undo()
    session.undo()
    session.redo()
    session.redo()
    assert len(_ink(session.episode).strokes) == 4


def test_a_finished_page_stays_quick_to_draw_on():
    rng = random.Random(3)
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _ink(ep)
    ops = []
    for _ in range(1500):
        x, y = rng.uniform(20, 230), rng.uniform(20, 340)
        ops.append({"op": "add_stroke", "page": 1, "layer_id": ink.id, "stabilize": 0,
                    "points": [[x + i * 1.2, y + rng.uniform(-2, 2), rng.uniform(0.3, 1)] for i in range(20)]})
    apply_ops(ep, ops)
    render_page(ep.pages[0], 110, mode="proof", episode=ep)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[30, 30], [200, 300]], "stabilize": 0}])
    start = time.perf_counter()
    quick = render_page(ep.pages[0], 110, mode="proof", episode=ep)
    assert time.perf_counter() - start < 0.5
    renders._STROKE_CACHE.clear()
    assert quick.tobytes() == render_page(ep.pages[0], 110, mode="proof", episode=ep).tobytes()
    # a line taken away (the eraser) draws the layer again, correctly
    apply_ops(ep, [{"op": "erase", "page": 1, "layer_id": ink.id, "points": [[20, 20], [240, 350]], "width_mm": 4}])
    erased = render_page(ep.pages[0], 110, mode="proof", episode=ep)
    renders._STROKE_CACHE.clear()
    assert erased.tobytes() == render_page(ep.pages[0], 110, mode="proof", episode=ep).tobytes()


# --- the window --------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


def test_a_new_books_first_lines_print(qapp, tmp_path: Path):
    from test_m13 import _drag

    from genko.app import exporting
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.show()
    assert win.target_layer().role == LayerRole.INK and "印刷されません" not in win.layers.target.text()
    win.act_pen.trigger()
    _drag(win.canvas, None, None, path=[(60 + i * 5, 150) for i in range(30)])
    win.commit_now()
    result = exporting.run(win.episode, project, "png", tmp_path / "out", dpi=60)
    from PIL import Image

    image = Image.open(result["files"][0]).convert("L")
    assert min(image.getpixel((x, y)) for x in range(image.width // 3, image.width // 2) for y in range(image.height // 3,
                                                                                                        image.height // 2)) < 100
    # choosing the name layer says it does not print
    name = next(layer for layer in win.current_page().layers if layer.role == LayerRole.NAME)
    win.set_target_layer(name.id)
    win.layers.refresh()
    assert "印刷されません" in win.layers.target.text()
    win.close()
