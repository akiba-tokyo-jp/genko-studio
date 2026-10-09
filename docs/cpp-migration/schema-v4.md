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
| `native.color_raster_v1` / `native.color_raster_tiles_v1` | M3 | 高精度カラー画素とそのタイル保存（下の節） |

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

### 4.5 実装で確定した細則（M1）

- `rev` は確定世代。中断された `prepare`（abort または修復で abort した txn）の `rev` も再利用しないため、確定世代は飛ぶことがある（単調増加は保つ）。
- `abort` 行と、修復で書く `commit` 行（`recovered: true`）にも `v: 4` を付ける。
- 旧履歴（§6）を対象にする Undo/Redo の `target` は `legacy:<map.json の entries の番号>`。
- v4 の監査行の `rev` は、その変更を確定した世代（Undo/Redo なら Undo/Redo 自身の世代）。対象の世代は `target` で辿る。変換の `migrated` 行は `txn` を持つ。
- 状態素材は正準 JSON（キー辞書順）なので、Undo/Redo で戻した後の `project.json` では、自由形式の辞書（`panel`・`studio`・`extra` 等）のキー順が辞書順になる。内容と、型付きの項目の順序は変わらない。
- 大きい op 記録を `.ops.json` に分ける基準は Python と同じく正準 JSON の**文字数** 16000 超。
- 変換で使えない旧スナップショット（Python の gc で消えた、または補修が必要）は変換を止めず、`legacy/report.json` に記録する。その地点への Undo は拒否する。
- 変換は `project.lock` 以外の全ファイルを二度読みして比べ、`studio/*`・`exports/` などのその他のファイルは同じバイトで新しい原稿へ持ち越す。中断した変換の作業フォルダー `.<名前>.migrating-*` は出力先の隣に残る（次の変換の前に削除してよい）。
- 書込みの排他は待たずに失敗する（CLI・API。Python と同じ）。GUI は `ProjectLock::acquire(timeout)` で待つ。

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

## カラーペン線（native.color_stroke_v1）

高精度 `convert_layer(to="pen")` は `preserve_precision: true` を明示する。従来の省略呼出しは、8bitへ精度喪失し得る変換の拒否契約を維持する。GUIの「ペンレイヤーに変換」は必ずtrueで呼び、カラー線機能を宣言して保存する。マスク・効果・スクリーン・色指定・パッチ・既存線・panel_eachが付く高精度元画像は、まだ正確なペン変換ができないため原本とbatch全体を変更せず拒否する。

既存の精密カラーペン層に `convert_layer(to="pen")` を再送した場合、RGB8へ再トレースせず、線と精密色を無変更で保持する。アフィン・遠近・メッシュの線変形は `color_rgb` を保持する。

## 高精度カラー画素（native.color_raster_v1、native.color_raster_tiles_v1）

層の `color_raster` は straight sRGB の RGBA16（u16）または RGBA32F（f32、HDR可）で、ページ全面に引き伸ばして表示する。資産は GKCR（`"GKCR"`、版1、標本バイト数2/4、予約0、LE幅・高さ、LE標本）で拡張子 `.colorrgba`。

- 保存は256px角のタイル単位（M3①-1）: `{"tiles": [ref…（行ごと）], "tile": 256, "width", "height", "precision", "space": "srgb", "alpha": "straight"}`。各タイルはそれ自体がGKCR資産で、内容アドレスのため変更のないタイルは共有される。一部を描き替えた保存は触れたタイルだけを増やす（履歴が全面の複製で膨らまない）。`features` に `native.color_raster_tiles_v1` を加える。
- 旧形式 `{"asset": ref, …}`（全面1資産）も読む。
- 消しゴム・範囲削除・移動/変形/ワープ・貼り付け、ペン・塗り・グラデーション等の描込み、マスク（表示・結合）は画素の精度のまま行う（`render/color_edit`）。道具の届かない画素はバイト不変。描込みは層に線・塗りを残さず画素へ焼き込む。

`features`へ`native.color_stroke_v1`を登録する。未対応の読み手は従来の未知feature規則により編集・保存を拒否する。既存のRGB8線とモノクロ線の形式は変えない。

線の辞書形式・packed形式に任意の`color_rgb`を加える。値は`{"precision":"u16"|"f32","values":[r,g,b]}`。RGBはstraight sRGBの正規化値（16bitは整数値/65535、32bitは有限のfloat32範囲、HDRを保持）。値は厳密に3個、未知キー・未知precision・非有限値を拒否する。`opacity`は従来どおり独立の0〜1のalpha。`color_rgb`がある線は`rgb`の8bit previewを色の正本にしない。線の座標・幅・筆圧は既存形式のまま編集可能とする。

## 紙質（paper-texture@1、BRUSH-01）

ブラシの紙質。C++版だけの機能で、Python版の原稿・ブラシの意味は変えない（紙質のないブラシの設定・描画は従来どおり）。

- 原稿のブラシ `brush.custom[<key>]` に任意の `paper` を加える。値は次のキーをこの順で持つオブジェクト。未知キー・範囲外・非有限値・bool の数値は拒否する。
  `{"asset": "sha256:…", "density": 0..1 (0.5), "scale": 0.1..10 (1), "rotation": -360..360 (0, 度・紙面で時計回り), "flip_x": bool, "flip_y": bool, "invert": bool, "blend": "multiply"|"subtract", "coords": "paper"|"stroke", "seam": "repeat"|"mirror", "seed": 0..2147483647 (0)}`
- `asset` は紙の画像（グレーの PNG、`assets/<ab>/<64桁>.png`）。取り込んだ PNG・JPEG・BMP・GIF を白の上に重ねて（透明は白い紙＝インクを取らない）Pillow の `convert("L")` と同じ式でグレーにしたもの（16 ビットのグレーは上位 8 ビット）。縦横 4096 画素まで（見出しで判定）、元のファイルは 64 MiB まで。紙質として扱うのは `asset` を持つオブジェクトだけ（他の書き手が残した `paper` は Python と同じく紙質ではない）。元のファイルの場所は記録しない（消しても別 PC でも同じ）。書き手はブラシが参照する紙の画像だけを書き、読み手はハッシュを照合し、描ける画像か（復号・大きさ）を確かめる。欠けた・名前と中身の違う・描けない画像と読めない設定は報告して読取専用で開く（紙質なしや別の画像で描かない）。
- 倍率 1 で画像の 1 画素が 1/300 インチ。`coords: "paper"` は紙面の mm に固定（表示の拡大・縮小や解像度で紙目の物理サイズは変わらない。重なった線は同じ紙目）、`"stroke"` は線の最初の点から測り、線の id ごとに始まりをずらす（消しゴムで線を切ると、切れた線はそれぞれ新しい id と最初の点を持つので、紙目はそこで新しく始まる。切っても紙目を保つには紙面固定を使う）。
- 合成順序（`render/paper.hpp`）: ブラシ自身の被覆（先端・スタンプ・質感・縁）→ 筆圧での薄さ（`pressure_opacity`）→ 紙質 → 線の不透明度（線×ブラシ）→ 同じ色の線をまとめてレイヤーへ。
- 紙質: 画素 (X, Y) の中心 `x_mm = (X + 0.5) × 25.4 / dpi`（y も）から基準点（paper: 0、stroke: 線の最初の点）を引き、`-rotation` 回して `u = (dx·cos + dy·sin) / (scale × 25.4 / 300)`、`v = (dy·cos − dx·sin) / …`、反転は符号。`U = floor((u − 0.5) × 256) + ou × 256`（V も）。`(ou, ov)` は seed（stroke は線の id の FNV-1a 64 と xor）の SplitMix64 を画像の幅・高さで割った余り。周りの 4 画素を 1/256 の重みで混ぜ（四捨五入）、継ぎ目は繰り返しか折り返し。`invert` で 255 − 値。取るインク `A = ((255 − 値) × floor(density × 255 + 0.5) + 127) / 255`、乗算 `被覆 × (255 − A) / 255`（四捨五入）、減算 `max(0, 被覆 − A)`。sin・cos は 90 度ごとは厳密、残りは固定の多項式（+−×÷ だけ）。どの OS でも同じ画素。
- 拡張を使う op が初めて適用されたとき（`define_brush` の `paper`）に `features` へ `paper-texture@1` を加える。書き手もブラシが紙質を持てば加える。紙質を使わない原稿は変わらない。
- op: `define_brush` の `paper` は上の設定に加えて、画像を `"png"`（画像ファイルの base64。`asset` とどちらか一つ）で受ける。履歴の op 記録では `png` を `"<N base64 chars>"` に縮める。`genko schema`（公開スキーマ）は Python のまま。
- 自分のブラシのライブラリ: Python 版と共有する `brushes.json` には紙質を書かない（Python 版の編集が `paper` を落とすため）。C++ 版は隣の `brush_papers.json`（`{"genko_brush_papers": 1, "papers": {<key>: 紙質}}`）と `brush_papers/<64桁>.png` に置く（`brushes.json` に無くなったブラシの紙質は次の保存で落とし、どれも名指さない画像は 1 時間を過ぎたものを消す）。`.genkobrush` は紙質の画像を `paper.png`（base64）で含む。
