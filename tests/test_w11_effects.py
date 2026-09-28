"""Effect lines as CLIP STUDIO has them: bundles, the jitters one by one, the taper, spacing and length."""

from __future__ import annotations

import math

import pytest

from genko import effects
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops


def _page():
    return new_episode("t", 1, 1, PageSpec.b5_doujin()).pages[0]


def _angles(g, centre):
    return sorted(math.atan2(ln["points"][-1][1] - centre[1], ln["points"][-1][0] - centre[0]) % math.tau for ln in g["lines"])


def test_focus_lines_come_in_bundles_with_room_between():
    page = _page()
    params = {"count": 40, "bundle": 5, "bundle_gap": 0.7, "jitter_position": 0, "center": [90, 120]}
    g = effects.geometry({"id": "f", "kind": "focus", "params": params}, page)
    angles = _angles(g, (90, 120))
    gaps = sorted(b - a for a, b in zip(angles, angles[1:]))
    assert gaps[-1] > 4 * gaps[0]  # (wide gaps between bundles, narrow inside them)
    assert sum(1 for gap in gaps if gap > 2 * gaps[0]) >= 6


def test_the_jitters_one_by_one_and_the_taper():
    page = _page()
    even = effects.geometry({"id": "f", "kind": "focus", "params": {"count": 30, "jitter_length": 0, "jitter_width": 0,
                                                                   "jitter_position": 0, "center": [90, 120]}}, page)
    assert len({round(ln["width_mm"], 4) for ln in even["lines"]}) == 1
    stops = {round(math.dist(ln["points"][-1][:2], (90, 120)), 2) for ln in even["lines"]}
    assert max(stops) - min(stops) < 25  # (an ellipse's clear middle: no random stops)
    wide = effects.geometry({"id": "f", "kind": "focus", "params": {"count": 30, "jitter_length": 0, "jitter_width": 1,
                                                                   "jitter_position": 0, "center": [90, 120]}}, page)
    assert len({round(ln["width_mm"], 4) for ln in wide["lines"]}) > 10
    out = effects.geometry({"id": "f", "kind": "focus", "params": {"count": 10, "taper": "out"}}, page)
    first = out["lines"][0]["points"]
    assert first[0][2] < 0.1 and first[-1][2] == pytest.approx(1.0)  # (thin at the outer end)
    none = effects.geometry({"id": "f", "kind": "speed", "params": {"count": 10, "taper": False}}, page)
    assert all(p[2] == 1.0 for ln in none["lines"] for p in ln["points"])


def test_speed_lines_by_spacing_and_focus_lines_by_length():
    page = _page()
    g = effects.geometry({"id": "s", "kind": "speed", "params": {"spacing_mm": 4}}, page)
    g2 = effects.geometry({"id": "s", "kind": "speed", "params": {"spacing_mm": 8}}, page)
    assert len(g["lines"]) == pytest.approx(2 * len(g2["lines"]), abs=2)
    f = effects.geometry({"id": "f", "kind": "focus", "params": {"count": 12, "length_mm": 15, "jitter_length": 0, "center": [90, 120]}}, page)
    for ln in f["lines"]:
        assert math.dist(ln["points"][0][:2], ln["points"][-1][:2]) == pytest.approx(15, abs=0.01)


def test_bad_settings_are_refused():
    episode = new_episode("t", 1, 1, PageSpec.b5_doujin())
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "add_effect", "page": 1, "kind": "focus", "params": {"taper": "sideways"}}])
    with pytest.raises(ApplyError):
        apply_ops(episode, [{"op": "add_effect", "page": 1, "kind": "speed", "params": {"bundle": 0}}])


def test_the_effect_panel_sets_bundles_and_taper(tmp_path):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    from genko.app.main import MainWindow

    window = MainWindow()
    try:
        page = window._current()
        window.apply_ops([{"op": "add_effect", "page": page.index, "kind": "focus", "id": "fx"}])
        window.canvas.selected_effect_id = "fx"
        panel = window.materials
        panel._fill_effects()
        assert "bundle" in panel.effect_fields and "taper" in panel.effect_fields
        panel._effect_set("bundle", 4)
        panel._effect_set("taper", "none")
        panel._effect_set("length_mm", 0)
        params = next(e for e in window._current().effects if e["id"] == "fx")["params"]
        assert params["bundle"] == 4 and params["taper"] is False and "length_mm" not in params
    finally:
        window.close()
