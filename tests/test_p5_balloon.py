from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def test_vertical_text_occupies_more_height_than_width():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_line",
                "page": 1,
                "text": "本日締",
                "x_mm": 40,
                "y_mm": 40,
                "w_mm": 16,
                "h_mm": 60,
                "wrap": "vertical",
                "balloon": "none",
            }
        ],
    )
    img = render_page(ep.pages[0], 150, mode="print", episode=ep)
    x0, y0 = mm_to_px(40, 150), mm_to_px(40, 150)
    x1, y1 = mm_to_px(56, 150), mm_to_px(100, 150)
    ink = [
        (x, y)
        for x in range(x0, min(x1, img.width))
        for y in range(y0, min(y1, img.height))
        if img.getpixel((x, y))[0] < 80
    ]
    xs = [p[0] for p in ink]
    ys = [p[1] for p in ink]
    assert ink
    assert max(ys) - min(ys) > max(xs) - min(xs)


def test_ruby_run_sits_to_the_right_of_vertical_base():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {
                "op": "add_line",
                "page": 1,
                "text": "漢",
                "x_mm": 50,
                "y_mm": 50,
                "w_mm": 24,
                "h_mm": 40,
                "wrap": "vertical",
                "balloon": "none",
            }
        ],
    )
    line_id = ep.story[0].id
    apply_ops(ep, [{"op": "set_balloon_path", "id": line_id, "wrap": "vertical", "ruby_runs": [["漢", "かん"]]}])
    img = render_page(ep.pages[0], 150, mode="print", episode=ep)
    base_x = mm_to_px(50, 150)
    ruby_band = img.crop((base_x + 8, mm_to_px(50, 150), base_x + 40, mm_to_px(90, 150))).convert("L")
    assert ruby_band.getextrema()[0] < 80
