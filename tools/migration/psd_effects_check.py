"""PC4 E6 (SPEC.md COLOR-09「レイヤースタイル」): a PSD's layer effects as psd-tools reads them, for test_layer_styles —
each layer (by its name) with its effects' master switch and each effect (its class's name, whether it is on, its
opacity, distance, size, angle, blend mode where it has them) and its fill opacity (iOpa, 0..255).

    <reference python> tools/migration/psd_effects_check.py OUT.json FILE
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Tag


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def effect_view(effect) -> dict:
    out = {"kind": type(effect).__name__, "enabled": bool(getattr(effect, "enabled", True))}
    for name in ("opacity", "distance", "size", "angle"):
        try:
            value = number(getattr(effect, name))
        except Exception:  # (a property the effect does not hold)
            value = None
        if value is not None:
            out[name] = value
    try:
        out["blend_mode"] = str(getattr(effect, "blend_mode"))
    except Exception:
        pass
    return out


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    layers = []
    for layer in psd.descendants():
        effects = layer.effects
        item = {"name": layer.name, "enabled": bool(getattr(effects, "enabled", True)), "effects": [effect_view(e) for e in effects]}
        try:
            fill = layer._record.tagged_blocks.get_data(Tag.BLEND_FILL_OPACITY)
        except Exception:
            fill = None
        item["fill_opacity"] = None if fill is None else int(fill)
        layers.append(item)
    return {"layers": layers}


def main() -> int:
    out, path = sys.argv[1], sys.argv[2]
    try:
        result = view(path)
    except Exception as error:  # (said, for the test to show)
        result = {"error": repr(error)}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
