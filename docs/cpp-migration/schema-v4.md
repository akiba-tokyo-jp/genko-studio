# 原稿形式 v4（schema-v4）

版: 1.0（M0 で確定、M1 で実装）。C++版が書く唯一の形式。読込は v1・v2・v3・v4。

## 1. フォルダー

`.genko` は拡張子付きの**フォルダー**（ZIP ではない）。

```
<name>.genko/
  project.json              原稿の決定事項（本書 §2）
  project.lock              書込みの OS 排他（内容は表示用）
  assets/<ab>/<sha256><suffix>   内容アドレスの素材。書き換えない
  studio/journal.jsonl      保存・Undo・Redo の記録（§4）
  studio/audit.jsonl        承認の変更（永久保存。§5）
  studio/journal.partial    末尾の途中行を切り出して保管したもの（あれば）
  studio/…                  現行と同じ制作用ファイル（presence.json、requests/、logs/ など）
  legacy/                   旧形式からの変換で保全した履歴（§6）
  exports/                  書き出し先（現行と同じ）
```

素材の参照は `sha256:<64桁の小文字16進>`。パスは `assets/<先頭2桁>/<64桁><suffix>`。suffix は `.png`・`.strokes.json`・`.ops.json`・`.state.json`・`.project.json`（旧形式のスナップショット）など、`.` で始まり `/` `\` `..` を含まない。書込みは一時ファイル → flush/fsync → 同じフォルダー内で rename。既に同名があれば書かない（同じ内容）。

## 2. project.json

v3 の全フィールドを同じ名前・単位・意味で持ち、次を加える。

| キー | 型 | 意味 |
|---|---|---|
| `version` | 4 | この形式 |
| `min_reader` | 4 | 読むのに必要な最低版。これより古い読み手は開かない |
| `writer` | `{"app": "genko-native", "version": "0.1.0"}` | 最後に書いた実装 |
| `book_id` | 32桁16進 | 原稿の不変 ID（変換時・新規作成時に付与。複製では新しく付ける） |
| `features` | 文字列の配列 | この原稿が使う拡張（§3）。辞書順・重複なし |
| `revision` | int64 | **単調増加の確定世代**。新規作成は 1。保存・Undo・Redo・復旧の採用ごとに +1。過去の値を再利用しない |

v3 から外すもの: なし（未知キーも `extra` として書き戻す）。v2 の `pages[].texts`・`name_strokes`・`ink_strokes`、層の `strokes` 直書き・`raster_relpath` は書かない（読込時に v3 と同じく `layers`・素材参照へ移す）。

`project.json` の書式: UTF-8、LF、インデント 2、キーは型付きフィールドの定義順（v3 の `_payload` と同じ順。extra は先頭）、`ensure_ascii` なし。

## 3. 拡張（features）

拡張は `<名前>@<版>`。読み手は知らない拡張を含む原稿を**読み取り専用**で開き、保存を拒否する（理由を表示）。

| 拡張 | 工程 | 内容 |
|---|---|---|
| `ruby-layout@1` | M4 | 台詞の `ruby_spans`（書記素範囲 `[start,end)`、読み、書体・倍率・揃え・距離・字間）。TEXT-01 |
| `paper-texture@1` | M3 | ブラシの紙質（素材参照、濃度、倍率、回転、反転、合成、適用座標、seed）。BRUSH-01 |
| `psd-text@1` | M4 | PSD の編集可能文字の出力設定（原稿側の保存項目がある場合のみ） |

拡張を使う op が初めて適用されたときに `features` へ加える。拡張を使わない原稿は `features: []`。

## 4. 履歴（studio/journal.jsonl）

1 行 1 JSON。行は `\n` で終わる。v4 の行は `v: 4` を持つ。種類は `kind`。

### 4.1 状態

`state`: `project.json` から `revision`・`writer` を除いた内容を**正準 JSON**（キー辞書順・空白なし）にした素材（suffix `.state.json`）。同じ内容は同じ参照になる。Undo/Redo は状態で照合し、`revision` の一致では照合しない。

### 4.2 保存のトランザクション

1. 新しい素材（筆跡ブロブ、ラスター、`.state.json`、大きい op 記録 `.ops.json`）を確定する。
2. `prepare` 行を追記して fsync:
   `{"v":4,"kind":"prepare","txn":"<32桁16進>","action":"edit|undo|redo|recover","rev":N+1,"base_rev":N,"actor":"…","at":<epoch秒>,"before":"sha256:…(state)","after":"sha256:…(state)","project_sha256":"<新しい project.json バイト列の sha256 16進>","target":"<undo/redo 対象の txn>"?,"ops":[…]?,"ops_asset":"sha256:…"?,"audit":[…]?}`
3. `project.json` を一時ファイル → fsync → rename → フォルダーの fsync（POSIX）。
4. `audit` があれば `studio/audit.jsonl` に `{"v":4,"txn":…,"rev":…,"actor":…,"at":…,"changes":[…],"via"?:"undo|redo"}` を追記して fsync。
5. `commit` 行 `{"v":4,"kind":"commit","txn":…,"rev":N+1}` を追記して fsync。ここで初めて「保存済み」とする。

### 4.3 修復（起動時・保存失敗時・次の書込みの前。排他の下で行う）

- 末尾の行が途中で切れていれば、その断片を `studio/journal.partial` に追記して保全し、journal をその行の直前で切り詰める。それより前の行は消さない。
- `commit` も `abort` もない最後の `prepare` があれば:
  - 今の `project.json` の sha256 が `project_sha256` と一致 → 置換は済んでいる。`audit` を txn 単位で重複なく補い、`{"kind":"commit","txn":…,"rev":…,"recovered":true}` を追記する。
  - 一致しない → 置換は起きていない。`{"kind":"abort","txn":…}` を追記する。`project.json` は旧状態のまま。
- 同じ `txn` の再試行は一度だけ確定する。既に `commit` 済みの txn をもう一度確定しようとしたら、その確定世代を返す。
- `project.json` が必要な素材を欠く場合は、最後の整合した状態を読み取り専用で開き、空の原稿で上書きしない。

### 4.4 Undo/Redo の積み上げ

`commit` 済みの `prepare` だけを順に再生する: `edit` → Undo 列へ積み Redo 列を空にする。`undo` → Undo 列の末尾を Redo 列へ。`redo` → Redo 列の末尾を Undo 列へ。`recover` → 列を変えない。先頭に §6 の旧履歴を置く。

Undo は「今の状態 = 対象の `after`」を確かめ（違えば外部変更として拒否。`force` で強制）、対象の `actor` と違えば拒否（`force` で強制）、承認の変化を伴えば人間だけに許す。対象の `before` 状態を新しい世代で書く（§4.2 の `action: undo`）。Redo は逆。

保持: journal は削除しない。GC が守る範囲は直近 100 個の `commit` と、§6 の旧履歴全体。

## 5. 承認監査（studio/audit.jsonl）

v3 の行（`rev`・`actor`・`at`・`changes`・`via`）をそのまま残し、v4 は `txn` を加える。txn が同じ行は 1 つだけ（修復で二重に書かない）。読み手は v3 行と v4 行の両方を読む。

## 6. 旧形式からの変換

変換器は**移行元を書き換えずに別の `.genko` を作る**。

1. 移行元の排他: `project.lock` があれば OS 排他を取る（内容は書かない）。なければ作らない。どちらの場合も、取得前後で `project.json`・`studio/journal.jsonl`・`studio/audit.jsonl`・参照する素材と `pages/` のハッシュを 2 度読みして比べ、途中で変わっていたら公開せず停止する（`source_changed`）。
2. 固定コピー: 移行元を一時フォルダーに複製し、以後は複製だけを読む。
3. `project.json` を v1/v2/v3 の規則（Python の `migrate.py`・`io.py` と同じ）で読み、v4 として書く。v2 の `pages/NNN/*.png` は素材へ移す。未知キーは `extra` に保持。NaN/Infinity・欠けた素材・壊れたハッシュ・未知の版は報告し、`--accept-repairs` なしでは書かない。
4. 旧履歴の保全: 旧 `studio/journal.jsonl`・`studio/audit.jsonl`・`project.v2.bak.json` を `legacy/` へそのままのバイトで保存し、旧スナップショット（`.project.json` 素材）と、それらが参照する素材・`pages/` ラスターを新しい素材ストアへ同じ内容で取り込む。
5. 対応表 `legacy/map.json`: `{"v":4,"source_version":3,"boundary_rev":1,"entries":[{"old_rev":…,"actor":…,"at":…,"kind":"commit|undo|redo","before_old":"sha256:…","after_old":"sha256:…","before":"sha256:…(v4 state)","after":"sha256:…(v4 state)"}]}`。旧スナップショットを v4 状態へ変換した参照を持つ。旧 journal の最後の `after` と変換時の `project.json` が一致しない場合（旧版の外部変更）は `entries` を空にし、その旨を報告する。
6. 監査の連結: 旧 `audit.jsonl` の行を新しい `studio/audit.jsonl` の先頭にそのまま写し、`{"v":4,"kind":"migrated","from_version":…,"at":…,"actor":…}` を 1 行加える。過去の承認を新しい承認として作り直さない。
7. 新原稿の `revision` は 1。journal の最初の行は `action: "migrate"` の prepare/commit（Undo 列を変えない）。
8. 変換報告 `legacy/report.json` を書き、同じ内容を CLI に返す（欠けた素材、補修、未知キー、フォント、対応表の件数、所要時間）。

## 7. 新規作成

`genko new` は v4 を書く（`revision: 1`、`book_id` 新規、`features: []`、journal に `action: "edit"` の最初の commit）。

## 8. 互換の範囲

- Python 版（v3 まで）は v4 を開かない（`UnsupportedProjectVersion` で止まる）。v4 → v3 への自動変換はしない。
- 戻し方は `PLAN.md` §6。移行前の原本を Python 版で開く。
