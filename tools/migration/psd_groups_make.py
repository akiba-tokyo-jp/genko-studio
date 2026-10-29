"""PC5 I-b (SPEC.md AUTO-01「グループ」, review P3-9): a PSD of groups made by psd-tools (not by Genko), for
test_contract_psd's groupsMadeByPsdTools — 157 x 157 RGB, its layers (top first):

  hid (group, hidden)         black (grey 0, the whole picture)
  mul (group, multiply)       dark (grey 128, the left half)
  base                        grey 200, the whole picture

(The layers are made without the PSD — frompil(…, None, …) — so that their transparency stays.)

    <reference python> tools/migration/psd_groups_make.py OUT.psd
"""

import sys

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import Group, PixelLayer
from psd_tools.constants import BlendMode, Tag

W = H = 157


def grey(value, width=W):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.paste((value, value, value, 255), (0, 0, width, H))
    return im


def main() -> int:
    psd = PSDImage.new("RGB", (W, H), color=(255, 255, 255))
    psd.append(PixelLayer.frompil(grey(200), None, "base"))
    mul = Group.new("mul", parent=psd)
    mul.append(PixelLayer.frompil(grey(128, W // 2), None, "dark"))
    mul.blend_mode = BlendMode.MULTIPLY  # (and in lsct, as Photoshop writes a group's blend)
    setting = mul._record.tagged_blocks.get_data(Tag.SECTION_DIVIDER_SETTING)
    setting.signature, setting.blend_mode = b"8BIM", BlendMode.MULTIPLY
    hid = Group.new("hid", parent=psd)
    hid.append(PixelLayer.frompil(grey(0), None, "black"))
    hid.visible = False
    psd.save(sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
