"""Panels as CLIP STUDIO has them: a line stays in the panel it was begun in."""

from __future__ import annotations

from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import render_page


def _two_panels():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "split_frame", "page": 1, "axis": "vertical", "ratio": 0.5, "gutter_mm": 6}])
    left, right = sorted(episode.pages[0].leaf_frames(), key=lambda f: f.rect.x)
    return episode, left, right


def _ink_at(episode, x_mm: float, y_mm: float, dpi: int = 100) -> int:
    image = render_page(episode.pages[0], dpi, mode="proof", episode=episode).convert("L")
    return image.getpixel((round(x_mm / 25.4 * dpi), round(y_mm / 25.4 * dpi)))


def test_a_line_stays_in_the_panel_it_begins_in():
    episode, left, right = _two_panels()
    y = left.rect.y + left.rect.height / 2
    start = left.rect.x + left.rect.width / 2
    end = right.rect.x + right.rect.width / 2
    apply_ops(episode, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[start, y], [end, y]], "width_mm": 1.2}])
    inside = _ink_at(episode, start + 5, y)
    over = _ink_at(episode, right.rect.x + 10, y)
    assert inside < 100 and over > 200  # (cut at the left panel's edge)
    ink = next(layer for layer in episode.pages[0].layers if layer.role.value == "ink")
    apply_ops(episode, [{"op": "set_layer", "page": 1, "id": ink.id, "panel_each": False}])
    assert _ink_at(episode, right.rect.x + 10, y) < 100  # (all the panels together: it runs on)


def test_new_drawing_layers_cut_panel_by_panel_and_the_setting_is_saved(tmp_path):
    from genko.io import load_episode, save_episode

    episode, _left, _right = _two_panels()
    apply_ops(episode, [{"op": "add_layer", "page": 1, "kind": "pen", "id": "p1"},
                        {"op": "add_layer", "page": 1, "kind": "pen", "id": "p2", "panel_each": False}])
    layers = {layer.id: layer for layer in episode.pages[0].layers}
    assert layers["p1"].panel_each and not layers["p2"].panel_each
    save_episode(episode, tmp_path / "b.genko")
    again = {layer.id: layer for layer in load_episode(tmp_path / "b.genko").pages[0].layers}
    assert again["p1"].panel_each and not again["p2"].panel_each


def test_drawn_panels_take_the_basic_frames_place_and_keep_their_own():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    basic = episode.pages[0].leaf_frames()[0]
    apply_ops(episode, [{"op": "add_frame", "page": 1, "rect": [20, 20, 80, 60], "id": "a"},
                        {"op": "add_frame", "page": 1, "rect": [110, 20, 70, 60], "id": "b"},
                        {"op": "add_frame", "page": 1, "points": [[20, 90], [180, 90], [180, 170], [60, 200], [20, 200]], "id": "c"}])
    page = episode.pages[0]
    ids = [leaf.id for leaf in page.leaf_frames()]
    assert ids == ["b", "a", "c"] and basic.id not in ids  # (right to left, then down: the basic frame went)
    assert page._find("c").poly and page._find("c").custom
    apply_ops(episode, [{"op": "delete_frame", "page": 1, "frame_id": "a"}])
    page = episode.pages[0]
    assert [leaf.id for leaf in page.leaf_frames()] == ["b", "c"]
    assert page._find("b").rect.x == 110  # (the others stay where they are)


def test_a_freehand_outline_is_simplified_and_small_panels_are_refused():
    import math

    import pytest

    from genko.ops import ApplyError

    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ring = [[100 + 40 * math.cos(t / 100 * 2 * math.pi), 120 + 30 * math.sin(t / 100 * 2 * math.pi)] for t in range(100)]
    apply_ops(episode, [{"op": "add_frame", "page": 1, "points": ring, "id": "o"}])
    poly = episode.pages[0]._find("o").poly
    assert 8 <= len(poly) < 60
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "add_frame", "page": 1, "rect": [10, 10, 3, 3]}])


def test_a_split_panel_deleted_leaves_its_neighbour_in_place_and_the_last_one_stays():
    import pytest

    from genko.ops import ApplyError

    episode, left, right = _two_panels()
    apply_ops(episode, [{"op": "delete_frame", "page": 1, "frame_id": left.id}])
    page = episode.pages[0]
    assert [leaf.id for leaf in page.leaf_frames()] == [right.id]
    assert page._find(right.id).rect.x == right.rect.x
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "delete_frame", "page": 1, "frame_id": right.id}])


def test_drawn_panels_follow_a_new_paper_size():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_frame", "page": 1, "rect": [20, 20, 80, 60], "id": "a"}])
    from genko import frames as geo

    root = episode.pages[0].frames[0]
    before = episode.pages[0]._find("a").rect
    old = geo.Rect(root.rect.x, root.rect.y, root.rect.width, root.rect.height)
    geo.relayout(root, geo.corners(geo.Rect(0, 0, old.width * 2, old.height * 2)))
    after = episode.pages[0]._find("a").rect
    assert after.x == 2 * before.x and after.width == 2 * before.width


def test_round_corners_cut_the_picture_and_the_border():
    from genko import frames as geo

    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    frame = episode.pages[0].leaf_frames()[0]
    apply_ops(episode, [{"op": "set_frame", "page": 1, "frame_id": frame.id, "corner_mm": 12}])
    frame = episode.pages[0].leaf_frames()[0]
    r = frame.rect
    assert frame.corner_mm == 12 and len(geo.outline(frame)) > 12
    assert not geo.contains(frame, r.x + 1, r.y + 1) and geo.contains(frame, r.x + 13, r.y + 1)
    apply_ops(episode, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[r.x + 0.5, r.y + 30], [r.x + 20, r.y + 30]],
                         "width_mm": 1.0}, {"op": "set_layer", "page": 1, "layer": "ink", "panel_each": False}])
    image = render_page(episode.pages[0], 100, mode="proof", episode=episode).convert("L")
    corner = image.getpixel((round((r.x + 1.5) / 25.4 * 100), round((r.y + 1.5) / 25.4 * 100)))
    assert corner > 200  # (outside the rounded corner: neither border nor paper ink)


def test_own_layouts_are_kept_and_come_back_on_other_paper(tmp_path, monkeypatch):
    from genko.studio import layout

    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    episode = new_episode("t", 1, 2, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_frame", "page": 1, "rect": [20, 20, 80, 60], "id": "a"},
                        {"op": "add_frame", "page": 1, "points": [[110, 20], [180, 20], [170, 80], [110, 80]], "id": "b"},
                        {"op": "set_frame", "page": 1, "frame_id": "a", "corner_mm": 5, "line": {"kind": "dashed", "dash_mm": 4}}])
    layout.save_user_template("二つ", episode.pages[0])
    kept = layout.user_templates()
    assert [t["name"] for t in kept] == ["二つ"]
    other = new_episode("t", 1, 1, PageSpec.b4_comic())
    tree = layout.user_template_tree(kept[0], other.pages[0])
    apply_ops(other, [{"op": "set_layout", "page": 1, "tree": tree, "force": True}], agent="human:leaf")
    leaves = other.pages[0].leaf_frames()
    assert len(leaves) == 2 and other.pages[0].frames[0].split_axis == "free"
    rounded = next(f for f in leaves if f.corner_mm)
    assert rounded.line["dash_mm"] == 4 and rounded.rect.width > 80  # (B4 is wider than B5)
    # a page of drawn panels is cleared back to its basic frame for a built-in template
    layout.clear_page(episode, episode.pages[0], agent="human:leaf")
    assert len(episode.pages[0].leaf_frames()) == 1 and episode.pages[0].frames[0].split_axis is None
    layout.delete_user_template("二つ")
    assert layout.user_templates() == []
