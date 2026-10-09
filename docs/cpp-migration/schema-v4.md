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
| `ruby-layout@1` | M4 | 台詞の `ruby_spans`（書記素範囲 `[start,end)`、読み）と、台詞の `style` の書体・倍率・揃え・距離・字間。TEXT-01（末尾の「ルビの位置と組み方」） |
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
- 自分のブラシのライブラリ: Python 版と共有する `brushes.json` には紙質を書かない（Python 版の編集が `paper` を落とすため）。C++ 版は隣の `brush_papers.json`（`{"genko_brush_papers": 1, "papers": {<key>: 紙質}}`）と `brush_papers/<64桁>.png` に置く（`brushes.json` に無くなったブラシの紙質は次の保存で落とし、どれも名指さない画像は 1 時間を過ぎたものを消す。保存が名指す画像は既にあれば更新時刻を新しくする。`genko_brush_papers` が整数の 1 でない・無いファイルは読めないものとして扱い、書き換えず、その画像も消さない）。`.genkobrush` は紙質の画像を `paper.png`（base64）で含む。

## ルビの位置と組み方（ruby-layout@1。TEXT-01、M4）

台詞のルビを、本文の中の位置で持つ。Python 版の `ruby_runs`（`[[親文字, 読み], …]`。描くときに親文字を前から順に探す）は、同じ語が 2 度ある文で違う語に読みが付き、本文を直すと別の語へ移ることがある。この拡張を使う台詞は位置で持ち、本文の編集にルビが追従する。

### 単位

- 範囲は本文の**拡張書記素クラスタ**（Unicode UAX #29）の半開区間 `[start, end)`。UTF-16 の単位や符号位置ではない（結合文字の付いた字、絵文字の ZWJ 列、国旗、異体字セレクタの付いた字はそれぞれ 1 つ）。
- 書記素の規則は Qt 6.11.2 の `QTextBoundaryFinder`（Qt が持つ **Unicode 17.0** のデータ）で決める。Qt の版を変えると Unicode の版が変わりうるので、試験（`test_ruby` の `tableIsFixed`）で 17.0 であることを確かめる。
- 範囲は本文の位置であり、禁則や枠の大きさによる折り返し（組版）では変わらない。

### 台詞の `ruby_spans`

```json
"ruby_runs": [["東京", "とうきょう"]],
"ruby_spans": [
  {"start": 0, "end": 2, "base": "東京", "ruby": "とうけい", "review": "ambiguous"},
  {"start": 3, "end": 5, "base": "東京", "ruby": "とうきょう"}
]
```

| キー | 型 | 意味 |
|---|---|---|
| `start`・`end` | 整数 | 本文の書記素の `[start, end)`。描くルビは `0 <= start < end <= 書記素の数` で、互いに重ならない。`start` の順に並べる |
| `base` | 文字列 | 親文字。描くルビでは本文の `[start, end)` と同じ文字列 |
| `ruby` | 文字列 | 読み（空でない 1 行） |
| `review` | 文字列（任意） | **要確認**の理由。`edited`（編集が親文字にかかった）・`ambiguous`（どの語に付くか決められない）・`unplaced`（`ruby_runs` の親文字が本文に見つからない）。要確認のルビは描かず、`base`・`ruby` は元のまま。範囲は最後に見えた所（目安。`unplaced` は `[0, 0)`） |

- `ruby_spans` を持つ台詞の `ruby_runs` は、描くルビの `[base, ruby]` を順に並べた写し。読み手は `ruby_spans` から作り直す。
- `ruby_spans` を持たない台詞は Python 版と同じく `ruby_runs` だけを持ち、同じに読み書きする（Python 版の原稿は、この拡張を使うまで保存の内容が変わらない）。
- 形が壊れている（整数でない範囲、本文の外、描くルビの重なり、`base` が本文と違う、理由が 3 つのどれでもない）原稿は修復の報告をして読み取り専用で開く。`--accept-repairs` で受け入れると、そのルビは要確認として残す（捨てない）。

### 組み方（台詞の `style`。C++ 版で加えたキー）

| キー | 型・範囲 | 既定（キーなし） | 意味 |
|---|---|---|---|
| `ruby_font` | 文字列 | 本文の書体 | ルビだけの書体（`style.font` と同じ。同梱の書体名か書体ファイル） |
| `ruby_align` | `head`・`center`・`tail` | `center` | 親文字の先頭から／中央に／末尾まで |
| `ruby_gap_mm` | 数（mm）-1〜10 | 0 | ルビと本文の距離 |
| `ruby_tracking` | 数（無次元）-0.5〜2 | 0 | ルビの字と字の間（ルビ 1 字の大きさに対する比） |
| `ruby_scale`（Python 版のキー） | 数（無次元） | 0.5 | 本文 1 字に対するルビの大きさ（0.25〜0.8 に収める）。位置で持つ台詞では横書きにも効く（Python 版の横書きは常に 0.5 字） |

- 既定のままの位置で持つ台詞は、同じ所を `ruby_runs` で持つ Python 版の台詞と同じ画素に描く（`test_ruby_render` で照合）。違うのは、読みが本文全体より長いときだけ（Python 版は文字組の端で読みを切る。位置で持つ台詞は文字組を広げて読みを全部描き、点検で `ruby_overflow` を出す）。
- 点検（`checks.page_issues`）: 要確認のルビは `ruby_review`（error）。組み方の問題は warning で、縮めたり消したりしない: `ruby_overflow`（読みが本文全体より長い）、`ruby_overlap`（読みどうしが重なる）、`ruby_font_fallback`（ルビの書体に無い字を別の書体で描いた）、`ruby_glyph_missing`（どの書体にも無い字）、`ruby_not_drawn`（親文字が描かれない）。親文字より長い読みが列・行の端で内側へ寄るのは Python 版と同じで、警告しない。
- 要確認のルビがある間、正式な書き出しは止まる（`exporting.run` の `official` は `code: ruby_review` で断る。スタジオの preflight にも `ruby_review` の error を加え、M5 で移植する正式な書き出しもそこで止まる）。普通の書き出しは止めない（要確認のルビは描かない）。

### op

- `add_line`・`edit_line` に `ruby_spans`（`[{start, end, ruby, base?, review?}]`。`null` は無し）を加えた。`ruby_runs` と同時には指定しない。`edit_line` で `text` と一緒に渡すと新しい本文の位置。`genko schema`（Python 版の公開一覧）には載せない（`merge_visible` の `flatten` と同じ扱い）。
- `edit_line` で `text` だけを変えると、ルビは次の規則で追従する（`core/ruby.hpp`）。
  - 前への編集（範囲の直前への挿入を含む）は両端を動かす。後ろへの編集（範囲の末尾への挿入を含む）は変えない。
  - 親文字をすべて消す（親文字全体の削除、または親文字全体を含む置換）と、そのルビは描かない（本文から外す。Undo で本文・範囲・読みが戻る）。
  - 親文字の中への挿入、一部の削除・置換、範囲をまたぐ置換、親文字の中の改行、親文字の最後の字に付く結合文字は、要確認（`edited`）。
  - 新しい本文全体との比較は書記素の最長共通部分列で行い、親文字をどの最適な対応でも同じ 1 か所に保つときだけそこへ動かす。複数の場所に置けるか、ある対応では残り別の対応では消えるときは要確認（`ambiguous`）。どの対応でも消えるときは外す。（本文が長く書記素の数の積が 400 万を超えるときは、共通の先頭と末尾だけで判断する）
- `replace_text` は一致した所を編集の位置として同じ規則で追従する（一致と置換文字列の共通の先頭・末尾を除いた部分を編集とする。「京」→「京都」は「都」の挿入）。
- `ruby_runs` を受けたときの変換（明示ルール）: 順に、`run` と `run[0]`・`run[1]` が真のものについて、`str(run[0])` を親文字、`str(run[1])` を読みとし、前のルビの親文字が見つかった所の後ろから、本文の中で最初に見つかる所（符号位置で探す。Python 版の描き方と同じ）。両端が書記素の境目にあるときだけ見つかったとする。見つからないものは、位置で持つ台詞に変換するときは要確認（`unplaced`）として残す。
- 位置で持たない台詞（Python 版の `ruby_runs` だけ）は、編集後の `ruby_runs` が明示ルールで追従の結果と同じ所を指すあいだは Python 版と同じに変える（`edit_line` は `ruby_runs` を変えず、`replace_text` は親文字が本文に残るものを残す）。指さなくなる編集（Python 版なら別の語に読みが付く、親文字が書き換わったのに読みを描き続ける・黙って消す）では、その台詞は以後 `ruby_spans` で持つ。組み方のキーを付けたときも明示ルールで `ruby_spans` に変える。
- op の結果（`results`）に、要確認にしたルビの数 `ruby_review`、外したルビの数 `ruby_removed` を加える（位置で持つ台詞が関わるときだけ。Python 版の台詞のままなら Python 版と同じく何も加えない）。
- 拡張を使う op が初めて適用されたときに `features` へ `ruby-layout@1` を加える（書き手も、台詞が `ruby_spans` か組み方のキーを持つときに加える）。

### 画面（台詞の欄）

- 本文の入力欄はルビを ｜親文字《読み》 の記法で表示する（位置で持つ台詞は、その位置に）。入力した記法の位置がルビの位置になる（Python 版の `ruby_runs` で同じ所を指せるときは Python 版と同じ op を送り、指せないときは `ruby_spans` を送る）。直した本文では、まず元のルビを上の規則で追従させ、記法で親文字の中に字を足した・親文字を一部消したなど、元の読みのまま親文字が変わったルビは要確認にする。IME の変換中の文字は原稿を変えない（確定して決定したときに 1 回の op）。
- 要確認のルビのある台詞は一覧に【ルビ要確認】と出し、台詞を選ぶとルビの一覧に「要確認: 親文字《読み》（理由）」と出る。「振り直す…」で親文字（本文の中の語。同じ語が複数あれば場所を選ぶ）と読みを選び直し、「外す」で外す。
- 選択ツールの設定（台詞の文字とフキダシ）に、ルビの書体・揃え・本文との距離・字間を加えた（それぞれのキーだけの `edit_line`）。
