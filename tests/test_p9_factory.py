import io

from PIL import Image

from genko.export import export_print
from genko.lt import to_line_art
from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.psd import export_psd
from genko.render import mm_to_px
from genko.stroke import pack_point


def test_set_lt_threshold_changes_binarize():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_lt", "page": 1, "threshold": 40}])
    assert ep.pages[0].lt_threshold == 40
    gray = Image.new("L", (8, 8), 80)
    low = to_line_art(gray, method="adaptive", threshold=40)
    high = to_line_art(gray, method="adaptive", threshold=200)
    assert low.getpixel((0, 0)) != high.getpixel((0, 0))


def test_step_onion_walks_previous_page():
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "step_onion", "page": 3, "delta": -1}])
    assert ep.pages[2].onion_from == 2
    apply_ops(ep, [{"op": "step_onion", "page": 3, "delta": -1}])
    assert ep.pages[2].onion_from == 1


def test_mannequin_grows_wrist_and_ankle_joints():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "pos": [100, 160, 0]}])
    joints = ep.pages[0].prims[0]["joints"]
    assert "l_wrist" in joints and "r_ankle" in joints
    mid = ep.pages[0].prims[0]["id"]
    apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": mid, "joints": {"l_wrist": {"yaw": 0.5}}}])
    assert ep.pages[0].prims[0]["joints"]["l_wrist"]["yaw"] == 0.5


def test_export_uses_spec_dpi_for_b4(tmp_path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    paths = export_print(ep, tmp_path, fmt="png", dpi=ep.spec.dpi, crop_marks=False)
    img = Image.open(paths[0])
    assert img.width == mm_to_px(ep.spec.width_mm, 600)


def test_psd_named_raster_has_nonzero_channel(tmp_path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    buf = io.BytesIO()
    Image.new("RGB", (16, 16), (255, 0, 0)).save(buf, format="PNG")
    apply_ops(ep, [{"op": "add_layer", "page": 1, "name": "RedInk"}])
    extra = ep.pages[0].layers[-1]
    apply_ops(
        ep,
        [{"op": "put_raster", "page": 1, "id": extra.id, "png_base64": __import__("base64").b64encode(buf.getvalue()).decode()}],
    )
    path = export_psd(ep, tmp_path / "r.psd", dpi=36)
    data = path.read_bytes()
    assert b"RedInk" in data
    assert data.count(b"\xff") > 50


def test_pack_point_tilt_widens_pressure():
    flat = pack_point(1, 2, 0.5, tilt=0)
    tilted = pack_point(1, 2, 0.5, tilt=1.0)
    assert tilted[2] > flat[2]
