# 移行台帳（自動生成）

基準コミット `1b7b1d7030d4188d7ebd5e398d010b3ed98b220f`。`tools/migration/build_ledger.py` が生成する。手で編集しない。

行数: 763。未割当: 0。

| 種別 | M1 | M2 | M3 | M4 | M5 | M6 | 計 |
|---|---|---|---|---|---|---|---|
| cli | 8 | 1 | 1 | 1 | 39 | 1 | 51 |
| export_format | 0 | 0 | 0 | 14 | 0 | 0 | 14 |
| gui_action | 0 | 52 | 103 | 27 | 7 | 11 | 200 |
| gui_action_group | 0 | 3 | 5 | 0 | 0 | 1 | 9 |
| http | 0 | 0 | 0 | 0 | 12 | 0 | 12 |
| mcp_resource | 0 | 0 | 0 | 0 | 4 | 0 | 4 |
| mcp_tool | 0 | 0 | 0 | 0 | 41 | 0 | 41 |
| op | 0 | 34 | 72 | 16 | 46 | 0 | 168 |
| python_module | 14 | 19 | 41 | 23 | 42 | 7 | 146 |
| test_file | 12 | 10 | 46 | 19 | 31 | 0 | 118 |

## 状態（C++ の木との突き合わせ）

ported は C++ のソースにあり C++ の試験（両方の版で流す契約試験の事例を含む）が指すもの、in_progress は一部だけ（足りないものを missing に）、not_applicable は Python だけの仕組み（理由を note に）、pending は見つからないもの。accepted は計画のすべての構成（Windows を含む）の `--pair` が合格したときだけ `--accepted` で付く。

| 種別 | pending | in_progress | ported | accepted | blocked | not_applicable |
|---|---|---|---|---|---|---|
| cli | 0 | 0 | 51 | 0 | 0 | 0 |
| export_format | 0 | 0 | 14 | 0 | 0 | 0 |
| gui_action | 0 | 0 | 200 | 0 | 0 | 0 |
| gui_action_group | 0 | 0 | 9 | 0 | 0 | 0 |
| http | 0 | 0 | 12 | 0 | 0 | 0 |
| mcp_resource | 0 | 0 | 4 | 0 | 0 | 0 |
| mcp_tool | 0 | 0 | 41 | 0 | 0 | 0 |
| op | 0 | 0 | 168 | 0 | 0 | 0 |
| python_module | 0 | 0 | 143 | 0 | 0 | 3 |
| test_file | 0 | 0 | 118 | 0 | 0 | 0 |

### ported でない行

- `module:app/__init__`（not_applicable）: Package docstring only ("Desktop UI."); nothing to port.
- `module:mcp/__init__`（not_applicable）: Package docstring only; the server is src/genko/mcp/server.py.
- `module:studio/__init__`（not_applicable）: Package docstring only.
