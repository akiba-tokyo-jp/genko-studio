"""PC5 J-d1 (SPEC.md PROD-01「計測」): a PSD's measurement scale and count as psd-tools reads them, for
test_contract_measure — image resource 1074 (a version-16 descriptor: pixelLength, logicalLength, logicalUnits) and
whether resource 1080 (the count) is there — and a PSD psd-tools writes with both (a scale of 150 pixels = 2 mm, a count
descriptor whose bytes Genko keeps).

    <reference python> tools/migration/psd_measure_check.py view OUT.json FILE
    <reference python> tools/migration/psd_measure_check.py make OUT.psd
"""

import json
import sys

from psd_tools import PSDImage
from psd_tools.constants import Resource
from psd_tools.psd.descriptor import Descriptor, DescriptorBlock, Double, Integer, String
from psd_tools.psd.image_resources import ImageResource


def plain(value):
    for attr in ("value",):
        if hasattr(value, attr):
            return getattr(value, attr)
    return str(value)


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    resources = psd.image_resources
    out = {"scale": None, "count": Resource.COUNT_INFO in resources, "count_hex": None}
    if Resource.MEASUREMENT_SCALE in resources:
        block = resources.get_data(Resource.MEASUREMENT_SCALE)
        out["scale"] = {k.decode("latin-1") if isinstance(k, bytes) else str(k): plain(v) for k, v in block.items()}
        out["scale_version"] = block.version
    if out["count"]:
        out["count_hex"] = resources[Resource.COUNT_INFO].data.tobytes().hex()
    return out


def make(path: str) -> None:
    psd = PSDImage.new("RGB", (40, 30), color=(255, 255, 255), depth=8)
    scale = DescriptorBlock(classID=b"null")
    scale[b"pixelLength"] = Double(150.0)
    scale[b"logicalLength"] = Double(2.0)
    scale[b"logicalUnits"] = String("mm")
    count = DescriptorBlock(classID=b"null")
    count[b"Vrsn"] = Integer(1)
    count[b"countGroupCount"] = Integer(1)
    resources = psd._record.image_resources
    resources[Resource.MEASUREMENT_SCALE] = ImageResource(key=Resource.MEASUREMENT_SCALE.value, data=scale)
    resources[Resource.COUNT_INFO] = ImageResource(key=Resource.COUNT_INFO.value, data=count)
    psd.save(path)
    again = view(path)  # (a file psd-tools cannot read again is not one)
    assert again["scale"] is not None and again["count"], again


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
