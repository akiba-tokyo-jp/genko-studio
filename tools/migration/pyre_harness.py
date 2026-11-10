#!/usr/bin/env python3
"""The Python reference for test_contract_pyre (native/src/core/pyre: Python 3.12's re for str patterns, ported):
what re does with patterns × texts, and the Unicode tables re reads, from the reference Python. Development only.

    python3 tools/migration/pyre_harness.py tables OUT          Python's character properties for every code point (as
                                                                ranges), its case mappings (as runs), _casefix's extra
                                                                cases, and the names unicodedata.lookup takes: every
                                                                name, each again in lower case and changed a little,
                                                                with what lookup gives for it (a code point or None);
                                                                also checks native/src/core/data/unicode_names.bin
                                                                against lookup
    python3 tools/migration/pyre_harness.py cases IN OUT        each case of IN ({"pattern", "flags"?, "repl", "texts"})
                                                                run (see run_case)
    python3 tools/migration/pyre_harness.py corpus OUT --seed N --count K
                                                                K random cases (patterns made from re's grammar, some
                                                                broken on purpose, × Japanese and ASCII texts with the
                                                                characters re's cases and categories turn on), run

A case's record: {"pattern", "flags", "repl", "text", "compile": null | [type, message], "groups", "spans": [[[s, e]…]…]
| [type, message], "subn": [text, n] | [type, message], "findall": n | [type, message]}. The corpus leaves out the cases
Python takes more than a second for (counted in "too_slow": a pattern that backtracks without end, which says nothing
of the port; core/pyre refuses such a search when its budget is spent, test_pyre shows it).
"""
from __future__ import annotations

import _sre
import argparse
import json
import random
import re
import signal
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pyre_tables  # noqa: E402  (the same properties the tables are made of)


def dump(value, out: str) -> None:
    Path(out).write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def error_of(exc: BaseException) -> list:
    return [type(exc).__name__, str(exc)]


# --- tables ----------------------------------------------------------------------------------------------------------


def tables(out: str) -> None:
    result = {name: pyre_tables.ranges(test) for name, test in pyre_tables.properties().items()}
    result["lower"] = pyre_tables.runs(_sre.unicode_tolower)
    result["upper"] = pyre_tables.runs(pyre_tables.upper)
    from re._casefix import _EXTRA_CASES

    result["extra_cases"] = sorted([k, list(v)] for k, v in _EXTRA_CASES.items())
    # the names: each as it is, in lower case, and a little changed; lookup's answer for each
    rng = random.Random(20261011)
    probes: dict[str, int | None] = {}

    def probe(name: str) -> None:
        if name in probes:
            return
        try:
            found = unicodedata.lookup(name)
            probes[name] = ord(found) if len(found) == 1 else None
        except KeyError:
            probes[name] = None

    for c in range(0x110000):
        name = unicodedata.name(chr(c), None)
        if name is None:
            continue
        probe(name)
        if c % 7 == 0:
            probe(name.lower())
        if c % 13 == 0:
            probe(name + " ")
            probe(name[:-1])
            probe(name.replace(" ", "-", 1))
    blob = (ROOT / "native/src/core/data/unicode_names.bin").read_bytes()
    text = zlib.decompress(blob[4:]).decode("ascii")
    assert struct.unpack(">I", blob[:4])[0] == len(text)
    listed = 0
    for line in text.splitlines():
        code, name = line.split(";")
        assert unicodedata.lookup(name) == chr(int(code, 16)), name
        probe(name)
        probe(name.lower())
        listed += 1
    for extra in ("HANGUL SYLLABLE GA", "HANGUL SYLLABLE GAG", "HANGUL SYLLABLE XYZ", "hangul syllable ga", "HANGUL SYLLABLE ",
                  "CJK UNIFIED IDEOGRAPH-4E00", "CJK UNIFIED IDEOGRAPH-4e00", "cjk unified ideograph-4E00", "CJK UNIFIED IDEOGRAPH-4E0",
                  "CJK UNIFIED IDEOGRAPH-2A6DF", "CJK UNIFIED IDEOGRAPH-2A6E0", "CJK UNIFIED IDEOGRAPH-323AF", "CJK UNIFIED IDEOGRAPH-0004E00",
                  "LATIN CAPITAL LETTER GHA", "BYTE ORDER MARK", "BOM", "NBSP", "LATIN SMALL LETTER A WITH MACRON AND GRAVE",
                  "TANGUT IDEOGRAPH-17000", "KHITAN SMALL SCRIPT CHARACTER-18B00", "", " ", "A" * 300, "SPACE\x00",
                  "ＳＰＡＣＥ", "SPACEé", "CJK COMPATIBILITY IDEOGRAPH-F900", "cjk compatibility ideograph-f900"):
        probe(extra)
    for _ in range(3000):
        probe("".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ -") for _ in range(rng.randint(1, 30))))
    result["names"] = [[name, cp] for name, cp in probes.items()]
    result["listed"] = listed
    dump(result, out)


# --- cases -----------------------------------------------------------------------------------------------------------


class TooSlow(Exception):
    """A case Python took more than its second for (timed_case)."""


def run_case(pattern: str, flags: int, repl: str, text: str) -> dict:
    out = {"pattern": pattern, "flags": flags, "repl": repl, "text": text}
    try:
        compiled = re.compile(pattern, flags)
    except TooSlow:
        raise
    except Exception as exc:  # re.error, OverflowError, ValueError
        out["compile"] = error_of(exc)
        return out
    out["compile"] = None
    out["groups"] = compiled.groups
    try:
        out["spans"] = [[list(m.span(g)) for g in range(compiled.groups + 1)] for m in compiled.finditer(text)]
    except TooSlow:
        raise
    except Exception as exc:
        out["spans"] = error_of(exc)
    try:
        new, n = compiled.subn(repl, text)
        out["subn"] = [new, n]
    except TooSlow:
        raise
    except Exception as exc:
        out["subn"] = error_of(exc)
    try:
        out["findall"] = len(compiled.findall(text))
    except TooSlow:
        raise
    except Exception as exc:
        out["findall"] = error_of(exc)
    return out


def cases(path: str, out: str) -> None:
    records = []
    for case in json.loads(Path(path).read_text(encoding="utf-8"))["cases"]:
        flags = int(case.get("flags", 0))
        repls = case.get("repls", [case.get("repl", "<\\g<0>>")])
        for text in case["texts"]:
            for repl in repls:
                records.append(run_case(case["pattern"], flags, repl, text))
    dump(records, out)


# --- the random corpus -------------------------------------------------------------------------------------------------

# characters re treats in its own ways: cases (ſ K İ ı ß Σ ς σ µ ϐ 𐐀 ǅ ﬅ ẞ Ω), categories (fullwidth and Arabic digits,
# spaces, underscores, combining marks), lines (\n \r U+2028 U+0085), and Japanese
ODD = "".join(map(chr, (0xFF10, 0xFF11, 0x0663, 0x3000, 0xA0, 0x2028, 0x85, 0x0301, 0x212A)))
ALPHABET = ("abcABCxyzXYZ019_- .,!?\n\t\r" + "ſKİıßΣςσµμϐβǅǆﬅﬆẞΩω𐐀𐐨" + ODD +
            "あいうえおアイウエオかきくけこ東京大阪台詞！？「」、。ー〜…々漢字" + "😀🎉")
# what a valid pattern is made of, and what breaks one (each error re gives)
PIECES = ["a", "b", "x", "A", "ß", "σ", "Σ", "ſ", "K", "İ", "ı", "あ", "台詞", "東京", "。", "！", " ", "\\.", "\\n", "\\t", "\\x41",
          "\\u3042", "\\U0001F600", "\\N{HIRAGANA LETTER A}", "\\N{LATIN SMALL LETTER SHARP S}", "\\N{latin capital letter k}",
          "\\101", "\\0", "\\d", "\\D", "\\w", "\\W", "\\s", "\\S", ".", "\\\\", "\\-", "\\あ", "\\{", "{", "}", "]", "-"]
ANCHORS = ["\\b", "\\B", "\\A", "\\Z", "^", "$"]
BAD_PIECES = ["\\N{nope}", "\\q", "\\8", "\\x4", "\\u30", "\\U0011FFFF", "\\N", "\\N{", "\\777", "\\", "*", "+", "?", "{1}", ")"]
CLASSES = ["[abc]", "[^abc]", "[a-z]", "[A-Z0-9_]", "[\\d\\s]", "[^\\W\\d]", "[あ-ん]", "[ア-ン]", "[一-龥]", "[]a]", "[^]]", "[a-]",
           "[-a]", "[\\]]", "[[]", "[ß-ſ]", "[σΣ]", "[\\x00-\\x7f]", "[\\u3040-\\u309f]", "[𐐀-𐐏]", "[^\\n]", "[.]", "[kK]", "[ǅ]",
           "[ﬅﬆ]", "[İi]", "[\\N{HIRAGANA LETTER A}-\\N{HIRAGANA LETTER O}]", "[\\u0100-\\u024f]", "[\\u0400-\\u052f]",
           "[\\u1c80-\\u1c88]", "[\\U00010400-\\U0001044f]", "[\\w&&]", "[a--]", "[\\b]", "[\\s\\S]", "[^\\x00-\\uffff]"]
BAD_CLASSES = ["[z-a]", "[\\d-z]", "[", "[a", "[\\A]", "[\\8]", "[a-\\d]", "[\\x0g]"]
QUANTS = ["*", "+", "?", "*?", "+?", "??", "*+", "++", "?+", "{2}", "{1,3}", "{,2}", "{2,}", "{0}", "{1,3}?", "{2}+", "{,}", "{a}",
          "{1,"]
BAD_QUANTS = ["{3,1}", "**", "{99999999999}", "{4294967295}", "*??", "+*"]
FLAGS = ["(?i)", "(?m)", "(?s)", "(?x)", "(?a)", "(?u)", "(?ims)", "(?im)(?s)", "(?t)"]
BAD_FLAGS = ["(?L)", "(?a)(?u)", "(?q)", "(?i", "(?", "(?-i)", "(?au)", "(?i-:", "(?i-q:", "a(?i)"]
TEXTS_SHORT = ["", "a", "ab", "aaa", "abab", "aXbXc", "ABC abc", "a\nb\nc", "ab\n", "\n", "ßẞss", "ſsS", "ΣσςΣ", "Kk" + chr(0x212A),
               "İiıI", "µμΜ", "ǅǆǄ", "ﬅﬆst", "𐐀𐐨", "台詞と台詞", "東京と大阪", "あいう、アイウ。", "「おはよう！」",
               "".join(map(chr, (0xFF10, 0xFF11, 0x0663))) + "3", "a_b c" + chr(0x3000) + "d", "x" + chr(0x2028) + "y\x85z\r\n",
               "e" + chr(0x0301), "😀a🎉", "aaaaaaaaaaaab", "abcabcabc", "ab-cd", "{1}", "a{2}", "[x]"]


def random_text(rng: random.Random) -> str:
    if rng.random() < 0.5:
        return rng.choice(TEXTS_SHORT)
    return "".join(rng.choice(ALPHABET) for _ in range(rng.randint(0, 16)))


class Maker:
    """Patterns from re's grammar: most of them valid (groups referred to once closed, look-behinds of one width), some
    broken on purpose (bad = True)."""

    def __init__(self, rng: random.Random) -> None:
        self.rng = rng
        self.bad = False
        self.groups = 0
        self.closed: list[int] = []
        self.names: list[str] = []

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def atom(self, depth: int) -> tuple[str, bool]:
        """(text, whether a quantifier may follow)"""
        rng = self.rng
        if self.bad and self.chance(0.08):
            return rng.choice(BAD_PIECES + BAD_CLASSES), True
        r = rng.random()
        if depth < 3 and r < 0.28:
            return self.group(depth + 1)
        if r < 0.42:
            return rng.choice(CLASSES), True
        if r < 0.48 and self.closed:
            g = rng.choice(self.closed)
            names = [n for n in self.names]
            return rng.choice([f"\\{g}", f"(?P={rng.choice(names)})" if names else f"\\{g}"]), True
        if r < 0.55:
            return rng.choice(ANCHORS), False
        return rng.choice(PIECES), True

    def piece(self, depth: int) -> str:
        atom, can_repeat = self.atom(depth)
        if (can_repeat or self.bad) and self.chance(0.35):
            atom += self.rng.choice(BAD_QUANTS if self.bad and self.chance(0.2) else QUANTS)
        return atom

    def sequence(self, depth: int) -> str:
        return "".join(self.piece(depth) for _ in range(self.rng.randint(0, 3)))

    def alternation(self, depth: int) -> str:
        return "|".join(self.sequence(depth) for _ in range(self.rng.randint(1, 3)))

    def group(self, depth: int) -> tuple[str, bool]:
        rng = self.rng
        kind = rng.randint(0, 12)
        if kind <= 3:
            self.groups += 1
            g = self.groups
            inner = self.alternation(depth)
            self.closed.append(g)
            return "(" + inner + ")", True
        if kind == 4:
            self.groups += 1
            g = self.groups
            name = rng.choice(["n", "名前", "x", "_", "Σ"]) + str(g)
            inner = self.alternation(depth)
            self.closed.append(g)
            self.names.append(name)
            return f"(?P<{name}>" + inner + ")", True
        if kind == 5:
            return "(?:" + self.alternation(depth) + ")", True
        if kind == 6:
            return "(?>" + self.alternation(depth) + ")", True
        if kind == 7:
            return rng.choice(["(?=", "(?!"]) + self.alternation(depth) + ")", self.bad
        if kind == 8:
            inner = rng.choice(["a", "ab", "\\d", "[あい]", "x|y", "東京|大阪", "(?:a|b)c", "\\b", "^", ".", "台."])
            if self.bad and self.chance(0.5):
                inner = rng.choice(["a*", "x|yz", "\\1", "a{1,2}"])
            return rng.choice(["(?<=", "(?<!"]) + inner + ")", self.bad
        if kind == 9:
            flags = rng.choice(["(?i:", "(?-i:", "(?s:", "(?m:", "(?x:", "(?a:", "(?i-s:", "(?-x:", "(?u:", "(?ms-i:"])
            if self.bad and self.chance(0.4):
                flags = rng.choice(["(?L:", "(?-a:", "(?i-i:", "(?t:", "(?-:", "(?i-u:"])
            return flags + self.alternation(depth) + ")", True
        if kind == 10 and (self.closed or self.bad):
            target = str(rng.choice(self.closed)) if self.closed else "1"
            if self.names and self.chance(0.3):
                target = rng.choice(self.names)
            if self.bad and self.chance(0.4):
                target = rng.choice(["0", "99", "x y", "zz", "1a"])
            no = "|" + self.sequence(depth) if self.chance(0.6) else ""
            return f"(?({target})" + self.sequence(depth) + no + ")", True
        if kind == 11:
            return "(?#" + rng.choice(["comment", "あ", "(x"]) + ")", False
        if self.bad:
            return rng.choice(["(" + self.alternation(depth), self.alternation(depth) + ")", "(?P<1>a)", "(?P<n>a)(?P<n>b)", "(?<a)",
                               "(?Px)", "(?P=1)", "(a)(?(1)b|c|d)"]), True
        return "(?:" + self.alternation(depth) + ")", True

    def pattern(self) -> str:
        rng = self.rng
        self.bad = self.chance(0.25)
        self.groups = 0
        self.closed = []
        self.names = []
        head = ""
        if self.chance(0.2):
            head = rng.choice(BAD_FLAGS if self.bad and self.chance(0.4) else FLAGS)
        text = head + self.alternation(0)
        if self.chance(0.05):  # verbose, with spaces and a comment
            text = "(?x)" + text + " # note\n " + self.sequence(0)
        if self.bad and self.chance(0.3):
            at = rng.randint(0, len(text))
            text = text[:at] + rng.choice("()[]{}?*+|\\^$.-") + text[at:]
        return text or "a"

    def repl(self) -> str:
        rng = self.rng
        g = self.groups
        good = ["", "x", "<\\g<0>>", "\\g<0>\\g<0>", "\\n", "\\t-\\\\-", "置換", "\\0\\101", "[\\" + str(g) + "]" if g else "[\\g<0>]",
                "<\\g<" + str(g) + ">>", ("(\\g<" + self.names[0] + ">)") if self.names else "<\\g<0>>", "\\1" if g else "\\g<0>",
                "\\2\\1" if g >= 2 else "x\\g<0>"]
        bad = ["\\99", "\\g<", "\\q", "a\\", "\\g<zz>", "\\g<1a>", "\\g<" + str(g + 1) + ">", "\\" + str(g + 1), "\\g<-1>"]
        return rng.choice(bad if self.chance(0.12) else good)


def timed_case(pattern: str, flags: int, repl: str, text: str) -> dict | None:
    """run_case, or None when Python takes more than a second (a pattern that backtracks without end: such a case says
    nothing of the port, and core/pyre refuses it once its budget is spent; test_pyre has those)."""
    def stop(signum, frame):
        raise TooSlow()

    previous = signal.signal(signal.SIGALRM, stop)
    signal.setitimer(signal.ITIMER_REAL, 1.0)
    try:
        return run_case(pattern, flags, repl, text)
    except TooSlow:
        return None
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def corpus(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    maker = Maker(rng)
    records = []
    slow = 0
    while len(records) < count:
        pattern = maker.pattern()
        flags = rng.choice([0, 0, 0, re.IGNORECASE])
        repl = maker.repl()
        for _ in range(rng.randint(1, 3)):
            record = timed_case(pattern, flags, repl, random_text(rng))
            if record is None:
                slow += 1
            else:
                records.append(record)
    dump({"records": records[:count], "too_slow": slow}, out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("tables")
    p.add_argument("out")
    p = sub.add_parser("cases")
    p.add_argument("cases")
    p.add_argument("out")
    p = sub.add_parser("corpus")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=1000)
    args = parser.parse_args(argv)
    assert sys.version_info[:2] == (3, 12), "run with the reference Python 3.12"
    if args.cmd == "tables":
        tables(args.out)
    elif args.cmd == "cases":
        cases(args.cases, args.out)
    else:
        corpus(args.out, args.seed, args.count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
