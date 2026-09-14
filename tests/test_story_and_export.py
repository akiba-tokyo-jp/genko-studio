from pathlib import Path

from genko.export import export_plan, export_png_sequence
from genko.models import LayerRole, PageSpec, new_episode
from genko.pipeline import InkBlockedError, advance


def test_story_editor_binds_lines_to_page_and_optional_frame():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    ep.add_line(page_index=1, text="始めよう。", speaker="主人公")
    ep.add_line(page_index=2, text="待て。", speaker="相手", frame_id=ep.pages[1].frames[0].id)
    assert [line.text for line in ep.story_for_page(1)] == ["始めよう。"]
    assert ep.story_for_page(2)[0].frame_id == ep.pages[1].frames[0].id


def test_export_plan_skips_name_and_draft_layers():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    roles = export_plan(ep.pages[0])
    assert LayerRole.NAME not in roles
    assert LayerRole.DRAFT not in roles
    assert LayerRole.FRAMES in roles
    assert LayerRole.TEXT in roles


def test_ink_cannot_start_until_name_is_ok():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    try:
        advance(ep.pages[0], to="ink")
        raise AssertionError("expected InkBlockedError")
    except InkBlockedError:
        pass
    ep.pages[0].name_ok = True
    advance(ep.pages[0], to="ink")
    assert ep.pages[0].stage == "ink"


def test_png_export_writes_one_file_per_page_without_draft_pixels(tmp_path: Path):
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    ep.pages[0].name_ok = True
    ep.pages[0].paint(LayerRole.NAME, (0, 0, 0))
    ep.pages[0].paint(LayerRole.INK, (32, 32, 32))
    out = export_png_sequence(ep, tmp_path, working_dpi=72)
    assert len(out) == 2
    from PIL import Image

    img = Image.open(out[0])
    # NAME is black; export must not keep a black canvas from the name layer.
    extrema = img.convert("L").getextrema()
    assert extrema[0] > 10
