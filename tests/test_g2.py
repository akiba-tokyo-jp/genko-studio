"""G2: the chosen line's settings beside the tool, panels that need no long scroll, compact layers, a
steadied line while drawing, and no agent stages in a book drawn alone."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode, stroke_points  # noqa: E402
from genko.ops import apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "a", "text": "今日こそ言わなきゃ", "x_mm": 150, "y_mm": 40, "w_mm": 30,
                    "h_mm": 45, "wrap": "vertical"}])
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.close()


def test_the_chosen_lines_settings_sit_beside_the_tool(window, qapp):
    window.act_select.trigger()
    window.canvas.selected_line_id = "a"
    window._on_line_selected("a", False)
    qapp.processEvents()
    box = window.story.style_box
    assert window.tool_settings.stack.currentWidget().isAncestorOf(box) and box.isVisible()
    assert "今日こそ" in window.story.style_title.text() and window.story.font.isEnabled()
    window.story.size.setValue(7)
    window.story._style_changed()
    assert window._line("a").style.get("size_mm") == 7
    # the lines panel shows its list and words without scrolling
    window.show_dock("台詞")
    qapp.processEvents()
    scroll = next(d for d in window.studio_docks if d.windowTitle() == "台詞").widget()
    assert scroll.widget().minimumSizeHint().height() <= scroll.viewport().height()
    # the materials in three pages
    assert [window.materials.tabs.tabText(i) for i in range(3)] == ["素材", "トーン", "効果線"]


def test_layers_are_compact_and_empty_ones_have_no_picture(window):
    panel = window.layers
    panel.refresh()
    assert panel.list.iconSize().height() <= 24
    assert all(panel.list.item(i).icon().isNull() for i in range(panel.list.count()))  # nothing drawn yet


def test_no_agent_stage_in_a_book_drawn_alone(window):
    assert not window.act_name_ok.isVisible()
    assert "コマ 1 個" in window.status.text()


def test_the_line_while_drawing_is_steadied_like_the_finished_one(window):
    from PIL import ImageChops
    from test_f4 import _mouse, _screen

    from genko import brushes

    canvas = window.canvas
    window.act_pen.trigger()
    window.brush.taper.setChecked(False)
    window.brush.steady.setValue(7)
    import math

    path = [(60 + i * 1.5, 150 + 6 * math.sin(i * 1.3)) for i in range(50)]  # a shaky hand
    canvas.mousePressEvent(_mouse(canvas, "press", _screen(canvas, *path[0])))
    for pt in path[1:]:
        canvas.mouseMoveEvent(_mouse(canvas, "move", _screen(canvas, *pt)))
        canvas._live_sync()
    live = canvas._live.coverage()
    tail = canvas._live.tail
    canvas.mouseReleaseEvent(_mouse(canvas, "release", _screen(canvas, *path[-1]), buttons=False))
    ink = next(layer for layer in window.current_page().layers if layer.role == LayerRole.INK)
    stroke = ink.strokes[-1]
    shift = [(p[0] + canvas._pan_x / canvas._scale, p[1] + canvas._pan_y / canvas._scale, *p[2:]) for p in stroke_points(stroke)]
    mask, (x0, y0) = brushes.draw(live.size, shift, canvas._scale * 25.4, stroke.width_mm, stroke.kind)
    final = live.copy()
    final.paste(0, (0, 0, *final.size))
    final.paste(mask, (x0, y0))
    if tail is not None:  # (the moving end is drawn apart; count it in)
        from PIL import Image

        piece = Image.frombytes("RGBA", (tail[2].width(), tail[2].height()), bytes(tail[2].constBits())).split()[3]
        live.paste(ImageChops.lighter(live.crop((tail[0], tail[1], tail[0] + piece.width, tail[1] + piece.height)), piece),
                   (tail[0], tail[1]))
    a, b = live.point(lambda v: 255 if v > 96 else 0), final.point(lambda v: 255 if v > 96 else 0)
    overlap = ImageChops.multiply(a, b).histogram()[255] / max(1, ImageChops.lighter(a, b).histogram()[255])
    assert overlap > 0.9, overlap
