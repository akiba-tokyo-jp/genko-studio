"""PC5 H-b (SPEC.md VIEW-01「ガイド」, FILE-01「PSD・PSB」): a PSD's guides as psd-tools reads them, for
test_contract_guides — the image resource 1032 (グリッドとガイド): its version, the grid's cycles and each guide's place
(1/32 of a pixel, signed) and way (0 upright, 1 across); or a PSD written again by psd-tools with guides put in it.

    <reference python> tools/migration/psd_guides_check.py read OUT.json FILE
    <reference python> tools/migration/psd_guides_check.py add IN.psd OUT.psd '[[place, way], ...]'
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Resource
from psd_tools.psd.image_resources import GridGuidesInfo, ImageResource


def signed(value: int) -> int:
    return value - (1 << 32) if value >= (1 << 31) else value


def read(out_path: str, path: str) -> None:
    psd = PSDImage.open(path)
    resources = psd._record.image_resources
    out = {"guides": None}
    if Resource.GRID_AND_GUIDES_INFO in resources:
        info = resources[Resource.GRID_AND_GUIDES_INFO].data
        out["guides"] = {
            "version": info.version,
            "horizontal": info.horizontal,
            "vertical": info.vertical,
            "items": [[signed(place), way] for place, way in info.data],
        }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f)


def add(in_path: str, out_path: str, items: str) -> None:
    psd = PSDImage.open(in_path)
    data = [(place & 0xFFFFFFFF, way) for place, way in json.loads(items)]
    info = GridGuidesInfo(version=1, horizontal=576, vertical=576, data=data)
    resources = psd._record.image_resources
    resources[Resource.GRID_AND_GUIDES_INFO] = ImageResource(key=Resource.GRID_AND_GUIDES_INFO, name="", data=info)
    psd.save(out_path)


if __name__ == "__main__":
    if sys.argv[1] == "read":
        read(sys.argv[2], sys.argv[3])
    else:
        add(sys.argv[2], sys.argv[3], sys.argv[4])
