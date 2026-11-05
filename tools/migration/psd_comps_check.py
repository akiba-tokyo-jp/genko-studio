"""PC5 J-a (SPEC.md PROD-01「レイヤーカンプ」): a PSD's layer comps as psd-tools reads them, for test_contract_layer_comps —
the image resource 1065 (its CompList descriptor: each comp's name, compID and capturedInfo, its comment, lastAppliedComp)
and each layer's metadata (shmd) item cmls (its LyrI and layerSettings: each setting's compList, enab and Ofst), the
layer's id (lyid) beside it.

    <reference python> tools/migration/psd_comps_check.py OUT.json FILE
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Resource, Tag
from psd_tools.psd.base import ListElement


def plain(value):
    """A descriptor's value as JSON: its texts, numbers, booleans, lists and objects (keys as their 4 characters)."""
    if isinstance(value, ListElement):
        return [plain(v) for v in value]
    if isinstance(value, dict) or hasattr(value, "items"):
        out = {}
        for k, v in value.items():
            raw = k.value if hasattr(k, "value") else k
            key = raw.decode("latin-1") if isinstance(raw, (bytes, bytearray)) else str(raw)
            out[key] = plain(v)
        return out
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if hasattr(value, "value") and not isinstance(value, (bytes, bytearray, str)):
        return plain(value.value)
    if isinstance(value, (bytes, bytearray)):
        return value.decode("latin-1")
    if isinstance(value, str):
        return value.rstrip("\x00")
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value
    return str(value)


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    record = psd._record
    out = {"comps": None, "layers": []}
    resources = record.image_resources
    if Resource.LAYER_COMPS in resources:
        out["comps"] = plain(resources[Resource.LAYER_COMPS].data)
    for layer in psd.descendants():
        blocks = layer._record.tagged_blocks
        item = {"name": layer.name, "visible": layer.visible, "lyid": None, "cmls": None, "shmd": []}
        if blocks is not None:
            if Tag.LAYER_ID in blocks:
                item["lyid"] = int(blocks.get_data(Tag.LAYER_ID))
            if Tag.METADATA_SETTING in blocks:
                for setting in blocks.get_data(Tag.METADATA_SETTING):
                    key = setting.key.decode("latin-1")
                    item["shmd"].append(key)
                    if key == "cmls":
                        item["cmls"] = plain(setting.data)
        out["layers"].append(item)
    return out


def main() -> int:
    out_path, path = sys.argv[1], sys.argv[2]
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(view(path), f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
