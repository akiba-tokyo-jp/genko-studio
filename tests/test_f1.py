"""F1: manuscript paper to the trade's sizes — paper, finished size (仕上がり), bleed (裁ち落とし) and the
basic frame (基本枠) with top / bottom / binding / fore-edge margins; old books unchanged; exports by area;
crop marks; bleed panels; spreads that meet at the gutter; changing a book's paper."""

import io
import os
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.export import export_print  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PAPER_PRESETS, LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import mm_to_px, render_page, render_spread  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


LEGACY_B4 = PageSpec(257, 364, 600, 3, 10, "mono", preset="commercial-b4")  # how B4 books were made before F1


def _rect(r):
    return (round(r.x, 3), round(r.y, 3), round(r.width, 3), round(r.height, 3))


def test_presets_have_the_trade_sizes():
    b4 = new_episode("t", 1, 1, PageSpec.b4_comic()).pages[0]
    assert _rect(b4.paper_rect_mm()) == (0, 0, 257, 364)
    assert _rect(b4.trim_rect_mm()) == (18.5, 27, 220, 310)
    assert _rect(b4.bleed_rect_mm()) == (13.5, 22, 230, 320)
    assert _rect(b4.inner_rect_mm()) == (38.5, 47, 180, 270)
    assert b4.frames[0].rect.width == 180  # a new page's panel is the basic frame
    for key, (w, h), (fw, fh) in (("b5", (182, 257), (150, 220)), ("a5", (148, 210), (120, 180))):
        page = new_episode("t", 1, 1, PAPER_PRESETS[key][1]()).pages[0]
        trim, frame, bleed = page.trim_rect_mm(), page.inner_rect_mm(), page.bleed_rect_mm()
        assert (trim.width, trim.height) == (w, h) and (frame.width, frame.height) == (fw, fh)
        assert bleed.x >= 0 and bleed.y >= 0 and bleed.x + bleed.width <= page.spec.width_mm  # the paper holds the bleed
    assert "基本枠 180×270" in PageSpec.b4_comic().describe()


def test_the_binding_and_fore_edge_margins_swap_with_the_page_side():
    spec = PageSpec.custom(257, 364, 220, 310, 5, top=18, bottom=22, inner=25, outer=15)
    ep = new_episode("t", 1, 2, spec)
    first, second = ep.pages  # right-bound: page 1 on the left (binding on its right), page 2 on the right
    t = first.trim_rect_mm()
    a, b = first.inner_rect_mm(), second.inner_rect_mm()
    assert first.binding_edge() == "right" and second.binding_edge() == "left"
    assert round(a.x - t.x, 3) == 15 and round(t.x + t.width - (a.x + a.width), 3) == 25
    assert round(b.x - t.x, 3) == 25 and round(t.x + t.width - (b.x + b.width), 3) == 15
    assert round(a.y - t.y, 3) == 18 and round(t.y + t.height - (a.y + a.height), 3) == 22
    with pytest.raises(ValueError):
        PageSpec.custom(200, 300, 220, 310, 5, 10, 10, 10, 10)  # the paper is smaller than the page
    with pytest.raises(ValueError):
        PageSpec.custom(257, 364, 220, 310, 5, 10, 10, 120, 120)  # no room for the frame


def test_old_books_open_and_draw_exactly_as_before(tmp_path: Path):
    ep = new_episode("t", 1, 1, LEGACY_B4)
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "name_ok", "page": 1},
                   {"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [[20, 20], [240, 340]], "kind": "mili"},
                   {"op": "add_line", "page": 1, "text": "こんにちは", "x_mm": 200, "y_mm": 30, "w_mm": 15, "h_mm": 40}])
    before = np.asarray(render_page(ep.pages[0], 60, mode="print", episode=ep))
    project = tmp_path / "old.genko"
    save_episode(ep, project)
    raw = (project / "project.json").read_text(encoding="utf-8")
    assert "trim_w_mm" not in raw and "margins_mm" not in raw  # old books keep their old spec
    again = load_episode(project)
    assert _rect(again.pages[0].inner_rect_mm()) == (13, 13, 231, 338)
    assert _rect(again.pages[0].trim_rect_mm()) == (3, 3, 251, 358)
    assert np.array_equal(np.asarray(render_page(again.pages[0], 60, mode="print", episode=again)), before)
    # and a new spec travels with the book
    fresh = new_episode("t", 1, 1, PageSpec.b5_doujin())
    save_episode(fresh, tmp_path / "new.genko")
    assert load_episode(tmp_path / "new.genko").spec == PageSpec.b5_doujin()


def test_exports_by_area(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    size = {}
    for area in ("paper", "bleed", "trim"):
        [path] = export_print(ep, tmp_path / area, fmt="png", dpi=100, area=area)
        size[area] = Image.open(path).size
    assert size["paper"] == (mm_to_px(257, 100), mm_to_px(364, 100))
    assert abs(size["bleed"][0] - mm_to_px(230, 100)) <= 1 and abs(size["bleed"][1] - mm_to_px(320, 100)) <= 1
    assert abs(size["trim"][0] - mm_to_px(220, 100)) <= 1 and abs(size["trim"][1] - mm_to_px(310, 100)) <= 1
    [pdf] = export_print(ep, tmp_path / "pdf", fmt="pdf", dpi=100, area="trim")
    assert pdf.stat().st_size > 0
    with pytest.raises(ValueError):
        export_print(ep, tmp_path / "x", fmt="png", dpi=100, area="page")


def test_crop_marks_sit_outside_the_bleed():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    dpi = 150
    grey = np.asarray(render_page(ep.pages[0], dpi, mode="print", episode=ep, crop_marks=True).convert("L"))
    plain = np.asarray(render_page(ep.pages[0], dpi, mode="print", episode=ep, crop_marks=False).convert("L"))
    marks = (grey < 128) & (plain >= 128)
    ys, xs = np.nonzero(marks)
    assert len(xs) > 50
    page = ep.pages[0]
    b = page.bleed_rect_mm()
    x0, y0, x1, y1 = (mm_to_px(v, dpi) for v in (b.x, b.y, b.x + b.width, b.y + b.height))
    inside = (xs > x0 + 1) & (xs < x1 - 1) & (ys > y0 + 1) & (ys < y1 - 1)
    assert not inside.any()  # nothing of the marks is printed on the page
    t = page.trim_rect_mm()
    # the corner mark at the trim line (内トンボ) is to the left of the bleed at the trim's top
    assert marks[mm_to_px(t.y, dpi) - 1:mm_to_px(t.y, dpi) + 2, : x0].any()


def test_bleed_panels_run_out_to_the_bleed_not_the_paper():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    frame = ep.pages[0].frames[0]
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    apply_ops(ep, [{"op": "name_ok", "page": 1}, {"op": "set_frame", "page": 1, "frame_id": frame.id, "bleed": True},
                   {"op": "fill_area", "page": 1, "layer_id": ink.id, "area": {"poly": [[0, 0], [257, 0], [257, 364], [0, 364]]},
                    "rgb": [0, 0, 0]}])
    image = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    b = ep.pages[0].bleed_rect_mm()
    assert image.getpixel((mm_to_px(b.x + 1, 100), mm_to_px(b.y + 50, 100))) < 60  # inside the bleed: black
    assert image.getpixel((mm_to_px(b.x - 3, 100), mm_to_px(b.y + 50, 100))) > 200  # the paper beyond: white


def test_spreads_meet_at_the_gutter():
    ep = new_episode("t", 1, 3, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])
    left, right = (ep.pages[1], ep.pages[2]) if ep.pages[1].side() == "left" else (ep.pages[2], ep.pages[1])
    image = render_spread(ep, 2, 3, dpi=50, mode="print")
    lt, rt = left.trim_rect_mm(), right.trim_rect_mm()
    assert abs(image.width - (mm_to_px(lt.x + lt.width, 50) + mm_to_px(257, 50) - mm_to_px(rt.x, 50))) <= 1
    trimmed = render_spread(ep, 2, 3, dpi=50, mode="print", to_trim=True)
    assert abs(trimmed.width - mm_to_px(440, 50)) <= 2 and abs(trimmed.height - mm_to_px(310, 50)) <= 1
    # a line drawn past the gutter on the left page lands on the right page, at the same place on the finished book
    ink = LayerRole.INK
    for page in (left, right):
        apply_ops(ep, [{"op": "name_ok", "page": page.index}])
    gutter = lt.x + lt.width
    apply_ops(ep, [{"op": "add_stroke", "page": left.index, "layer": "ink", "points": [[gutter + 10, 100], [gutter + 20, 100]]}])
    moved = next(layer for layer in ep.pages[right.index - 1].layers if layer.role == ink).strokes[-1]
    assert moved.points[0][0] == pytest.approx(rt.x + 10)


def test_changing_the_paper_moves_the_book_onto_the_new_frame(tmp_path: Path):
    ep = new_episode("t", 1, 2, LEGACY_B4)
    root = ep.pages[0].frames[0]
    ink = next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)
    old_frame = ep.pages[0].inner_rect_mm()
    corner = (old_frame.x + old_frame.width, old_frame.y)
    apply_ops(ep, [{"op": "name_ok", "page": 1},
                   {"op": "split_frame", "page": 1, "frame_id": root.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6},
                   {"op": "add_stroke", "page": 1, "layer_id": ink.id, "points": [list(corner), [corner[0] - 20, corner[1] + 20]],
                    "kind": "mili", "width_mm": 1},
                   {"op": "add_line", "page": 1, "text": "台詞", "x_mm": corner[0] - 20, "y_mm": corner[1] + 5, "w_mm": 12, "h_mm": 24},
                   {"op": "add_ruler", "page": 1, "kind": "radial", "points": [list(corner)]}])
    apply_ops(ep, [{"op": "set_page_spec", "preset": "b4"}])
    page = ep.pages[0]
    new = page.inner_rect_mm()
    assert ep.spec == PageSpec.b4_comic() and page.spec == ep.spec
    frames = page.leaf_frames()
    assert all(new.x - 0.01 <= f.rect.x and f.rect.x + f.rect.width <= new.x + new.width + 0.01 for f in frames)
    top = min(frames, key=lambda f: f.rect.y)
    assert top.rect.y == pytest.approx(new.y)
    stroke = next(layer for layer in page.layers if layer.role == LayerRole.INK).strokes[-1]
    assert stroke.points[0] == pytest.approx((new.x + new.width, new.y))
    assert stroke.width_mm < 1  # sizes shrink with the frame
    line = ep.story[0]
    assert new.x <= line.x_mm and line.x_mm + line.w_mm <= new.x + new.width + 0.01
    assert page.rulers[0]["points"][0] == pytest.approx([new.x + new.width, new.y])
    # numbers, without moving
    apply_ops(ep, [{"op": "set_page_spec", "paper": [210, 297], "trim": [182, 257], "bleed_mm": 3, "margins": [18, 18, 16, 16],
                    "move": False}])
    assert ep.spec.trim_size() == (182, 257) and ep.pages[0].inner_rect_mm().width == 150
    assert stroke.points[0] == pytest.approx((new.x + new.width, new.y))
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_page_spec", "preset": "b3"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_page_spec", "paper": [100, 100], "trim": [182, 257]}])
    save_episode(ep, tmp_path / "moved.genko")
    assert load_episode(tmp_path / "moved.genko").spec.trim_size() == (182, 257)


def test_paint_layers_move_with_the_paper():
    import base64

    from genko.raster import WORKING_DPI

    ep = new_episode("t", 1, 1, LEGACY_B4)
    page = ep.pages[0]
    frame = page.inner_rect_mm()
    image = Image.new("RGBA", (mm_to_px(257, WORKING_DPI), mm_to_px(364, WORKING_DPI)), (0, 0, 0, 0))
    cx, cy = frame.x + frame.width / 2, frame.y + frame.height / 2
    image.paste((0, 0, 0, 255), tuple(mm_to_px(v, WORKING_DPI) for v in (cx - 5, cy - 5, cx + 5, cy + 5)))
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    apply_ops(ep, [{"op": "name_ok", "page": 1}, {"op": "add_layer", "page": 1, "kind": "paint", "id": "paint"},
                   {"op": "put_raster", "page": 1, "id": "paint", "png_base64": base64.b64encode(buf.getvalue()).decode()},
                   {"op": "set_page_spec", "preset": "b4"}])
    new = ep.pages[0].inner_rect_mm()
    grey = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    assert grey.getpixel((mm_to_px(new.x + new.width / 2, 100), mm_to_px(new.y + new.height / 2, 100))) < 60


def test_nombre_and_checks_use_the_trim():
    from genko import checks, nombre

    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    page = ep.pages[0]
    t, frame = page.trim_rect_mm(), page.inner_rect_mm()
    [item] = nombre.placements(ep, page)
    assert frame.y + frame.height < item["y_mm"] < t.y + t.height and item["x_mm"] == pytest.approx(t.x + t.width / 2)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "外", "x_mm": t.x + t.width - 5, "y_mm": 60, "w_mm": 12, "h_mm": 20}])
    assert "text_outside_trim" in {i["code"] for i in checks.book(ep)["issues"]}


# --- the window ---------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")


def test_new_project_offers_the_papers_and_custom_numbers(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QDialog

    from genko.app.dialogs import NewProjectDialog, PaperDialog

    dialog = NewProjectDialog()
    keys = [dialog.paper.itemData(i) for i in range(dialog.paper.count())]
    assert keys[:4] == ["b4", "b5", "a5", "a4"] and keys[-1] == "custom"
    assert "基本枠 180×270" in dialog.paper_note.text()
    # custom numbers
    paper = PaperDialog(None, PageSpec.b4_comic())
    paper.trim_w.setValue(200)
    paper.inner.setValue(25)
    assert paper.spec().trim_size()[0] == 200 and paper.ok_button.isEnabled()
    paper.trim_w.setValue(400)  # bigger than the paper
    assert not paper.ok_button.isEnabled() and "小さく" in paper.summary.text()
    paper.preset.setCurrentIndex(paper.preset.findData("a5"))
    assert paper.spec() == PAPER_PRESETS["a5"][1]() and paper.op()["preset"] == "a5"

    def accept(self):
        self._preset_key = "b5"
        return QDialog.DialogCode.Accepted

    monkeypatch.setattr(PaperDialog, "exec", accept)
    dialog.paper.setCurrentIndex(dialog.paper.findData("custom"))
    assert dialog.chosen_spec() == PageSpec.b5_doujin()
    dialog.folder.setText(str(tmp_path))
    dialog.title.setText("同人誌")
    dialog.create()
    assert load_episode(dialog.created).spec == PageSpec.b5_doujin()


def test_changing_the_paper_from_the_window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QDialog, QMessageBox

    from genko.app.dialogs import PaperDialog
    from genko.app.main import MainWindow

    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: None)
    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, LEGACY_B4), project)
    win = MainWindow(project)
    win.resize(1280, 720)
    win.show()

    def accept(self):
        self.preset.setCurrentIndex(self.preset.findData("b4"))
        return QDialog.DialogCode.Accepted

    monkeypatch.setattr(PaperDialog, "exec", accept)
    win.act_paper.trigger()
    assert win.episode.spec == PageSpec.b4_comic()
    assert win.current_page().frames[0].rect.width == pytest.approx(180)
    assert "基本枠 180×270" in win.last_notice
    win.act_undo.trigger()
    assert win.episode.spec == LEGACY_B4
    win.close()
