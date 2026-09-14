from genko.models import PageSpec, new_episode


def test_new_page_has_one_root_frame_matching_inner_rect():
    page = new_episode("t", 1, 1, PageSpec.a4_mono()).pages[0]
    assert len(page.frames) == 1
    root = page.frames[0]
    assert root.rect == page.inner_rect_mm()
    assert root.children == []


def test_split_root_horizontally_creates_two_stacked_frames_with_gutter():
    page = new_episode("t", 1, 1, PageSpec.a4_mono()).pages[0]
    top, bottom = page.split_frame(page.frames[0].id, axis="horizontal", ratio=0.4, gutter_mm=5)
    assert len(page.leaf_frames()) == 2
    assert top.rect.height + bottom.rect.height + 5 == page.inner_rect_mm().height
    assert top.rect.y < bottom.rect.y
    assert abs(top.rect.height / bottom.rect.height - 0.4 / 0.6) < 0.02


def test_split_vertically_creates_right_to_left_reading_order():
    page = new_episode("t", 1, 1, PageSpec.a4_mono()).pages[0]
    left, right = page.split_frame(page.frames[0].id, axis="vertical", ratio=0.5, gutter_mm=3)
    leaves = page.leaf_frames()
    assert leaves[0].id == right.id
    assert leaves[1].id == left.id
