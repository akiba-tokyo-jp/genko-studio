import json
from pathlib import Path
from zipfile import ZipFile

from genko.export import export_epub, export_psd
from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_pressure_varies_stroke_width():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "name_ok", "page": 1},
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "ink",
                "points": [[20, 40, 0.2], [80, 40, 1.0], [140, 40, 0.2]],
            },
        ],
    )
    assert len(ep.pages[0].ink_strokes[0][0]) == 3
    img = render_page(ep.pages[0], 150, mode="print", episode=ep)
    thin = img.getpixel((mm_to_px(20, 150), mm_to_px(40, 150)))
    thick = img.getpixel((mm_to_px(80, 150), mm_to_px(40, 150)))
    assert thick[0] <= thin[0]


def test_simplify_and_edit_stroke():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    pts = [[10, 10], [10.1, 10.1], [10.2, 10.0], [40, 40]]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "name", "points": pts}])
    apply_ops(ep, [{"op": "simplify_stroke", "page": 1, "layer": "name", "index": 0, "epsilon_mm": 1.0}])
    assert len(ep.pages[0].name_strokes[0]) <= 3
    apply_ops(
        ep,
        [{"op": "edit_stroke", "page": 1, "layer": "name", "index": 0, "points": [[1, 1], [2, 2]]}],
    )
    assert ep.pages[0].name_strokes[0][0][0] == 1.0


def test_perspective_ruler_and_prim3d():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "set_ruler", "page": 1, "kind": "perspective", "points": [[40, 20], [170, 20]]},
            {"op": "add_prim3d", "page": 1, "kind": "box", "pos": [100, 150, 0], "size": [40, 50, 30], "rot": [0.2, 0.5, 0]},
        ],
    )
    assert ep.pages[0].ruler["kind"] == "perspective"
    assert ep.pages[0].prims[0]["kind"] == "box"
    name = render_page(ep.pages[0], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[0], 72, mode="print", episode=ep)
    assert name.tobytes() != printed.tobytes()


def test_lt_convert_puts_strokes_on_ink(tmp_path: Path):
    from PIL import Image

    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    img = Image.new("RGB", (64, 64), (255, 255, 255))
    for x in range(64):
        img.putpixel((x, 32), (0, 0, 0))
    png = tmp_path / "edge.png"
    img.save(png)
    apply_ops(
        ep,
        [
            {"op": "put_raster", "page": 1, "layer": "bg", "path": str(png)},
            {"op": "name_ok", "page": 1},
            {"op": "lt_convert", "page": 1, "layer": "bg", "to": "ink"},
        ],
    )
    assert ep.pages[0].ink_strokes


def test_tickets():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [{"op": "add_ticket", "page": 1, "role": "bg", "assignee": "ai", "rate": "page"}],
    )
    ticket_id = ep.tickets[0]["id"]
    apply_ops(ep, [{"op": "set_ticket", "id": ticket_id, "status": "done"}])
    assert ep.tickets[0]["status"] == "done"


def test_psd_and_epub(tmp_path: Path):
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "今だ", "speaker": "A", "x_mm": 40, "y_mm": 40}])
    psd = export_psd(ep, tmp_path / "out.psd", dpi=36)
    assert psd.read_bytes()[:4] == b"8BPS"
    epub = export_epub(ep, tmp_path / "out.epub", dpi=36)
    with ZipFile(epub) as zf:
        names = zf.namelist()
        assert "mimetype" in names
        assert any(name.endswith(".xhtml") for name in names)


def test_openapi_and_job_id():
    from genko.server import JOBS, handle_request, openapi_spec

    spec = openapi_spec()
    assert spec["openapi"].startswith("3.")
    assert "/v1/apply" in spec["paths"]
    status, body = handle_request("GET", "/openapi.json", b"")
    assert status == 200
    data = json.loads(body)
    assert data["openapi"].startswith("3.")
    assert JOBS == {} or isinstance(JOBS, dict)
