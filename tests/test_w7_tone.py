"""Tones as CLIP STUDIO has them: the dots' shape, the screen moved, and greys toned at export."""

from __future__ import annotations

import numpy as np
import pytest
from PIL import Image

from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.render import render_page, to_bitonal


def _tone_page(**extra):
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "add_tone", "page": 1, "id": "t1", **{"density": 0.3, "lpi": 30, **extra}}])
    return episode


def _print(episode, dpi=200) -> np.ndarray:
    return np.asarray(render_page(episode.pages[0], dpi, mode="print", episode=episode, finish=True).convert("L"))


def test_dot_shapes_differ_and_keep_the_black_share():
    from genko.screentone import threshold_tile

    tiles = {shape: np.asarray(threshold_tile(8, 8, shape)) for shape in ("round", "square", "diamond", "ellipse")}
    for tile in tiles.values():
        share = (tile < round(0.3 * 256)).mean()
        assert abs(share - 0.3) < 0.02
    for shape in ("square", "diamond", "ellipse"):  # (on a small cell some levels coincide; across them all they differ)
        assert any(((tiles[shape] < v) != (tiles["round"] < v)).any() for v in range(20, 240, 10))


def test_a_tone_takes_a_dot_shape_and_moves_its_screen():
    round_ = _print(_tone_page(density=0.5, lpi=15))
    square = _print(_tone_page(dot_shape="square", density=0.5, lpi=15))
    assert not np.array_equal(round_, square)
    round_ = _print(_tone_page())
    episode = _tone_page()
    apply_ops(episode, [{"op": "set_tone", "page": 1, "id": "t1", "offset_mm": [0.5, 0]}])
    moved = _print(episode)
    assert not np.array_equal(round_, moved)
    shift = round(0.5 / 25.4 * 200)
    frame = episode.pages[0].leaf_frames()[0].rect
    y = round((frame.y + frame.height / 2) / 25.4 * 200)
    x0 = round((frame.x + 20) / 25.4 * 200)
    # the same dots, `shift` pixels to the right (inside the panel, away from its edges)
    assert np.array_equal(round_[y:y + 40, x0:x0 + 200], moved[y:y + 40, x0 + shift:x0 + shift + 200])
    apply_ops(episode, [{"op": "set_tone", "page": 1, "id": "t1", "move_by_mm": [-0.5, 0]}])
    layer = next(item for item in episode.pages[0].layers if item.id == "t1")
    assert "offset_mm" not in layer.tone  # (moved back: no offset kept)
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "set_tone", "page": 1, "id": "t1", "dot_shape": "star"}])


def test_layer_toning_takes_a_shape_and_an_offset():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(episode, [{"op": "set_layer", "page": 1, "layer": "ink", "screen": {"lpi": 40, "shape": "diamond", "offset_mm": [1, 0]}}])
    layer = next(item for item in episode.pages[0].layers if item.role.value == "ink")
    assert layer.screen["shape"] == "diamond" and layer.screen["offset_mm"] == [1.0, 0.0]
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "set_layer", "page": 1, "layer": "ink", "screen": {"lpi": 40, "shape": "star"}}])


def test_greys_are_toned_at_export_in_black_and_white(tmp_path):
    grey = Image.new("L", (400, 200), 255)
    grey.paste(150, (0, 0, 200, 200))
    grey.paste(0, (200, 0, 260, 200))
    plain = np.asarray(to_bitonal(grey, 180).convert("L"))
    assert (plain[:, :200] == 0).all()  # (the threshold: a mid grey goes solid black)
    toned = np.asarray(to_bitonal(grey, 180, {"lpi": 60, "dpi": 600}).convert("L"))
    share = (toned[:, :200] == 0).mean()
    assert 0.2 < share < 0.6  # (dots: some of it black)
    assert (toned[:, 200:260] == 0).all() and (toned[:, 300:] == 255).all()  # (solid black and paper stay)

    from genko.export import export_print

    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    files = export_print(episode, tmp_path, fmt="tiff", dpi=150, screen={"lpi": 60, "shape": "square"})
    assert files and files[0].exists()
    with pytest.raises(ValueError):
        export_print(episode, tmp_path, fmt="tiff", dpi=150, screen={"lpi": 500})
