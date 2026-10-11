# CPython の re（C++ へ移植）

- 取得元: CPython v3.12.3（https://github.com/python/cpython のタグ `v3.12.3`）の `Lib/re/_parser.py`・`Lib/re/_compiler.py`・`Lib/re/_casefix.py`・`Lib/re/_constants.py`、`Modules/_sre/sre_lib.h`・`Modules/_sre/sre.c`、`Modules/unicodedata.c` の名前の引き方（`_getcode`・`find_syllable`）。
- ライセンス: `LICENSE`（PSF License Agreement for Python。Secret Labs AB の sre の許諾を含む）。
- 目的: 台詞の一括置換（`replace_text` の `regex: true`）と台詞の検索を、参照の Python 3.12 の `re` と同じ結果にするため（docs/cpp-migration/SPEC.md COMP-01a、利用者の判断 2026-10-11）。
- 移植先: `native/src/core/pyre.cpp`（置き換えの書式 `parse_template` と照合・置換）、`pyre_parse.cpp`（`_parser.py`）、`pyre_compile.cpp`（`_compiler.py`）、`pyre_match.cpp`（`sre_lib.h` の `SRE(match)`・`SRE(search)`）。C のソースはこのフォルダーに置かない（C++ に書き直したもの）。
- Unicode の表（`native/src/core/pyre_unicode.cpp` と `native/src/core/data/unicode_names.bin`）は `tools/migration/pyre_tables.py` が参照の Python（unicodedata 15.0.0）から作る。
- 変えた所: 文字列のパターン（str）だけ（bytes と LOCALE は Python でも str では使えない）。検索の高速化（INFO の接頭辞・文字集合）は結果が同じなので移さない。照合の手数に上限を付け、超えたら断る（Python には無い。SPEC COMP-01a）。かっこの入れ子は 200 段まで（Python は約 490 段で RecursionError）。
