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

状態は ledger.json の `status`。各工程の出口で `accepted` まで更新し、`pending` が残る工程は完了扱いにしない。
