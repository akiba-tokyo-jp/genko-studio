from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import mm_to_px, render_page


def _ink(img, x0, y0, x1, y1):
    pts = [
        (x, y)
        for x in range(max(0, x0), min(x1, img.width))
        for y in range(max(0, y0), min(y1, img.height))
        if img.getpixel((x, y))[0] < 80
    ]
    assert pts, f"no ink in {(x0, y0, x1, y1)}"
    return pts


def _render(text: str, w_mm: float = 16, h_mm: float = 20, wrap: str = "vertical"):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "set_meta", "font_path": r"C:\Windows\Fonts\YuGothM.ttc"}])
    apply_ops(
        ep,
        [
            {
                "op": "add_line",
                "page": 1,
                "text": text,
                "x_mm": 40,
                "y_mm": 40,
                "w_mm": w_mm,
                "h_mm": h_mm,
                "wrap": wrap,
                "balloon": "none",
            }
        ],
    )
    img = render_page(ep.pages[0], 150, mode="print", episode=ep)
    x0, y0 = mm_to_px(40, 150), mm_to_px(40, 150)
    x1, y1 = mm_to_px(40 + w_mm, 150), mm_to_px(40 + h_mm, 150)
    return img, x0, y0, x1, y1


def test_choonpu_is_vertical_stroke_not_horizontal_dash():
    img, x0, y0, x1, y1 = _render("ー", w_mm=16, h_mm=20)
    ink = _ink(img, x0, y0, x1, y1)
    xs = [p[0] for p in ink]
    ys = [p[1] for p in ink]
    assert max(ys) - min(ys) > max(xs) - min(xs)


def test_kuten_sits_in_top_right_of_em_box():
    img, x0, y0, x1, y1 = _render("。", w_mm=16, h_mm=16)
    ink = _ink(img, x0, y0, x1, y1)
    cx = (x0 + x1) / 2
    cy = (y0 + y1) / 2
    right = sum(1 for x, _y in ink if x >= cx)
    top = sum(1 for _x, y in ink if y <= cy)
    assert right > len(ink) * 0.55
    assert top > len(ink) * 0.55


def test_small_kana_sits_right_in_em_box():
    img_small, x0, y0, x1, y1 = _render("っ", w_mm=16, h_mm=16)
    img_full, *_ = _render("つ", w_mm=16, h_mm=16)
    ink_s = _ink(img_small, x0, y0, x1, y1)
    ink_f = _ink(img_full, x0, y0, x1, y1)
    cx_s = sum(p[0] for p in ink_s) / len(ink_s)
    cx_f = sum(p[0] for p in ink_f) / len(ink_f)
    assert cx_s > cx_f


def test_corner_bracket_opens_downward_not_rightward():
    img, x0, y0, x1, y1 = _render("「", w_mm=16, h_mm=16)
    ink = _ink(img, x0, y0, x1, y1)
    xs = [p[0] for p in ink]
    ys = [p[1] for p in ink]
    width = max(xs) - min(xs)
    height = max(ys) - min(ys)
    top_band = [x for x, y in ink if y <= min(ys) + max(2, height * 0.35)]
    assert top_band
    assert max(top_band) - min(top_band) > width * 0.45
