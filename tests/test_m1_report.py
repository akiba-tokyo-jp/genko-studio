"""The Hermes test report (M1, group A): results that said ok but were wrong, and the gaps it found."""

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
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.studio.service import HumanService, StudioService  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _book(pages=2):
    ep = new_episode("t", 1, pages, PageSpec.b5_doujin())
    for page in ep.pages:
        page.name_ok = True
    return ep


def _png(image: Image.Image) -> str:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


# --- 1. covers ----------------------------------------------------------------------------------------------------


def test_a_jacket_and_a_back_cover_both_reach_the_ebook(tmp_path):
    from genko.export import export_epub

    ep = _book(2)
    apply_ops(ep, [{"op": "add_cover", "kind": "jacket", "spine_mm": 8, "flap_mm": 60}, {"op": "add_cover", "kind": "back"}])
    book = export_epub(ep, tmp_path / "b.epub", 20)
    with zipfile.ZipFile(book) as archive:
        opf = archive.read("OEBPS/content.opf").decode()
    assert opf.index("cover_front") < opf.index("p001") < opf.index("p002") < opf.index("cover_back")
    spine = opf[opf.index("<spine"):]
    assert 'idref="page_cover_front" properties="rendition:page-spread-center"' in spine
    assert 'idref="page_cover_back" properties="rendition:page-spread-center"' in spine
    only_jacket = _book(1)
    apply_ops(only_jacket, [{"op": "add_cover", "kind": "jacket", "spine_mm": 8, "flap_mm": 0}])
    with zipfile.ZipFile(export_epub(only_jacket, tmp_path / "j.epub", 20)) as archive:
        opf = archive.read("OEBPS/content.opf").decode()
    assert "cover_front" in opf and "cover_back" in opf  # (the jacket's own back, cut out of it)


# --- 2. colour layers on a monochrome page ---------------------------------------------------------------------


def test_an_imported_colour_psd_prints_in_grey_on_a_monochrome_page(tmp_path):
    from genko.psd import write_psd
    from genko.render import render_page

    colour = Image.new("RGBA", (182, 257), (0, 0, 0, 0))
    colour.paste((220, 40, 40, 255), (40, 60, 140, 200))
    psd = write_psd(tmp_path / "c.psd", Image.new("RGB", (182, 257), "white"), [("赤", colour)])
    ep = _book(1)
    apply_ops(ep, [{"op": "import_psd", "page": 1, "path": str(psd), "fit": "paper"}])
    for mode in ("print", "proof"):
        image = render_page(ep.pages[0], 40, mode=mode, episode=ep).convert("RGB")
        r, g, b = image.getpixel((image.width // 2, image.height // 2))
        assert max(r, g, b) - min(r, g, b) < 12, (mode, (r, g, b))
    save_episode(ep, tmp_path / "b.genko")  # (the mark that it came from a painting app is kept)
    again = load_episode(tmp_path / "b.genko")
    assert any((layer.source or {}).get("kind") == "psd" for layer in again.pages[0].layers)
    colour_book = new_episode("t", 1, 1, PageSpec.b5_doujin())
    spec = PageSpec(**{**colour_book.spec.__dict__, "expression": "color"})
    colour_book.spec = spec
    for page in colour_book.pages:
        page.spec = spec
        page.name_ok = True
    apply_ops(colour_book, [{"op": "import_psd", "page": 1, "path": str(psd), "fit": "paper"}])
    image = render_page(colour_book.pages[0], 40, mode="print", episode=colour_book).convert("RGB")
    r, g, b = image.getpixel((image.width // 2, image.height // 2))
    assert r > 150 and g < 100  # (a colour book keeps its colours)


# --- 3. masks in the per-layer PNGs -------------------------------------------------------------------------------


def test_layer_pngs_keep_their_masks(tmp_path):
    from genko.export import export_layers

    ep = _book(1)
    solid = Image.new("RGBA", (400, 560), (20, 20, 20, 255))
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "m", "name": "隠す"},
                   {"op": "put_raster", "page": 1, "id": "m", "png_base64": _png(solid)},
                   {"op": "set_layer_mask", "page": 1, "id": "m", "fill": "hide"}])
    files = export_layers(ep, tmp_path / "L", dpi=30)
    masked = next(f for f in files if "隠す" in f.name)
    assert Image.open(masked).getchannel("A").getextrema()[1] == 0


# --- 4. filters reach shape fills ---------------------------------------------------------------------------------


def test_a_filter_reaches_a_shape_fill():
    ep = _book(1)
    apply_ops(ep, [{"op": "add_layer", "page": 1, "kind": "paint", "id": "s"},
                   {"op": "add_shape", "page": 1, "layer_id": "s", "shape": "rect", "box": [20, 30, 40, 40], "fill": True,
                    "fill_rgb": [30, 30, 30], "line": False}])
    layer = next(item for item in ep.pages[0].layers if item.id == "s")
    assert layer.patches or layer.strokes
    apply_ops(ep, [{"op": "filter_raster", "page": 1, "id": "s", "kind": "invert"}])
    layer = next(item for item in ep.pages[0].layers if item.id == "s")
    assert not layer.patches and layer.raster_png
    image = Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
    from genko.raster import WORKING_DPI

    x, y = round(40 / 25.4 * WORKING_DPI), round(50 / 25.4 * WORKING_DPI)
    r, g, b, a = image.getpixel((x, y))
    assert a > 0 and r > 200  # (the dark square turned light)


# --- 5. the check's list, and what ops found ----------------------------------------------------------------------


@pytest.fixture
def agent(tmp_path: Path):
    service = StudioService(tmp_path, "ai:hermes")
    assert service.create_project("demo.genko", "demo", 2).ok
    return service


def test_check_returns_what_it_found(agent):
    path = agent.project_path("demo.genko")
    ep = load_episode(path)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "はみだし", "x_mm": 500, "y_mm": 500, "id": "x"}])
    save_episode(ep, path)
    found = agent.check("demo.genko").to_dict()
    assert found["errors"] + found["warnings"] >= 1 and found["checks"]


def test_replace_text_says_how_many_and_where(agent):
    path = agent.project_path("demo.genko")
    ep = load_episode(path)
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "かさを持って", "id": "a"},
                   {"op": "set_assignee", "pages": [2], "who": "さくら"}])
    save_episode(ep, path)
    result = agent.apply_ops("demo.genko", [{"op": "replace_text", "find": "かさ", "replace": "傘"}], commit=True).to_dict()
    report = result["results"][0]
    assert report["replaced"] == 1 and report["where"][0]["line"] == "a" and report["where"][0]["page"] == 1
    status = agent.status("demo.genko").to_dict()
    assert status["pages"][1]["assignee"] == "さくら"
    snap = agent.inspect("demo.genko", "snapshot").to_dict()["snapshot"]
    assert snap["pages"][1]["assignee"] == "さくら"


def test_the_timeline_shows_in_the_snapshot(agent):
    agent.apply_ops("demo.genko", [{"op": "set_animation", "page": 1, "fps": 8, "frames": 6},
                                   {"op": "add_anim_folder", "page": 1, "id": "f"},
                                   {"op": "add_cel", "page": 1, "folder": "f", "id": "c"}], commit=True)
    snap = agent.inspect("demo.genko", "snapshot").to_dict()["snapshot"]
    anim = snap["pages"][0]["animation"]
    assert anim["fps"] == 8 and anim["frames"] == 6 and anim["tracks"][0]["cels"] == [[1, "c"]]


def test_audit_lists_the_approvals(agent):
    from genko.studio.evaluate import audit

    path = agent.project_path("demo.genko")
    HumanService(path, "human:leaf").approve_name([1])
    report = audit(path)
    assert report["entries"] and report["entries"][-1]["actor"] == "human:leaf"
    assert any("name" in str(c.get("what")) for c in report["entries"][-1]["changes"])


# --- 6. the face of a character sheet ---------------------------------------------------------------------------


def test_the_face_comes_from_where_the_agent_said(tmp_path):
    from genko.studio.service import face_crop

    sheet = Image.new("RGB", (300, 200), "white")
    sheet.paste((255, 0, 0), (10, 10, 60, 60))  # (the face close-up, top left)
    face = face_crop(sheet, [10 / 300, 10 / 200, 50 / 300, 50 / 200])
    assert face.size == (50, 50) and face.getpixel((25, 25)) == (255, 0, 0)
    from genko.studio import importer

    assert importer._face_box({"face_box01": [0.1, 0.1, 0.2, 0.3]}, 0) == [0.1, 0.1, 0.2, 0.3]
    with pytest.raises(importer.ImportError_):
        importer._face_box({"face_box01": [0.9, 0.1, 0.5, 0.3]}, 0)


# --- 7, 8. the words of an art request -------------------------------------------------------------------------


def test_the_page_role_goes_to_one_panel_and_english_carries_the_action(tmp_path):
    from test_m4_units import _approved

    agent, project = _approved(tmp_path)
    ep = load_episode(project)
    page = ep.pages[0]
    page.plan = {**(page.plan or {}), "turn_role": "hook"}
    save_episode(ep, project)
    frames = load_episode(project).pages[0].leaf_frames()
    hooks = []
    for frame in frames:
        request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data["request"]
        hooks.append("次のページへ引く最後のコマ" in request["prompt"]["ja"])
        if (frame.panel or {}).get("action"):
            assert "Action (in Japanese): " + frame.panel["action"] in request["prompt"]["en"]
    assert hooks == [False] * (len(frames) - 1) + [True]
    sheet = agent.generation_request("demo.genko", character_id=load_episode(project).bible.characters[0]["id"])
    assert "漫画のコマ" not in sheet.data["request"]["prompt"]["ja"] and "漫画の絵" in sheet.data["request"]["prompt"]["ja"]


# --- 9. import_psd paths ----------------------------------------------------------------------------------------


def test_import_psd_reads_from_the_book_and_stays_under_root(agent, tmp_path):
    from test_j10 import _sample_psd

    path = agent.project_path("demo.genko")
    _sample_psd(path / "studio" / "layers.psd")
    ep = load_episode(path)
    for page in ep.pages:
        page.name_ok = True
    save_episode(ep, path)
    ok = agent.apply_ops("demo.genko", [{"op": "import_psd", "page": 1, "path": "studio/layers.psd", "id": "p"}], commit=True)
    assert ok.ok, ok.to_dict()
    assert ok.data["results"][0]["layers"]
    outside = tmp_path.parent / "outside.psd"
    _sample_psd(outside)
    refused = agent.apply_ops("demo.genko", [{"op": "import_psd", "page": 1, "path": str(outside)}])
    assert not refused.ok and refused.issues[0].code == "path_outside_root"


# --- 10. Kindle's long edge ---------------------------------------------------------------------------------------


def test_kindle_defaults_to_its_own_long_edge(agent):
    result = agent.export("demo.genko", format="kindle").to_dict()
    with zipfile.ZipFile(result["files"][0]) as archive:
        images = [n for n in archive.namelist() if n.startswith("OEBPS/images/")]
        size = Image.open(io.BytesIO(archive.read(images[0]))).size
    assert max(size) == 2560


# --- 11. how much text fits ---------------------------------------------------------------------------------------


def test_an_overflowing_balloon_says_how_much_fits():
    from genko.studio.letter import fits

    words = fits((0, 0, 180, 76.8), "thought")
    assert "1 列" in words and "列まで" in words


# --- 12. the name preview ----------------------------------------------------------------------------------------


def test_the_name_preview_keeps_its_bands_in_their_panels_and_shows_the_blocking():
    from genko.studio.review import annotate

    ep = _book(1)
    page = ep.pages[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": page.frames[0].id, "axis": "vertical", "ratio": 0.5}])
    page = ep.pages[0]
    left, right = page.leaf_frames()[:2]
    left.panel = {"slot": "p1"}
    right.panel = {"slot": "p2"}
    plan = {"panels": [{"slot": "p2", "shot": "MS", "angle": "eye", "action": "とても長い説明" * 20,
                        "characters": [{"id": "hina", "pos": "center", "scale": 0.8}]}],
            "tiers": [{"cols": [{"slot": "p1"}, {"slot": "p2"}]}]}
    dpi = 40
    base = Image.new("RGB", (400, 560), "white")
    out = annotate(base, page, dpi, plan)
    assert out.getbbox()
    from genko.render import mm_to_px

    first = page.leaf_frames()[1]  # (p2, the left panel: its long band would run right, into p1)
    other = page.leaf_frames()[0]
    band_y = mm_to_px(first.rect.y + first.rect.height, dpi) - 2
    # (nothing of the first panel's band spills over the gutter into the next panel)
    spill = [out.getpixel((x, band_y)) for x in range(mm_to_px(other.rect.x, dpi) + 3, mm_to_px(other.rect.x + other.rect.width, dpi) - 3)]
    assert all(px == (255, 255, 255) for px in spill)
    # (the blocking: the person's head box is drawn in the first panel)
    head = [out.getpixel((x, y)) for x in range(mm_to_px(first.rect.x, dpi), mm_to_px(first.rect.x + first.rect.width, dpi))
            for y in range(mm_to_px(first.rect.y, dpi), mm_to_px(first.rect.y + first.rect.height, dpi) - 12)]
    assert (200, 80, 60) in head


def test_a_thoughts_bubbles_stay_in_its_panel():
    from genko import balloons
    from genko.models import StoryLine

    line = StoryLine(id="t", text="……", page_index=1, balloon="thought", x_mm=12, y_mm=150, w_mm=20, h_mm=40, frame_id="f")
    trail = balloons._thought_trail(line, {"f": (10, 20, 100, 175)})
    assert 12 <= trail[0] <= 108 and 22 <= trail[1] <= 193
