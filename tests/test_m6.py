"""M6: printable monochrome — tones, the mono finish, line art, balloons, effects, export, PSD."""

import io
import zipfile
from pathlib import Path

import pytest
from PIL import Image, ImageDraw, ImageOps

from genko import lineart, screentone
from genko.export import export_epub, export_print, safe_name
from genko.io import load_episode, save_episode
from genko.models import Binding, PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_frame, render_page, to_bitonal

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


@pytest.fixture(autouse=True)
def _config(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


# --- tones -------------------------------------------------------------------------------


@pytest.mark.parametrize("dpi", [150, 300, 600])
@pytest.mark.parametrize("density", [0.1, 0.2, 0.3, 0.5, 0.7, 0.9])
def test_am_dots_cover_the_requested_share(dpi: int, density: float):
    tone = screentone.dots(density, (600, 600), dpi, lpi=60, angle=45)
    assert set(tone.tobytes()) <= {0, 255}
    assert abs(screentone.coverage(tone) - density) <= 0.03


def test_dots_grow_round_then_turn_into_white_holes():
    light = screentone.dots(0.2, (200, 200), 600, 60, 45)
    dark = screentone.dots(0.8, (200, 200), 600, 60, 45)
    # a light tone is isolated black dots; a dark tone is black with isolated white holes
    assert light.getpixel((0, 0)) == 255 or light.getpixel((5, 5)) == 255
    assert screentone.coverage(ImageOps.invert(dark)) == pytest.approx(0.2, abs=0.03)
    assert screentone.effective_lpi(600, 60, 45) == pytest.approx(60, abs=2)


def test_fm_noise_and_tone_layers_print_the_right_share():
    assert abs(screentone.coverage(screentone.noise(0.3, (400, 400))) - 0.3) <= 0.03
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame = ep.pages[0].leaf_frames()[0]
    apply_ops(ep, [{"op": "add_tone", "page": 1, "frame_id": frame.id, "lpi": 60, "density": 0.3}])
    dpi = 300
    printed = render_page(ep.pages[0], dpi, mode="print", episode=ep)
    x0, y0 = mm_to_px(frame.rect.x + 20, dpi), mm_to_px(frame.rect.y + 20, dpi)
    area = printed.crop((x0, y0, x0 + 600, y0 + 600)).convert("L")
    assert abs(screentone.coverage(area) - 0.3) <= 0.03
    # proofs show a flat grey instead of dots
    proof = render_page(ep.pages[0], 100, mode="proof", episode=ep).convert("L")
    px = proof.getpixel((mm_to_px(frame.rect.x + 20, 100), mm_to_px(frame.rect.y + 20, 100)))
    assert 160 < px < 200


# --- the mono finish of placed art --------------------------------------------------------------


def _grey_art(size=(1200, 900)) -> bytes:
    image = Image.new("L", size, 255)
    draw = ImageDraw.Draw(image)
    w, h = size
    for y in range(h):  # a sky from light to mid grey
        draw.line((0, y, w, y), fill=int(230 - 110 * y / h))
    draw.rectangle((0, int(h * 0.75), w, h), fill=15)  # solid black ground
    draw.ellipse((w * 0.35, h * 0.2, w * 0.65, h * 0.6), fill=250, outline=0, width=8)  # a face with an outline
    buf = io.BytesIO()
    image.convert("RGB").save(buf, format="PNG")
    return buf.getvalue()


def _project_with_art(root: Path):
    import sys

    sys.path.insert(0, str(Path(__file__).parent))
    import test_m5 as m5

    agent, project, human = m5._project(root)
    frame = load_episode(project).pages[0].leaf_frames()[1]
    request = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data
    path = Path(request["inbox"]) / "a.png"
    path.write_bytes(_grey_art())
    cand = agent.import_images("demo.genko", request["request"]["id"], [{"file": f"studio/inbox/{request['request']['id']}/a.png",
                                                                         "origin": {"tool_id": "t"}}]).data["candidates"][0]
    assert agent.adopt("demo.genko", cand, page=1, frame_id=frame.id).ok
    return agent, project, human, frame


def test_printed_art_is_pure_black_and_white_with_tone_and_lines(tmp_path: Path):
    agent, project, human, frame = _project_with_art(tmp_path)
    episode = load_episode(project)
    page = episode.pages[0]
    printed = render_frame(page, frame.id, 300, mode="print", episode=episode).convert("L")
    art_only = printed.crop((20, 20, printed.width // 3, printed.height // 2))  # sky, away from balloons and borders
    assert set(art_only.tobytes()) <= {0, 255}
    cov = screentone.coverage(art_only)
    assert 0.05 < cov < 0.4  # the sky became dots, not solid black or white
    ground = printed.crop((20, int(printed.height * 0.9), printed.width - 20, printed.height - 20))
    assert screentone.coverage(ground) > 0.97  # beta stays solid
    proof = render_frame(page, frame.id, 150, mode="proof", episode=episode).convert("L")
    assert len(set(proof.crop((10, 10, proof.width // 3, proof.height // 2)).tobytes())) <= 6  # flat steps, no dots
    # the 1-bit TIFF of the page has no grey (by construction) and keeps the tone
    tiff = export_print(episode, tmp_path / "tiff", fmt="tiff", dpi=150)[0]
    with Image.open(tiff) as img:
        assert img.mode == "1"
    # a layer's own finish overrides the book's
    episode.studio.setdefault("style", {})["finish"] = {"steps": [0.5]}
    save_episode(episode, project)
    heavy = render_frame(load_episode(project).pages[0], frame.id, 300, mode="print", episode=load_episode(project)).convert("L")
    assert screentone.coverage(heavy.crop((20, 20, heavy.width // 3, heavy.height // 2))) > cov + 0.1


def test_colour_pages_are_not_finished(tmp_path: Path):
    agent, project, human, frame = _project_with_art(tmp_path)
    episode = load_episode(project)
    from dataclasses import replace

    for page in episode.pages:
        page.spec = replace(page.spec, expression="color")
    image = render_frame(episode.pages[0], frame.id, 150, mode="print", episode=episode).convert("L")
    assert len(set(image.crop((10, 10, image.width // 3, image.height // 2)).tobytes())) > 20


# --- line art -------------------------------------------------------------------------------------


def test_lineart_keeps_lines_and_solids_and_drops_gradients():
    art = Image.open(io.BytesIO(_grey_art((600, 450))))
    ink = lineart.extract(art)
    alpha = ink.split()[3]
    assert alpha.getpixel((300, 20)) == 0  # sky gradient: no ink
    assert alpha.getpixel((300, 440)) == 255  # solid black ground stays
    assert alpha.getpixel((int(600 * 0.35) + 2, int(450 * 0.4))) == 255  # the face outline
    assert alpha.getpixel((300, 180)) == 0  # inside the face: paper
    specks = Image.new("L", (100, 100), 255)
    ImageDraw.Draw(specks).point([(50, 50), (20, 70)], fill=0)
    assert lineart.extract(specks).getbbox() is None  # single-pixel dirt is dropped


def test_derive_lineart_candidate_and_ink_adoption(tmp_path: Path):
    agent, project, human, frame = _project_with_art(tmp_path)
    derived = agent.derive("demo.genko", 1, frame.id)
    assert derived.ok and derived.images and 0 < derived.data["ink_share"] < 0.5
    panel = load_episode(project).pages[0]._find(frame.id).panel
    cand = next(c for c in panel["candidates"] if c["id"] == derived.data["candidate"])
    assert cand["origin"]["kind"] == "genko" and cand["mode"] == "derive" and cand["parent"] == panel["adopted"]["art"]
    assert panel["attempts"]["images"] == 1  # Genko's own derivations do not use up the image budget
    again = agent.derive("demo.genko", 1, frame.id)
    assert again.data["candidate"] == derived.data["candidate"]  # deterministic
    assert agent.adopt("demo.genko", derived.data["candidate"], page=1, frame_id=frame.id, to="ink").ok
    page = load_episode(project).pages[0]
    roles = [(layer.source or {}).get("to") for layer in page.layers if layer.kind.value == "placed"]
    assert roles == ["art", "ink"]  # the lines sit over the toned art
    assert load_episode(project).pages[0]._find(frame.id).panel["adopted"] == {"art": panel["adopted"]["art"], "ink": derived.data["candidate"]}


# --- balloons, sfx, effects ----------------------------------------------------------------------


def test_ellipse_balloons_hold_their_text_and_tails_are_visible():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    from genko.studio.letter import measure

    w, h = measure(["来てくれたんだ", "やっぱり"], "speech")
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "やっぱり\n来てくれたんだ", "balloon": "speech", "wrap": "vertical",
                    "x_mm": 60, "y_mm": 40, "w_mm": w, "h_mm": h, "tail": [70, 120]}])
    dpi = 200
    image = render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("L")
    x0, y0, x1, y1 = (mm_to_px(v, dpi) for v in (60, 40, 60 + w, 40 + h))
    # everything dark in the upper half of the balloon's box sits within the ellipse (the text does not
    # poke out; the lower half also holds the root of the tail)
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    for y in range(y0, round(cy), 3):
        for x in range(x0, x1, 3):
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 > 1.08 and image.getpixel((x, y)) < 100:
                pytest.fail(f"ink outside the ellipse at {(x, y)}")
    # the tail's base is wide: a horizontal cut just below the balloon crosses at least a few mm of white inside black
    row = [image.getpixel((x, y1 + mm_to_px(3, dpi))) for x in range(x0, x1)]
    darks = [i for i, v in enumerate(row) if v < 100]
    assert darks and (max(darks) - min(darks)) >= mm_to_px(1.5, dpi)


def test_ruby_sits_beside_every_base():
    from genko.render import _font
    from genko.tategaki import compose

    font = _font(None, 40)
    with_ruby = compose("漢字の読み", font, 40, 400, ruby_runs=[["漢字", "かんじ"], ["読", "よ"]])
    plain = compose("漢字の読み", font, 40, 400)
    assert with_ruby.width == plain.width + 20
    ruby_band = with_ruby.crop((40, 0, 60, with_ruby.height)).split()[3]
    rows = [y for y in range(ruby_band.height) if any(ruby_band.getpixel((x, y)) for x in range(ruby_band.width))]
    assert min(rows) < 80 and max(rows) > 120  # かんじ beside 漢字 at the top, よ beside 読 further down
    assert not any(ruby_band.getpixel((x, y)) for x in range(20) for y in range(82, 118))  # nothing beside の


def test_sfx_has_no_balloon_and_effects_stay_in_their_panel():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    frame = ep.pages[0].leaf_frames()[0]
    apply_ops(ep, [{"op": "split_frame", "page": 1, "frame_id": frame.id, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 6}])
    top, bottom = ep.pages[0].leaf_frames()
    apply_ops(ep, [{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": top.id, "params": {}},
                   {"op": "add_line", "page": 1, "text": "ドン", "balloon": "sfx", "wrap": "vertical", "x_mm": 80, "y_mm": bottom.rect.y + 10,
                    "w_mm": 30, "h_mm": 40}])
    dpi = 100
    image = render_page(ep.pages[0], dpi, mode="print", episode=ep).convert("L")
    gutter = image.crop((mm_to_px(top.rect.x + 5, dpi), mm_to_px(top.rect.y + top.rect.height + 1, dpi),
                         mm_to_px(top.rect.x + top.rect.width - 5, dpi), mm_to_px(bottom.rect.y - 1, dpi)))
    assert gutter.getextrema()[0] > 200  # the focus lines stop at the panel border
    inner = image.crop((mm_to_px(top.rect.x + 2, dpi), mm_to_px(top.rect.y + 2, dpi),
                        mm_to_px(top.rect.x + top.rect.width - 2, dpi), mm_to_px(top.rect.y + top.rect.height - 2, dpi)))
    assert 0.05 < screentone.coverage(inner) < 0.6
    centre = image.getpixel((mm_to_px(top.rect.x + top.rect.width / 2, dpi), mm_to_px(top.rect.y + top.rect.height / 2, dpi)))
    assert centre > 200  # focus lines leave the centre clear
    sfx = image.crop((mm_to_px(80, dpi), mm_to_px(bottom.rect.y + 10, dpi), mm_to_px(110, dpi), mm_to_px(bottom.rect.y + 50, dpi)))
    assert screentone.coverage(sfx) > 0.08  # large glyphs, no balloon outline around them


def test_finish_keeps_balloons_off_reported_faces(tmp_path: Path):
    """Acceptance: with the test face boxes, balloons and faces do not overlap."""
    import sys

    sys.path.insert(0, str(Path(__file__).parent))
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    m5._sheets(agent, human)
    human._apply([{"op": "set_studio", "policy": {"pilot": False}}])
    for page in load_episode(project).pages:
        for frame in page.leaf_frames():
            m5._art(agent, project, page.index, frame.id)
        human.approve_art([page.index])
        assert agent.finish_page("demo.genko", page.index, commit=True).data["committed"]
    episode = load_episode(project)
    overlaps = 0

    def hits(box, face):
        x, y, w, h = box
        fx, fy, fw, fh = face
        return min(x + w, fx + fw) - max(x, fx) > 0 and min(y + h, fy + fh) - max(y, fy) > 0

    def room_without_faces(frame, w, h, faces):
        # could a balloon this size sit anywhere in the panel (3 mm margins) without touching a face?
        r = frame.rect
        y = r.y + 3
        while y + h <= r.y + r.height - 3:
            x = r.x + 3
            while x + w <= r.x + r.width - 3:
                if not any(hits((x, y, w, h), f) for f in faces):
                    return True
                x += 1
            y += 1
        return False

    for page in episode.pages:
        for frame in page.leaf_frames():
            faces = [r["rect_mm"] for r in (frame.panel or {}).get("regions", []) if r.get("kind") == "face"]
            for line in episode.story_for_page(page.index):
                if line.frame_id != frame.id:
                    continue
                box = (line.x_mm, line.y_mm, line.w_mm, line.h_mm)
                if any(hits(box, f) for f in faces) and room_without_faces(frame, line.w_mm, line.h_mm, faces):
                    overlaps += 1  # a face-free spot existed and the planner did not take it
    assert overlaps == 0


# --- export -------------------------------------------------------------------------------------------


def test_print_uses_the_spec_resolution_and_safe_file_names(tmp_path: Path):
    ep = new_episode('夏:の/約束?*"', 1, 1, PageSpec.a4_mono())
    from dataclasses import replace

    ep.spec = replace(ep.spec, dpi=120)
    for page in ep.pages:
        page.spec = replace(page.spec, dpi=120)
    [path] = export_print(ep, tmp_path, fmt="png")
    assert path.name == "夏_の_約束____ep01_p001.png"
    with Image.open(path) as img:
        assert img.width == mm_to_px(ep.pages[0].spec.width_mm, 120)
    assert safe_name("CON") == "_CON" and safe_name(" . ") == "genko"


def test_pack_defaults_to_the_spec_dpi(tmp_path: Path, monkeypatch):
    from genko import pack

    seen = []
    monkeypatch.setattr(pack, "export_print", lambda episode, dest, fmt, dpi, crop_marks: seen.append(dpi) or [])
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    pack.export_pack(ep, tmp_path)
    assert seen == [600, 600]  # the old 150 dpi cap is gone
    assert "600" in (tmp_path / "list.csv").read_text(encoding="utf-8")


def test_epub_is_fixed_layout_and_right_to_left(tmp_path: Path):
    ep = new_episode("夏<&>", 1, 3, PageSpec.a4_mono(), Binding.RIGHT)
    path = export_epub(ep, tmp_path / "book.epub", dpi=36)
    with zipfile.ZipFile(path) as zf:
        assert zf.namelist()[0] == "mimetype" and zf.read("mimetype") == b"application/epub+zip"
        opf = zf.read("OEBPS/content.opf").decode("utf-8")
        assert 'version="3.0"' in opf and 'page-progression-direction="rtl"' in opf
        assert "rendition:layout\">pre-paginated" in opf and "夏&lt;&amp;&gt;" in opf
        assert 'properties="page-spread-left"' in opf and "OEBPS/nav.xhtml" in zf.namelist()
        assert 'name="viewport"' in zf.read("OEBPS/p001.xhtml").decode("utf-8")
    assert not list(tmp_path.glob(".*epub*"))  # no temporary folder left behind


# --- PSD ------------------------------------------------------------------------------------------------


def test_psd_has_the_pages_real_layers_with_unicode_names(tmp_path: Path):
    psd_tools = pytest.importorskip("psd_tools")
    agent, project, human, frame = _project_with_art(tmp_path)
    episode = load_episode(project)
    apply_ops(episode, [{"op": "add_tone", "page": 1, "frame_id": frame.id, "lpi": 60, "density": 0.2},
                        {"op": "add_effect", "page": 1, "kind": "speed", "frame_id": frame.id, "params": {}}], agent="human:a")
    save_episode(episode, project)
    episode = load_episode(project)
    from genko.psd import export_psd_pages

    paths = export_psd_pages(episode, tmp_path / "psd", dpi=72)
    assert len(paths) == 4 and paths[0].name.endswith("_p001.psd")
    psd = psd_tools.PSDImage.open(paths[0])
    names = [layer.name for layer in psd]
    lines = [ln for ln in episode.story_for_page(1)]
    assert names[0] == "紙" and names[-1] == "ノンブル"
    assert sum(n.startswith("絵 ") for n in names) == 1
    assert {"トーン", "効果", "コマ枠"} <= set(names)
    assert sum(n.startswith("台詞 ") for n in names) == len(lines)
    assert len(names) == 1 + 1 + 3 + len(lines) + 1
    art = next(layer for layer in psd if layer.name.startswith("絵 "))
    x0, y0 = mm_to_px(frame.rect.x, 72), mm_to_px(frame.rect.y, 72)
    assert abs(art.bbox[0] - x0) <= 2 and abs(art.bbox[1] - y0) <= 2  # cropped to the panel, placed where it prints
    assert len(set(art.topil().convert("L").tobytes())) > 20  # the greyscale art, before the mono finish
    assert psd.size == (mm_to_px(episode.pages[0].spec.width_mm, 72), mm_to_px(episode.pages[0].spec.height_mm, 72))
    res = psd.image_resources.get_data(1005)
    assert res.horizontal / 65536 == 72  # 16.16 fixed point


def test_bitonal_tiff_of_a_finished_page_has_only_black_and_white(tmp_path: Path):
    agent, project, human, frame = _project_with_art(tmp_path)
    episode = load_episode(project)
    image = render_page(episode.pages[0], 150, mode="print", episode=episode)
    bitonal = to_bitonal(image)
    assert bitonal.mode == "1"
    # the art area was already black and white before thresholding: nothing grey was thrown away
    x0, y0 = mm_to_px(frame.rect.x + 3, 150), mm_to_px(frame.rect.y + 3, 150)
    patch = image.crop((x0, y0, x0 + 100, y0 + 100)).convert("L")
    assert set(patch.tobytes()) <= {0, 255}
