"""PC4 F-d (SPEC.md FILE-01「PSD・PSB」): a PSD or PSB as psd-tools reads it, for test_contract_psd — its version, size,
depth and mode; each layer (by its name, in psd-tools' order) with its tagged blocks (each key's payload as psd-tools
writes it again, sha256: the same parse, the same bytes), its blending ranges, its box, and the samples of its colour
channels and transparency (sha256 of each, as stored at the file's depth); the image resources (id → sha256 of their
data) and the global tagged blocks (key → sha256 of their payload).

    <reference python> tools/migration/psd_keep_check.py OUT.json [--samples] FILE…

With --samples each channel's samples too ("values": numbers as stored — 8 and 16 bits whole, 32 bits floats).
"""

import hashlib
import json
import struct
import sys

from psd_tools import PSDImage


def digest(data) -> str:
    return hashlib.sha256(data).hexdigest()


def payload(block, version):
    """A tagged block's payload as psd-tools holds it: its bytes, or what it writes of what it parsed."""
    data = block.data
    if isinstance(data, (bytes, bytearray)):
        return bytes(data)
    if hasattr(data, "tobytes"):
        try:
            return data.tobytes(version=version, padding=1)
        except TypeError:
            return data.tobytes()
    return repr(data).encode("utf-8")


def key_of(k) -> str:
    raw = k.value if hasattr(k, "value") else k
    return raw.decode("latin-1") if isinstance(raw, (bytes, bytearray)) else str(raw)


def values(raw: bytes, depth: int) -> list:
    if depth == 16:
        return list(struct.unpack(">%dH" % (len(raw) // 2), raw[:len(raw) // 2 * 2]))
    if depth == 32:
        return list(struct.unpack(">%df" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
    return list(raw)


def view(path: str, samples: bool = False) -> dict:
    psd = PSDImage.open(path)
    record = psd._record
    version = record.header.version
    out = {"version": version, "width": record.header.width, "height": record.header.height, "depth": record.header.depth,
           "mode": int(record.header.color_mode), "layers": [], "resources": {}, "globals": {}}
    for rid, res in record.image_resources.items():
        data = res.data
        raw = bytes(data) if isinstance(data, (bytes, bytearray)) else (data.tobytes() if hasattr(data, "tobytes") else repr(data).encode())
        out["resources"][str(int(rid))] = digest(raw)
    blocks = record.layer_and_mask_information.tagged_blocks
    if blocks is not None:
        for k, block in blocks.items():
            name = key_of(k)
            if name in ("Lr16", "Lr32", "Layr"):
                continue
            out["globals"][name] = digest(payload(block, version))
    info = record.layer_and_mask_information.layer_info
    if info is None or info.layer_records is None:
        for k in (b"Lr16", b"Lr32"):
            if blocks is not None:
                for kk, block in blocks.items():
                    if key_of(kk) == k.decode():
                        info = block.data
    if info is not None and info.layer_records is not None:
        for rec, channels in zip(info.layer_records, info.channel_image_data):
            item = {"name": rec.name, "box": [rec.left, rec.top, rec.right, rec.bottom], "blocks": {}, "ranges": None, "channels": {},
                    "values": {}}
            if rec.tagged_blocks is not None:
                for k, block in rec.tagged_blocks.items():
                    name = key_of(k)
                    if name == "luni":
                        item["name"] = block.data.value if hasattr(block.data, "value") else str(block.data)
                    item["blocks"][name] = digest(payload(block, version))
            ranges = rec.blending_ranges
            if ranges is not None and ranges.composite_ranges is not None:
                item["ranges"] = [list(map(list, ranges.composite_ranges))] + [list(map(list, c)) for c in ranges.channel_ranges]
            w, h = rec.right - rec.left, rec.bottom - rec.top
            for info_c, data in zip(rec.channel_info, channels):
                cid = int(info_c.id)
                if cid < -1 or w <= 0 or h <= 0:
                    continue
                try:
                    raw = data.get_data(w, h, record.header.depth, version)
                except Exception as exc:  # (a channel psd-tools cannot decode: said)
                    raw = repr(exc).encode()
                item["channels"][str(cid)] = digest(raw)
                if samples:
                    item["values"][str(cid)] = values(raw, record.header.depth)
            out["layers"].append(item)
    return out


def main():
    samples = sys.argv[2:3] == ["--samples"]
    out = {path: view(path, samples) for path in sys.argv[3 if samples else 2:]}
    with open(sys.argv[1], "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
