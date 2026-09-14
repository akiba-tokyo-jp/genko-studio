from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.render import mm_to_px, render_page


def test_oil_stroke_blends_into_existing_ink():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "name_ok", "page": 1},
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "ink",
                "kind": "oil",
                "rgb": [255, 0, 0],
                "width_mm": 4,
                "points": [[40, 80], [90, 80]],
            },
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "ink",
                "kind": "oil",
                "rgb": [0, 0, 255],
                "width_mm": 4,
                "points": [[60, 80], [110, 80]],
            },
        ],
    )
    img = render_page(ep.pages[0], 72, mode="print", episode=ep)
    px = img.getpixel((mm_to_px(75, 72), mm_to_px(80, 72)))
    assert px[0] > 40 and px[2] > 40
    assert px[0] < 220 and px[2] < 220
    assert max(px) < 240


def test_onion_skin_in_name_not_print():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "name_ok", "page": 1},
            {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 80], [120, 80]]},
            {"op": "set_onion", "page": 2, "from": 1},
        ],
    )
    name = render_page(ep.pages[1], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[1], 72, mode="print", episode=ep)
    x, y = mm_to_px(80, 72), mm_to_px(80, 72)
    assert name.getpixel((x, y)) != printed.getpixel((x, y))


def test_page_lock_blocks_other_agent():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "lock_page", "page": 1, "agent": "ai"}], agent="ai")
    try:
        apply_ops(ep, [{"op": "add_line", "page": 1, "text": "x"}], agent="human")
        raise AssertionError("expected ApplyError")
    except ApplyError as exc:
        assert "lock" in str(exc).lower() or "locked" in str(exc).lower()
    apply_ops(ep, [{"op": "add_line", "page": 2, "text": "ok"}], agent="human")
    assert ep.story_for_page(2)[0].text == "ok"


def test_font_path_roundtrip():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_meta", "font_path": "C:/fonts/user.ttf"}])
    assert ep.font_path.endswith("user.ttf")


def test_tone_angle_changes_dot_grid():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    fid = ep.pages[0].frames[0].id
    apply_ops(ep, [{"op": "add_tone", "page": 1, "frame_id": fid, "lpi": 30, "density": 0.7, "angle": 0}])
    apply_ops(ep, [{"op": "add_tone", "page": 2, "frame_id": ep.pages[1].frames[0].id, "lpi": 30, "density": 0.7, "angle": 45}])
    a = render_page(ep.pages[0], 72, mode="print", episode=ep)
    b = render_page(ep.pages[1], 72, mode="print", episode=ep)
    assert a.tobytes() != b.tobytes()


def test_pose_mannequin_stores_joints():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "pos": [100, 160, 0]}])
    mid = ep.pages[0].prims[0]["id"]
    apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": mid, "joints": {"l_arm": {"yaw": 0.8}}}])
    assert ep.pages[0].prims[0]["joints"]["l_arm"]["yaw"] == 0.8
    name = render_page(ep.pages[0], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[0], 72, mode="print", episode=ep)
    assert name.tobytes() != printed.tobytes()
