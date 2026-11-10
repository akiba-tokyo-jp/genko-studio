# CPython の re（C++ へ移植）

- 取得元: CPython v3.12.3（https://github.com/python/cpython のタグ `v3.12.3`）の `Lib/re/_parser.py`・`Lib/re/_compiler.py`・`Lib/re/_casefix.py`・`Lib/re/_constants.py`、`Modules/_sre/sre_lib.h`・`Modules/_sre/sre.c`、`Modules/unicodedata.c` の名前の引き方（`_getcode`・`find_syllable`）。
- ライセンス:
  - `LICENSE`: CPython v3.12.3 の LICENSE（PSF License Agreement for Python と、B 部に CNRI の Python 1.6.1 の許諾）。
  - `LICENSE-SRE`: Secret Labs AB の著作権表示と「この SRE は CNRI's Python 1.6 license で再配布できる」旨（`Lib/re/__init__.py`・`Modules/_sre/sre.c`・`sre_lib.h` の冒頭の文面どおり）。移植した `native/src/core/pyre*.cpp`・`pyre*.hpp` の冒頭にもこの表示を残す。
  - `LICENSE-Unicode`: Unicode License v3 の全文（Unicode 15.0.0 の文字の性質・名前の表と NameAliases.txt を写した `pyre_unicode.cpp`・`data/unicode_names.bin`）。
- 目的: 台詞の一括置換（`replace_text` の `regex: true`）と台詞の検索を、参照の Python 3.12 の `re` と同じ結果にするため（docs/cpp-migration/SPEC.md COMP-01a、利用者の判断 2026-10-11）。
- 移植先: `native/src/core/pyre.cpp`（置き換えの書式 `parse_template` と照合・置換）、`pyre_parse.cpp`（`_parser.py`）、`pyre_compile.cpp`（`_compiler.py`）、`pyre_match.cpp`（`sre_lib.h` の `SRE(match)`・`SRE(search)`）。C のソースはこのフォルダーに置かない（C++ に書き直したもの）。
- Unicode の表（`native/src/core/pyre_unicode.cpp` と `native/src/core/data/unicode_names.bin`）は `tools/migration/pyre_tables.py` が参照の Python（unicodedata 15.0.0）から作る。
- 変えた所: 文字列のパターン（str）だけ（bytes と LOCALE は Python でも str では使えない）。検索の高速化（INFO の接頭辞・文字集合）は結果が同じなので移さない。照合の手数（sre の歩と、歩の間の仕事をその量だけ）に上限を付け、超えたら断る（Python には無い。SPEC COMP-01a）。sre の状態（印の配列）は 1 回の置換・検索で 1 つ（sre と同じ）。かっこの入れ子は 200 段まで（Python は約 490 段で RecursionError）。
