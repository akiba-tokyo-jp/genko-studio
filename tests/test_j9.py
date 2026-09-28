"""J9: covers (front, back, a jacket with spine and flaps), the book preview in spreads, one action on every
page, find and replace in every line, and who draws which page."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book(pages=4):
    return new_episode("t", 1, pages, PageSpec.b5_doujin())


def _ink(page):
    return next(layer for layer in page.layers if layer.role == LayerRole.INK)


def test_covers_have_their_own_paper_and_no_nombre(tmp_path):
    from genko import covers, nombre

    ep = _book()
    apply_ops(ep, [{"op": "add_cover", "kind": "jacket", "spine_mm": 8, "flap_mm": 60}, {"op": "add_cover", "kind": "back"}])
    jacket = ep.pages[4]
    assert covers.cover_of(jacket)["kind"] == "jacket" and not jacket.numero
    assert jacket.spec.trim_size()[0] == pytest.approx(2 * 182 + 8 + 2 * 60)
    assert [name for _x0, _x1, name in covers.folds(jacket)] == ["袖", "表紙", "背", "裏表紙", "袖"]
    apply_ops(ep, [{"op": "add_page"}])
    assert not covers.is_cover(ep.pages[4]) and nombre.number(ep, ep.pages[4]) == 5  # (new pages go before the covers)
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko")
    assert again.pages[5].spec.width_mm == pytest.approx(jacket.spec.width_mm)
    apply_ops(again, [{"op": "set_page_spec", "preset": "b4"}])
    assert again.pages[5].spec.trim_size()[0] == pytest.approx(2 * 220 + 8 + 120)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_cover", "kind": "back"}])
    with pytest.raises(ApplyError):
        apply_ops(_book(), [{"op": "add_cover", "kind": "jacket", "spine_mm": 0}])


def test_covers_come_first_and_last_in_exports(tmp_path):
    import zipfile

    from genko import covers
    from genko.export import export_epub, export_png_sequence

    ep = _book(2)
    apply_ops(ep, [{"op": "add_cover", "kind": "back"}, {"op": "add_cover", "kind": "front"}])
    order = [covers.file_stem(p) for p in covers.pages_in_order(ep)]
    assert order == ["cover_front", "p001", "p002", "cover_back"]
    names = sorted(path.name for path in export_png_sequence(ep, tmp_path / "png", 30))
    assert any("cover_front" in n for n in names) and any("cover_back" in n for n in names)
    book = export_epub(ep, tmp_path / "b.epub", 30)
    with zipfile.ZipFile(book) as archive:
        opf = next(archive.read(n).decode() for n in archive.namelist() if n.endswith(".opf"))
    assert opf.index("cover_front") < opf.index("p001") < opf.index("cover_back")


def test_find_and_replace_in_every_line():
    ep = _book(2)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "太郎は走った", "speaker": "太郎", "id": "a"},
                   {"op": "add_line", "page": 2, "text": "太郎！待って", "id": "b", "ruby_runs": [["太郎", "たろう"]]}])
    op = {"op": "replace_text", "find": "太郎", "replace": "次郎"}
    apply_ops(ep, [op])
    texts = {line.id: line for line in ep.story}
    assert texts["a"].text == "次郎は走った" and texts["b"].text == "次郎！待って"
    assert texts["a"].speaker == "太郎" and texts["b"].ruby_runs == []  # (speakers only when asked; stale ruby goes)
    apply_ops(ep, [{"op": "replace_text", "find": "次郎", "replace": "三郎", "speakers": True, "pages": [1]}])
    texts = {line.id: line for line in ep.story}
    assert texts["a"].text == "三郎は走った" and texts["b"].text == "次郎！待って"
    apply_ops(ep, [{"op": "replace_text", "find": r"(\w)郎", "replace": r"\1朗", "regex": True}])
    assert {line.id: line.text for line in ep.story}["b"] == "次朗！待って"
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "replace_text", "find": "いない", "replace": "x", "must_find": True}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "replace_text", "find": "(", "replace": "x", "regex": True}])


def test_the_same_ops_on_every_page():
    ep = _book(3)
    apply_ops(ep, [{"op": "add_cover", "kind": "front"}])
    apply_ops(ep, [{"op": "for_pages", "ops": [{"op": "add_layer", "kind": "paint", "id": "sky", "name": "空"}]}])
    body = [p for p in ep.pages if not (p.extra or {}).get("cover")]
    assert all(any(layer.id == "sky" for layer in p.layers) for p in body)
    assert not any(layer.id == "sky" for layer in ep.pages[-1].layers)  # (not the cover)
    apply_ops(ep, [{"op": "for_pages", "pages": [1, 3], "ops": [{"op": "set_layer", "id": "sky", "opacity": 0.5}]}])
    assert next(layer for layer in ep.pages[0].layers if layer.id == "sky").opacity == 0.5
    assert next(layer for layer in ep.pages[1].layers if layer.id == "sky").opacity != 0.5
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "for_pages", "ops": [{"op": "delete_page"}]}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "for_pages", "pages": [9], "ops": [{"op": "set_layer", "id": "sky", "visible": False}]}])


def test_for_pages_keeps_the_approval_rules():
    ep = _book(2)
    ep.strict_gates = True
    with pytest.raises(ApplyError):  # ink before the name is approved: refused page by page as usual
        apply_ops(ep, [{"op": "for_pages", "ops": [{"op": "add_stroke", "layer": "ink", "points": [[10, 10], [20, 20]]}]}],
                  agent="ai:hermes")


def test_who_draws_which_page():
    ep = _book(3)
    apply_ops(ep, [{"op": "set_assignee", "pages": [1, 2], "who": "さくら"}])
    assert ep.pages[0].extra["assignee"] == "さくら" and "assignee" not in ep.pages[2].extra
    apply_ops(ep, [{"op": "set_assignee", "pages": [1], "who": ""}])
    assert "assignee" not in ep.pages[0].extra


def test_spreads_open_as_a_book():
    from genko.app.bookview import book_pages, spreads

    ep = _book(4)
    apply_ops(ep, [{"op": "add_cover", "kind": "jacket", "spine_mm": 6, "flap_mm": 0}])
    pages = book_pages(ep)
    assert [kind for kind, _p in pages] == ["front", "page", "page", "page", "page", "back"]
    assert spreads(len(pages)) == [[0], [1, 2], [3, 4], [5]]


def test_agents():
    from genko.studio.service import AGENT_OPS

    assert {"add_cover", "replace_text", "for_pages", "set_assignee"} <= AGENT_OPS


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = _book(3)
    apply_ops(ep, [{"op": "add_line", "page": 2, "text": "こんにちは太郎", "id": "a"}, {"op": "add_cover", "kind": "front"}])
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


def test_the_book_preview_turns(window, qapp):
    from genko.app.bookview import BookPreview

    preview = BookPreview(window)
    preview.show()
    assert preview.caption().startswith("表紙")
    left, right = preview.sides(0)
    assert right is None and left == 0  # (a right-bound book: the cover alone, on the left)
    preview.flip(1)  # (the next spread)
    for _ in range(20):
        preview._step()
    assert preview.spread == 1 and "1 ページ" in preview.caption()
    preview.view.repaint()
    preview.close()


def test_replace_dialog_and_page_labels(window, qapp):
    from genko.app.bookview import ReplaceDialog

    dialog = ReplaceDialog(window)
    dialog.find.setText("太郎")
    dialog.replace.setText("花子")
    assert dialog.search() == 1 and dialog.results.count() == 1
    dialog.replace_all()
    assert next(line for line in window.episode.story if line.id == "a").text == "こんにちは花子"
    assert window._page_text(window.episode.pages[-1]).startswith("表紙")
    window._select_page(0)
    window.apply_ops([{"op": "set_assignee", "pages": [1], "who": "あお"}])
    assert "担当 あお" in window._page_text(window.episode.pages[0])
