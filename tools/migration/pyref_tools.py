#!/usr/bin/env python3
"""What the reference Python's own modules and Pillow make, for the C++ tests that compare with them (no Genko here):

    python3 tools/migration/pyref_tools.py numbers OUT --seed N --count K
        repr, format(…, "g"), round, sum, math.hypot and math.dist of random doubles (their bits): OUT
    python3 tools/migration/pyref_tools.py json-dumps OUT --seed N --count K
        json.dumps of random values (indent 1, 2, 4 or none; ASCII or not; sorted and compact): OUT
    python3 tools/migration/pyref_tools.py json-errors OUT
        json.loads's error for each of a list of texts that are not JSON: OUT
    python3 tools/migration/pyref_tools.py make-psds CASES OUT
        the PSD and PSB files of a case file's "files" (native/tests/contract/psd_cases.json): OUT/<name>.psd
    python3 tools/migration/pyref_tools.py psd-images READS OUT
        Pillow's Image.open and load() of each of the JSON list READS ({"file", "cut"?: the file's first bytes only}):
        a PSD as one picture, or the exception: OUT

For native/tests/contract/test_contract_storage.cpp and test_contract_psd.cpp. Development only; nothing here goes into
the product. Run it with the reference Python (Pillow 12.3.0): $GENKO_PYREF.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


# --- the numbers and JSON underneath ------------------------------------------------------------------------------------


SPECIAL_FLOATS = [0.1 + 0.2, 1e-7, 1e16, 1e22, -0.0, 0.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
                  123456789.123, 1e-05, 0.0001, 1 / 3, 2.675, 0.5, 1.5, -2.5, 9007199254740993.0, 1e15]


SPECIAL_INTS = [0, 1, -1, 2**31, 2**53 + 1, -(2**62), 2**63 - 1, -(2**63), 2**64 - 1, 12345678901234]


TEXTS = ["", "東京と東京", "始めよう🙂", "a\tb\nc", "quote \" and back\\slash", "/slash/", "\u0001\u001f\u007f",
         "\u2028\u2029", "e\u0301 combining", "𠮷野家", "ｶﾀｶﾅ", "　全角スペース", "<tag>&amp;", "zero\u0000byte"]


def rand_float(rng: random.Random, geometric: bool = False) -> float:
    if geometric:
        return rng.choice([round(rng.uniform(-50, 400), rng.randrange(0, 6)), rng.uniform(0, 300), 0.1 + 0.2,
                           -0.0, 1 / 3, 2.675])
    if rng.random() < 0.4:
        return rng.choice(SPECIAL_FLOATS)
    return rng.choice([rng.uniform(-1e3, 1e3), rng.uniform(0, 1), round(rng.uniform(-1e6, 1e6), 3),
                       rng.uniform(-1, 1) * 10.0 ** rng.randint(-300, 300)])


def rand_num(rng: random.Random, geometric: bool = True):
    """An int or a float, as Python keeps either."""
    if rng.random() < 0.4:
        return rng.randint(-20, 400) if geometric else rng.choice(SPECIAL_INTS[:8])
    return rand_float(rng, geometric)


def rand_text(rng: random.Random) -> str:
    if rng.random() < 0.5:
        return rng.choice(TEXTS)
    alphabet = "abcXYZ019 あいう漢字🙂\"\\/\t\n\u00e9\u3000"
    return "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 12)))


def rand_key(rng: random.Random, avoid: set[str]) -> str:
    while True:
        key = rng.choice(["future_", "x_", "未来_", "k"]) + rand_text(rng)[:6].replace("\u0000", "")
        if key not in avoid:
            return key


def rand_json(rng: random.Random, depth: int = 0):
    kind = rng.randrange(10 if depth < 3 else 6)
    if kind == 0:
        return None
    if kind == 1:
        return rng.random() < 0.5
    if kind == 2:
        return rng.choice(SPECIAL_INTS) if rng.random() < 0.3 else rng.randint(-10**6, 10**6)
    if kind == 3:
        return rand_float(rng)
    if kind in (4, 5):
        return rand_text(rng)
    if kind in (6, 7):
        return [rand_json(rng, depth + 1) for _ in range(rng.randrange(4))]
    return {rand_key(rng, set()): rand_json(rng, depth + 1) for _ in range(rng.randrange(4))}


def bits(x: float) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", x))[0]


def rand_double(rng: random.Random) -> float:
    while True:
        r = rng.random()
        if r < 0.5:
            x = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        elif r < 0.8:
            x = rng.uniform(-1000, 1000)
        elif r < 0.9:
            x = round(rng.uniform(-1000, 1000), rng.randrange(0, 6))
        else:
            x = rng.choice(SPECIAL_FLOATS)
        if math.isfinite(x):
            return x


def numbers(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    reprs = [[bits(x), repr(x), format(x, "g")] for x in (rand_double(rng) for _ in range(count))]
    rounds = []
    for _ in range(count):
        x = rand_double(rng)
        if rng.random() < 0.5:
            x = rng.uniform(-1e4, 1e4)
        n = rng.randrange(-6, 12)
        try:
            rounds.append([bits(x), n, bits(round(x, n))])
        except OverflowError:
            rounds.append([bits(x), n, None])
    sums = []
    for _ in range(count // 4):
        items = []
        for _ in range(rng.randrange(0, 9)):
            items.append(rng.randint(-1000, 1000) if rng.random() < 0.3 else rng.choice([rng.uniform(-1e3, 1e3), 0.1, 0.2, 1e16, -1e16, 1.0, -0.0]))
        total = sum(items)
        sums.append([[["i", v] if isinstance(v, int) else ["f", bits(v)] for v in items],
                     ["i", total] if isinstance(total, int) else ["f", bits(total)]])
    hypots = []
    for _ in range(count // 4):
        a, b, c, d = (rng.uniform(-1e3, 1e3) * 10.0 ** rng.randint(-5, 5) for _ in range(4))
        hypots.append([bits(a), bits(b), bits(c), bits(d), bits(math.hypot(a, b)), bits(math.dist((a, b), (c, d)))])
    Path(out).write_text(dumps({"repr": reprs, "round": rounds, "sum": sums, "hypot": hypots}), encoding="utf-8")


def json_dumps_cases(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    cases = []
    for _ in range(count):
        value = rand_json(rng)
        if rng.random() < 0.5:
            value = {rand_key(rng, set()): rand_json(rng) for _ in range(rng.randrange(0, 6))}
        text = json.dumps(value, ensure_ascii=rng.random() < 0.3, indent=rng.choice([None, 1, 4]))
        cases.append({"text": text,
                      "indent2": json.dumps(value, ensure_ascii=False, indent=2),
                      "default": json.dumps(value, ensure_ascii=False),
                      "canonical": json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
                      "ascii": json.dumps(value, ensure_ascii=True)})
    Path(out).write_text(dumps(cases), encoding="utf-8")


JSON_ERRORS = ["", " ", "[", "]", "{", "}", "[1,]", "{\"a\":1,}", "{\"a\"", "{\"a\":", "{\"a\":1", "[1", "[1 2]",
               "{\"a\" 1}", "{1:2}", "\"abc", "\"a\\x\"", "\"\\u12\"", "\"\\u12G4\"", "\"a\nb\"", "nul", "tru", "fals",
               "-", "-a", "01", "1.", "1e", "1e+", ".5", "+1", "1 2", "{} x", "[] []", "\ufeff[]", "[\n1,\n2,\n]",
               "{\n  \"a\": [1,\n  2\n  x]}", "\"東京\" 1", "[\"東京\", ]", "NaN x", "[Infinity,",
               "{\"a\": 1, \"b\": }", "\t\r\n", "[1]\r\n x", "[\"\\ud83d\\ude00\", 1 2]", "{\"a\":1}}", "[-0, -]",
               "[1e400, 2] x", "{\"x\": \"\\t\"} \"y\""]


def json_errors(out: str) -> None:
    cases = []
    for text in JSON_ERRORS:
        try:
            json.loads(text)
            message = None
        except json.JSONDecodeError as exc:
            message = str(exc)
        cases.append([text, message])
    Path(out).write_text(dumps(cases), encoding="utf-8")


# --- PSD files and Pillow's reading of them -------------------------------------------------------------------------------


_PSD_LONG_KEYS = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"}


def _psd_rowbytes(width: int, depth: int) -> int:
    return (width * depth + 7) // 8 if depth == 1 else width * max(1, depth // 8)


def _psd_samples(seed: int, width: int, height: int, depth: int, fill=None) -> bytes:
    """One channel's samples (rows of the depth's bytes, big-endian): `fill` everywhere, or a pattern from `seed` with
    runs (for PackBits' repeats) and noise (for its literals); 32-bit floats reach below 0 and above 1, with NaN and the
    infinities now and then."""
    if width <= 0 or height <= 0:
        return b""
    rng = random.Random(seed)
    out = bytearray()
    if depth == 1:
        rowbytes = _psd_rowbytes(width, 1)
        for y in range(height):
            for x in range(rowbytes):
                out.append(fill if fill is not None else (0xFF if (x + y + seed) % 5 == 0 else rng.randrange(256) if y % 3 else 0))
        return bytes(out)
    bpp = max(1, depth // 8)
    for y in range(height):
        for x in range(width):
            if fill is not None:
                v = fill
            elif (y + seed) % 3 == 0:
                v = (x // 4) * 37 + y * 11 + seed * 5  # (runs of four)
            else:
                v = x * 9 + y * 5 + seed * 17 + rng.randrange(16)
            if depth == 32:
                if fill is not None:
                    f = float(fill)
                else:
                    r = rng.random()
                    f = float("nan") if r < 0.02 else float("inf") if r < 0.03 else float("-inf") if r < 0.04 else (v % 61) / 50.0 - 0.1
                out += struct.pack(">f", f)
            elif depth == 16:
                out += struct.pack(">H", fill if fill is not None else (v * 257 + rng.randrange(256)) & 0xFFFF)
            else:
                out += (v & 0xFF).to_bytes(1, "big") * bpp
    return bytes(out)


def _packbits(row: bytes) -> bytes:
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        run = 1
        while i + run < n and row[i + run] == row[i] and run < 128:
            run += 1
        if run >= 3:
            out += bytes([257 - run, row[i]])
            i += run
            continue
        start = i
        while i < n and i - start < 128 and not (i + 2 < n and row[i] == row[i + 1] == row[i + 2]):
            i += 1
        out += bytes([i - start - 1]) + row[start:i]
    return bytes(out)


def _psd_compressed(raw: bytes, comp: int, width: int, height: int, depth: int, psb: bool) -> bytes:
    """A channel's data as the file keeps it: its compression (2 bytes), then raw rows (0), PackBits rows after their
    byte counts (1), zlib (2), or zlib of each row's differences from the sample before (3, as psd.py undoes them)."""
    import zlib

    rowbytes = _psd_rowbytes(width, depth)
    head = struct.pack(">H", comp)
    if comp == 1:
        rows = [_packbits(raw[y * rowbytes:(y + 1) * rowbytes]) for y in range(max(0, height))]
        return head + b"".join(struct.pack(">I" if psb else ">H", len(r)) for r in rows) + b"".join(rows)
    if comp == 2:
        return head + zlib.compress(raw)
    if comp == 3:
        bpp = max(1, depth // 8)
        mod = 1 << (8 * bpp)
        out = bytearray()
        for y in range(max(0, height)):
            row = raw[y * rowbytes:(y + 1) * rowbytes]
            prev = 0
            for x in range(0, len(row), bpp):
                v = int.from_bytes(row[x:x + bpp], "big")
                out += ((v - prev) % mod).to_bytes(bpp, "big")
                prev = v
        return head + zlib.compress(bytes(out))
    return head + raw


def _psd_default_ids(mode: int) -> list:
    return [-1, 0, 1, 2, 3] if mode == 4 else [-1, 0] if mode in (0, 1, 2, 8) else [-1, 0, 1, 2]


def _psd_hex(value) -> bytes:
    return bytes.fromhex(value) if isinstance(value, str) else bytes(value)


def psd_bytes(spec: dict) -> bytes:
    """A PSD (version 1) or PSB (2) file from a spec (psd_cases.json's "files"): the header ("mode", "depth", "size",
    "channels"), colour mode data, resources (a resolution by default), the layers from the bottom ("layers": each its
    "box" [left, top, right, bottom], "channels" [{"id", "comp", "seed", "fill", "len", "hex", "hex_repeat"}], "name" (Pascal, Latin-1)
    or "name_hex", "luni", "blend", "opacity", "clip", "flags", "sig", "mask" {"box", "default", "flags", "len",
    "size"}, "ranges", "section", "text", "adjust", "blocks" [[sig, tag, hex]], "extra_len", "repeat": the record that many
    times), the merged picture
    ("merged": {"comp", "seed", "planes", "hex"} or null for none) and the file's end ("truncate": a length, or
    [mark, offset] from "records", "channel_data" or "merged"; "patch": [[offset or [mark, offset], hex]]). Lengths can be
    given wrong ("count", "li_len", "lm_len", "res_len", "colour_len", a channel's "len", a mask's "len", "extra_len").
    "hex_only": the whole file, byte for byte."""
    if "hex_only" in spec:  # (a file given byte for byte)
        return _psd_hex(spec["hex_only"])
    version = int(spec.get("version", 1))
    psb = version == 2
    mode = int(spec.get("mode", 3))
    depth = int(spec.get("depth", 8))
    width, height = spec.get("size", [16, 12])
    colour = 4 if mode == 4 else 3 if mode in (3, 7, 9) else 1
    merged = spec.get("merged", {})
    planes = merged.get("planes", colour) if isinstance(merged, dict) else colour
    nchan = int(spec.get("channels", planes))
    length = (lambda n: struct.pack(">Q", n)) if psb else (lambda n: struct.pack(">I", n))
    marks: dict = {}
    out = bytearray(b"8BPS" + struct.pack(">H", version) + b"\x00" * 6)
    out += struct.pack(">HIIHH", nchan, height, width, depth, mode)
    colour_data = _psd_hex(spec.get("colour_data", ""))
    out += struct.pack(">I", spec.get("colour_len", len(colour_data))) + colour_data
    if "resources_hex" in spec:
        resources = _psd_hex(spec["resources_hex"])
    else:
        resources = b""
        for rid, body in spec.get("resources", [[1005, struct.pack(">IHHIHH", 72 << 16, 1, 1, 72 << 16, 1, 1).hex()]]):
            data = _psd_hex(body)
            resources += b"8BIM" + struct.pack(">HH", rid, 0) + struct.pack(">I", len(data)) + data + b"\x00" * (len(data) % 2)
    out += struct.pack(">I", spec.get("res_len", len(resources))) + resources
    layers = spec.get("layers")
    if layers is None and "layer_section_hex" not in spec:
        out += length(spec.get("lm_len", 0))
    elif "layer_section_hex" in spec:
        out += _psd_hex(spec["layer_section_hex"])
    else:
        records = bytearray()
        data = bytearray()
        layers = [layer for layer in layers for _ in range(int(layer.get("repeat", 1)))]
        for layer in layers:
            left, top, right, bottom = layer.get("box", [0, 0, width, height])
            w, h = right - left, bottom - top
            chans = []
            for n, ch in enumerate(layer.get("channels") or [{"id": cid} for cid in _psd_default_ids(mode)]):
                cid = int(ch.get("id", n))
                cw, chh = w, h
                if cid == -2 and "mask" in layer:
                    ml, mt, mr, mb = layer["mask"].get("box", [left, top, right, bottom])
                    cw, chh = mr - ml, mb - mt
                if "hex" in ch:
                    payload = _psd_hex(ch["hex"])
                elif "hex_repeat" in ch:  # ([head, unit, count]: the head, then the unit `count` times)
                    head, unit, times = ch["hex_repeat"]
                    payload = _psd_hex(head) + _psd_hex(unit) * int(times)
                else:
                    fill = ch.get("fill")  # (an opaque alpha unless it is given or seeded)
                    if fill is None and cid == -1 and "seed" not in ch:
                        fill = {16: 0xFFFF, 32: 1.0}.get(depth, 255)
                    raw = _psd_samples(int(ch.get("seed", 100 * len(records) + n + 1)), cw, chh, depth, fill)
                    payload = _psd_compressed(raw, int(ch.get("comp", layer.get("comp", spec.get("comp", 1)))), cw, chh, depth, psb)
                chans.append((cid, int(ch.get("len", len(payload))), payload))
            records += struct.pack(">iiii", top, left, bottom, right) + struct.pack(">H", len(chans))
            for cid, size, _ in chans:
                records += struct.pack(">h", cid) + length(size)
            records += layer.get("sig", "8BIM").encode("latin-1") + layer.get("blend", "norm").encode("latin-1")
            records += bytes([int(layer.get("opacity", 255)), int(layer.get("clip", 0)), int(layer.get("flags", 0)), 0])
            extra = bytearray()
            if "mask" in layer:
                m = layer["mask"]
                ml, mt, mr, mb = m.get("box", [left, top, right, bottom])
                body = struct.pack(">iiii", mt, ml, mb, mr) + bytes([int(m.get("default", 0)), int(m.get("flags", 0))])
                body += b"\x00" * max(0, int(m.get("size", 20)) - len(body))
                extra += struct.pack(">I", int(m.get("len", len(body)))) + body
            else:
                extra += struct.pack(">I", 0)
            ranges = _psd_hex(layer.get("ranges", ""))
            extra += struct.pack(">I", len(ranges)) + ranges
            name = _psd_hex(layer["name_hex"]) if "name_hex" in layer else str(layer.get("name", "")).encode("latin-1")
            pascal = bytes([len(name)]) + name
            extra += pascal + b"\x00" * ((4 - len(pascal) % 4) % 4)
            blocks = [list(b) for b in layer.get("blocks", [])]
            if "luni" in layer:
                text = str(layer["luni"]).encode("utf-16-be")
                blocks.insert(0, ["8BIM", "luni", (struct.pack(">I", len(text) // 2) + text).hex()])
            if "section" in layer:
                blocks.append(["8BIM", layer.get("section_key", "lsct"), struct.pack(">I", int(layer["section"])).hex()])
            if layer.get("text"):
                blocks.append(["8BIM", "TySh", "0001"])
            if layer.get("adjust"):
                blocks.append(["8BIM", layer["adjust"], "00020000"])
            for sig, tag, body_hex in blocks:
                body = _psd_hex(body_hex)
                size = struct.pack(">Q", len(body)) if psb and tag in _PSD_LONG_KEYS else struct.pack(">I", len(body))
                extra += sig.encode("latin-1") + tag.encode("latin-1") + size + body + b"\x00" * (len(body) % 2 if layer.get("pad", True) else 0)
            records += struct.pack(">I", int(layer.get("extra_len", len(extra)))) + extra
            for _, _, payload in chans:
                data += payload
        info = struct.pack(">h", int(spec.get("count", len(layers))))
        marks["records"] = len(out) + (16 if psb else 8) + 2
        marks["channel_data"] = marks["records"] + len(records)
        info += records + data
        if len(info) % 2:
            info += b"\x00"
        section = length(int(spec.get("li_len", len(info)))) + info + _psd_hex(spec.get("global_mask", "00000000"))
        out += length(int(spec.get("lm_len", len(section)))) + section
    marks["merged"] = len(out)
    if merged is not None and "hex" in merged:  # (the merged picture's data given byte for byte)
        out += struct.pack(">H", int(merged.get("comp", 1))) + _psd_hex(merged["hex"])
    elif merged is not None:
        comp = int(merged.get("comp", 1))
        raws = [_psd_samples(int(merged.get("seed", 7000)) + c, width, height, depth) for c in range(planes)]
        if comp == 1:
            rowbytes = _psd_rowbytes(width, depth)
            rows = [_packbits(raw[y * rowbytes:(y + 1) * rowbytes]) for raw in raws for y in range(height)]
            out += struct.pack(">H", 1) + b"".join(struct.pack(">I" if psb else ">H", len(r)) for r in rows) + b"".join(rows)
        elif comp == 2:
            import zlib

            out += struct.pack(">H", 2) + zlib.compress(b"".join(raws))
        else:
            out += struct.pack(">H", comp) + b"".join(raws)
    out += _psd_hex(spec.get("append", ""))

    def at(where) -> int:
        return where if isinstance(where, int) else marks[where[0]] + int(where[1])

    for where, patch in spec.get("patch", []):
        start = at(where)
        out[start:start + len(_psd_hex(patch))] = _psd_hex(patch)
    if "truncate" in spec:
        del out[at(spec["truncate"]):]
    return bytes(out)


def make_psds(specs_path: str, out: str) -> None:
    """Each PSD of a case file's "files" ({name: spec}) written as OUT/<name>.psd (psd_bytes)."""
    files = json.loads(Path(specs_path).read_text(encoding="utf-8"))["files"]
    folder = Path(out)
    folder.mkdir(parents=True, exist_ok=True)
    for name, spec in files.items():
        (folder / f"{name}.psd").write_bytes(psd_bytes(spec))


def _image_digest(image) -> str | None:
    """The mode, size and pixels of a PIL image ("<mode>|<w>x<h>|" and its bytes, sha256), or None."""
    import hashlib

    if image is None:
        return None
    h = hashlib.sha256(f"{image.mode}|{image.size[0]}x{image.size[1]}|".encode("utf-8"))
    h.update(image.tobytes())
    return h.hexdigest()


def psd_images(reads_path: str, out: str) -> None:
    """Pillow's Image.open and load() of each of the JSON list at reads_path ({"file", "cut"?}): a PSD as one picture (PsdImagePlugin's
    merged picture): {"format", "mode", "size", "image": _image_digest, "rgba": the digest of convert("RGBA") or
    [type, message]}, or {"error": the exception's type, "message" (a BytesIO's address taken out), "stage": "open" |
    "load"}."""
    import io
    import re as regex
    import warnings

    from PIL import Image

    records = []
    for item in json.loads(Path(reads_path).read_text(encoding="utf-8")):
        data = Path(item["file"]).read_bytes()
        if "cut" in item:
            data = data[:int(item["cut"])]
        stage = "open"
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")  # (DecompressionBombWarning: only a warning)
                with Image.open(io.BytesIO(data)) as image:
                    stage = "load"
                    image.load()
                    record = {"format": image.format, "mode": image.mode, "size": list(image.size), "image": _image_digest(image)}
                    try:
                        record["rgba"] = _image_digest(image.convert("RGBA"))
                    except Exception as exc:
                        record["rgba"] = [type(exc).__name__, str(exc)]
        except Exception as exc:
            record = {"error": type(exc).__name__, "message": regex.sub(r" at 0x[0-9a-f]+>", ">", str(exc)), "stage": stage}
        records.append(record)
    Path(out).write_text(dumps(records), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pyref_tools.py")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("numbers", "json-dumps"):
        p = sub.add_parser(name)
        p.add_argument("out")
        p.add_argument("--seed", type=int, required=True)
        p.add_argument("--count", type=int, required=True)
    p = sub.add_parser("json-errors")
    p.add_argument("out")
    p = sub.add_parser("make-psds")
    p.add_argument("cases")
    p.add_argument("out")
    p = sub.add_parser("psd-images")
    p.add_argument("reads")
    p.add_argument("out")
    args = parser.parse_args(argv)
    if args.cmd == "numbers":
        numbers(args.out, args.seed, args.count)
    elif args.cmd == "json-dumps":
        json_dumps_cases(args.out, args.seed, args.count)
    elif args.cmd == "json-errors":
        json_errors(args.out)
    elif args.cmd == "make-psds":
        make_psds(args.cases, args.out)
    else:
        psd_images(args.reads, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
