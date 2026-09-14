from pathlib import Path

from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode


def test_save_and_load_preserves_pages_frames_and_story(tmp_path: Path):
    ep = new_episode("試作", 1, 2, PageSpec.a4_mono())
    ep.add_line(1, "始めよう。", speaker="主人公")
    ep.pages[0].name_ok = True
    ep.pages[0].split_frame(ep.pages[0].frames[0].id, "horizontal", 0.5, 4)
    dest = tmp_path / "試作.genko"
    save_episode(ep, dest)
    loaded = load_episode(dest)
    assert loaded.title == "試作"
    assert len(loaded.pages) == 2
    assert loaded.pages[0].name_ok is True
    assert len(loaded.pages[0].leaf_frames()) == 2
    assert loaded.story_for_page(1)[0].text == "始めよう。"
