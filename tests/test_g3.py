"""G3: menus people look for — レイヤー and 台詞, ruler and 3D together, recent books, close and quit,
select all and delete in 編集, and ウィンドウ for the panels."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
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
def window(qapp, tmp_path: Path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    from genko.app.main import MainWindow, remember_project

    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "add_line", "page": 1, "id": "a", "text": "やあ", "x_mm": 150, "y_mm": 40, "w_mm": 30, "h_mm": 30,
                    "wrap": "vertical"}])
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    remember_project(project)
    win = MainWindow(project)
    win.show()
    yield win
    win.close()


def _menu(win, title):
    return next(a.menu() for a in win.menuBar().actions() if a.text() == title)


def _texts(menu):
    return [a.text() for a in menu.actions() if a.text()]


def test_the_menus_people_look_for(window):
    titles = [a.text() for a in window.menuBar().actions()]
    assert titles == ["ファイル", "編集", "表示", "ツール", "レイヤー", "台詞", "トーン・効果線", "選択", "定規・3D", "コマ", "ページ",
                      "ウィンドウ", "ヘルプ"]
    file_menu = _texts(_menu(window, "ファイル"))
    assert "最近使った原稿" in file_menu and "閉じる" in file_menu and "Genko を終わる" in file_menu
    edit = _texts(_menu(window, "編集"))
    assert "すべて選択" in edit and "選択範囲を消す" in edit
    assert "マスク" in _texts(_menu(window, "レイヤー")) and "フキダシの形" in _texts(_menu(window, "台詞"))
    ruler3d = _texts(_menu(window, "定規・3D"))
    assert "直線定規" in ruler3d and "デッサン人形を置く" in ruler3d and "ポーズ" in ruler3d
    # recent books
    recent = window.recent_menu
    recent.aboutToShow.emit()
    assert any(a.text() == "b" for a in recent.actions())


def test_layer_menu_works(window):
    before = len(window.current_page().layers)
    window.act_layer_pen.trigger()
    assert len(window.current_page().layers) == before + 1
    window.act_layer_dup.trigger()
    assert len(window.current_page().layers) == before + 2
    window.act_layer_merge.trigger()
    assert len(window.current_page().layers) == before + 1
    mask_menu = window.layer_mask_menu
    assert "選択範囲からマスクを作る" in _texts(mask_menu)


def test_line_menu_works(window):
    window.act_line_delete.trigger()
    assert "台詞" in window.last_notice  # nothing chosen yet: says what to do
    window.canvas.selected_line_id = "a"
    window._set_selected_balloon("shout")
    assert window._line("a").balloon == "shout"
    window.act_line_wrap.trigger()
    assert window._line("a").wrap == "horizontal"
    window.act_balloon_pen.trigger()
    assert window.canvas.tool == "text" and window.canvas.balloon_pen and window.text_settings.draw_balloon.isChecked()
    window.act_line_delete.trigger()
    assert window._line("a") is None
