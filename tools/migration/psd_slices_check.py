"""PC5 J-b (SPEC.md PROD-01「スライス」): a PSD's slices as psd-tools reads them, for test_contract_slices — image resource
1050 (version 6: the bounds, the group's name, each slice's id, group, origin, layer id, name, type, rect, URL, target,
message, alt, cell text and its HTML flag, alignment and background; the outsets of the descriptor after the slices) —
and a PSD psd-tools writes with a version 6 resource of its own (a user slice with its options, an auto slice).

    <reference python> tools/migration/psd_slices_check.py view OUT.json FILE
    <reference python> tools/migration/psd_slices_check.py make OUT.psd
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Resource
from psd_tools.psd.image_resources import ImageResource, Slices, SlicesV6, SliceV6


def plain(value):
    if hasattr(value, "value"):
        v = value.value
        return v.rstrip("\0") if isinstance(v, str) else v
    return str(value)


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    resources = psd.image_resources
    out = {"slices": None}
    if Resource.SLICES not in resources:
        return out
    block = resources.get_data(Resource.SLICES)
    out["version"] = block.version
    if block.version != 6:
        out["slices"] = []
        return out
    v6 = block.data
    out["bounds"] = list(v6.bbox)
    out["group"] = v6.name
    items = []
    outsets = {}
    for item in v6.items:
        items.append(
            {
                "id": item.slice_id,
                "group_id": item.group_id,
                "origin": item.origin,
                "layer_id": item.associated_id,
                "name": item.name,
                "type": item.slice_type,
                "rect": list(item.bbox),
                "url": item.url,
                "target": item.target,
                "message": item.message,
                "alt": item.alt_tag,
                "cell_is_html": bool(item.cell_is_html),
                "cell_text": item.cell_text,
                "align": [item.horizontal_align, item.vertical_align],
                "color": [item.alpha, item.red, item.green, item.blue],
            }
        )
        # (the descriptor after the slices: psd-tools hangs it on the last one)
        if item.data is not None:
            for d in item.data.get(b"slices", []):
                key = {k.decode("latin-1") if isinstance(k, bytes) else str(k): plain(v) for k, v in d.items()}
                outsets[str(key.get("sliceID"))] = [key.get("topOutset", 0), key.get("leftOutset", 0), key.get("bottomOutset", 0), key.get("rightOutset", 0)]
    out["slices"] = items
    out["outsets"] = outsets
    return out


def make(path: str) -> None:
    psd = PSDImage.new("RGB", (120, 90), color=(255, 255, 255), depth=8)
    user = SliceV6(
        slice_id=3, group_id=0, origin=2, name="hero", slice_type=1, bbox=[20, 10, 60, 30], url="https://example.org/",
        target="_blank", message="m", alt_tag="Hero", cell_is_html=False, cell_text="", horizontal_align=0, vertical_align=0,
        alpha=255, red=0x12, green=0x34, blue=0x56,
    )
    auto = SliceV6(slice_id=4, group_id=0, origin=0, name="", slice_type=1, bbox=[0, 0, 120, 10])
    v6 = SlicesV6(bbox=[0, 0, 90, 120], name="site", items=[user, auto])
    psd._record.image_resources[Resource.SLICES] = ImageResource(key=Resource.SLICES.value, data=Slices(version=6, data=v6))
    psd.save(path)
    again = view(path)  # (a file psd-tools cannot read again is not one)
    assert again["slices"] and len(again["slices"]) == 2, again


def main() -> int:
    mode, out_path = sys.argv[1], sys.argv[2]
    if mode == "make":
        make(out_path)
        return 0
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(view(sys.argv[3]), f, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
