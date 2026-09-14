from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


def test_apply_rolls_back_on_bad_op():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    try:
        apply_ops(
            ep,
            [
                {"op": "add_line", "page": 1, "text": "x"},
                {"op": "explode"},
            ],
        )
        raise AssertionError("expected ApplyError")
    except ApplyError:
        pass
    assert ep.story_for_page(1) == []


def test_dry_run_does_not_mutate():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    out = apply_ops(ep, [{"op": "add_page"}], dry_run=True)
    assert out["ok"] is True
    assert len(ep.pages) == 1
    assert out["snapshot"]["pages"][-1]["index"] == 2


def test_undo_restores_line():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "hello"}])
    assert ep.story_for_page(1)[0].text == "hello"
    apply_ops(ep, [{"op": "undo"}])
    assert ep.story_for_page(1) == []


def test_edit_and_delete_line():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "a", "speaker": "A"}])
    line_id = ep.story[0].id
    apply_ops(ep, [{"op": "edit_line", "id": line_id, "text": "b"}])
    assert ep.story[0].text == "b"
    apply_ops(ep, [{"op": "delete_line", "id": line_id}])
    assert ep.story == []


def test_delete_and_duplicate_page():
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "delete_page", "page": 2}])
    assert [page.index for page in ep.pages] == [1, 2]
    apply_ops(ep, [{"op": "duplicate_page", "page": 1}])
    assert len(ep.pages) == 3
    assert ep.pages[2].index == 3


def test_delete_stroke():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "name", "points": [[1, 1], [2, 2]]}])
    apply_ops(ep, [{"op": "delete_stroke", "page": 1, "layer": "name", "index": 0}])
    assert ep.pages[0].name_strokes == []
