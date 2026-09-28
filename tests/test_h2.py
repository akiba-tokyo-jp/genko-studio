"""H2 and H3: the lettering form waits for a line, tools can show their names, the help knows today's
tools; the 電子音 balloon, font weights, and printing."""

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
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


@pytest.fixture
def window(qapp, tmp_path: Path):
    from PySide6.QtCore import QSettings

    from genko.app.main import MainWindow

    QSettings("Genko", "Genko Studio").remove("ui/tool_names")
    ep = new_episode("t", 1, 3, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "a", "text": "もしもし", "x_mm": 150, "y_mm": 40, "w_mm": 30,
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


# --- H2 ------------------------------------------------------------------------------------------------


def test_no_greyed_out_lettering_form_until_a_line_is_chosen(window, qapp):
    window.act_select.trigger()
    window.story.list.setCurrentRow(-1)
    window.story._picked()
    qapp.processEvents()
    assert not window.story.style_body.isVisible()
    assert "台詞をクリックすると" in window.story.style_title.text()
    window._on_line_selected("a", False)
    qapp.processEvents()
    assert window.story.style_body.isVisible() and "もしもし" in window.story.style_title.text()


def test_tools_can_show_their_names_and_it_is_remembered(window, qapp):
    from PySide6.QtCore import QSettings, Qt

    assert window.tool_palette.toolButtonStyle() == Qt.ToolButtonStyle.ToolButtonIconOnly
    window.act_tool_names.trigger()
    assert window.tool_palette.toolButtonStyle() == Qt.ToolButtonStyle.ToolButtonTextBesideIcon
    assert str(QSettings("Genko", "Genko Studio").value("ui/tool_names")) == "1"
    assert window.act_marquee.iconText() == "長方形選択" and window.act_marquee.text() == "範囲選択（長方形）"
    assert any(act is window.act_tool_names for top in window.menuBar().actions() if top.menu() for act in top.menu().actions())
    window.act_tool_names.trigger()
    assert str(QSettings("Genko", "Genko Studio").value("ui/tool_names")) == "0"


def test_fill_lasso_fill_and_picker_icons_look_different(qapp):
    from genko.app.icons import icon

    def pixels(name):
        image = icon(name).pixmap(32, 32).toImage()
        return bytes(image.constBits())[: image.sizeInBytes()]

    shots = {name: pixels(name) for name in ("fill", "lassofill", "picker", "gradient")}
    assert len(set(shots.values())) == 4


def test_the_help_knows_todays_tools(window):
    from genko.app import help as helps

    for words in ("{大|", "レイヤー移動", "グラデーション", "全体図", "道具の名前を表示", "印刷", "電子音"):
        assert words in helps.GUIDE + helps.FAQ, words


# --- H3 ------------------------------------------------------------------------------------------------


def _line_image(balloon: str, **style):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    op = {"op": "add_line", "page": 1, "text": "ピンポーン", "x_mm": 60, "y_mm": 60, "w_mm": 60, "h_mm": 50,
          "balloon": balloon, "tail": [40, 140]}
    if style:
        op["style"] = style
    apply_ops(ep, [op])
    return ep, render_page(ep.pages[0], 150, mode="print", episode=ep).convert("L")


def test_an_electric_balloon_is_jagged_and_its_tail_zigzags():
    assert "electric" in balloons.SHAPES
    edge = balloons._electric((0, 0, 100, 60))
    assert len(edge) >= 28
    # sharp: the edge goes in and out (not a smooth ellipse)
    dists = [((x - 50) ** 2 / 50 ** 2 + (y - 30) ** 2 / 30 ** 2) ** 0.5 for x, y in edge]
    assert max(dists) - min(dists) > 0.1
    tail = balloons._tail_polygon("electric", (0, 0, 100, 60), (-20, 120), None, 10)
    straight = balloons._tail_polygon("speech", (0, 0, 100, 60), (-20, 120), None, 10)
    assert tail != straight
    _, electric = _line_image("electric")
    _, speech = _line_image("speech")
    assert electric.tobytes() != speech.tobytes()
    from genko.app.lettering import KIND_LABEL

    assert KIND_LABEL["electric"].startswith("電子音")


def test_weights_go_normal_bold_heavy():
    ink = []
    for weight in ("normal", "bold", "heavy"):
        _, image = _line_image("none", weight=weight, size_mm=8)
        ink.append(sum(1 for v in image.getdata() if v < 128))
    assert ink[0] < ink[1] < ink[2]
    with pytest.raises(ApplyError):
        ep = new_episode("t", 1, 1, PageSpec.b4_comic())
        apply_ops(ep, [{"op": "add_line", "page": 1, "text": "あ", "style": {"weight": "black"}}])
    # a part of the line heavy, typed as {極太|…}
    from genko.app.lettering import parse_marks

    text, _ruby, _dots, styles = parse_marks("{極太|ドーン}と")
    assert text == "ドーンと" and styles == [["ドーン", {"bold": 2}]]


def test_the_weight_field_sets_the_lines_weight(window, qapp):
    window.act_select.trigger()
    window._on_line_selected("a", False)
    qapp.processEvents()
    window.story.weight.setCurrentIndex(2)
    window.story._style_changed()
    line = next(ln for ln in window.episode.story if ln.id == "a")
    assert line.style.get("weight") == "heavy" and balloons.line_weight(balloons.style_of(line)) == 2


def test_printing_puts_each_page_on_a_sheet(window, tmp_path: Path):
    from PySide6.QtPrintSupport import QPrinter

    from genko.app.printing import PrintDialog, print_pages

    printer = QPrinter(QPrinter.PrinterMode.ScreenResolution)
    printer.setOutputFormat(QPrinter.OutputFormat.PdfFormat)
    out = tmp_path / "print.pdf"
    printer.setOutputFileName(str(out))
    assert print_pages(window.episode, printer, [1, 3], "trim") == 2
    data = out.read_bytes()
    assert data.startswith(b"%PDF") and data.count(b"/Type /Page") - data.count(b"/Type /Pages") == 2
    dialog = PrintDialog(window)
    dialog.pages.setText("2-3")
    assert dialog.chosen() == [2, 3]
    dialog.pages.setText("9")
    with pytest.raises(ValueError):
        dialog.chosen()
    assert window.act_print.shortcut().toString() == "Ctrl+P"
