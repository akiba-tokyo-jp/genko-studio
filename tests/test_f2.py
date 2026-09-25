"""F2: nothing stops the work — a person drawing alone inks whenever they like, every error reads in
Japanese, and problems show as a notice instead of a window that must be closed."""

import os
import re
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.app import wording  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402

SRC = Path(__file__).parent.parent / "src" / "genko"
# every module whose messages reach people through ApplyError
SOURCES = ["ops.py", "rulers.py", "tones.py", "effects.py", "nombre.py", "models.py", "mannequin.py", "selection.py", "selops.py",
           "pagespec.py", "warp.py", "brushes.py", "frames.py", "materials/__init__.py", "lock.py", "journal.py", "app/session.py", "__main__.py"]


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _messages() -> list[str]:
    out = []
    for name in SOURCES:
        text = (SRC / name).read_text(encoding="utf-8")
        for raw in re.findall(r'raise (?:ApplyError|ValueError|WarpError)\(f?"([^"]+)"', text):
            out.append(re.sub(r"\{[^{}]*(\{[^{}]*\}[^{}]*)*\}", "3", raw))
    return sorted(set(out))


def test_every_error_reads_in_japanese():
    messages = _messages()
    assert len(messages) > 120
    english = []
    for message in messages:
        if re.search(r"[ぁ-んァ-ヶ一-龥]", message):
            continue  # already Japanese
        shown = wording.error(message)
        if re.search(r"[A-Za-z]{4,}", shown.replace("Genko", "")):
            english.append((message, shown))
    assert english == []
    # through the op prefix too
    assert wording.error("ops[2] fill: nothing to fill there (the click is on a line)").startswith("線の上なので塗れません")


def test_a_person_alone_inks_before_the_name_is_approved():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[50, 60], [80, 90]]},
                   {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[50, 60], [80, 90]]}])
    assert len(next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK).strokes) == 2
    # a book made with agents keeps the order, and says why in Japanese
    studio = new_episode("t", 1, 1, PageSpec.b4_comic())
    studio.strict_gates = True
    with pytest.raises(ApplyError) as err:
        apply_ops(studio, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[50, 60], [80, 90]]}])
    assert "エージェントと進める原稿" in wording.error(str(err.value))


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
    from PySide6.QtWidgets import QMessageBox

    def blocked(*args, **kwargs):
        raise AssertionError(f"a window stopped the work: {args[2] if len(args) > 2 else args}")

    monkeypatch.setattr(QMessageBox, "information", blocked)
    monkeypatch.setattr(QMessageBox, "warning", blocked)
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    qapp.processEvents()
    yield win
    win.close()


def _drag(canvas, path):
    from test_m13 import _drag as drag

    drag(canvas, None, None, path=path)


def test_a_new_book_inks_at_once_and_problems_are_notices(window):
    ink = next(layer for layer in window.current_page().layers if layer.role == LayerRole.INK)
    window.set_target_layer(ink.id)
    window.act_pen.trigger()
    _drag(window.canvas, [(60 + i, 100) for i in range(20)])
    assert next(layer for layer in window.current_page().layers if layer.role == LayerRole.INK).strokes
    # fill on a line: a red notice, no window
    window.apply_ops([{"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[40, 150], [200, 150]], "width_mm": 3,
                       "kind": "mili", "stabilize": 0, "taper": False}])
    window.act_fill.trigger()
    _drag(window.canvas, [(100, 150)])
    assert window.last_error.startswith("線の上なので塗れません") and "⚠" in window.status.text()
    # a locked layer, nothing to paste, no panel chosen
    window.apply_ops([{"op": "set_layer", "page": 1, "id": ink.id, "locked": True}])
    window.act_pen.trigger()
    _drag(window.canvas, [(60, 200), (90, 210)])
    assert "描けません" in window.last_notice
    window.act_paste.trigger()
    window.current_page().selected_frame_id = None
    window.act_merge.trigger()
    window.act_border.trigger()
    assert "コマ" in window.last_notice
