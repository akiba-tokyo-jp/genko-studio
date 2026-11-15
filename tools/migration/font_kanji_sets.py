"""font_kanji_sets.py <Unihan_OtherMappings.txt> <out.json>

The character sets native/tests/unit/test_font_faces.cpp counts each lettering face's kanji against (the catalogue's
kanji_jis_level1, kanji_jis_level2 and kanji_joyo: native/resources/fonts/FONTS.json "coverage_rules"):

  - JIS X 0208 level 1 (rows 16-47: 2,965 kanji) and level 2 (rows 48-84: 3,390), each cell read through the EUC-JP
    table (row + 0xA0, cell + 0xA0);
  - the 2,136 jōyō kanji: the code points Unihan's kJoyoKanji marks with their reading year (Unicode 17.0.0,
    Unihan_OtherMappings.txt), and for the four whose official form is not in JIS X 0208 (𠮟 塡 剝 頰) the usual form
    Unihan maps to them (叱 填 剥 頬), which counts too.

A table of code points only: nothing here draws or measures a font.
"""
import json
import sys


def jis_rows(lo, hi):
    out = []
    for row in range(lo, hi + 1):
        for cell in range(1, 95):
            try:
                ch = bytes([row + 0xA0, cell + 0xA0]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4E00 <= ord(ch) <= 0x9FFF:
                out.append(ch)
    return out


def joyo(path):
    official, usual = [], {}
    version = ""
    for line in open(path, encoding="utf-8"):
        if line.startswith("# Unicode Version"):
            version = line.split("Version", 1)[1].strip()
        if line.startswith("#") or "\tkJoyoKanji\t" not in line:
            continue
        cp, _, val = line.rstrip("\n").split("\t")
        ch = chr(int(cp[2:], 16))
        if val.startswith("U+"):
            usual[chr(int(val[2:], 16))] = ch  # the official form -> the usual (JIS X 0208) form
        else:
            official.append(ch)
    return official, usual, version


def main():
    level1, level2 = jis_rows(16, 47), jis_rows(48, 84)
    official, usual, version = joyo(sys.argv[1])
    assert len(level1) == 2965 and len(level2) == 3390, (len(level1), len(level2))
    assert len(official) == 2136 and len(usual) == 4, (len(official), len(usual))
    out = {
        "about": "The kanji a lettering face is counted against (tools/migration/font_kanji_sets.py): JIS X 0208 level 1 and "
                 "level 2 by row and cell, and the jōyō kanji of Unihan's kJoyoKanji with the usual forms that count for four of them.",
        "unihan": "Unicode " + version + " Unihan_OtherMappings.txt (kJoyoKanji)",
        "jis_level1": "".join(level1),
        "jis_level2": "".join(level2),
        "joyo": "".join(sorted(official)),
        "joyo_usual_form": dict(sorted(usual.items())),
    }
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(out, ensure_ascii=False, indent=1) + "\n")


if __name__ == "__main__":
    main()
