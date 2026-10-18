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
  studio/reply_drafts/      AI への返事の下書き（C++ 版のアプリだけ。末尾「返事の下書き」）
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
| `ruby-layout@1` | M4 | 台詞の `ruby_spans`（書記素範囲 `[start,end)`、読み、ルビごとの書体・倍率・揃え・距離・字間）。TEXT-01（末尾の「ルビの位置と組み方」） |
| `paper-texture@1` | M3 | ブラシの紙質（素材参照、濃度、倍率、回転、反転、合成、適用座標、seed）。BRUSH-01 |
| `psd-text@1` | M4 | PSD の編集可能文字の出力設定（原稿側の保存項目がある場合のみ） |
| `native.color_raster_v1` / `native.color_raster_tiles_v1` | M3 | 高精度カラー画素とそのタイル保存（下の節） |
| `color-mode@1` | CMYK/Lab | ページごとの色モード（CMYK・Lab、8/16 bit、埋め込んだ ICC、色の表、GKIR の画素）。COLOR-02（末尾の「色モード」） |
| `color-model@1` | PC1a・PC1b・PC1d | 色モデル: RGB のプロファイル・精度（8/16/32 bit）・黒点補正、グレースケール、32 bit の CMYK・Lab・グレー（GKIR 版 2）、高精度の画素の `space`、本の文字のガンマ（`text_gamma`、PC1b）、インデックスカラー・2 階調・ダブルトーン・マルチチャンネル（GKIR の Multichannel、PC1d）。COLOR-03（末尾の「色モデル」） |
| `channels@1` | PC1c | ページの `channels`（アルファ・特色チャンネル。GKIR 版 2 の Mask の画素）。COLOR-03（末尾の「チャンネル」） |
| `adjustments@1` | PC2-0〜PC2b | 補正レイヤーの `adjust` の C++ 版だけの種類と、Python の種類に足す `native` の設定（描画モード・式の選び方・チャンネルの組など）。COLOR-04（末尾の「補正」） |
| `smart-objects@1` | PC3（節 A） | 本の `smart_contents`（スマートオブジェクトの中身: 層を包んだページ 1 枚、または置いたファイルそのもの）と、層の `kind: "smart"` の `smart`（中身・行列・補間ほか）。COLOR-05（末尾の「スマートオブジェクト」） |
| `masks@1` | PC3-0（B1・B2 が使う） | 層のマスクの `linked`・`density`・`feather_mm`、アルファチャンネルの `quick_mask`。COLOR-06（末尾の「マスク」） |
| `vector@1` | PC3-0（C-a が使う） | ページの `paths`・`work_path`・`clipping_path`、層の `vector_mask`。COLOR-07（末尾の「パスとベクトル」） |
| `pen-shapes@1` | PC3-0（C-b が使う） | ペンの層の形の部品（`shapes_blob`・`shape_count`）。COLOR-07（末尾の「パスとベクトル」） |
| `transform@1` | PC3-0（C-d が使う） | 層の `link`・`lock_position`。COLOR-07（末尾の「パスとベクトル」） |
| `page-spec@1` | PC3-0（C-e が使う） | ページだけの用紙 `native_spec`（`trim_origin_mm` も）。COLOR-07（末尾の「パスとベクトル」） |

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
- 既知の制限: **C++ 版の原稿（v4）には、旧版（Python 版）の `genko gc`・`genko undo` を使わない。** 旧版の `genko gc` は v4 を断らずに動き、v3 の journal が名指すもの（直近の行の `before`/`after` と、`.project.json` スナップショットの中身、今の `project.json`）だけを残すため、1 日より古い次の素材を消す: v4 の状態（`.state.json`）の中だけが名指す素材（前の筆跡・絵など）、変換した旧履歴（`legacy/map.json` が名指す状態と旧スナップショット）、journal の直近より前の状態。旧版の `genko undo` は v4 の journal を読めずに止まる（原稿は変えない）。
- 消された後の C++ 版（2026-10、M5④）: 原稿は今の中身のまま開け、保存・gc・journal の修復も続けられる。消えた状態（またはそれが要る素材）より前に戻る Undo、その状態へ進む Redo は、原稿を書かずに `history_missing` で断る（書くのは、どの書込みも先に行う journal の修復だけ。CLI・MCP・アプリ。言葉は wording「…記録（履歴の状態）が原稿から消えているため、ここより前には戻せません…」）。`genko doctor` は消えた状態、要る素材が消えた状態、読めない（名前の中身でない）状態、この build では戻せない状態の数と、Undo がいくつ前まで戻れるかを `problems` に出す（直さない。戻せるかは Undo と同じ判断: 読めて、素材がそろい、読み込みに問題がない状態だけを戻せるとする）。

## カラーペン線（native.color_stroke_v1）

高精度 `convert_layer(to="pen")` は `preserve_precision: true` を明示する。従来の省略呼出しは、8bitへ精度喪失し得る変換の拒否契約を維持する。GUIの「ペンレイヤーに変換」は必ずtrueで呼び、カラー線機能を宣言して保存する。マスク・効果・スクリーン・色指定・パッチ・既存線・panel_eachが付く高精度元画像は、まだ正確なペン変換ができないため原本とbatch全体を変更せず拒否する。

既存の精密カラーペン層に `convert_layer(to="pen")` を再送した場合、RGB8へ再トレースせず、線と精密色を無変更で保持する。アフィン・遠近・メッシュの線変形は `color_rgb` を保持する。

## 高精度カラー画素（native.color_raster_v1、native.color_raster_tiles_v1）

層の `color_raster` は straight sRGB の RGBA16（u16）または RGBA32F（f32、HDR可）で、ページ全面に引き伸ばして表示する。資産は GKCR（`"GKCR"`、版1、標本バイト数2/4、予約0、LE幅・高さ、LE標本）で拡張子 `.colorrgba`。

- 保存は256px角のタイル単位（M3①-1）: `{"tiles": [ref…（行ごと）], "tile": 256, "width", "height", "precision", "space": "srgb", "alpha": "straight"}`。`space` は画素の色空間: `"srgb"`、またはプロファイル付きの RGB のページ（color-model@1）ではそのプロファイルの参照 `"sha256:…"`（画素はそのプロファイルの符号化した値）。ページの空間と違えば読み取り専用。本の高精度の画素は合わせて 1 GiB まで（color-model@1 から。前は 256 MiB）。各タイルはそれ自体がGKCR資産で、内容アドレスのため変更のないタイルは共有される。一部を描き替えた保存は触れたタイルだけを増やす（履歴が全面の複製で膨らまない）。`features` に `native.color_raster_tiles_v1` を加える。
- 旧形式 `{"asset": ref, …}`（全面1資産）も読む。
- 消しゴム・範囲削除・移動/変形/ワープ・貼り付け、ペン・塗り・グラデーション等の描込み、マスク（表示・結合）は画素の精度のまま行う（`render/color_edit`）。道具の届かない画素はバイト不変。描込みは層に線・塗りを残さず画素へ焼き込む。
- 高精度の画素は画素レイヤー・線の層（v2 のラスターを変換したもの）・トーンの層が持てる（PC2d から。トーンの層はトーンとして描き、画素は描かない: 8 bit のページの `raster_png` と同じ）。その層の線・絵（パッチ）・効果・層の色・トーン化・マスク・フォルダーの中は、描くときに画素と合わせて 1 枚の高精度の絵にする（SPEC.md COLOR-04）。ほかの種類（塗り・補正・配置・フォルダー）の層の高精度の画素は読み取り専用。

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
  {"start": 0, "end": 2, "base": "東京", "ruby": "とうけい", "font": "maru", "review": "ambiguous"},
  {"start": 3, "end": 5, "base": "東京", "ruby": "とうきょう", "scale": 0.6, "align": "head", "gap_mm": 1.0, "tracking": 0.2}
]
```

| キー | 型 | 意味 |
|---|---|---|
| `start`・`end` | 整数 | 本文の書記素の `[start, end)`。描くルビは `0 <= start < end <= 書記素の数` で、互いに重ならない。`start` の順に並べる |
| `base` | 文字列 | 親文字。描くルビでは本文の `[start, end)` と同じ文字列 |
| `ruby` | 文字列 | 読み（空でない 1 行） |
| `font`・`scale`・`align`・`gap_mm`・`tracking` | （任意） | そのルビだけの組み方（下の「組み方」） |
| `review` | 文字列（任意） | **要確認**の理由。`edited`（編集が親文字にかかった）・`ambiguous`（どの語に付くか決められない）・`unplaced`（`ruby_runs` の親文字が本文に見つからない）。要確認のルビは描かず、`base`・`ruby`・組み方は元のまま。範囲は最後に見えた所（目安。`unplaced` は `[0, 0)`） |

- `ruby_spans` を持つ台詞の `ruby_runs` は、描くルビの `[base, ruby]` を順に並べた写し。読み手は `ruby_spans` から作り直す。
- `ruby_spans` を持たない台詞は Python 版と同じく `ruby_runs` だけを持ち、同じに読み書きする（Python 版の原稿は、この拡張を使うまで保存の内容が変わらない）。
- 書く順は `start`・`end`・`base`・`ruby`・組み方（`font`・`scale`・`align`・`gap_mm`・`tracking` の、持つものだけ）・`review`。
- 形が壊れている原稿は修復の報告をして読み取り専用で開く。`--accept-repairs` で受け入れると読んだとおりに書く。読み方はルビごと: 読める所まで読み（整数でない範囲は切り捨てた整数、`base` が無ければその位置の本文、理由が 3 つのどれでもなければ `edited`）、形の崩れたルビと本文に合わないルビ（本文の外は本文の中に切り詰める、`base` が本文と違う、描くルビと重なる）は要確認（`edited`）として残す。そのルビだけで、ほかのルビはそのまま。捨てず、`ruby_runs` の写しから前から探して描くこともしない。読み（`ruby`）の無い項目はルビを持たないので読まない（報告する）。`ruby_spans` がリストでないときは、写しの `ruby_runs` の各ルビを要確認（`ambiguous`。行き先が決められない）として残し、どれも描かない。組み方の値が範囲の外・型違いのときも修復の報告をして読み取り専用で開き、受け入れるとその値だけを無し（台詞の組み方）にする（ルビはそのまま）。

### 組み方（ルビごと。`ruby_spans` の各ルビに持つ。C++ 版だけ）

| キー | 型・範囲 | 無いとき | 意味 |
|---|---|---|---|
| `font` | 文字列（空でない） | 台詞の書体 | そのルビだけの書体（`style.font` と同じ。同梱の書体名か書体ファイル）。見つからない（同梱の書体名でも、ある書体ファイルでもない）ときは台詞の書体で描き、点検で `ruby_font_fallback` を出す |
| `scale` | 数（無次元）0.25〜0.8 | 台詞の大きさ（下） | 本文 1 字に対するルビの大きさ |
| `align` | `head`・`center`・`tail` | `center` | 親文字の先頭から／中央に／末尾まで |
| `gap_mm` | 数（mm）-1〜10 | 0 | ルビと本文の距離 |
| `tracking` | 数（無次元）-0.5〜2 | 0 | ルビの字と字の間（そのルビ 1 字の大きさに対する比） |

- 値の無い組み方は台詞の組み方（Python 版の描き方）のまま。`scale` の無いルビの大きさは、縦書きでは台詞の `style.ruby_scale`（Python 版のキー。無ければ 0.5）、横書きでは本文の半分（Python 版の横書きは `ruby_scale` を読まない）。ルビの `scale` があればそれが勝つ（縦横とも）。台詞の `style` にルビの組み方のキーは `ruby_scale` と `mono_ruby`（どちらも Python 版のキー）のほかに無い。
- モノルビ（台詞の `mono_ruby`）は Python 版と同じく縦書きだけ（横書きの Python 版の文字組はモノルビを使わない。位置で持つ台詞も同じ画素）。モノルビで親文字 1 字ごとに分けた読みが互いに重なるときは `ruby_overlap` を出す。
- 1 行に組み方の違うルビがあるとき、縦書きのルビの列（横書きは行の上の段）は、その台詞のルビのうち「大きさ＋距離」がいちばん大きいものに合わせて取り、各ルビは本文から自分の距離の所に自分の大きさで描く。
- 組み方は編集でルビと一緒に動く（前への編集で動く、要確認になる、外して Undo で戻る、`replace_text`・記法での打ち直し・「振り直す…」でも保つ）。
- 既定のままの位置で持つ台詞は、同じ所を `ruby_runs` で持つ Python 版の台詞と同じ画素に描く（`test_ruby_render` で照合）。違うのは、読みが本文全体より長いときだけ（Python 版は文字組の端で読みを切る。位置で持つ台詞は文字組を広げて読みを全部描き、点検で `ruby_overflow` を出す）。
- 点検（`checks.page_issues`）: 要確認のルビは `ruby_review`（error。どこにも置かれていない台詞 `line_unplaced` でも出す）。組み方の問題は warning で、縮めたり消したりしない: `ruby_overflow`（読みが本文全体より長く、文字組を広げて描いた）、`ruby_overlap`（読みどうし、またはモノルビで分けた読みどうしが重なる）、`ruby_font_fallback`（ルビの書体に無い字を別の書体で描いた。ルビの書体が見つからず台詞の書体で描いた）、`ruby_glyph_missing`（描いた書体にその字が無い。横書きで読みを 1 つの書体で描くとき、最初の字で選んだ書体に無い字も）、`ruby_not_drawn`（親文字が描かれない）。親文字より長い読みが列・行の端で内側へ寄るのは Python 版と同じで、警告しない。文字組ができない台詞（横書きの OpenType 機能など）は組み方の警告を出さず、ほかの点検を止めない。
- 要確認のルビがある間、正式な書き出しは止まる（`exporting.run` の `official` は `code: ruby_review` で断る。スタジオの preflight にも `ruby_review` の error を加え、M5 で移植する正式な書き出しもそこで止まる）。普通の書き出しは止めない（要確認のルビは描かない）。

### op

- `add_line`・`edit_line` に `ruby_spans`（`[{start, end, ruby, base?, review?, font?, scale?, align?, gap_mm?, tracking?}]`。`null` は無し）を加えた。組み方は各ルビの項目に書き、`null` はその組み方を無しにする（`edit_line` は台詞のルビ全体を渡すので、変えないルビの組み方もそのまま渡す）。範囲の外・型違いの値は丸めずに断る（`a ruby's gap_mm must be between -1 and 10` など。日本語は wording.cpp）。`ruby_runs` と同時には指定しない。`edit_line` で `text` と一緒に渡すと新しい本文の位置。`genko schema`（Python 版の公開一覧）には載せない（`merge_visible` の `flatten` と同じ扱い）。
- `edit_line` で `text` だけを変えると、ルビは次の規則で追従する（`core/ruby.hpp`）。
  - 前への編集（範囲の直前への挿入を含む）は両端を動かす。後ろへの編集（範囲の末尾への挿入を含む）は変えない。
  - 親文字をすべて消す（親文字全体の削除、または親文字全体を含む置換）と、そのルビは描かない（本文から外す。Undo で本文・範囲・読みが戻る）。
  - 親文字の中への挿入、一部の削除・置換、範囲をまたぐ置換、親文字の中の改行、親文字の最後の字に付く結合文字・異体字セレクタ・ZWJ（1 字の親文字に付くときも。外さない）は、要確認（`edited`）。
  - 新しい本文全体との比較は書記素の最長共通部分列で行い、どの最適な対応でも親文字の字がすべて同じ 1 か所に保たれるときだけそこへ動かす。複数の場所に置けるか、ある対応では残り別の対応では消えるか、一部だけ残す対応があるときは要確認（`ambiguous`）。どの対応でも消えるときは外す。
  - 本文が長く書記素の数の積が 400 万を超えるときは、共通の先頭と末尾だけで判断する。共通の先頭を先に取った場合と共通の末尾を先に取った場合の両方で見て、両者でルビの行き先（そのまま・動く・外す・要確認）が違えば要確認（`ambiguous`。同じ語が編集の前後にあって、どこを編集したか決められない）。共通の先頭・末尾に残ったルビも、その親文字の語が変わった部分（どちらの見方でも、編集前・編集後どちらの本文でも）にあれば要確認（`ambiguous`。両端に 1 つずつの編集など、どの語に付いていたか共通の先頭・末尾では決められない）。
- `replace_text` は一致した所を編集の位置として同じ規則で追従する。親文字にかかる一致が親文字全体を覆う（1 つの一致、または続けて並ぶ複数の一致で）とき: 1 つの一致の置換文字列が親文字を 1 度含むなら、ルビはその中の親文字へ動く（残りは親文字の前後に足した字。「東京」→「「東京」」は [1, 3)。Python 版の `ruby_runs` も同じ所に描く）。2 度以上含むなら、どちらか決められず要確認（`ambiguous`）。ただし最初の親文字（Python 版の `ruby_runs` が見つける所）に残るときはそのまま（「東京」→「東京と東京」「東京と大東京」は [0, 2)。置換文字列との比較で親文字の前に字を足しただけに見えても、残る所が最初の親文字でなければ要確認: 「東京」→「大東京と東京」）。含まなければ、置換文字列に共通の字があってもルビを外す（Python 版が `ruby_runs` から外すのと同じ。「東京」→「東西」「京」「東新京」、「ああ」の「あ」→「い」）。一致と置換文字列の共通の先頭・末尾は、親文字の外に字を足すだけか（「京」→「京都」は「都」の挿入、「東京へ」→「東京まで」は「へ」の置換で、ルビはそのまま）、親文字を変えるか（一部なら要確認）を見分けるためだけに使う。
- `ruby_runs` を受けたときの変換（明示ルール）: 順に、`run` と `run[0]`・`run[1]` が真のものについて、`str(run[0])` を親文字、`str(run[1])` を読みとし、前のルビの親文字が見つかった所の後ろから、本文の中で最初に見つかる所（符号位置で探す。Python 版の描き方と同じ）。両端が書記素の境目にあるときだけ見つかったとする。見つからないものは、位置で持つ台詞に変換するときは要確認（`unplaced`）として残す。
- 位置で持つ台詞が `ruby_runs` を受けたとき（`edit_line`・`set_balloon_path`。Python 版のクライアントは台詞の `ruby_runs` をそのまま送り返す）: op の前の写し（描くルビの `[親文字, 読み]`）と同じ `ruby_runs` なら、ルビは同じ op の本文の編集に追従させたまま（位置・組み方とも。その編集で要確認になったものも要確認のまま、前の語へは移さない）。違うときは明示ルールで置き直し、各ルビは同じ親文字と読みのルビ（順に）の組み方を、無ければ同じ範囲・同じ親文字のルビの組み方を受け継ぐ。
- 位置で持たない台詞（Python 版の `ruby_runs` だけ）は、編集後の `ruby_runs` が明示ルールで追従の結果と同じ所を指すあいだは Python 版と同じに変える（`edit_line` は `ruby_runs` を変えず、`replace_text` は親文字が本文に残るものを残す）。指さなくなる編集（Python 版なら別の語に読みが付く、親文字が書き換わったのに読みを描き続ける・黙って消す）では、その台詞は以後 `ruby_spans` で持つ。
- op の結果（`results`）に、要確認にしたルビの数 `ruby_review`、外したルビの数 `ruby_removed` を加える（Python 版の台詞を Python 版と同じに変えたときは、Python 版と同じく何も加えない。Python 版なら残った同じ語へ読みが移るところを外したときなど、Python 版と違う結果には加える）。
- 拡張を使う op が初めて適用されたときに `features` へ `ruby-layout@1` を加える（書き手も、台詞が `ruby_spans` を持つときに加える）。`ruby_spans` が空になった台詞は Python 版の形（`ruby_runs` だけ）に戻る。

### 画面（台詞の欄）

- 本文の入力欄はルビを ｜親文字《読み》 の記法で表示する（位置で持つ台詞は、その位置に）。打った記法が台詞のルビになる: 記法の位置がルビの位置（Python 版の `ruby_runs` で同じ所を指せるときは Python 版と同じ op を送り、指せないときは `ruby_spans` を送る）。直した本文では、同じ親文字と読みの記法は元のルビ（組み方を保ち、追従の結果が要確認でも描く。追従でそこに描くと決まったルビを先に、ほかは出てくる順に）、元のルビが追従した所に別の読みで打った記法も元のルビ、記法で親文字の中に字を足した・親文字を一部消したなど元の読みのまま親文字が変わったルビは要確認、記法を消したルビは外す。記法に出ない要確認のルビは、上の規則で追従させて残す。Python 版の台詞で、`ruby_runs` は同じでも記法の位置が明示ルールの所と違う（同じ語の 2 つ目へ付け替えた）ときは、変更として送る。IME の変換中の文字は原稿を変えない（確定して決定したときに 1 回の op）。
- 要確認のルビのある台詞は一覧に【ルビ要確認】と出し、台詞を選ぶとルビの一覧に「要確認: 親文字《読み》（理由）」と出る。「振り直す…」で親文字（本文の中の語。同じ語が複数あれば場所を選ぶ）と読みを選び直し、「外す」で外す。Python 版の `ruby_runs` の台詞では、結果を `ruby_runs` で言えるとき（描く所が明示ルールの所と同じ）は `ruby_runs` を送り、台詞は Python 版の形のまま。
- ルビの一覧の下に、選んだルビの組み方（書体・大きさ・揃え・本文との距離・字間）を置く。変えると、そのルビの項目だけが変わった台詞のルビ全体の `edit_line`（`ruby_spans`）を送る（既定に戻すとそのキーが無くなる。大きさの「行の設定」は `scale` 無し。「行の設定」より大きく 0.25 字より小さい値は 0.25 字にする。範囲外の値は日本語で断って元の値に戻す）。ルビを選んでいないときは変えられない。Python 版の `ruby_runs` の台詞のルビに組み方を付けると、その台詞は位置で持つ。選択ツールの設定（台詞の文字とフキダシ）のルビの行は、Python 版の「ルビの大きさ」（`ruby_scale`）と「モノルビ」だけ。「振り直す…」は選び直したルビに元の組み方を保つ。
- （PSD-01 の書き出し用。op ではない内部の口）`render::text::ruby_places(line, dpi)` が、描く各ルビ（親文字が列・行をまたぐときは部分ごと）の頁上の箱・各字の位置・大きさ・書体と、その親文字の箱を返す。効果文字の曲げ・傾け・ワープ・長体平体・回転のある台詞では `exact` が偽（箱は組んだ所）。

## 返事の下書き（studio/reply_drafts/。UX-02、M5④）

アプリの承認箱（相談への返事・差し戻しの理由）とコマの詳細（AI への指示）で、書いてまだ送っていない言葉を原稿に残す。C++ 版のアプリだけのファイルで、`project.json`・journal・監査・Undo/Redo・承認とは関係がない（書いても原稿の版・改訂番号は変わらず、保存を待たない）。

- 場所: `studio/reply_drafts/<キーの sha256、64 桁の小文字16進>.json`。依頼（コマ）ごとに 1 つ。キーは依頼の ticket の id、コマの指示は `panel:<コマの id>`。
- 中身: UTF-8 の JSON 1 行と改行。`{"format": "genko-reply-draft@1", "item": <キー>, "text": <言葉>, "at": "YYYY-MM-DDTHH:MM:SS"}`（`at` は書いた時刻、ローカル時刻）。
- 書き方: 打つのが止まって 0.5 秒後（窓を閉じるとき・アプリを終えるときはすぐ、書き終わるまで待つ）に、ファイル全体を一時ファイル → fsync → rename で置き換える（`storage::write_atomic`）。書き込みと削除はアプリの画面とは別のスレッドで、一つずつ順に行う（原稿の保存と重なると fsync に 100 ms 以上かかるため）。原稿の排他（`project.lock`）は取らない。`studio/reply_drafts` がフォルダーでない（リンクなど）ときは、読みも書きも消しもせずに知らせる。
- 同じ依頼の下書きを二つのアプリ（二人）が書くと、後に書いた方が残る（後勝ち。決めた扱い）。原稿のフォルダーに `project.json` が無くなっていたら書かない（フォルダーを作り直さない）。
- 上限: 言葉は UTF-8 で 64 KiB まで。超えると書かず（前に書いた短い下書きも消す）、「返事の下書きが長すぎるため（64 KiB まで）、原稿には残しません…」と知らせる（言葉は欄に残る）。読むときはファイル 256 KiB まで、1 冊で 4096 ファイルまで（それより後のファイルは読まず、消さない）。
- 消すとき: 送ったとき、欄を空にしたとき、「返事なしで閉じる」で依頼を閉じたとき。依頼が別の所で閉じられた・無くなったときの下書きは、C++ 版の `genko gc`（v4 の原稿だけ）が消す: 開いている ticket・開いている提案（`studio.proposals` の `status: "open"`）の id（数でも `str()` にした形）でも、原稿にあるコマの `panel:<id>` でもない下書きのうち、書いてから 24 時間を過ぎたもの（作ったばかりで、まだ保存されていないコマの指示を消さないため）。書き込みが途中で止まった一時ファイル（`<下書きの名前>.<pid>.<16進>.tmp`）も 24 時間を過ぎたら消す。下書きとして読めないファイル・知らない形式の下書き・ほかの名前のファイルは gc では消さない。`studio/reply_drafts` がリンクなら何も消さない。`genko gc` の `removed` には `studio/reply_drafts/<名前>.json` として素材の後に並ぶ（dry-run では消さずに並べる）。
- 壊れたファイル: 名前が `<64 桁>.json` で、ふつうのファイルでない・256 KiB を超える・UTF-8 の JSON でない・`format` が下書きの形式でない・`item`/`text` が文字列でない・言葉が 64 KiB を超える・名前が `item` の sha256 と合わない、のどれかなら読まずに消し、「返事の下書きのファイルが壊れていたため、読まずに捨てました: <ファイル名>（理由）」と窓に出す。ほかの名前のファイル（書きかけの一時ファイルなど）は読まず、消さない。`studio/reply_drafts` がリンクなら読まない。 `format` が `genko-reply-draft@` で始まる別の版（`@2` など、新しい Genko のもの）は壊れたファイルとせず、読まずに消さずに残し、そう知らせる。
- 読むとき: アプリがその原稿の下書きを初めて要るとき（依頼やコマを選んだとき）に、版を確かめてから一度だけ読む（メモリーにある言葉が勝つ）。ほかのアプリが後から書いた下書きは、開き直すまで読まない。
- 書く原稿: v4 で、変更できる（読取専用でない）原稿だけ。v1〜v3 の原稿（`needs_migration`、M5②）・読取専用で開いた原稿・まだ保存していない原稿の下書きは、アプリを閉じるまでメモリーだけに残す。別名で保存すると、下書きは新しい原稿にも書く（元の原稿の下書きも残る）。
- Python 版: v4 を読まない（§8）。v2・v3 の原稿にこのフォルダーがあっても、読み込み・保存（v2→v3 の移行を含む）・書き出し・Undo/Redo・gc・doctor・`genko studio` は触れず、バックアップ（ZIP）にはそのまま入る（`test_contract_studio_gui` の `pythonLeavesReplyDrafts`）。C++ 版の変換（§6）は `studio/*` を同じバイトで持ち越し、gc・doctor・保存は触れない。

## 色モード（color-mode@1。CMYK・Lab のページ、COLOR-02）

ページごとの色モード。C++ 版だけの機能（Python 版は v4 を読まない）。色モードの無いページは今までどおりの RGB のページで、形式も画素も変わらない。

- ページの `color`（ページの最上位のキー。`spec` には置かない: `set_page_spec` は本全体の判型を置き換えるため）と、本の `color`（`project.json` の最上位。新しく作るページ（`add_page`・表紙・`new`）の既定）。無い、または `null` は RGB。
  `{"mode": "cmyk"|"lab", "depth": 8|16, "profile": "sha256:<64>", "intent": "perceptual"|"relative"|"saturation"|"absolute", "inks": [[[r, g, b], [v…]], …]}`
  - キーはこの順で書く。`profile` は CMYK だけ（必須）、Lab には置かない。`intent` は RGB の色をページへ変換するときの意図（既定 `"relative"`。いつも書く）。`inks` は色の表（下）。未知のキー・範囲外・非有限値・bool の数は読めないものとして読み取り専用で開く。
  - `profile` は CMYK の出力用 ICC プロファイル（色空間 CMYK）。`assets/<ab>/<64>.icc`（中身の sha256。16 MiB まで）。読み手はハッシュを照合し、Little CMS で開けて色空間が CMYK であることを確かめる。無い・名前と中身が違う・開けないプロファイルは報告して読み取り専用で開く（別のプロファイルで描かない）。同じプロファイルの複数のページは一つの素材を指す。
  - `inks`（色の表、1 ページ 1024 項目まで）: ページの要素の色（下）が RGB でこの色のとき、このインキで描く。`[r, g, b]` は 0..255 の整数、ページの中で重複しない。CMYK のインキは `[c, m, y, k]`（各 0..1、インキの割合）、Lab は `[L, a, b]`（L 0..100、a・b −128..127）。
- 層の画素（`ink_raster`）: CMYK・Lab のページの画素レイヤーは `"ink_raster": "sha256:<64>"`（`assets/<ab>/<64>.gkir`）を持つ。`raster_png`・`color_raster` とどれか一つ。ページ全面に引き伸ばして描く（`raster_png` と同じ）。そのページの色モードと精度の画素だけを持つ（違えば読み取り専用）。
  - GKIR 形式: `"GKIR"`、版 1（u8）、色空間（u8。1 = CMYK、2 = Lab）、精度（u8。8 か 16）、予約 0（u8）、幅・高さ（u32 LE。各 1..100000、幅×高さ 1.2 億まで）、その後に zlib（RFC 1950）の流れ 1 つ。中身は行ごとに PNG の行フィルター（0〜4、PNG と同じ式。1 画素のバイト数で）の種類 1 バイトと、その行の標本。標本は色の版（CMYK は C・M・Y・K、Lab は L・a・b）の後にアルファ（straight）、16 bit は big-endian。流れの後ろに余分なバイトがあれば読めない。
  - 標本の意味: CMYK は 0 がインキなし、最大値が 100%。Lab は ICC の符号化（8 bit: L×255/100、a+128、b+128。16 bit: L×65535/100、(a+128)×257、(b+128)×257。四捨五入）。白色点は D50。
- 要素の色: 線・塗り・グラデーション・文字・フキダシ・枠線・効果線・トーン・紙の色などは、どのページでも今までどおり RGB で持つ（op も同じ）。CMYK・Lab のページでは描くときに次の順でインキに当てる: (1) そのページの `inks`、(2) Genko の既定の墨（RGB の灰色 0・10・15・20）は CMYK で K 100%・Lab で L 0、白（255）はインキなし・L 100、(3) ほかの灰色（r = g = b）は CMYK で K だけ（K = 1 − v/255）、Lab で a = b = 0（L は sRGB から D50 の Lab へ変換した値）、(4) それ以外の色は sRGB からページのプロファイル（Lab は D50 の Lab）へ、ページの `intent`・黒点補正で変換。高精度の線の色（`color_rgb`）は RGB のページの精度で、CMYK・Lab のページでは線の `rgb`（8 bit）にこの規則を当てる（インキの精度は色の表とプロファイルが決める）。
- `features` に `color-mode@1` を加える（ページか本に `color` があるとき）。

## 色モデル（color-model@1。RGB のプロファイル・グレー・32 bit・特別な色モード、COLOR-03）

色モード（color-mode@1）を広げる。C++ 版だけの機能。色モデルの無いページ（RGB・8 bit・sRGB）は今までどおりで、形式も画素も変わらない。

- ページと本の `color`（color-mode@1 と同じ場所）は次のどれか。無い、または `null` は RGB・8 bit・sRGB。
  - RGB: `{"mode": "rgb", "depth": 8|16|32, "profile": "sha256:<64>"?, "intent": …, "bpc": false?}`。`profile` は sRGB 以外のときだけ（RGB のマトリックス・シェーパーの ICC。素材 `.icc`）。`bpc`（黒点補正）は false のときだけ書く（既定 true）。色の表は持たない。8 bit・sRGB・`"relative"`・黒点補正ありは書かない（色モデル無し）。その形を書いたもの（`{"mode": "rgb", "depth": 8, "intent": "relative"}`）は書き手の形でないので、`"bpc": true` と同じく読み取り専用で開く（保存で黙って消さない）。
  - グレー: `{"mode": "gray", "depth": 8|16|32, "profile": "sha256:<64>", "intent": …, "bpc": false?, "inks": […]}`。`profile` は必須（グレーの ICC）。色の表のインキは `[k]`（0..1、墨の割合。明るさは 1 − k）。
  - CMYK・Lab: color-mode@1 のまま。`depth` に 32 を加え、`bpc` を同じく加える。
  - インデックスカラー（PC1d）: `{"mode": "indexed", "depth": 8, "intent": …, "bpc": false?, "table": {"colors": [[r, g, b], …], "transparent": 番号|null}}`。RGB の系（ページの `rgb`）で、精度は 8 だけ、`profile` は書かない（sRGB）。`colors` は 1〜256 色（0..255 の整数）、`transparent` は表の番号か null（いつも書く）。`table` のキーはこの 2 つだけ。
  - 2 階調（PC1d）: `{"mode": "bitmap", "depth": 8, "profile": "sha256:<64>", "intent": …, "bpc": false?, "inks": […], "bitmap": {"method": "threshold"|"pattern"|"diffusion"|"halftone", "frequency": 1..999, "angle": -180..180, "shape": "round"|"ellipse"|"line"|"square"|"diamond"|"cross"}}`。グレーの系（`profile` はグレーの ICC で必須）、精度は 8 だけ。`bitmap` は作ったときの記録（描き方は変えない）。
  - ダブルトーン（PC1d）: `{"mode": "duotone", "depth": 8|16|32, "profile": "sha256:<64>", "intent": …, "bpc": false?, "inks": […], "duotone": [{"name": 1〜64 字, "color": [r, g, b], "curve": [[in, out], …]}, …]}`。グレーの系。インキは 1〜4 つ、曲線は 2〜13 点（`in` は 0..100 の狭義増加、`out` は 0..100）。
  - マルチチャンネル（PC1d）: `{"mode": "multichannel", "depth": 8|16|32, "profile": "sha256:<64>"?, "intent": …, "bpc": false?, "inks": […], "plates": [{"name": 1〜64 字, "color": [r, g, b], "solidity": 0..1, "key": true?, "curve": [[in, out], …]?}, …], "from": "rgb"|"cmyk"|"lab"|"gray"|"duotone"}`。版は 1〜32（名前はページの中で一つ。`key`（灰色の要素を置く版）は true のときだけ書く、`curve` はダブルトーンから作った版のその曲線）。`from` は作った元のモード（要素の色の当て方）で、`profile` は `from` が cmyk（CMYK の ICC）・gray・duotone（グレーの ICC）のとき必須、rgb・lab のときは書かない。色の表のインキの値は版の数だけ（濃度 0..1）。色相・彩度・カラー・輝度の描画モードの層は版で合成できないので、それを持つマルチチャンネルのページは読み取り専用（補正レイヤーは層の描画モードを見ないので、その `native.blend` で決める: 末尾の「補正」）。
  - キーはこの順で書く（`mode, depth, profile, intent, bpc, inks, table, bitmap, duotone, plates, from`。読むときは順を問わない）。未知のキー・そのモードに無いキー（グレーの `bitmap` など）・範囲外の値・色空間の違うプロファイル（RGB のページにグレーの ICC など）は読み取り専用で開く（別のプロファイルで描かない）。プロファイルの素材は color-mode@1 と同じく照合する（内蔵のプロファイルも素材として持つ）。
- 層の画素:
  - RGB のページの画素レイヤーは、8 bit なら `raster_png`、16・32 bit なら高精度の画素（`color_raster`。u16・f32）。プロファイル付きのページでは画素はそのプロファイルの値（`space` が参照）。`raster_png` は層の種類（画素レイヤー・v2 の線の層のラスター・消しゴムを掛けたトーンなど）によらず、プロファイル付きのページではいつもそのプロファイルの値。精度はページの `depth` と同じでなくてもよい（16 bit のページの 8 bit の PNG など）。
  - グレー・CMYK・Lab のページは `ink_raster`（GKIR）。そのページの色空間と精度の画素だけを持つ。
  - インデックスのページは 8 bit の `raster_png` だけ（印刷する画素レイヤーの見える画素の色は表の色に寄せる: op の後で。ネーム・下描きの層は印刷しないので元の色のまま持ち、表示の写像で表の色に見える）。2 階調・ダブルトーンのページはグレーの GKIR（2 階調は 8 bit。印刷する画素レイヤーは標本と α が 0・255 だけ、ネーム・下描きの層は元の値のまま）。インデックスのページの層は高精度の画素（`color_raster`）を持たない（あれば読み取り専用）。マルチチャンネルのページは GKIR の Multichannel（版の数はページの `plates` の数）。
- GKIR 版 2（版 1 が持てない画素だけ。版 1 が持てる CMYK・Lab の 8・16 bit はいつも版 1 で書く: 同じ画素はいつも同じバイト）: `"GKIR"`、版 2（u8）、色空間（u8。3 = グレー、4 = マルチチャンネル（PC1d）、ほかは版 1 と同じ 1 = CMYK・2 = Lab）、精度（u8。8・16・32）、色の版の数（u8。グレー 1、CMYK 4、Lab 3、マルチチャンネル 1〜32）、幅・高さ（u32 LE）、zlib の流れ（版 1 と同じ: 行ごとに PNG の行フィルターの種類と標本）。標本は色の版の後にアルファ（straight）。16 bit は big-endian、32 bit は IEEE 754 binary32 の big-endian。32 bit の値は有限で、0..1（グレーの明るさだけは HDR として 0..1 の外も持てる）。グレーの標本は明るさ（0 が黒）。マルチチャンネルの標本は版ごとの濃度（0 = インキなし、最大値 = インキ 100%）。
- 要素の色: マルチチャンネルのページは ページの `inks` → 灰色（Genko の墨は濃度 1）は `key` の版（曲線があれば通す）→ ほかの色は `from` のモードの規則（rgb は 1 − 値、cmyk はプロファイル、lab は D50、gray・duotone はプロファイルの灰色を `key` の版に）。インデックスのページは RGB と同じに描いて表の最も近い色、2 階調はグレーと同じに描いて 50% で 2 値、ダブルトーンはグレーの規則（PC1d）。グレーのページは color-mode@1 の規則で当てる（ページの `inks` → Genko の既定の墨（灰色 0・10・15・20）は明るさ 0 → ほかの灰色 v は明るさ v/255（白は 1。ページのプロファイルによらず、CMYK の K だけと同じ考え） → ほかの色は sRGB からページのプロファイルへ、意図・黒点補正）。プロファイル付きの RGB のページは、要素の sRGB の色をページの意図・黒点補正でそのプロファイルへ変換して描く（sRGB のちょうどの白と黒はページの白と黒）。
- 本の文字のガンマ（PC1b。カラー設定の「文字の色をガンマで混ぜる」、PhotoCraft の blendTextGamma）: 本の `color` の最後のキー `"text_gamma": 数`（1 より大きく 2.2 以下）。台詞・文字の字のふちを、下の色とこのガンマで混ぜて描く（((1 − a)·下^g + a·字^g)^(1/g)）。本に持つので、アプリ・コマンドライン・MCP・書き出しで同じ画素になる（機械の設定では変わらない）。無いときは今までどおり（Python 版と同じ画素）。本の色モデルが既定（RGB・8 bit・sRGB）でも、このキーのために `{"mode": "rgb", "depth": 8, "intent": "relative", "text_gamma": …}` を書く（読むときは `text_gamma` を外して残りを本の色モデルとして読む）。ページの `color` にあるとき・範囲外・数でないときは読み取り専用。op は C++ 版だけの `set_text_gamma`（COLOR-03）。
- `features` に `color-model@1` を加える（ページか本に RGB のモード・グレー・32 bit・`bpc: false`・インデックスカラー・2 階調・ダブルトーン・マルチチャンネルのどれかがあるとき。GKIR 版 2 の素材を持つときも。本に `text_gamma` があるときも）。`color-mode@1` も一緒に書く（ページの `color` の場所が同じため）。color-model@1 を知らない読み手は、未知の拡張として読み取り専用で開く。

## チャンネル（channels@1。アルファ・特色チャンネル、COLOR-03）

ページごとのアルファチャンネルと特色チャンネル。C++ 版だけの機能。チャンネルの無いページは今までどおりで、形式も画素も変わらない。

- ページの `channels`（無い、または空はチャンネル無し。空なら書かない）: パネルの順のリスト（1 ページ 50 個まで）。
  - アルファ: `{"id": "ch_<12 桁の 16 進>", "name": "Alpha 1", "kind": "alpha", "raster": "sha256:<64>"|null, "color": [r, g, b], "opacity": 0..1, "indicates": "masked"|"selected"}`。`color`（0..255 の整数）は重ねて見せる色（sRGB）、`opacity` はその不透明度、`indicates` は色を付ける所（`masked`: 選ばれていない所、`selected`: 選ばれた所）。
  - 特色: `{"id", "name", "kind": "spot", "raster", "color": [r, g, b], "solidity": 0..1}`。`color` はインキの画面での見え方（sRGB）、`solidity` はインキの濃度（0 透明・1 不透明）。
  - キーは種類のものだけ、この順で書く（読むときは順を問わない）。`id` はページの中で一つ。`name` は 1〜64 字、前後に空白が無く、ページの中で一つ。未知のキー・範囲外の値・重なりは読み取り専用で開く（そのまま書き戻す）。
  - `raster`: チャンネルの画素の素材（`assets/<ab>/<64>.gkir`）。`null` は全部 0（何も選ばれていない・インキなし）。GKIR 版 2 の Mask（色空間 5、色の版の数 1、アルファ無し）で、精度はページの深度（色モデルの `depth`。無ければ 8）。標本は濃度（0 = 選ばれていない・インキなし、最大値 = 選ばれた・インキ 100%。32 bit は 0..1 の float）。ページ全面に引き伸ばす（作るときの解像度はアルファ 200 dpi、特色は原稿の dpi。ほかの大きさも読む。画素の op で描いても解像度は変わらない: op には 200 dpi で貸し、変えた画素だけをチャンネルの解像度へ戻すので、変わらない画素の標本はそのまま）。ページの深度と違う精度・Mask でない画素は読み取り専用。
- 保存しないもの: 描く先のチャンネル、目（見せる・隠す）、重ねの表示（ウィンドウの状態）。
- 選択範囲をストック（`saved_areas`、Python のもの）は今までの形のまま（チャンネルではない）。
- `features` に `channels@1` を加える（チャンネルを持つページがあるとき。無くなれば外す）。channels@1 を知らない読み手は、未知の拡張として読み取り専用で開く。

## 補正（adjustments@1。補正レイヤーの C++ 版だけの設定と種類、COLOR-04）

補正レイヤー（`kind: "adjust"`）の `adjust` を広げる。C++ 版だけの機能。Python の形の補正（Python の種類 `levels`・`curve`・`hue`・`invert`・`posterize`・`threshold`・`gradient_map`・`bitonal`・`brightness_contrast` と露光量 `exposure`（`native.exposure_v1`）に、その種類のキーだけ）は今までどおりで、形式も画素も変わらない（`features` に足さない）。

- **`native`**（Python の種類の `adjust` の最後のキー。C++ 版だけの設定を 1 つの物で）: どの種類も `"blend"`（Genko の 20 の描画モード。補正後の絵を下の絵にこのモードで重ねる。層の `blend` は補正レイヤーでは見ない）。`levels`・`curve` は `"method": "genko"|"upstream"`（式。無ければ genko）、`"space": "rgb"|"cmyk"|"lab"|"gray"`、`"channels": {名前: 組}`（名前は space ごと: rgb は red・green・blue、cmyk は cyan・magenta・yellow・black、lab は lightness・a・b、gray は gray。levels の組は `{"black": 0..253, "white": 2..255（black + 2 以上）, "gamma": 0.01..9.99, "out_black": 0..255, "out_white": 0..255}`、curve の組は `[[in, out], …]`（0..255、2〜19 点、in の順に 0.5 以上離れる））。`channels` を持つなら `space` も書く。`brightness_contrast` は `"method": "modern"|"legacy"`、`posterize` は `"method": "floor"`、`gradient_map` は `"reverse"`・`"dither"`（bool）。
  - 書くのは与えられたキーだけ、この順（blend、method、space、channels／reverse、dither）。空の `native` は書かない。
  - 知らないキー・型の違い・範囲外・非有限・`channels` の space に無い名前・ページが合成できない `blend`（Lab のページの Lab で使えない描画モード、マルチチャンネルのページの色相・彩度・カラー・輝度。マルチチャンネルのページは `channels` を持たない）は読み取り専用で開く。
- **C++ 版だけの種類**（`adjust` の `kind`）: `hue_saturation`・`vibrance`・`color_balance`・`black_white`・`photo_filter`・`channel_mixer`・`selective_color`・`color_lookup`。設定は種類の表（`native/src/core/adjust_kinds.cpp`。既定と範囲は COLOR-04・設計 2.3）のキーを、既定を埋めて表の順に書く（正規形）。`native` は `blend` だけ。知らないキー・型の違い・範囲外・非有限は読み取り専用。補正レイヤーに置けない種類（`filter_raster` だけの `desaturate`・`equalize`・局所の補正）も読み取り専用。
  - 色の補正（PC2b）の正規形: `hue_saturation` の 6 つの色域（`reds`…`magentas`）はどれも `{"hue", "saturation", "lightness", "range": [4 つの角度]}` を書き、`range` は増える順・1 回転の中・中心が −30..330 の形（op が直す。保存されたものは形と範囲だけ確かめる）。`colorize` が true の `hue` は 0..360、`saturation` は 0..100。`photo_filter` の `filter`（既定の名前）は op の入力だけで、`color` にその色を入れて書かない（保存されていれば名前を確かめ、描くときはその名前の色を使う）。`channel_mixer` は `gray`（モノクロの行）をいつも書く。色は `[r, g, b]`（0..255 の整数）、ほかの数は `[…]` か数。
- 種類の式がまだ入っていない版（PC2-0）は、形を確かめるだけで描くときに省いて記録し（`adjust:<種類>`）、書き出しは断る（`not_yet_ported`）。
- **`color_lookup` の正規形**（PC2a）: `{"kind": "color_lookup", "look"?: 作った見た目の名前, "lut"?: "sha256:<64>", "size"?: 2..129, "name": 文字, "interpolation": "trilinear"|"tetrahedral", "dither": bool, "native"?: {"blend"}}`（この順。`look` と `lut` は片方だけ、`size` は `lut` のあるときだけ）。op が受け取る `data`（ファイルの中身）と `file_name` は保存しない。履歴から戻した状態（Undo・Redo の状態は正規の JSON でキーが辞書順）では、Python の形の補正と同じくキーが辞書順になる（読み手はキーの順を問わない）。
- **3D LUT の素材**（PC2a。`assets/<ab>/<64>.gklut`、`lut` の参照先）: `"GKLU"`・版 1（u8）・大きさ n（u8、2..129）・zlib で縮めた float32（リトルエンディアン）の n³ × 3（R・G・B。R が一番速く回る .cube の順）。参照は縮める前ではなく、この形のファイル全体の sha256。書き手は原稿の `color_lookup` の層が参照する表をすべて書き（無い表があれば保存を断る）、読み手は読めない・形の違う表を `broken_asset` として読み取り専用で開く。どの op の後でも、参照する表を原稿が持つことを確かめる（`validate_adjustments`。持たなければ op を断る: ほかの原稿のページを取り込む `import_pages` は表も写す）。
- `features` に `adjustments@1` を加える（`native` を持つか C++ 版だけの種類の補正レイヤーがあるとき。無くなれば外す）。露光量はこれまでどおり `native.exposure_v1` も書く。adjustments@1 を知らない読み手（PC1 までの C++ 版）は、未知の拡張として読み取り専用で開く。

## スマートオブジェクト（smart-objects@1。COLOR-05、PC3 節 A）

スマートオブジェクト（SO）は、中身（層を包んだページ 1 枚、または置いたファイルそのもの）を本に 1 つ持ち、層がそれを行列で置いて描く。SO の無い原稿は今までどおりで、形式も画素も変わらない（`features` に足さない）。

- **本の `smart_contents`**（`studio` の後、`pages` の前。SO の層かほかの中身が使う中身だけを書く。鍵は中身の id `sc_<12 桁の 16 進>`、辞書順）:
  - 層を包んだ中身: `{"kind": "page", "name": もとの層の名前, "spec": 判型（本の `spec` と同じ形）, "page": ページ 1 枚（`pages[]` の要素と同じ形）}`。ページの `index` は 1、台詞は持たない（`story` は本のページのものだけ）。コマ（`frames`）は包んだときのページの写しで、層の `panel_clip`・`panel_each` と絵の SO の `clip_to` の切り抜きにだけ使う（枠線・紙・ノンブルは描かない）。色モデル（ページの `color`）はもとのページと同じ。中身のページの層は普通の層と同じ形（画素・線・マスクは素材の参照）。
  - 元のファイル: `{"kind": "file", "name": ファイル名, "asset": "sha256:<64>", "suffix": ".png"|".jpg"|".tif"|".webp"|".bmp"|".gif"|".psd"|".psb", "format": "png"|"jpeg"|"tiff"|"webp"|"bmp"|"gif"|"psd", "size_px": [w, h], "dpi": 数, "depth": 1|8|16|32, "mode": "gray"|"rgb"|"cmyk"|"lab"（アルファがあれば末尾に "a"）, "profile": {"use": "keep"|"embedded"|"assign", "icc"?: "sha256:<64>", "intent"?: "perceptual"|"relative"|"saturation"|"absolute", "bpc"?: bool}, "link"?: {…}, "psd"?: {…}}`（この順）。ファイルのバイトは変えずに `assets/<ab>/<64><suffix>`（参照はファイル全体の sha256）。`size_px`・`dpi`・`depth`・`mode` は置いたときに見出しから（`dpi` はファイルに無ければページの dpi）。中身の大きさ（mm）は `size_px × 25.4 / dpi`。`profile` は色の扱い: `keep`（値のまま、ページの空間の値とみなす）、`embedded`（ファイルに埋め込まれたプロファイルからページの空間へ）、`assign`（`icc` の ICC プロファイル、`assets/<ab>/<64>.icc`、とみなして変換）。`intent`・`bpc` は `keep` でないときに書く。`link`（リンクした中身の記録、PC3 A1）は `{"path": ファイルの場所（その機械の絶対パス。4096 バイトまで）, "relative"?: 原稿のフォルダーからの場所（区切りは "/"。4096 バイトまで）, "sha256": 最後に読んだファイルのバイトの sha256（64 桁の 16 進。`sha256:` は付けない）, "bytes": その大きさ}`（この順。ほかの鍵は持たない）。原稿はいつもリンク先の最後の写し（`asset`）を持ち、描画・保存・取り消しはそれで決まる（リンク先が無いのは欠損ではない）。`psd`（PSD の配置レイヤーの記録、PC3 A3）は後の群が書く（この版は形だけ確かめて書き戻す）。
  - op が `data`（base64）で受けたファイルと `assign` のプロファイルは、書き手が素材として書く（`Document::smart_files`）。
- **層** `{"kind": "smart", …今の層の鍵（visible・opacity・blend・clip・mask・effect・screen・color など）, "frame_id"?: コマ, "clip_to": "frame"|"bleed"|"none", "finish"?: {…}, "source"?: {…}, "smart": {…}}`。SO の層は自分の画素・線・塗り（`asset`・`strokes`・`patches`・`ink_raster`・`color_raster`・`fill`・`adjust`・`tone`・`placement_mm`・`fit`）を持たない（あれば読み取り専用）。`frame_id`・`clip_to`・`finish`・`source` は配置画像（`kind: "placed"`）と同じ意味（コマで切る・白黒の仕上げ・出どころ）。
- **`smart`**（書くのはこの鍵だけ）: `"contents": "sc_…"`（必須）、`"matrix": [a, b, c, d, e, f]`（中身の mm → ページの mm。x' = a x + c y + e、y' = b x + d y + f。`transform_area` の `matrix` と同じ並び。無ければ恒等）、`"interp": "lanczos"|"bicubic"|"bilinear"|"nearest"`（絵の SO は lanczos、層を包んだ SO は bicubic で作る）。スマートフィルター（PC3 A2）: `"filters"`（下から順のリスト。1 つの SO に 64 個まで。各 `{"kind": filter_raster の種類, "params": {…その設定…}, "blend": 20 の描画モードのどれか, "opacity": 0..1, "visible": bool}`。この順。`kind` は「SO に掛けられる」種類だけ: `filter_raster` のもとからの 22 種と、PC2 の補正・局所の補正の種類。`match_color`・プラグインは入らない。C++ 版の種類と `native` のある種類の `params` は正規の形、ほかは op で受けたまま。カラールックアップは `look` だけで `lut`・`data` は持たない）、`"filters_on"`（bool。無ければ有効。false のときだけ書く）、`"filter_mask"`（`{"asset": "sha256:<64>", "enabled": bool}`。ページ全体の 150 dpi のグレーの PNG、`assets/<ab>/<64>.png`。層のマスクと同じ）。フィルターもマスクも無ければこの 3 つの鍵は書かない。後の群の鍵: `"perspective"`（4 隅の [x, y]、凸）、`"warp"`、`"stack"`（スタックモード: entropy・kurtosis・maximum・mean・median・minimum・range・skewness・standard_deviation・summation・variance）、`"psd"`（いずれも物）。この版はそれらを形だけ確かめ、描くときは `not_yet_ported`（`smart:perspective`・`smart:warp`・`smart:stack` など）で省いて記録する（書き出しは断る）。
- **読み手の検査**（書き手・各 op の後も同じ）: 層の `contents` が本にある、中身の id の形、中身どうしに輪が無い（中身が自分を見せない）、中身の入れ子は深さ 8 まで、行列が有限で潰れない（|ad − bc| > 1e-9 mm²）、名前は 255 字まで、知らない鍵・種類・形式・精度・色・色の扱い・意図が無い、ファイルの素材と `assign` の ICC が `assets/` にある、スマートフィルターの鍵・種類・描画モード・不透明度が上の形で、PC2 の種類と色調の種類の設定がその種類として読める、フィルターマスクの PNG が `assets/` にある。中身のページは本のページと同じ検査（色モード・色モデル・チャンネル）を受け、その高精度の画素は本の予算（1 GiB）に本のページの画素と一緒に数える。外れた原稿は理由を付けて読み取り専用で開く（素材の欠けは `missing_asset`）。
- 複製（`duplicate_layer`・ページの複製）は同じ中身を指す（中身を変えると全部変わる）。`smart_copy` は中身を写して新しい id にする。作品の結合（`import_pages`）は中身も持ってくる（id が重なれば新しい id）。ほかの原稿の素材は今と同じく先に写す。
- 中身は project.json の中にあるので、履歴の状態・GC・Undo・Redo・復旧（§4）は普通の op と同じに中身と素材を守る（GC は状態の中の `sha256:` を全部拾う）。コマが消えて外れた絵の SO は配置画像と同じく `studio.orphans` に記録し、ファイルの中身を記録の `smart_contents`（id → 元のファイルの形）に持つ。
- `features` に `smart-objects@1` を加える（書く中身があるとき。無くなれば外す）。smart-objects@1 を知らない読み手は、未知の拡張として読み取り専用で開く（PC2 までの C++ 版は層の種類 `smart` を知らないので、原稿を開かずに形の誤りを言う。どちらも原稿を書き換えない）。

## マスク（masks@1。レイヤーマスクのリンク・濃度・ぼかし、クイックマスク、COLOR-06）

層のマスク（`mask`）とアルファチャンネルに C++ 版だけの設定を足す。設定を持たないマスク・チャンネルは今までどおりで、形式も画素も変わらない。

- 層の `mask` に足すキー（既定のときは書かない。マスクの今のキーの後に、この順）:
  - `"linked": true` — マスクを層と一緒に動かす。無いときはページに固定（今の原稿のマスクはこのまま描ける）。画面で新しく作るマスクは `true`。
  - `"density": 0..1` — 濃度。描くときの値は `255 − density·(255 − v)`（0 で全部見せる）。既定 1。
  - `"feather_mm": 0..100` — ぼかし（mm）。描くときにマスクを箱 3 回で近づけたガウスでぼかす。既定 0。
  - 数は小数 4 桁に丸めて書く。型の違い・範囲外・非有限は読み取り専用で開く。
- アルファチャンネル（channels@1 の `kind: "alpha"`）に `"quick_mask": true`（クイックマスクのチャンネル。1 ページに 1 つまで、特色には付けない）。キーは `indicates` の後。
- `features` に `masks@1` を加える（設定を持つマスクか、クイックマスクのチャンネルがあるとき。無くなれば外す）。masks@1 を知らない読み手は、未知の拡張として読み取り専用で開く。

## パスとベクトル（vector@1・pen-shapes@1・transform@1・page-spec@1。COLOR-07）

パス・ベクトルマスク・ペンの層の形の部品・層のリンクと位置のロック・ページだけの用紙。C++ 版だけの機能。どれも持たない原稿は今までどおりで、形式も画素も変わらない。中身を作る op は C-a〜C-e が入れる（PC3-0 は読み書きと検査の枠）。

- **パス**（単位 mm、y は下向き、ページの座標）: `{"subpaths": [{"closed": bool, "op": "combine"|"subtract"|"intersect"|"exclude", "knots": [点…]}…], "fill_rule": "nonzero"|"evenodd", "inverted": bool}`（この順）。点は角なら `[x, y]`、ハンドルを持てば `{"anchor": [x, y], "in"?: [x, y], "out"?: [x, y], "smooth"?: true}`（`in`・`out` は anchor と同じなら書かない）。座標は ±100000 mm の中の有限の数で、小数 4 桁に丸めて書く。1 つのパスの点は 100000 まで。
- **ページ**（`vector@1`）: `"paths": [{"name": 1〜64 字（前後に空白なし、ページの中で一つ）, "path": パス}…]`（998 まで）、`"work_path": パス`、`"clipping_path": {"name": 保存したパスの名前, "flatness": 0..100}`。空の `paths` は書かない。
- **層**（`vector@1`）: `"vector_mask": {"path": パス, "enabled"?: false, "linked"?: false, "density"?: 0..1, "feather_mm"?: 0..100}`（既定 使う・リンク・濃度 1・ぼかし 0 は書かない）。
- **ペンの層の形の部品**（`pen-shapes@1`。ペンの層だけ）: 層の `"shapes_blob": "sha256:<64>"`（`assets/<ab>/<64>.shapes.json`、部品の配列を正規形の JSON で）と `"shape_count"`。部品は `{"id": 1〜64 バイト（層の中で一つ）, "name"?: 1〜64 字, "path": パス, "fill": null|{"rgb": [r, g, b]|null}|{"gradient": {…}}, "stroke": null|線, "live"?: 生きた形, "opacity"?: 0..1, "over"?: true}`。線は `{"width_mm": 0..200, "fill", "opacity"?, "align"?: "inside"|"center"|"outside", "cap"?: "butt"|"round"|"square", "join"?: "miter"|"round"|"bevel", "miter_limit"?: 1..1000, "dashes"?: [長さ…（64 まで）], "dash_offset"?}`（既定 中央・butt・miter・100 は書かない）。生きた形は `{"kind": "rect", "rect": [x, y, w, h], "radii": [4 つ]}`・`{"kind": "ellipse", "rect"}`・`{"kind": "polygon", "rect", "sides": 3..100, "star_ratio": 0..1, "angle"?}`・`{"kind": "line", "from", "to", "weight_mm"}`。1 つの層の部品は 10000 まで。パターンの塗りはまだ読み取り専用で開く。素材が無い・壊れているときは素材の欠けとして読み取り専用で開く。
- **層のリンク・位置のロック**（`transform@1`）: 層の `"link": 1〜64 バイトの文字`（同じ文字の層が一緒に動く）・`"lock_position": true`。
- **ページだけの用紙**（`page-spec@1`）: ページの `"native_spec"`（本の `spec` と同じ形の用紙。本と違う用紙を持つページだけ）。`"trim_origin_mm": [x, y]` は仕上がりの左上の位置（無ければ用紙の真ん中に置いた位置）。
- 知らないキー・型の違い・範囲外・非有限・名前の重なり・無い名前を指すクリッピングパス・ペンの層でない層の形の部品は、読み取り専用で開く（そのまま書き戻す）。
- `features` に、使う物があるときだけそれぞれを加える（`vector@1`: パス・作業用パス・クリッピングパス・ベクトルマスク、`pen-shapes@1`: 形の部品を持つ層、`transform@1`: リンクか位置のロックを持つ層、`page-spec@1`: ページだけの用紙）。無くなれば外す。知らない読み手は未知の拡張として読み取り専用で開く。
