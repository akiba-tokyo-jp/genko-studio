from pathlib import Path

from PIL import Image

from genko.export import export_print, export_strip
from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_b4_and_publisher_presets():
    b4 = PageSpec.b4_comic()
    assert b4.width_mm == 257
    assert b4.height_mm == 364
    assert b4.bleed_mm == 5 and b4.trim_size() == (220, 310) and b4.frame_size() == (180, 270)
    shueisha = PageSpec.publisher("shueisha")
    assert shueisha.preset == "shueisha"
    unknown = PageSpec.publisher("mystery-house")
    assert unknown.preset == "none"
    assert unknown.width_mm == 257


def test_print_mode_draws_page_number():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    img = render_page(ep.pages[0], 72, mode="print", episode=ep, crop_marks=False)
    band = img.crop((img.width // 2 - 20, img.height - 40, img.width // 2 + 20, img.height)).convert("L")
    assert band.getextrema()[0] < 80


def test_export_bitonal_tiff_and_pdf(tmp_path: Path):
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "name_ok", "page": 1}, {"op": "flood_fill", "page": 1, "layer": "ink", "x_mm": 40, "y_mm": 40, "rgb": [0, 0, 0]}])
    tiffs = export_print(ep, tmp_path / "tiff", fmt="tiff", dpi=72)
    assert len(tiffs) == 2
    img = Image.open(tiffs[0])
    assert img.mode == "1"
    pdfs = export_print(ep, tmp_path / "pdf", fmt="pdf", dpi=72)
    assert pdfs[0].suffix == ".pdf"
    assert pdfs[0].stat().st_size > 100


def test_webtoon_strip_is_taller_than_one_page(tmp_path: Path):
    ep = new_episode("t", 1, 3, PageSpec.webtoon())
    path = export_strip(ep, tmp_path / "strip.png", dpi=36)
    img = Image.open(path)
    one = render_page(ep.pages[0], 36, mode="print", episode=ep)
    assert img.height >= one.height * 3 - 2


def test_autosave_flag_roundtrips(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_autosave", "enabled": True}])
    assert ep.autosave is True
    dest = tmp_path / "auto.genko"
    save_episode(ep, dest)
    loaded = load_episode(dest)
    assert loaded.autosave is True
