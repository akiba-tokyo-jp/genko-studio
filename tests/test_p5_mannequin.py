from genko.models import PageSpec, new_episode
from genko.ops import apply_ops
from genko.render import render_page


def test_mannequin_not_in_print():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "pos": [100, 160, 0]}])
    name = render_page(ep.pages[0], 72, mode="name", episode=ep)
    printed = render_page(ep.pages[0], 72, mode="print", episode=ep)
    assert name.tobytes() != printed.tobytes()
    # construction ink (90,90,140) should appear in name, not print
    def has_guide(image) -> bool:
        w, h = image.size
        for y in range(0, h, 8):
            for x in range(0, w, 8):
                px = image.getpixel((x, y))
                if abs(px[0] - 90) < 20 and abs(px[2] - 140) < 30:
                    return True
        return False

    assert has_guide(name) is True
    assert has_guide(printed) is False
