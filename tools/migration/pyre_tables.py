#!/usr/bin/env python3
"""The Unicode tables of native/src/core/pyre (Python 3.12's re for str patterns, ported), written from the reference
Python's own database (CPython 3.12, unicodedata 15.0.0) — never from this build's Qt, whose Unicode is newer.

    python3 tools/migration/pyre_tables.py write NAMEALIASES   # native/src/core/pyre_unicode.cpp and
                                                               # native/src/core/data/unicode_names.bin
    python3 tools/migration/pyre_tables.py check NAMEALIASES   # the two files are what this Python writes

NAMEALIASES: the Unicode Character Database's NameAliases.txt of 15.0.0 (https://www.unicode.org/Public/15.0.0/ucd/
NameAliases.txt, sha256 3e39509e…18e8): Python's unicodedata.lookup takes these names too but cannot list them, so
they are read from the file and each is checked against unicodedata.lookup.

What is written:
  - lower, upper: _sre.unicode_tolower (Py_UNICODE_TOLOWER) and Py_UNICODE_TOUPPER (the first character of
    str.upper(), which is what Py_UNICODE_TOUPPER gives for a character whose uppercase is longer), as runs;
  - cased (_sre.unicode_iscased), decimal (\\d), space (\\s), alnum (\\w with "_"), linebreak, alpha, printable,
    xid_start and xid_continue (str.isidentifier), as ranges;
  - unicode_names.bin: every name unicodedata.name gives but the ones made from the code point (HANGUL SYLLABLE …,
    CJK UNIFIED IDEOGRAPH-…), and the aliases: "<hex>;<NAME>\\n" lines, zlib-compressed after a 4-byte big-endian
    length (Qt's qCompress format).
Development only: nothing here goes into the product but the files it writes.
"""
from __future__ import annotations

import _sre
import hashlib
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "native/src/core/pyre_unicode.cpp"
NAMES = ROOT / "native/src/core/data/unicode_names.bin"
ALIASES_SHA256 = "3e39509e8fae3e5d50ba73759d0b97194501d14a9c63107a6372a46b38be18e8"
LINEBREAKS = {c for c in range(0x110000) if len(("a" + chr(c) + "b").splitlines()) == 2}


def upper(c: int) -> int:
    return ord(chr(c).upper()[0])


def properties() -> dict:
    """Each property as Python has it, for every code point."""
    return {
        "cased": lambda c: _sre.unicode_iscased(c),
        "decimal": lambda c: chr(c).isdecimal(),
        "space": lambda c: chr(c).isspace(),
        "alnum": lambda c: chr(c).isalnum(),
        "linebreak": lambda c: c in LINEBREAKS,
        "alpha": lambda c: chr(c).isalpha(),
        "printable": lambda c: chr(c).isprintable(),
        "xid_start": lambda c: c != 0x5F and chr(c).isidentifier(),
        "xid_continue": lambda c: ("a" + chr(c)).isidentifier(),
    }


def ranges(test) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    start = None
    for c in range(0x110001):
        inside = c < 0x110000 and test(c)
        if inside and start is None:
            start = c
        elif not inside and start is not None:
            out.append((start, c - 1))
            start = None
    return out


def runs(mapping) -> list[tuple[int, int, int, int]]:
    """(first, last, step, delta): first, first + step, …, last each map to itself + delta (step 1 or 2)."""
    changed = [(c, mapping(c) - c) for c in range(0x110000) if mapping(c) != c]
    out: list[list[int]] = []
    for c, delta in changed:
        if out:
            first, last, step, d = out[-1]
            if d == delta and ((first == last and c - last in (1, 2)) or (first != last and c - last == step)):
                out[-1] = [first, c, c - last, d]
                continue
        out.append([c, c, 1, delta])
    return [tuple(r) for r in out]


def cpp_text() -> str:
    lines = [
        "// Made by tools/migration/pyre_tables.py from the reference Python (CPython 3.12, unicodedata 15.0.0): do not edit.",
        "// The character properties Python's re reads (Modules/_sre: sre_lower_unicode, sre_upper_unicode, SRE_UNI_IS_*),",
        "// _sre.unicode_iscased, str.isidentifier for group names, str.isalpha for the inline flags' errors and",
        "// str.isprintable for repr() in the errors; test_contract_pyre checks every code point against Python.",
        "",
        '#include "core/pyre_internal.hpp"',
        "",
        "#include <algorithm>",
        "#include <iterator>",
        "",
        "namespace genko::core::pyre {",
        "",
        "namespace {",
        "",
        "struct CaseRun {",
        "    char32_t first;",
        "    char32_t last;",
        "    std::uint8_t step;",
        "    std::int32_t delta;",
        "};",
        "",
        "struct Range {",
        "    char32_t first;",
        "    char32_t last;",
        "};",
        "",
    ]

    def table(name: str, items: list, kind: str) -> None:
        lines.append(f"constexpr {kind} {name}[] = {{")
        row = "   "
        for item in items:
            cell = " {" + ", ".join(f"0x{v:x}" if i < 2 else str(v) for i, v in enumerate(item)) + "},"
            if len(row) + len(cell) > 118:
                lines.append(row)
                row = "   "
            row += cell
        if row.strip():
            lines.append(row)
        lines.append("};")
        lines.append("")

    table("kLower", runs(_sre.unicode_tolower), "CaseRun")
    table("kUpper", runs(upper), "CaseRun")
    for name, test in properties().items():
        table("k" + "".join(part.capitalize() for part in name.split("_")), ranges(test), "Range")
    lines += [
        "char32_t mapped(const CaseRun* begin, const CaseRun* end, char32_t c) {",
        "    const CaseRun* at = std::upper_bound(begin, end, c, [](char32_t value, const CaseRun& run) { return value < run.first; });",
        "    if (at == begin) return c;",
        "    --at;",
        "    if (c > at->last || (c - at->first) % at->step != 0) return c;",
        "    return static_cast<char32_t>(static_cast<std::int64_t>(c) + at->delta);",
        "}",
        "",
        "bool within(const Range* begin, const Range* end, char32_t c) {",
        "    const Range* at = std::upper_bound(begin, end, c, [](char32_t value, const Range& range) { return value < range.first; });",
        "    return at != begin && c <= (at - 1)->last;",
        "}",
        "",
        "}  // namespace",
        "",
        "char32_t lower(char32_t c) { return mapped(std::begin(kLower), std::end(kLower), c); }",
        "char32_t upper(char32_t c) { return mapped(std::begin(kUpper), std::end(kUpper), c); }",
    ]
    for name in properties():
        table_name = "k" + "".join(part.capitalize() for part in name.split("_"))
        lines.append(f"bool {name}(char32_t c) {{ return within(std::begin({table_name}), std::end({table_name}), c); }}")
    lines += ["", "}  // namespace genko::core::pyre", ""]
    return "\n".join(lines)


def aliases(path: Path) -> list[tuple[int, str]]:
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != ALIASES_SHA256:
        raise SystemExit(f"{path} is not NameAliases.txt of Unicode 15.0.0")
    out = []
    for line in data.decode("utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        code, name, _kind = line.split(";")
        cp = int(code, 16)
        if unicodedata.lookup(name) != chr(cp):
            raise SystemExit(f"unicodedata.lookup({name!r}) is not U+{cp:04X}")
        out.append((cp, name))
    return out


def names_blob(alias_file: Path) -> bytes:
    items = []
    for c in range(0x110000):
        name = unicodedata.name(chr(c), None)
        if name is None or name.startswith("HANGUL SYLLABLE ") or name.startswith("CJK UNIFIED IDEOGRAPH-"):
            continue
        items.append((c, name))
    items += aliases(alias_file)
    text = "".join(f"{c:X};{name}\n" for c, name in items).encode("ascii")
    return struct.pack(">I", len(text)) + zlib.compress(text, 9)


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[1] not in ("write", "check"):
        print(__doc__)
        return 2
    assert sys.version_info[:2] == (3, 12) and unicodedata.unidata_version == "15.0.0", "run with the reference Python 3.12"
    cpp = cpp_text().encode("utf-8")
    blob = names_blob(Path(argv[2]))
    if argv[1] == "write":
        CPP.write_bytes(cpp)
        NAMES.write_bytes(blob)
        print(f"wrote {CPP} ({len(cpp)} bytes) and {NAMES} ({len(blob)} bytes)")
        return 0
    same = CPP.read_bytes() == cpp and NAMES.read_bytes() == blob
    print("the same" if same else "different")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
