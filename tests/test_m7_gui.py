"""M7 GUI, offscreen: approval box, panel view, library, and the session under the window."""

import os
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
QtWidgets = pytest.importorskip("PySide6.QtWidgets")

sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode  # noqa: E402
from genko.studio import evaluate  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    try:
        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:  # no display libraries at all
        pytest.skip(f"Qt cannot start here: {exc}")
    return app


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _project(root: Path):
    import test_m5 as m5

    agent, project, human = m5._project(root)
    m5._sheets(agent, human)
    for frame in load_episode(project).pages[0].leaf_frames():
        m5._art(agent, project, 1, frame.id)
    agent.request_approval("demo.genko", "art", [1], "1 ページの作画を確認してください")
    agent.ask_human("demo.genko", "2 ページの構図が決まらない", page=2, item="gen_panel")
    return agent, project


def _window(qapp, project):
    from genko.app.main import MainWindow

    window = MainWindow(project)
    window.resize(1500, 950)
    window.show()
    qapp.processEvents()
    return window


def test_approval_box_approves_one_request_as_the_person(qapp, tmp_path: Path):
    agent, project = _project(tmp_path)
    window = _window(qapp, project)
    box = window.approvals
    assert [i.kind for i in box.items] == ["gate", "help"]
    assert "作画承認待ち" in " ".join(label.text() for label in window.process.labels)
    box.list.setCurrentRow(0)
    qapp.processEvents()
    assert box.preview._source is not None and not box.preview._source.isNull()  # looked at before deciding
    assert not box.choices.isVisible()
    box.approve()
    qapp.processEvents()
    episode = load_episode(project)
    assert episode.pages[0].art_ok  # written at once, the agent sees it
    assert [i.kind for i in box.items] == ["help"]
    audit = evaluate.audit(project)
    assert audit["ok"] and audit["changes_by_actor"].get("human:leaf")
    # the question gets a reply that reaches the agent
    box.list.setCurrentRow(0)
    box.reason.setText("2 コマ目は引きの絵にする")
    box.send_back()
    qapp.processEvents()
    items = agent.next("demo.genko", limit=30).data
    assert not [t for t in load_episode(project).tickets if t.get("kind") == "help" and t.get("status") == "open"]
    fixes = [t for t in load_episode(project).tickets if t.get("kind") == "fix" and t.get("status") == "open"]
    assert fixes and fixes[0]["text"] == "2 コマ目は引きの絵にする" and items is not None
    window.close()


def test_panel_view_instruction_regions_and_adoption_go_through_ops(qapp, tmp_path: Path):
    agent, project = _project(tmp_path)
    window = _window(qapp, project)
    page = window.current_page()
    frame = next(f for f in page.leaf_frames() if f.id != page.selected_frame_id)
    window._on_frame_selected(frame.id)
    qapp.processEvents()
    window.commit_now()
    from genko import journal

    # the selection went to disk as an op (not a direct edit of the model)
    assert any(op.get("op") == "select_frame" for op in journal.entries(project)[-1]["ops"])
    view = window.panel_view
    assert view.frame_id == frame.id and view.candidates.count() == 1
    for mode in range(view.mode.count()):
        view.mode.setCurrentIndex(mode)
        qapp.processEvents()
    view.instruction.setText("顔をもう少し大きく")
    view.send_instruction()
    view.region_kind.setCurrentIndex(view.region_kind.findData("keep"))
    view.image.drawn.emit(0.1, 0.1, 0.3, 0.2)
    window.commit_now()
    panel = load_episode(project).pages[0]._find(frame.id).panel
    assert panel["instruction"]["text"] == "顔をもう少し大きく" and "instruction" in panel["pinned"]
    assert panel["status"] == "fix_requested"
    keep = [r for r in panel["regions"] if r["kind"] == "keep"]
    assert keep and keep[0]["source"] == "user"
    kinds = {(i["kind"], i["target"].get("frame_id")) for i in agent.next("demo.genko", limit=30).data["items"]}
    assert ("fix_panel", frame.id) in kinds
    # adopting the candidate again from the view (a person's adoption)
    view.candidates.setCurrentRow(0)
    view.adopt()
    window.commit_now()
    assert load_episode(project).pages[0]._find(frame.id).panel["status"] == "adopted"
    window.close()


def test_the_window_picks_up_agent_commits_and_keeps_its_own_edits(qapp, tmp_path: Path):
    agent, project = _project(tmp_path)
    window = _window(qapp, project)
    window.apply_ops([{"op": "set_note", "page": 3, "note": "人間のメモ"}])
    assert window.session.dirty
    agent.record_review("demo.genko", 3, 0.7, "エージェントの点検")
    window._on_disk_change(str(project / "project.json"))  # what the file watcher calls
    qapp.processEvents()
    episode = load_episode(project)
    assert episode.pages[2].note == "人間のメモ" and episode.pages[2].plan["reviews"]["name"]["notes"] == "エージェントの点検"
    assert not window.session.dirty and window.session.base_revision == episode.revision
    window.close()


def test_dragging_a_balloon_leaves_the_model_alone_until_release(qapp, tmp_path: Path):
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent

    agent, project = _project(tmp_path)
    window = _window(qapp, project)
    canvas = window.canvas
    line = canvas.lines[0]
    before = (line.x_mm, line.y_mm)
    start = canvas._pt(line.x_mm + 1, line.y_mm + 1)

    def event(kind, pos, buttons):
        return QMouseEvent(kind, pos, canvas.mapToGlobal(pos), Qt.MouseButton.LeftButton, buttons, Qt.KeyboardModifier.NoModifier)

    canvas.mousePressEvent(event(QEvent.Type.MouseButtonPress, start, Qt.MouseButton.LeftButton))
    canvas.mouseMoveEvent(event(QEvent.Type.MouseMove, start + QPointF(40, 30), Qt.MouseButton.LeftButton))
    assert (line.x_mm, line.y_mm) == before  # only the preview moved
    canvas.mouseReleaseEvent(event(QEvent.Type.MouseButtonRelease, start + QPointF(40, 30), Qt.MouseButton.NoButton))
    window.commit_now()
    moved = next(ln for ln in load_episode(project).story if ln.id == line.id)
    assert moved.x_mm > before[0] and moved.y_mm > before[1]
    window.close()


def test_library_and_start_screen(qapp, tmp_path: Path):
    from genko.app.main import StartDialog, recent_projects, remember_project

    agent, project = _project(tmp_path)
    window = _window(qapp, project)
    window.library.refresh()
    assert window.library.list.count() == 2
    window.library.list.setCurrentRow(0)
    assert "tokens_en" in window.library.detail.toPlainText() and "顔" in window.library.detail.toPlainText()
    window.close()
    remember_project(project)
    assert recent_projects() == [project.resolve()]
    dialog = StartDialog()
    assert dialog.list.count() == 1
