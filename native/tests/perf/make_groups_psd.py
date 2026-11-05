"""PC5 I-b review P2-4: a B4 RGB PSD at 350 dpi (3541 x 5016) of groups, as a manga page's might be, for
test_perf_render's psd_of_groups_at_350dpi (GENKO_PERF_GROUPS_PSD). Its layers (top first):

  words                 normal                       (outside any group)
  effects (group)       pass through, opacity 255    glow1, glow2 (screen)
  person (group)        pass through, opacity 180    line (multiply), shade (group, multiply: shade1, shade2), colour
  background (group)    normal, opacity 255          ground, sky

Each layer is a page-size raster of random lines or a filled band (random.Random(1)), so that each is drawn over the
whole page. Made with psd-tools (1.10) and Pillow; it takes a minute and writes about 85 MB. (The layers are made
without the PSD — frompil(…, None, …) — so that their transparency stays: frompil converts to the PSD's RGB otherwise.)

    python make_groups_psd.py OUT.psd
"""

import random
import sys

from PIL import Image, ImageDraw
from psd_tools import PSDImage
from psd_tools.api.layers import Group, PixelLayer
from psd_tools.constants import BlendMode, Tag

W, H = 3541, 5016
rng = random.Random(1)


def lines(colour, n, width, alpha=255):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for _ in range(n):
        x, y = rng.uniform(0, W), rng.uniform(0, H)
        d.line([(x, y), (x + rng.uniform(-600, 600), y + rng.uniform(-600, 600))], fill=colour + (alpha,), width=width)
    return im


def filled(colour, top, bottom):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle([0, top, W, bottom], fill=colour + (255,))
    return im


def pass_through(group):
    """通過 as Photoshop writes it (psd-tools' setter leaves the record's blend normal): the record's and lsct's key 'pass'."""
    group._record.blend_mode = BlendMode.PASS_THROUGH
    setting = group._record.tagged_blocks.get_data(Tag.SECTION_DIVIDER_SETTING)
    setting.signature, setting.blend_mode = b"8BIM", BlendMode.PASS_THROUGH


def main() -> int:
    psd = PSDImage.new("RGB", (W, H), color=(255, 255, 255))
    background = Group.new("background", parent=psd)
    background.append(PixelLayer.frompil(filled((200, 220, 255), 0, H), None, "sky"))
    background.append(PixelLayer.frompil(filled((120, 160, 90), H * 2 // 3, H), None, "ground"))
    person = Group.new("person", parent=psd)
    person.append(PixelLayer.frompil(lines((230, 180, 150), 300, 40), None, "colour"))
    shade = Group.new("shade", parent=person)
    shade.append(PixelLayer.frompil(lines((80, 80, 140), 200, 30, 160), None, "shade1"))
    shade.append(PixelLayer.frompil(lines((60, 60, 100), 200, 20, 160), None, "shade2"))
    shade.blend_mode = BlendMode.MULTIPLY
    line = PixelLayer.frompil(lines((20, 20, 20), 1500, 6), None, "line")
    line.blend_mode = BlendMode.MULTIPLY
    person.append(line)
    pass_through(person)
    person.opacity = 180
    effects = Group.new("effects", parent=psd)
    for k in range(2):
        glow = PixelLayer.frompil(lines((255, 240, 120), 100, 50, 120), None, f"glow{k + 1}")
        glow.blend_mode = BlendMode.SCREEN
        effects.append(glow)
    pass_through(effects)
    psd.append(PixelLayer.frompil(lines((0, 0, 0), 60, 12), None, "words"))
    psd.save(sys.argv[1])
    print([(layer.name, layer.kind, layer._record.blend_mode.name, layer.opacity) for layer in PSDImage.open(sys.argv[1]).descendants()])
    return 0


if __name__ == "__main__":
    sys.exit(main())
