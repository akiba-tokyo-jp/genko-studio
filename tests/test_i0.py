"""I0: the six fixes from the fourth review — vertical lines by default, readable 電子音 balloons, a short
3D tool page, guides kept in their panel, printing with preview/size/spreads, and an action manager."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import balloons  # noqa: E402
from genko.io import save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 4, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "set_spread", "page": 2, "with": 3}])
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


def test_lines_are_vertical_unless_asked():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "もしもし", "id": "a"},
                   {"op": "add_line", "page": 1, "text": "Hello", "id": "b", "wrap": "horizontal"}])
    assert [ln.wrap for ln in ep.story] == ["vertical", "horizontal"]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_line", "page": 1, "text": "x", "wrap": "diagonal"}])


def test_an_electric_balloon_leaves_room_for_its_words():
    w, h, pad = 40.0, 55.0, 2.0
    electric = balloons._inner("electric", w, h, pad)
    speech = balloons._inner("speech", w, h, pad)
    assert electric[0] > speech[0] and electric[1] > speech[1]


def test_the_3d_page_is_short(window, qapp):
    from PySide6.QtWidgets import QPushButton

    window._tool("3d")
    qapp.processEvents()
    page = window.tool_settings.stack.currentWidget()
    buttons = [b for b in page.findChildren(QPushButton) if b.isVisible()]
    assert len(buttons) <= 6
    assert any(b.text().startswith("置く") and b.menu() for b in buttons)
    placed = [a.text() for a in next(b for b in buttons if b.text().startswith("置く")).menu().actions()]
    assert "背景: 廊下" in placed and "3D の箱を置く" in placed


def test_guides_stay_inside_their_panel():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    page = ep.pages[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": page.leaf_frames()[0].id, "axis": "horizontal", "ratio": 0.5}])
    top = ep.pages[0].leaf_frames()[0]
    r = top.rect
    apply_ops(ep, [{"op": "add_scene", "page": 1, "kind": "corridor", "id": "c", "frame_id": top.id,
                    "pos": [r.x + r.width / 2, r.y + r.height / 2, 0]}])
    prim = ep.pages[0].prims[0]
    assert prim["frame_id"] == top.id
    image = render_page(ep.pages[0], 40, mode="proof", episode=ep).convert("RGB")
    from genko.render import mm_to_px

    below = image.crop((0, mm_to_px(r.y + r.height + 8, 40), image.width, image.height))
    assert not any(pixel == (90, 90, 140) for pixel in below.getdata())  # nothing of the corridor under its panel
    inside = image.crop((mm_to_px(r.x, 40), mm_to_px(r.y, 40), mm_to_px(r.x + r.width, 40), mm_to_px(r.y + r.height, 40)))
    assert any(pixel == (90, 90, 140) for pixel in inside.getdata())


def test_printing_previews_and_takes_size_and_spreads(window, tmp_path: Path):
    from PySide6.QtPrintSupport import QPrinter

    from genko.app.printing import PrintDialog, print_pages, sheets

    groups = sheets(window.episode, [1, 2, 3, 4], spreads=True)
    assert [[p.index for p in g] for g in groups] in ([[1], [3, 2], [4]], [[1], [2, 3], [4]])
    assert len(sheets(window.episode, [1, 2, 3, 4], spreads=False)) == 4
    printer = QPrinter(QPrinter.PrinterMode.ScreenResolution)
    printer.setOutputFormat(QPrinter.OutputFormat.PdfFormat)
    out = tmp_path / "p.pdf"
    printer.setOutputFileName(str(out))
    assert print_pages(window.episode, printer, [1, 2, 3, 4], "trim", scale="actual", spreads=True) == 3
    dialog = PrintDialog(window)
    assert dialog.scale.findData("actual") >= 0 and dialog.spreads.text().startswith("見開き")
    preview = dialog.preview()
    assert preview is not None and preview.windowTitle() == "印刷のプレビュー"
    preview.close()


def test_the_action_manager_shows_reorders_and_leaves_out_steps(window, qapp):
    from genko.app import actions

    actions.store("影", [{"op": "add_layer", "page": 1, "kind": "paint", "name": "影", "id": "s"},
                         {"op": "edit_line", "id": "L9", "text": "やあ"},
                         {"op": "set_layer", "page": 1, "id": "s", "opacity": 0.5}])
    assert actions.page_bound(actions.load()["影"]["ops"]) == [False, True, False]
    dialog = window.manage_actions()
    assert dialog.names.count() == 1 and dialog.steps.count() == 3
    assert "このページだけ" in dialog.steps.item(1).text()
    dialog.steps.setCurrentRow(1)
    dialog.drop_step()
    assert [op["op"] for op in actions.load()["影"]["ops"]] == ["add_layer", "set_layer"]
    dialog.steps.setCurrentRow(1)
    dialog.move_step(-1)
    assert [op["op"] for op in actions.load()["影"]["ops"]] == ["set_layer", "add_layer"]
    dialog.close()
    # page-bound steps are skipped, the rest still plays
    actions.store("混ぜ", [{"op": "edit_line", "id": "L9", "text": "x"},
                           {"op": "add_layer", "page": 1, "kind": "paint", "name": "足した", "id": "n"}])
    assert window.play_action("混ぜ")
    assert any(layer.title == "足した" for layer in window.current_page().layers)
