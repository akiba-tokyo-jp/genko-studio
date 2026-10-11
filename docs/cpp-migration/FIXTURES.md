# 固定の検証原稿（FIXTURES）

ACCEPTANCE.md §2 の F1〜F5 の作り方と置き場所。fixture は合成原稿であり、実際の漫画の完成品質の証拠ではない。本番原稿（`/home/hermes/manga` など利用者の原稿）は使わない。

## 置き場所

- 小さな固定資産（数百 KB まで）はリポジトリの `native/tests/data/` に置き、`MANIFEST.json` にハッシュを記録する。
- 大きな原稿（F1・F2）は生成スクリプトで作る。生成は決定的（固定 seed）で、生成物の `project.json` と全素材の sha256 一覧をリポジトリに置き、試験の最初に照合する。

## 旧形式の原稿（M0 で作成済み）

`native/tests/data/legacy/` に、リポジトリの 3 つのコミットの**実際の Python 版の書き手**で作った原稿を置いた（`tools/migration/make_legacy_books.py`）。

| 原稿 | 書いたコミット | 内容 |
|---|---|---|
| `book-v1.genko` | 5b54b42（version なし） | 2 ページ、分割したコマ、ネーム線と墨線（点の列）、台詞 2 行（絵文字を含む） |
| `book-v2.genko` | 3210633（version 2） | B4、縦分割、筆圧つきの線と筆圧なしの線、`pages/001/bg.png` のラスター、ルビ、縦書き、未知の最上位キーとページキー |
| `book-v3.genko` | 1b7b1d7（version 3） | B5、筆跡ブロブ、ラスター素材、ルビ・傍点・絵文字の台詞、未知キー（64bit 整数）、保存 4 回・Undo・Redo の履歴、承認監査 1 件、AI と人の変更 |

## F1 大冊（M2 で生成器を作成）

既存 `tests/test_h1.py::_thick` と同じ: B4、32 ページ、各ページの墨線 1500 本、各線 20 点、筆圧 0.7、`random.Random(1)`。C++ の生成器は Python の `random` と同じ数列（MT19937 と `uniform` の変換）で同じ座標を作り、Python 版で作った同じ原稿の `project.json`・筆跡ブロブの内容と照合する。

## F2 実作画負荷（M3 で生成器を作成）

B4・600dpi・8 ページ。各ページ 20 レイヤー:

- 全ページのラスター 4 枚: 6071×8598 px（B4 用紙の 600dpi）の RGBA。内容は固定 seed の値ノイズ（2 段の周波数）＋ 斜線のハッチ。透明度は 0・128・255 が混在（白紙や単色で圧縮を有利にしない）。
- 局所ラスター 4 枚: 2000×2000 px のパッチを各 3 つ、ページ上の固定位置。
- 線画 4 枚: 各 2000 本、各 30 点、筆圧あり、幅 0.2〜1.2 mm、ブラシの種類を 5 種で巡回。
- マスク／補正 4 枚: レイヤーマスク 2 枚（固定 seed の多角形 40 個）、補正レイヤー 2 枚（levels と hue）。
- 文字／トーン等 4 枚: トーン 2 枚（網点・グラデーション）、効果線 1、3D 立方体のガイド 1。
- 台詞 20 個（縦書き 14、横書き 6）、ルビ 10 組、傍点 3、部分書式 3。

## F3 制作工程（M5）

既存 `tests/test_m4_pipeline.py` の 8 ページオフライン工程。企画・脚本・ネーム・固定の合成候補画像・採用・仕上げ・出力。外部 AI の生成時間・画質は測らない。

## F4 組版と納品（M4）

縦横書き、同じ語の繰返し（「東京と東京」）、禁則、縦中横（「AI」「12」）、部分書式、異体字セレクタ、複数ルビ、各種フキダシ、効果文字、ノンブル、CMYK、下描き・非出力層を持つ 4 ページ。ルビの編集追従の期待結果表（ACCEPTANCE.md AC-TEXT の例）を `native/tests/data/ruby_cases.json` に置く。

- 4 ページの本は試験の中で op から作る（`native/tests/unit/test_ruby_render.cpp` の `f4Book`: 作って保存し、読み直して同じ台詞・ルビ（ルビごとの組み方を含む。1 行に組み方の違うルビを並べる台詞がある）であることを確かめ、PNG・PDF・CMYK に書き出す）。生成物の固定ファイルは置かない（op と書体が決まれば同じ本になる）。
- `ruby_cases.json` は `tools/migration/make_ruby_cases.py` で書き、その sha256 を `native/tests/data/MANIFEST.json` に置く（試験が照合する）。各ケースは本文・最初のルビ（`ruby_spans` か Python の `ruby_runs`）・編集（`edit_line` か `replace_text`）と、期待する本文・`ruby_spans`・`ruby_runs`・点検のルビの警告・op の報告。範囲は Unicode 17.0（Qt 6.11.2）の書記素。同じ語の繰返し、結合文字、絵文字の並び（ZWJ・国旗）、異体字セレクタ（IVS・SVS）、全文置換、Python の `ruby_runs` の変換、はみ出し・重なり・代わりの書体、ルビごとの組み方（`font`・`scale`・`align`・`gap_mm`・`tracking`）が移動・要確認・全文置換・`ruby_runs` の受け取り・IME の確定でルビと一緒に動くこと、親文字全体の全文置換（置換文字列と共通の字がある、複数の一致で覆う、親文字を 1 度・2 度含む語への置換。Python 版の台詞は参照の Python の `replace_text` と同じ結果）、1 字の親文字に付く結合文字・IVS・ZWJ、最適な対応の一部が親文字を崩す編集、写しの `ruby_runs` の送り返し、IME の確定と記法の編集（`"gui"`、`test_gui_ruby` が入力メソッドのイベントと Delete キーで打つ。記法ごとルビを消す、Python 版の台詞で読みを同じ語の 2 つ目へ付け替える）を含む。`"undo": true` のケースは、本を保存し、編集を次の版として保存して `genko undo` と同じ取り消し（`storage::restore`）をし、台詞が元どおり（組み方も）か確かめる。長い本文（共通の先頭・末尾での判断。両端に 1 つずつの編集、同じ語の繰り返し）と、壊れた `ruby_spans` の読み方は `test_ruby` の中で作る。

- PSD-01（編集可能な PSD）の本は `native/tests/data/psd_text_book.json`（`genko new --pages 4` の A4 の本に当てる op の並び。sha256 は `MANIFEST.json`）。F4 の種類の台詞を 4 ページに置く: 縦書き・横書き、「東京と東京」の両方の語にルビ、文節の改行・禁則、縦中横（「AI」「12」）、部分書式（大きさ・太字・色）、Python 版の `ruby_runs` の台詞、異体字セレクタ、ルビの書体・先頭/末尾・距離・字間・大きさ、列をまたぐルビ、効果文字（単色の塗りと縁取り）、回転・長体・傾け・斜体、右・左・中央・上・下・均等揃え、各種フキダシ、ノンブル、下描き・非出力層。`test_psd_text`（自前の読込）と `test_contract_psd_text`（psd-tools、`tools/migration/psd_text_check.py`）がこれを編集可能な PSD に書いて構造を検査する。見た目優先（既定）の PSD は、`test_psd_text` が 4 ページの sha256（`native/tests/support/psd_book_hashes.hpp`。e146b96 の genko が書いたもの。1 ページ目は M5① で「大きな声で約束する」のルビを書記素の位置で「声」[3,4)・「約束」[5,7) に直した後のもので、参照の Python の PSD の 1 ページ目と同じバイトであることを `test_contract_psd_text` の imageWayIsPythons が同じ定数で確かめる）と、`test_contract_psd_text` が参照の Python の `psd.export_psd_pages`（`psd_text_check.py python-book`・`python-psd`。文字組は render_harness と同じ Pillow の BASIC、raqm なし）の PSD とバイト一致を確かめる。編集可能な PSD の台詞のレイヤーは、フキダシに文字のレイヤーをアルファで重ねると見た目優先の台詞のレイヤーと画素ごとに一致する（`layersMakeTheLine`。縁取りのある台詞は境界線で描くので除く。回した台詞は全体を補間し直すため、7×7 の範囲がフキダシだけ・1 つの文字レイヤーの不透明部分だけの画素を比べ、比べた画素が 9 割を超えることを確かめる）。
- **Krita での読込（ACCEPTANCE.md §6 の代替検査。M4 出口で 1 回、開発機には Krita を入れない）**: 同じ本を CLI で作って書き出し、Krita のコマンドで開いて PNG にする。

  ```bash
  G=build/linux-release/src/api/genko            # 試す genko（配布物ならその genko）
  OUT=$(mktemp -d)
  $G new "$OUT/psd-text.genko" --title PSD文字 --pages 4 --plain
  $G apply "$OUT/psd-text.genko" native/tests/data/psd_text_book.json
  $G export "$OUT/psd-text.genko" "$OUT/psd" --format psd --dpi 150 --psd-text editable --json   # 4 ファイル、text_layers 19・ruby_layers 13
  $G export "$OUT/psd-text.genko" "$OUT/png" --format png --dpi 150 --area paper --color rgb        # 見比べる絵
  for f in "$OUT"/psd/*.psd; do
    QT_QPA_PLATFORM=offscreen krita --export --export-filename "${f%.psd}.krita.png" "$f" || echo "NG: $f"
  done
  ls -l "$OUT"/psd/*.krita.png
  ```

  合格: 4 つの `.krita.png` ができ（Krita が読めない PSD では書き出しが失敗する）、どれも 1240 × 1754 px で、`$OUT/png` の同じページと見た目が同じ（文字・ルビ・フキダシ・ノンブルがそろい、効果文字の縁取りがある）。続けて 1 ページを Krita の画面で開き、レイヤーに「台詞 …」のグループと、その中の「フキダシ」「本文 #<台詞の ID>」「ルビ「…」 #<台詞の ID>-r<番号>」があること、効果文字（3 ページの「ドーン」「ゴゴゴ」）にレイヤースタイルの境界線があることを確かめる。リリース版の Krita（5.2 系）は PSD の文字レイヤーを画素のレイヤーとして読む（Krita の PSD 文字の読込は未統合の MR !1954）ので、Krita で文字として直せることは求めない。結果（Krita の版、日付、気づいたこと）は検証記録に残す。

## F5 保存異常（M1）

v1/v2/v3/v4 のコピー、欠けた素材、ハッシュ不一致、未知キー、途中で切れた journal 行、無効なパス、書込み不能のフォルダー、擬似 ENOSPC（故障注入）、別プロセスの排他。実ディスクは満杯にしない。故障注入は `storage` の書込み関数を差し替える試験用の口（`GENKO_FAULT` 環境変数、Release 配布物では無効）で行う。

## 紙質（BRUSH-01、M3）

`native/tests/data/paper/`（`MANIFEST.json` にハッシュ。`test_paper` が照合する）。紙質は C++ 版だけの機能なので、期待値は Python 版の出力ではない。

- `samples.json`: 解析可能な期待値。固定の 5×3 の紙・seed・全設定（濃さ・倍率・回転・反転・濃淡の反転・合成・適用座標・継ぎ目）の 7 件について、画素の mm 位置→画像上の位置（1/256 画素）→4 画素の混ぜ→紙の値→7 段の被覆に残るインク、と被覆画像 1 枚への適用。`tools/migration/paper_reference.py samples` が `render/paper.hpp` の記述から独立に計算する（IEEE double と整数だけ）。
- `grain.png`（96×96 RGBA、左上が透明へ抜ける）・`grain.jpg`・`grain.gif`・`grain.bmp` と、Pillow 12.3.0 が白に重ねてグレーにした結果 `grain-grey.json`（`paper_reference.py pictures`）。
- `expected/paper-settings.png`（全設定の紙質ブラシ・交差する線・筆圧・不透明度・紙質なしの同じ線）と `expected/inherited-brushes.png`（紙質なしの継承ブラシ全種）: 100×80 mm・300 dpi のペン入れレイヤーの原寸 8bit RGBA。この版の描画を品質確認して承認した期待画像（`GENKO_PAPER_WRITE_EXPECTED=1` で書き直す）。`inherited-brushes.png` が Python 基準の同じ線の描画（`render.layer_image`）と画素まで同じことは契約試験 `test_contract_paper` が確かめる（`test_paper` は Python を使わず、Windows でもそのまま走る）。
- 印刷の照合（`test_paper` printedPdf）: 原寸 PDF（300 dpi）を poppler 24.02 の `pdftocairo`（cairo 描画。固定 rasterizer）で、ネイティブ出力と同じ画素数（`-scale-to-x/-y`、300 dpi で mm を丸めた大きさ）に戻し、PNG 書き出しと 1 画素以内の位置・各チャンネル 2/255 以内で比べる（実測は差 0）。PDF の用紙は mm のままで画素の端数を持つため、文字どおりの `-r 300` では頁全体が再標本化される（`pdftocairo -r 300` で差 34 の画素が数個、`pdftoppm` は画像を平滑化して差 252）。Linux では `pdftocairo`（poppler-utils、native.yml の導入パッケージ）を必須とし、無ければ失敗（SKIP しない）。他の OS（Windows は開発中 deferred）は `GENKO_REQUIRE_PDFTOCAIRO=1` で必須、指定が無く無ければ SKIP し、validation_run.py はそれを予期しない SKIP として不合格にする（配布前の Windows 受入では poppler を用意する）。
