"""PC2 (SPEC.md COLOR-04, the user's decision 5): the adjustment layers of PSD files as psd-tools reads them, for the
C++ contract test (native/tests/contract/test_contract_adjust.cpp).

    python psd_adjust_check.py <answer.json> <file.psd>...

writes, for each file, its layers in order: {"name", "kind"} and what psd-tools reads of an adjustment layer (levels:
the first five records; curves: each curve's points; brightness/contrast; posterize; threshold; exposure; a gradient
map's flags and stops; a colour lookup's descriptor keys and its table's size). A file psd-tools cannot read gives
{"error": "<type>: <message>"}.
"""

import json
import sys

from psd_tools import PSDImage


def adjustment(layer):
    kind = layer.kind
    out = {}
    if kind == "levels":
        out["records"] = [[r.input_floor, r.input_ceiling, r.output_floor, r.output_ceiling, r.gamma] for r in list(layer.data)[:5]]
    elif kind == "curves":
        out["curves"] = [[list(p) for p in points] for points in layer.data.data]
    elif kind == "brightnesscontrast":
        out.update(brightness=layer.brightness, contrast=layer.contrast, legacy=layer.use_legacy)
    elif kind == "posterize":
        out["levels"] = int(layer.posterize)
    elif kind == "threshold":
        out["level"] = int(layer.threshold)
    elif kind == "exposure":
        out.update(exposure=layer.exposure, offset=layer.offset, gamma=layer.gamma)
    elif kind == "gradientmap":
        out.update(reversed=bool(layer.reversed), dithered=bool(layer.dithered), stops=len(layer.color_stops))
    elif kind == "colorlookup":
        data = layer._data

        def name(key):
            key = getattr(key, "value", key)
            return key.decode("latin-1") if isinstance(key, bytes) else str(key)

        out["keys"] = sorted(name(k) for k in data.keys())
        table = next((v for k, v in data.items() if name(k) == "LUT3DFileData"), b"")
        raw = getattr(table, "value", table)
        text = bytes(raw or b"").decode("latin-1")
        for line in text.splitlines():
            if line.startswith("LUT_3D_SIZE"):
                out["size"] = int(line.split()[1])
    return out


def main():
    answer = []
    for path in sys.argv[2:]:
        try:
            psd = PSDImage.open(path)
            layers = []
            for layer in psd.descendants():
                item = {"name": layer.name, "kind": layer.kind}
                item.update(adjustment(layer))
                layers.append(item)
            answer.append({"layers": layers})
        except Exception as error:  # (a file psd-tools cannot read: said, not raised)
            answer.append({"error": "%s: %s" % (type(error).__name__, error)})
    with open(sys.argv[1], "w", encoding="utf-8") as out:
        json.dump(answer, out, ensure_ascii=False)


if __name__ == "__main__":
    main()
