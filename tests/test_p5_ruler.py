from genko.models import PageSpec, new_episode
from genko.ops import apply_ops


def test_snapped_stroke_points_at_vanishing_point():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "set_ruler", "page": 1, "kind": "perspective", "points": [[100, 20]]},
            {
                "op": "add_stroke",
                "page": 1,
                "layer": "name",
                "points": [[20, 200], [40, 180]],
                "snap_ruler": True,
            },
        ],
    )
    stroke = ep.pages[0].name_strokes[0]
    x0, y0 = float(stroke[0][0]), float(stroke[0][1])
    x1, y1 = float(stroke[-1][0]), float(stroke[-1][1])
    # end sits on the line from start to vanishing point (100, 20)
    vx, vy = 100.0, 20.0
    dx, dy = vx - x0, vy - y0
    t = ((x1 - x0) * dx + (y1 - y0) * dy) / (dx * dx + dy * dy)
    px, py = x0 + t * dx, y0 + t * dy
    assert abs(px - x1) < 0.6
    assert abs(py - y1) < 0.6
