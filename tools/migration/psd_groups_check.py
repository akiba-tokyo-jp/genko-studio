"""PC5 I-b (SPEC.md AUTO-01「グループ」): a PSD's groups as psd-tools reads them, for test_contract_psd — each layer (in
psd-tools' order, depth first) with its name, kind, the name of its parent, its blend mode, opacity (0..255), visibility
and whether it has a mask; and the picture psd-tools composites of the layers (force=True, as RGB), each pixel's grey at
the points asked for.

    <reference python> tools/migration/psd_groups_check.py OUT.json X,Y[;X,Y…] FILE…
"""

import json
import sys

from psd_tools import PSDImage


def view(path: str, points: list) -> dict:
    psd = PSDImage.open(path)
    layers = []
    for layer in psd.descendants():
        parent = layer.parent
        layers.append({
            "name": layer.name,
            "kind": layer.kind,
            "parent": parent.name if parent is not None and parent is not psd else None,
            "blend": layer.blend_mode.name if layer.blend_mode is not None else None,
            "opacity": int(layer.opacity),
            "visible": bool(layer.visible),
            "mask": layer.mask is not None,
        })
    picture = psd.composite(force=True).convert("RGB")
    return {"layers": layers, "pixels": [list(picture.getpixel((x, y))) for x, y in points]}


def main() -> int:
    out, spots, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    points = [tuple(int(v) for v in p.split(",")) for p in spots.split(";") if p]
    result = {}
    for path in files:
        try:
            result[path] = view(path, points)
        except Exception as error:  # (shown to the test as what psd-tools said)
            result[path] = {"error": "%s: %s" % (type(error).__name__, error)}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
