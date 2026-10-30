"""PC5 I-c (SPEC.md AUTO-01「アートボード」): a PSD's artboards as psd-tools reads them, for test_contract_artboards — the file's
depth and size, and each top-level layer (top first) with its name, its kind (psd-tools' "artboard" for a group of an
artb block), its board (left, top, right, bottom), the background type and preset its artb block says, and its layers'
names (top first).

    <reference python> tools/migration/psd_artboards_check.py OUT.json FILE…
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Tag


def text(value):
    value = getattr(value, "value", value)
    if isinstance(value, bytes):
        value = value.decode("utf-8", "replace")
    return str(value).rstrip("\x00")


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    layers = []
    for layer in reversed(list(psd)):
        item = {"name": layer.name, "kind": layer.kind, "children": [c.name for c in reversed(list(layer))] if layer.is_group() else None}
        if layer.kind == "artboard":
            item["bbox"] = list(layer.bbox)
            for key in (Tag.ARTBOARD_DATA1, Tag.ARTBOARD_DATA2, Tag.ARTBOARD_DATA3):
                if key in layer.tagged_blocks:
                    data = layer.tagged_blocks.get_data(key)
                    item["background"] = int(data.get(b"artboardBackgroundType"))
                    item["preset"] = text(data.get(b"artboardPresetName"))
                    colour = data.get(b"Clr ")
                    if colour is not None:
                        item["colour"] = [round(float(colour.get(k))) for k in (b"Rd  ", b"Grn ", b"Bl  ")]
        layers.append(item)
    return {"depth": psd.depth, "width": psd.width, "height": psd.height, "layers": layers}


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
