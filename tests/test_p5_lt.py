from pathlib import Path

from PIL import Image

from genko.models import PageSpec, new_episode
from genko.ops import apply_ops


def test_adaptive_lt_traces_horizontal_bar(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    img = Image.new("RGB", (80, 40), (255, 255, 255))
    for y in range(18, 22):
        for x in range(5, 75):
            img.putpixel((x, y), (0, 0, 0))
    png = tmp_path / "bar.png"
    img.save(png)
    apply_ops(
        ep,
        [
            {"op": "put_raster", "page": 1, "layer": "bg", "path": str(png)},
            {"op": "name_ok", "page": 1},
            {"op": "lt_convert", "page": 1, "layer": "bg", "to": "ink", "method": "adaptive"},
        ],
    )
    strokes = ep.pages[0].ink_strokes
    assert strokes
    xs = [pt[0] for stroke in strokes for pt in stroke]
    ys = [pt[1] for stroke in strokes for pt in stroke]
    assert max(xs) - min(xs) > max(ys) - min(ys)
