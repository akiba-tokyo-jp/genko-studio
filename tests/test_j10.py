"""J10: PSD (and PSB) read in as layers, CMYK and colour profiles, every layer as its own file, Kindle, and the
timelapse of the work."""

import base64
import io
import os
import sys
import zipfile
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import LayerKind, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book(pages=2):
    ep = new_episode("t", 1, pages, PageSpec.b5_doujin())
    for page in ep.pages:
        page.name_ok = True
    return ep


def _sample_psd(path: Path, size=(182, 257)) -> Path:
    """A small PSD as a painting app writes it: a folder with two layers in it, a masked layer, a hidden one."""
    from genko.psd import write_psd

    w, h = size
    red = Image.new("RGBA", size, (0, 0, 0, 0))
    red.paste((220, 30, 30, 255), (10, 10, 60, 60))
    blue = Image.new("RGBA", size, (0, 0, 0, 0))
    blue.paste((30, 30, 220, 128), (40, 40, 120, 120))
    green = Image.new("RGBA", size, (30, 200, 30, 255))
    shown = Image.new("L", size, 0)
    shown.paste(255, (0, 0, w // 2, h))
    hidden = Image.new("RGBA", size, (0, 0, 0, 0))
    hidden.paste((0, 0, 0, 255), (0, 0, 5, 5))
    merged = Image.new("RGB", size, "white")
    return write_psd(path, merged, [
        ("緑", green, {"mask": shown}),
        ("人物", None, {"section": 3}),
        ("赤", red, {"opacity": 0.5, "blend": "multiply"}),
        ("青", blue, {"clip": True}),
        ("人物", None, {"section": 1}),
        ("消えてる", hidden, {"visible": False}),
    ], dpi=72)


def test_a_psd_reads_as_its_layers(tmp_path):
    from genko.psd import read_psd

    doc = read_psd(_sample_psd(tmp_path / "a.psd"))
    names = [(layer.name, layer.folder) for layer in doc.layers]
    assert names == [("緑", False), ("赤", False), ("青", False), ("人物", True), ("消えてる", False)]
    folder = names.index(("人物", True))
    red, blue = doc.layers[1], doc.layers[2]
    assert red.parent == folder and blue.parent == folder and doc.layers[0].parent is None
    assert red.opacity == pytest.approx(0.5, abs=0.01) and red.blend == "multiply" and blue.clip
    assert not doc.layers[4].visible
    assert red.image.getpixel((20, 20)) == (220, 30, 30, 255) and red.image.getpixel((100, 100))[3] == 0
    mask = doc.layers[0].mask
    assert mask.getpixel((10, 10)) == 255 and mask.getpixel((150, 10)) == 0
    assert doc.size == (182, 257)


def test_psd_files_from_other_apps(tmp_path):
    """psd-tools writes its channels packed (RLE), as Photoshop and CLIP STUDIO PAINT do."""
    psd_tools = pytest.importorskip("psd_tools")
    from psd_tools.api.layers import PixelLayer

    from genko.psd import read_psd

    doc = psd_tools.PSDImage.new("RGB", (80, 60))
    art = Image.new("RGBA", (80, 60), (0, 0, 0, 0))
    art.paste((10, 120, 250, 255), (5, 5, 40, 30))
    doc.append(PixelLayer.frompil(art, doc, "lines", top=0, left=0))
    doc.save(tmp_path / "b.psd")
    read = read_psd(tmp_path / "b.psd")
    assert [layer.name for layer in read.layers] == ["lines"]
    assert read.layers[0].image.getpixel((10, 10)) == (10, 120, 250, 255)
    assert read.merged is not None and read.merged.size == (80, 60)
    with pytest.raises(ValueError):
        read_psd(b"8BPS" + b"\x00" * 10)
    with pytest.raises(ValueError):
        read_psd(b"not a psd")


def test_import_psd_puts_the_layers_on_the_page(tmp_path):
    ep = _book()
    path = _sample_psd(tmp_path / "a.psd")
    op = {"op": "import_psd", "page": 1, "path": str(path), "id": "psd"}
    apply_ops(ep, [op])
    page = ep.pages[0]
    new = [layer for layer in page.layers if layer.id.startswith("psd-")]
    assert [layer.title for layer in new] == ["緑", "赤", "青", "人物", "消えてる"]
    folder = next(layer for layer in new if layer.kind == LayerKind.FOLDER)
    red = next(layer for layer in new if layer.title == "赤")
    assert red.parent_id == folder.id and red.blend == "multiply" and red.opacity == pytest.approx(0.5, abs=0.01)
    assert next(layer for layer in new if layer.title == "青").clip
    assert not next(layer for layer in new if layer.title == "消えてる").visible
    assert next(layer for layer in new if layer.title == "緑").mask is not None
    raster = Image.open(io.BytesIO(red.raster_png))
    ratio = raster.width / raster.height
    assert ratio == pytest.approx(page.spec.width_mm / page.spec.height_mm, rel=0.01)  # (a whole page layer)
    # (it lands in the bleed box: the corner of the paper outside it is clear)
    assert raster.getpixel((0, 0))[3] == 0
    save_episode(ep, tmp_path / "b.genko")
    again = load_episode(tmp_path / "b.genko")
    assert any(layer.title == "赤" and layer.raster_png for layer in again.pages[0].layers)
    from genko.render import render_page

    assert render_page(again.pages[0], 40, mode="print", episode=again).size[0] > 0
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_psd", "page": 1, "psd": base64.b64encode(b"garbage").decode()}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_psd", "page": 1}])


def test_import_psd_respects_the_name_gate(tmp_path):
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ep.strict_gates = True
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "import_psd", "page": 1, "path": str(_sample_psd(tmp_path / "a.psd"))}], agent="ai:hermes")


def test_genko_psd_goes_out_and_comes_back(tmp_path):
    from genko.psd import export_page_psd, read_psd

    ep = _book(1)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "sky", "name": "空", "blend": "screen"}])
    sky = next(layer for layer in ep.pages[0].layers if layer.id == "sky")
    picture = Image.new("RGBA", (200, 280), (0, 0, 0, 0))
    picture.paste((90, 160, 240, 255), (20, 20, 120, 100))
    buf = io.BytesIO()
    picture.save(buf, format="PNG")
    apply_ops(ep, [{"op": "put_raster", "page": 1, "id": "sky", "png_base64": base64.b64encode(buf.getvalue()).decode()},
                   {"op": "set_layer", "page": 1, "id": "sky", "opacity": 0.6},
                   {"op": "set_layer_mask", "page": 1, "id": "sky", "fill": "show"}])
    out = export_page_psd(ep, ep.pages[0], tmp_path / "p.psd", dpi=60)
    doc = read_psd(out)
    back = next(layer for layer in doc.layers if layer.name == "空")
    assert back.blend == "screen" and back.opacity == pytest.approx(0.6, abs=0.01) and back.mask is not None
    assert sky.blend == "screen"


def test_cmyk_black_prints_on_one_plate_and_ink_stays_under_the_limit():
    from genko import colour

    picture = Image.new("RGB", (4, 1))
    for x, rgb in enumerate([(0, 0, 0), (128, 128, 128), (200, 20, 20), (10, 0, 30)]):
        picture.putpixel((x, 0), rgb)
    cmyk = colour.to_cmyk(picture)
    assert cmyk.mode == "CMYK"
    assert cmyk.getpixel((0, 0)) == (0, 0, 0, 255)  # (black lines: K only)
    assert cmyk.getpixel((1, 0))[:3] == (0, 0, 0)  # (greys too)
    assert colour.ink_coverage(cmyk) <= colour.INK_LIMIT + 1
    proofed = colour.proof(Image.new("RGBA", (2, 2), (0, 255, 0, 128)))
    assert proofed.mode == "RGBA" and proofed.getpixel((0, 0))[3] == 128
    assert len(colour.srgb_icc()) > 100


def test_cmyk_and_profiles_in_the_print_export(tmp_path):
    from genko import colour
    from genko.export import export_print

    ep = _book(1)
    files = export_print(ep, tmp_path / "c", fmt="cmyk", dpi=40, color="cmyk", area="trim")
    with Image.open(files[0]) as tiff:
        assert tiff.mode == "CMYK" and round(tiff.info["dpi"][0]) == 40
    pdf = export_print(ep, tmp_path / "d", fmt="pdf", dpi=40, color="cmyk")[0]
    assert b"/DeviceCMYK" in pdf.read_bytes()
    png = export_print(ep, tmp_path / "e", fmt="png", dpi=40)[0]
    with Image.open(png) as image:
        assert image.info.get("icc_profile") == colour.srgb_icc()
    gray = export_print(ep, tmp_path / "f", fmt="tiff", dpi=40, color="gray")[0]
    with Image.open(gray) as image:
        assert image.mode == "L"
    with pytest.raises(ValueError):
        export_print(ep, tmp_path / "g", fmt="png", dpi=40, color="cmyk")
    srgb = tmp_path / "srgb.icc"
    srgb.write_bytes(colour.srgb_icc())
    with pytest.raises(ValueError):  # (an RGB profile is not the printer's)
        export_print(ep, tmp_path / "h", fmt="cmyk", dpi=40, color="cmyk", icc=str(srgb))


def test_every_layer_as_its_own_file(tmp_path):
    from genko.export import export_layers
    from genko.psd import page_layers

    ep = _book(2)
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 40], [100, 120]]}])
    files = export_layers(ep, tmp_path / "L", dpi=40)
    first = [f for f in files if "p001" in f.parent.name]
    assert len(first) == len(page_layers(ep.pages[0], ep, 40))
    assert any("ペン入れ" in f.name for f in first) and first[0].name.startswith("01_")
    with Image.open(next(f for f in first if "ペン入れ" in f.name)) as ink:
        assert ink.mode == "RGBA" and ink.getpixel((0, 0))[3] == 0


def test_kindle_book(tmp_path):
    from genko.export import export_kindle

    ep = _book(3)
    apply_ops(ep, [{"op": "add_cover", "kind": "front"}])
    book = export_kindle(ep, tmp_path / "k.epub", long_edge=600)
    with zipfile.ZipFile(book) as archive:
        opf = archive.read("OEBPS/content.opf").decode()
        images = [n for n in archive.namelist() if n.startswith("OEBPS/images/")]
        sizes = {Image.open(io.BytesIO(archive.read(n))).size for n in images}
        modes = {Image.open(io.BytesIO(archive.read(n))).mode for n in images}
        assert archive.namelist()[0] == "mimetype"
    assert all(n.endswith(".jpg") for n in images) and len(images) == 4
    assert len(sizes) == 1 and max(next(iter(sizes))) == 600
    assert modes == {"L"}  # (a monochrome book goes in grey)
    for meta in ('name="fixed-layout" content="true"', 'name="book-type" content="comic"',
                 'name="primary-writing-mode" content="horizontal-rl"', 'name="original-resolution"', 'name="cover"'):
        assert meta in opf
    assert 'page-progression-direction="rtl"' in opf and opf.index("cover_front") < opf.index("p001")


def test_the_formats_people_pick(tmp_path):
    from genko.app import exporting

    ep = _book(1)
    for key in ("cmyk", "layers", "kindle"):
        result = exporting.run(ep, None, key, tmp_path / key, dpi=40, long_edge=500)
        assert result["ok"], result
        assert result["files"]
    assert "icc" in exporting.BY_KEY["pdf"].options and "color" in exporting.BY_KEY["pdf"].options


def test_the_timelapse_records_each_save(tmp_path):
    from genko import timelapse

    project = tmp_path / "t.genko"
    ep = _book(2)
    save_episode(ep, project)
    assert timelapse.frames(project) == []  # (off until asked)
    apply_ops(ep, [{"op": "set_timelapse", "on": True}])
    save_episode(ep, project)
    assert [f["page"] for f in timelapse.frames(project)] == [1, 2]  # (every page when it starts)
    apply_ops(ep, [{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 30], [90, 90]]}])
    save_episode(ep, project)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "やあ", "id": "a", "x_mm": 60, "y_mm": 60}])
    save_episode(ep, project)
    assert [f["page"] for f in timelapse.frames(project)] == [1, 2, 2, 1]
    again = load_episode(project)
    assert timelapse.is_on(again)
    movie = timelapse.export(project, tmp_path / "out.webp", fps=4)
    with Image.open(movie) as image:
        assert image.n_frames == 4
        image.seek(3)
        image.load()
        assert image.info["duration"] == 250 + 2000  # (the finished picture is held for two seconds)
    one = timelapse.export(project, tmp_path / "p2.gif", page=2, fps=10, hold=0)
    with Image.open(one) as image:
        assert image.n_frames == 2
    short = timelapse.export(project, tmp_path / "s.png", fps=2, seconds=1, hold=0)
    with Image.open(short) as image:
        assert image.n_frames == 2
    apply_ops(ep, [{"op": "set_timelapse", "on": False}])
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 30], [90, 90]]}])
    save_episode(ep, project)
    assert len(timelapse.frames(project)) == 4
    if timelapse.ffmpeg() is None:
        with pytest.raises(ValueError):
            timelapse.export(project, tmp_path / "m.mp4")
    with pytest.raises(ValueError):
        timelapse.export(tmp_path / "none.genko", tmp_path / "x.webp")


def test_agents(tmp_path):
    from genko.studio.service import AGENT_OPS

    assert {"import_psd", "set_timelapse"} <= AGENT_OPS


def test_cli_formats(tmp_path):
    from genko.__main__ import main

    ep = _book(1)
    save_episode(ep, tmp_path / "c.genko")
    assert main(["export", str(tmp_path / "c.genko"), str(tmp_path / "k"), "--format", "kindle", "--long-edge", "500"]) == 0
    assert main(["export", str(tmp_path / "c.genko"), str(tmp_path / "c"), "--format", "cmyk", "--dpi", "40"]) == 0
    assert list((tmp_path / "c").glob("*.tiff"))


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    ep = _book(2)
    project = tmp_path / "w.genko"
    save_episode(ep, project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_psd_import_proof_and_timelapse_in_the_app(window, qapp, tmp_path):
    from genko import timelapse
    from genko.app.dialogs import ExportDialog, TimelapseDialog

    window._import_psd(str(_sample_psd(tmp_path / "a.psd")))
    assert any(layer.title == "赤" for layer in window.episode.pages[0].layers)
    window.act_cmyk_proof.trigger()
    assert window._cmyk_proof
    assert window._render_current(40) is not None
    window.act_cmyk_proof.trigger()
    window.act_timelapse.trigger()
    assert timelapse.is_on(window.episode)
    window.commit_now()
    assert timelapse.frames(window.path)
    window._refresh_status()
    assert window.act_timelapse.isChecked()
    dialog = TimelapseDialog(window, window.path, 1)
    assert "記録したコマ" in dialog.count.text()
    written = dialog.write(tmp_path / "t.webp")
    assert written.is_file()
    export = ExportDialog(window, window.episode, window.path, "human:leaf", current_page=1)
    export.format.setCurrentIndex(export.format.findData("cmyk"))
    assert not export.icc_row.isHidden() and export.rows["dpi"].isVisibleTo(export)
    export.format.setCurrentIndex(export.format.findData("kindle"))
    assert export.long_edge.value() == 2560
