"""PC5 I-a (SPEC.md AUTO-01「レイヤーの基本」): a PSD's layers' locks and channel blending restrictions as psd-tools reads them,
for test_contract_layers — each layer (in psd-tools' order, depth first) with its name, its lspf (the locks' bits: 1
transparency, 2 the pixels, 4 the position, 16 the artboards' auto-nesting, 2147483648 all; none: no block) and its brst (the
colour channels' indices left out of blending; none: no block).

    <reference python> tools/migration/psd_layer_basics_check.py OUT.json FILE…
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Tag


def value_of(block):
    data = getattr(block, "data", block)
    return data


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    layers = []
    for layer in psd.descendants():
        blocks = layer.tagged_blocks
        locks = blocks.get(Tag.PROTECTED_SETTING) if blocks is not None else None
        brst = blocks.get(Tag.CHANNEL_BLENDING_RESTRICTIONS_SETTING) if blocks is not None else None
        layers.append({
            "name": layer.name,
            "lspf": int(value_of(locks)) if locks is not None else None,
            "brst": [int(c) for c in value_of(brst)] if brst is not None else None,
        })
    return {"layers": layers}


def main() -> int:
    out, files = sys.argv[1], sys.argv[2:]
    result = {}
    for path in files:
        try:
            result[path] = view(path)
        except Exception as error:  # (shown to the test as what psd-tools said)
            result[path] = {"error": "%s: %s" % (type(error).__name__, error)}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
