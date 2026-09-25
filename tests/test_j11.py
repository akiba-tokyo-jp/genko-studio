"""J11: several books open at once (tabs), and one book in two windows that show each other's changes."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def _book(tmp_path: Path, name: str, title: str, pages: int = 3) -> Path:
    ep = new_episode(title, 1, pages, PageSpec.b5_doujin())
    for page in ep.pages:
        page.name_ok = True
    path = tmp_path / f"{name}.genko"
    save_episode(ep, path)
    return path


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    win = MainWindow(_book(tmp_path, "a", "一の巻"))
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    from genko.app import documents

    for other in documents.windows():
        other.commit_now()
        other.close()


def _stroke(page: int) -> dict:
    return {"op": "add_stroke", "page": page, "layer": "ink", "points": [[40, 40], [90, 120]]}


def test_several_books_in_tabs(window, qapp, tmp_path):
    b = _book(tmp_path, "b", "二の巻", pages=5)
    window.go_to_page(2)
    window.canvas.zoom_by(2.0)
    zoom = window.canvas.view_state()["scale"]
    window.open_project(b)
    assert window.doc_tabs.count() == 2 and window.doc_tabs.currentIndex() == 1
    assert window.episode.title == "二の巻" and len(window.episode.pages) == 5
    assert "二の巻" in window.doc_tabs.tabText(1) and "二の巻" in window.windowTitle()
    window.apply_ops([_stroke(1)])
    window._switch_document(0)  # (back to the first book: its page and zoom as they were; the second one written)
    assert window.episode.title == "一の巻" and window._current().index == 2
    assert window.canvas.view_state()["scale"] == pytest.approx(zoom)
    assert load_episode(b).pages[0].ink_strokes
    window.open_project(b)  # (open again: goes to its tab)
    assert window.doc_tabs.count() == 2 and window.episode.title == "二の巻"
    window.next_document()
    assert window.episode.title == "一の巻"
    window.act_next_doc.trigger()
    assert window.episode.title == "二の巻"
    window.close_document()  # (the tab closes; the window stays with the other book)
    assert window.doc_tabs.count() == 1 and window.episode.title == "一の巻" and window.isVisible()


def test_the_untitled_book_gives_its_place(qapp, tmp_path):
    from genko.app.main import MainWindow

    win = MainWindow()
    win.show()
    win.open_project(_book(tmp_path, "c", "三の巻"))
    assert win.doc_tabs.count() == 1 and win.episode.title == "三の巻"
    win.close()


def test_one_book_in_two_windows(window, qapp, tmp_path):
    other = window.new_window()
    for _ in range(3):
        qapp.processEvents()
    assert other.session is window.session and other.isVisible()
    other.go_to_page(1)
    window.go_to_page(1)
    other.canvas.fit_page()
    window.canvas.zoom_by(3.0)  # (one zoomed in to draw, the other shows the whole page)
    assert window.canvas.view_state()["scale"] != pytest.approx(other.canvas.view_state()["scale"])
    window.apply_ops([_stroke(1)])
    assert other.episode.pages[0].ink_strokes
    assert other.canvas.page is other.episode.pages[0]  # (its view shows the new stroke)
    other.apply_ops([_stroke(1)])
    assert len(window.canvas.page.ink_strokes) == 2
    window.apply_ops([{"op": "add_page", "count": 1}])
    assert other.pages.count() == 4
    window._undo()
    assert other.pages.count() == len(window.episode.pages)
    other.close()
    assert window.isVisible()
    window.apply_ops([_stroke(2)])  # (the closed window is no longer told)
    window.commit_now()
    assert load_episode(window.path).pages[1].ink_strokes


def test_the_same_book_opened_from_another_window_is_shared(window, qapp, tmp_path):
    from genko.app.main import MainWindow

    second = MainWindow(_book(tmp_path, "d", "四の巻"))
    second.show()
    second.open_project(window.path)
    assert second.session is window.session and second.doc_tabs.count() == 2
    second.apply_ops([_stroke(3)])
    assert window.episode.pages[2].ink_strokes
    second.close()


def test_window_menu_and_tabs_fit_a_small_screen(window, qapp):
    texts = [a.text() for a in window.view_menu.actions()]
    assert "新しいウィンドウ（同じ原稿）" in texts and "次の原稿" in texts
    window.resize(1024, 640)
    qapp.processEvents()
    assert window.doc_tabs.isVisible() and window.doc_tabs.height() < 60
