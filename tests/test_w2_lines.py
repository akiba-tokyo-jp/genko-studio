"""The line's entry and exit, pressure into darkness, steadying by speed, and the after-correction that
redraws a wobbly line as a smooth curve."""

from __future__ import annotations

import math
import os

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from genko import brushes  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import apply_ops  # noqa: E402
from genko.stroke import fit_curve, stabilize_points, taper_points  # noqa: E402


def _line(length_mm: float, n: int = 101) -> list:
    return [[length_mm * i / (n - 1), 0.0, 1.0] for i in range(n)]


def test_entry_and_exit_are_lengths_and_each_end_its_own():
    short, long_ = taper_points(_line(10), 2.0, 5.0), taper_points(_line(100), 2.0, 5.0)
    # 1 mm in: half way through the 2 mm entry, on both lines alike
    at = lambda pts, mm: min(pts, key=lambda p: abs(p[0] - mm))[2]  # noqa: E731
    assert at(short, 1.0) == pytest.approx(0.5, abs=0.06) and at(long_, 1.0) == pytest.approx(0.5, abs=0.06)
    assert at(long_, 50) == 1.0 and at(long_, 97.0) == pytest.approx(0.6, abs=0.02)
    only_exit = taper_points(_line(50), 0.0, 5.0)
    assert only_exit[0][2] == 1.0 and only_exit[-1][2] == pytest.approx(0.15)
    # without lengths: a quarter of the points at each end, as before
    assert taper_points(_line(50))[10][2] == pytest.approx(0.4, abs=0.01)


def test_a_light_touch_lightens_the_line():
    pts = [[5 + i * 0.5, 5.0, 0.2 if i < 20 else 1.0] for i in range(40)]
    flat, _ = brushes.draw((400, 200), pts, 200, 1.0, "maru")
    shaded, _ = brushes.draw((400, 200), pts, 200, 1.0, "maru", pressure_opacity=1.0)
    assert shaded.crop((0, 0, 50, shaded.height)).getextrema()[1] < flat.crop((0, 0, 50, flat.height)).getextrema()[1]
    assert shaded.crop((shaded.width - 60, 0, shaded.width - 20, shaded.height)).getextrema()[1] > 240


def test_quick_parts_are_steadied_more():
    slow = [[i * 0.1, math.sin(i) * 0.2] for i in range(30)]
    quick = [[3 + i * 1.0, math.sin(i) * 0.2] for i in range(30)]
    pts = slow + quick
    even = stabilize_points(pts, 5)
    by_speed = stabilize_points(pts, 5, by_speed=True)
    wobble = lambda p, lo, hi: sum(abs(y) for _, y in p[lo:hi])  # noqa: E731
    assert wobble(by_speed, 35, 55) < wobble(even, 35, 55)  # (the quick part is flatter)
    assert wobble(by_speed, 5, 25) >= wobble(even, 5, 25) * 0.99  # (the slow part keeps its shape)


def test_the_after_correction_redraws_a_smooth_curve():
    import random

    rng = random.Random(1)
    wobbly = [[i * 0.3, 10 * math.sin(i * 0.03) + rng.uniform(-0.1, 0.1), 0.5 + i / 400] for i in range(200)]
    fitted = fit_curve(wobbly, 0.3)
    assert fitted[0][:2] == pytest.approx(wobbly[0][:2]) and fitted[-1][:2] == pytest.approx(wobbly[-1][:2])
    turns = lambda p: sum(1 for a, b, c in zip(p, p[1:], p[2:]) if (b[1] - a[1]) * (c[1] - b[1]) < 0)  # noqa: E731
    assert turns(fitted) < turns(wobbly) / 4 and fitted[len(fitted) // 2][2] > fitted[0][2]


def test_the_op_keeps_them_and_the_book_saves_them(tmp_path):
    from genko.io import load_episode, save_episode

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    pts = [[30 + i, 40 + math.sin(i / 5), 0.3 + (i % 5) / 10] for i in range(60)]
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer": "ink", "points": pts, "taper": True, "taper_in_mm": 3,
                    "taper_out_mm": 0, "pressure_opacity": 0.6, "stabilize": 5, "stabilize_speed": True, "post_fit": 0.3}])
    ink = next(layer for layer in ep.pages[0].layers if layer.role.value == "ink")
    stroke = ink.strokes[0]
    assert stroke.pressure_opacity == 0.6 and stroke.pressure[0] < 0.2 and stroke.pressure[-1] > 0.2
    save_episode(ep, tmp_path / "b.genko")
    again = next(layer for layer in load_episode(tmp_path / "b.genko").pages[0].layers if layer.role.value == "ink")
    assert again.strokes[0].pressure_opacity == 0.6


def test_the_brush_panel_sends_them(tmp_path, monkeypatch):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    try:
        QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:
        pytest.skip(f"Qt cannot start here: {exc}")
    from genko.app.brush_panel import BrushPanel

    panel = BrushPanel()
    panel._loading = True  # (changes here are not kept in the person's settings)
    panel.taper.setChecked(True)
    panel.taper_in.setValue(2.5)
    panel.taper_out.setValue(-1)
    panel.ink_pressure.setValue(40)
    panel.steady.setValue(4)
    panel.speed_steady.setChecked(True)
    panel.post_fit.setValue(0.4)
    fields = panel.stroke_fields()
    assert fields["taper_in_mm"] == 2.5 and "taper_out_mm" not in fields
    assert fields["pressure_opacity"] == 0.4 and fields["stabilize_speed"] and fields["post_fit"] == 0.4
