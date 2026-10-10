# Genko C++ 引継ぎ（2026-10-08）

## M4 — 2026-10-09 記録（この節が最新）

### M3・M4 の残り（2026-10-09 訂正）
- **M3 出口の記録の訂正**: 下の「M3出口受入（Linux）」は BRUSH-01（紙質の画像を使うブラシ。改善 I06、AC-BRUSH の紙質、schema-v4 の `paper-texture@1`、PLAN の M3 出口「紙質を別PCへ持ち出して再現できる」）が未実装のまま記録した。その節の試験結果は正しいが、**M3 は完了していない**。BRUSH-01 を実装し、M4 出口の V3 で合わせて受け入れる。
- **M4 の残り**: TEXT-01（ルビの詳細組版と書記素範囲の追従、`ruby-layout@1`、改善 I04、AC-TEXT）、PSD-01（PSD の編集可能な文字、`psd-text@1`、改善 I05、AC-PSD の構造検査と Krita 読込）。M4 出口（AC-TEXT・AC-PSD・AC-EXPORT）はこれらの後。
- 2026-10-09 に一度始めた M4 出口の V3（凍結 2461338）は、M4 が未完成と分かったので ASan の途中で止めた。結果は使わない。

### M3 の残り BRUSH-01 紙質のブラシ（paper-texture@1、改善 I06）（aeaf239…7857af9）— 統合済み
- 凍結 7857af9。計画 `validation_policy.py --base 145530d --head 7857af9 --phase integration`（tier full、Linux 各95試験）。
  - linux-asan: 95/95 合格・CLI23 合格・サニタイザ指摘なし（10/10 00:29–03:21 UTC）。
  - linux-release: 95/95 合格・CLI23 合格（09:28–10:50 UTC）。1回目は作業プロセスの再起動で 59/95 の途中で止まり（interrupted として残す）、同じ計画で Release だけ回し直した。
- 内容: 本のブラシに任意の paper（素材 sha256 の灰色 PNG、濃度・倍率〔1 画素 = 1/300 インチ〕・回転・左右/上下反転・濃淡反転・乗算/減算・紙に固定/線の始点から・繰り返し/鏡映・seed）。合成順は被覆→筆圧→紙質→不透明度。整数の双一次補間と四則だけの sin/cos で OS に依らない。define_brush の paper、初めて使った時に features へ paper-texture@1。紙質の画像が無い・描けない・ハッシュ違いの本は読み取り専用。自分のブラシの紙質は brushes.json（Python と共有）を変えず brush_papers.json・brush_papers/。画面はブラシの「紙質」タブ。
- 利用者の決定（10/09）: 期待画像 native/tests/data/paper/expected/paper-settings.png・inherited-brushes.png を承認。保存先は別ファイル。公開契約の追加（define_brush の paper、brush.custom[*].paper と paper-texture@1、.genkobrush の paper.png）とこの細部（灰色だけ、1辺 4096 px・64 MiB、乗算・減算、反転、倍率の基準）を確定。
- レビュー1回（P1×1: 紙質の画像の欠けた本で描くと Qt の処理から例外で落ちる、P2×4: ハッシュ違いの画像の登録、印刷の試験の skip、16 ビットの灰色、ライブラリの紙質の消失）と修正の再確認（P3 のうち利用者のファイルを消しうる2件と Windows の skip を直した）。
- 印刷の検査は poppler の pdftocairo で PDF を同じ画素数に戻して差 0。Linux では pdftocairo が無ければ失敗（native.yml の apt に poppler-utils を足した。CI は有効にしていない）。Windows の受入にも poppler が要る。
- push 済み（145530d..7857af9）。M3 の出口（紙質を別PCへ持ち出して再現できる）は M4 出口の V3 で合わせて受け入れる。

### 進め方
- M3 と同じく群ごとに「実装（対象試験）→レビュー1回→凍結→V2（Linux Release＋ASan、validation_run.py）→native/integrationへ統合」。V3 は M4 出口だけ。開発は worktree の m4-dev。
- 実行環境（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Xeon 2.1GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18 / cgroup v1。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。V2 は ASan と Release を順に（各構成内 ctest 1並列）。Windows は開発中 deferred（配布前の Windows 実機受入は必須のまま）。

### 群A 台詞・フキダシ・縦書き（M4①）と本とページ・PSD の取込み（M4②a・②b）（b885c30…44789a9）— 統合済み
- 凍結 44789a9。計画 `validation_policy.py --base c8e140e --head 44789a9 --phase integration`（tier full、Linux 各87試験）。
  - linux-asan: 87/87 合格・CLI23 合格・サニタイザ指摘なし（11:36–14:31 UTC）。
  - linux-release: 87/87 合格・CLI23 合格（14:47–15:45 UTC）。result.json の failures は空。
- 経緯（同じ群の前の凍結）:
  - 9ad99e0: linux-release 87/87・CLI23 合格。linux-asan はリンク中に書込み枠を使い切って止まった（試験 87 本で ASan の構成が 27 GB を超えた）。→ 956e541: サニタイザの構成だけ、リンカーが受け付ければ `-z pack-relative-relocs` と `--compress-debug-sections=zlib` でリンク（試験の中身と報告のファイル・行は同じ。330 MB の試験が 198 MB、構成全体で約 19 GB）。Release・Debug は変えない。
  - 956e541: linux-asan 85/87。test_contract_psd が UBSan（幅 0 の層の画像の複製で Pillow の行ごとの memcpy が null の行を読む）、test_gui_lines が ASan（試験が一時の JSON の items() を range-for で回していた）。→ 44789a9: render::Image の複製は画素の無い画像を新しく作る（Genko 側で直した。libImaging は変えない）、試験の一時オブジェクトを名前のある変数に。test_image に幅・高さ 0 の画像の複製（直す前は ASan で落ちることを確かめた）。
- 当初は ① と ② を別の群にする予定だったが、渦巻きの float32 の修正（d352fd5）の試験が ②a の判型の変更に依るため1群にした。群レビューは2回（①の指摘 12c1c75、群全体の指摘 2f2c54a・9ad99e0）。
- push 済み（c8e140e..44789a9）。

### 群B 本とページの画面（M4②c）・書き出し14形式と genko export（M4③a）・書き出し・印刷・履歴・スマホ・目盛り・入稿前の点検の画面と点検（M4③b）（6f26dbc…cd402bb）— 統合済み
- 凍結 cd402bb。計画 `validation_policy.py --base 6b81e31 --head cd402bb --phase integration`（tier full、Linux 各92試験）。
  - linux-asan: 92/92 合格・CLI23 合格・サニタイザ指摘なし（16:14–19:06 UTC）。
  - linux-release: 92/92 合格・CLI23 合格（19:17–20:14 UTC）。result.json の failures は空。
- 群レビューは3人で分担（②c・③a・③b）、指摘 P1×1・P2×6・P3×11（1件は指摘の誤り）。セッションの直しは読取専用の再確認を5回（P1・P2 が無くなるまで）。
- push 済み（6b81e31..cd402bb）。

### 群B のレビューで直した所
- P1: 大きな本を読み込み中の書き出し・印刷・点検が、読んでいない頁を白紙で出し、点検も見落としていた → 読込み中は日本語で断る（formats::run・印刷・checks::book も読み残しのある本を断る）。
- 書き出し: 一時ファイル名に出力名を含めていたため日本語80字の題名で ENAMETOOLONG → 固定長の名前。頁ごとの PSD と pack も失敗時に何も残さない。置き換えは全行き先を先に確かめ、途中で失敗したら元に戻し、戻せない古いファイルの場所をすべて示す。行き先の状態が読めなければ何も置き換えない。dpi は全形式 1〜100000（CLI の断りは code value）、strip は大きさを確かめてから行を作る、PNG の行が int に入らなければ断る。頁の指定の大きすぎる数は Python と同じ「…ページはありません」。
- 作品の結合: コピーしない素材（リンク・中身が名前と合わない）を参照する頁があれば日本語で断り何も変えない（Python はリンクを辿ってコピーする）。読み取り専用でしか開けない本の理由を日本語に。
- 印刷の見本が描けない物を黙って白紙にしていた → 知らせる。プラグインの頁はプラグイン自身の断り。
- 時間のかかる操作（表紙を作る・頁を取り込む等）は Python の SLOW_OPS と同じく状態欄と待ちのカーソル。コマの道具の設定に「原稿」と原稿用紙の設定…・形を戻す。
- 履歴とセッション（保存・ジャーナル）: 履歴の並びの指摘から、セッションの非同期の取り消し・やり直しを直した。M2 から残っていた保存の不整合も含む（ほかの書き手の保存と重なった取り消しで保存の衝突が続き以後保存されない、書き込みの最後だけ失敗した編集が読み直しで二重に適用される、保存した復旧用コピーの採用の取り消しで衝突が続く、ジャーナルの取り消しの待ち中のやり直しで衝突が続く）。
  - ジャーナルの取り消し・やり直しの数は、最後に読み書きした深さと待ち行列を順にたどる一か所（journal_after）で決める。
  - 原稿に渡した段は取り下げない（書き込み中のやり直しは「書き終わってから」と断る）。
  - 読み直しの衝突は保ち、その間は何も書かない。読み直しは失敗しても再試行する。
  - 読み直しで写し直さない取り消し・やり直しは行わずに知らせる。原稿が取り込んだ段は写し直さない。
  - C++ だけの知らせ・断り（busy、行わなかった取り消し・やり直しの知らせ、recover_undo）は SPEC COMP-01a。
  - 既知の違い（残す）: 書いていない変更を、やり直すものが無い時にジャーナルの取り消しの前に取り消すと、その読み直しの後にやり直せない（本は一貫している。Python はやり直せる）。

### 群B の Python との違い（SPEC.md COMP-01a）
- この build が描けない台詞・絵（NotYetPorted）は点検・書き出し・印刷で黙って省かず日本語で知らせる（ARCHITECTURE §4a）。印刷は先に 10 dpi で描き、失敗する頁があればプリンターへ何も送らない。
- 書き出し: PNG・JPEG・TIFF・PDF・CMYK・PSD・pack は Pillow／Python とバイトまで一致（formats の Pillow と同じ書き手）。EPUB・Kindle と RGB の sRGB プロファイルは時刻の刻印だけ違う（Python 同士でも違う）。原稿の中の PNG（render::write_png）は変えない（ARCHITECTURE §4a）。失敗した書き出しはファイルを残さない。Python が例外の表示で終わる所は JSON の誤りで返す。置いた絵のある頁と official の書き出しは not_yet_ported（M5）。
- 点検が古くなるのは原稿が変わった時と別の原稿を出した時だけ（Python は頁を移っただけで「原稿が変わりました」）。高精度の色のレイヤーは画素として数える（C++ だけ）。点検に畳み込む preflight は checks.book が使わない dpi の表を省く。
- 履歴の並びは C++ のセッションの順。書き出しのダイアログの最小幅は画面に収める。履歴・点検の枠はページの枠とタブ。
- 本とページの画面: 「PSD をレイヤーのまま読み込む…」はファイルを尋ねる。ダイアログの間に原稿や頁が変わったら適用せず知らせる。読めないノンブルの値・巨大な頁の範囲は日本語で断る。原稿用紙で dpi だけ変えると数値を送る（Python は dpi を落とす）。新しい原稿のダイアログの名前は Python と同じ genko.export.safe_name。

### 利用者の決定（2026-10-09）— Python と意図して違う所
- D1 判型の変更（set_page_spec）: Python は表紙・カバー・帯のペイントの画素を本の用紙に切って消し、マスクも中身とずれる。C++ は各ページ自身の新しい用紙（カバー・帯は spec_for）の大きさで画素を動かし、マスクも中身と同じく旧基本枠から新基本枠へ動かす（SPEC §2「既知の不具合を再現しない」）。開けないマスクは Python と同じく触らない。
- D2 頁の取込み（import_pages）と頁の複製（duplicate_page）: Python はレイヤーに新しい id を振るのに参照を直さない。C++ は parent_id・台詞の style.below_layer・定規の layer_id・アニメーションのフォルダ・セル・ライトテーブル・saved_areas の {"layer": id} を新しい id へ付け替える（頁の複製は M1/M2 からの振る舞いも変わる）。
- 照合の印: 意図して違う所は契約の場合に `"cpp": "deviates"` と `cpp_says`（決定）・`cpp_differs`（違ってよい場所を正確に）・`cpp_keeps`（C++ が守ること）を書き、実際に違うこと・守れていること・保存と読み直しでも守れていること・Python の本では守れないことを確かめる。それ以外は Python と完全一致。SPEC.md COMP-01a に行を足した。

### 群A の C++ だけの断り（Python の検査の後。SPEC.md COMP-01a）
- 台詞: 文字列・null 以外の frame_id、有限でない数、画素を読めない style の画像。素材の画像の絶対パス・".."・ライブラリの外・リンク・64 MB 超。
- 本とページ: 有限でない紙・塗り足し・余白、画素を持てない大きさの紙、この build が読み取り専用で開く原稿からの取込み、copy_assets は1件 256 MiB まで（リンクを辿らない）。
- PSD: 1チャンネル 480 MB・絵 120 M 画素・合計 8 GiB・ファイル 2 GiB、取込み全体で画素の層の数 ×（頁の層＋画布の画素）が 128 × 120 M 画素まで（A4 600 dpi で約 210 層）。

### 群A の数値・描き方の判断
- 文字組は Pillow 12.3 の BASIC（raqm なし）に固定（render_harness・pyref_harness も）。OpenType 機能・32 MB を超える書体・読めない書体は頁全体を失敗させず、その台詞の文字だけ省いて知らせる（NotYetPorted text_features・large_font・default_font）。
- 文字のゆがみ（warped_letters）32 行のうち 3 行は numpy の SVD の誤差で ARCHITECTURE §9 の近似照合（compare_picture_near）の範囲内。他は画素まで一致。
- sinf・cosf・powf は Arm optimized-routines v25.01（MIT、glibc 2.39 の実装）を render/libm_float に移植し、全 2^32 の float でシステムの libm とビット一致。渦巻き・波・トーン・PSD がこれを使い OS に依らない。
- 高精度の色の頁（C++ だけの機能）のレイヤーの下の台詞は 8 ビットの頁と同じに描き、塗った画素だけを線形の光として戻す（塗った画素は 8 ビットの精度）。
- 台詞の描画は render/text/ に置いた（頁の描画がレイヤーの間にフキダシを描くため。ARCHITECTURE §1 の text/ 層とは違う）。
- 台詞パネルの色の欄が壊れている時は既定の色でダイアログを開く（Python は誤りを記録してダイアログを出さない）。

### 群A の先送り・未移植（理由）
- replace_text の regex: true は not_yet_ported（Python の re 全体の移植が要る。リテラルの検索と IGNORECASE は CPython 3.12 と全符号位置で照合済み）。採るかは利用者判断。
- Pillow の PsdImagePlugin（PSD を画像として開く平坦化）は別の読み手なので NotYetPorted。高精度の色の画素の判型の変更は NotYetPorted（C++ だけの機能）。

## M3出口受入（Linux）— 2026-10-09 記録

- **凍結SHA**: `9b344f5f74c9d7d8b2dc679dc89ea1e39a1645a8`（native/integration。M3 の4群を統合した後の記録の版）。計画は `validation_policy.py --base 61e2e2c --head 9b344f5 --phase milestone`（tier full、Linux 各81試験、Windows 各51試験＋契約30除外）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Xeon 2.1GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18 / cgroup v1。Qt 6.11.2（aqtinstall 3.3.0）、GCC 13.3。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、`OPENBLAS_CORETYPE=Haswell`・`NPY_DISABLE_CPU_FEATURES` は native.yml と同じ。root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。ディスクの書込み枠のため構成は順に実行: ASan（各構成内 ctest 1並列）→ ASan のビルドを消して Release と Debug を同時に。
- **結果**（validation_run.py の result.json）:
  - linux-asan: 81/81 合格、サニタイザ指摘なし、CLI23 合格
  - linux-release: 81/81 合格、CLI23 合格
  - linux-debug: 81/81 合格、CLI23 合格
  - `--pair`: 失敗は「構成artifactの欠落・重複」1件のみ＝Windows 2構成が開発中 deferred。Linux 3構成どうしの照合（計画・範囲・source/binary pins・生ログ・CLI23）は指摘なし。**Windowsを含む --pair 合格ではない。**
- **実xcb（Xvfb 1920×1080）GUI・E2E**（凍結SHAのビルド）: Release で gui_actions 14・gui_anim 6・gui_brushes 8・gui_canvas 76・gui_color 60・gui_colours 4・gui_effects 12・gui_guides 7・gui_layers 12・gui_layout 32・gui_materials 35・gui_paint 11・gui_pen 51・gui_select 13、Debug で gui_save 36・app_e2e 8（開く→描く→Undo→保存→終了→別プロセスで再開）、全合格・FAIL 0。Release の gui_save は故障注入が Debug だけのため SKIP 1（合格に数えない。Debug で 36 合格）。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。試験は表示のみで合否判定なし。測定機は上記コンテナで基準クラスの合否ではない）: 開いて最初の頁 456 ms、全頁の読込完了 568 ms／入力→ライブ線 p95 0.56・p99 2.90 ms（n=43780）／入力→描画 p95 1.44 ms／線確定→表示 p95 19.4 ms／Undo→表示 p95 26.5 ms／頁切替 最初 p95 36.3・精細 p95 89.3 ms（n=4）／保存中のイベントループ遅れ p99 4.46 ms／自動保存 p95 1.80 s（複数線の一括保存）。F1頁 350dpi 全描画 947 ms・1線追加 491 ms・512px部分 26 ms。
- **Windows**: 開発中 deferred（実 build・試験なし）。配布前の Windows 実機受入は必須のまま。
- **M3 の群**（各群の V2 と Python との差は下の「M3」の節）: ①高精度フィルター・補正・色管理・レイヤーパネル・範囲選択、②ABR・ブラシ、③アニメ・タイムラプス・プラグイン、④描画道具・線の編集・定規と3D・素材・トーン・効果線・サブビュー。
- **M3 の残課題・利用者判断**: 素材一覧のダブルクリックの振る舞い、COMP-04 のネイティブ拡張（実行ファイル型プラグインの登録形式＝公開契約の追加）、CMYK/Lab の扱い、素材ライブラリ一覧の 1 MB 上限。画像を読み込む（act_import）は M5、TIFF/WebP/PSD を画像として開く（Pillow の PSD 平坦化を含む）は M4 の形式。

## M3 — 2026-10-08 記録

### 進め方
- 群ごとに「実装（対象試験）→レビュー1回→凍結→V2（Linux Release＋ASan、validation_run.py）→native/integrationへ統合」。V3はマイルストーン出口だけ。
- Windows は開発中 deferred（計画に windows-debug/release は出るが実行しない）。配布前の Windows 実機受入は必須のまま。
- 実行環境（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU / RAM 16GB / Ubuntu 24.04.5 / kernel 6.18 / cgroup v1。参照 Python 3.12（Pillow 12.3.0）と native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。V2 は Release と ASan を順に（各構成内 ctest 1並列）。

### ①-1 高精度画素の消しゴム・範囲削除・移動・変形・貼り付け・マスク（5cdefd6, fd48310）— 統合済み
- 計画 `validation_policy.py --base a5e9466 --head fd48310 --phase integration`（tier full、Linux 各65試験）。
- linux-release: 65/65 合格・CLI23 合格。linux-asan: 65/65 合格・CLI23 合格・サニタイザ指摘なし。
- push 済み（a5e9466..fd48310）。

### ① 高精度フィルター21種・補正レイヤー9種・色管理・レイヤーパネル・範囲選択（2f13472…1bac9e7、試験修正 0d7f2bf）— 統合済み
- 凍結 1bac9e7。計画 `--base fd48310 --head 1bac9e7 --phase integration`（tier full、Linux 各69試験）。
  - linux-release: 69/69 合格・CLI23 合格。
  - linux-asan: 68/69・CLI23 合格。test_color_filters で heap-use-after-free。試験が edit() の返した本を一時のまま view() に渡し、画素を読む前に解放していた（製品コードではない）。
- 試験の修正 0d7f2bf（名前のある変数に保ってから読む）。計画 `--base 1bac9e7 --head 0d7f2bf --phase integration`: linux-release 全69/69・CLI23 合格、linux-asan 関連16/16（test_color_filters を含む）合格・サニタイザ指摘なし（この計画では ASan の CLI23 は対象外）。
- push 済み（fd48310..0d7f2bf）。

### ② Photoshopブラシ（.abr）の読込・自分のブラシ・ブラシパネルとツールの設定（1733e3a…4d423bb）— 統合済み
- 凍結 4d423bb。計画 `--base 6d0924f --head 4d423bb --phase integration`（tier full、Linux 各71試験）。
- linux-release: 71/71 合格・CLI23 合格。linux-asan: 71/71 合格・CLI23 合格・サニタイザ指摘なし。
- push 済み（6d0924f..4d423bb）。

### ③ アニメーション・タイムラプス・フィルターのプラグイン（2e5f9d9…26cc5ac）— 統合済み
- 凍結 26cc5ac。計画 `--base d51a1f8 --head 26cc5ac --phase integration`（tier full、Linux 各74試験）。
- linux-release: 74/74 合格・CLI23 合格。
- linux-asan: 1回目は試験の前のビルドで止まった（リンク中に "No space left on device"。このセッションの書込み枠を使い切った）。古い試験の一時フォルダ・pip/uv のキャッシュ・Debug のビルド（M3出口で作り直す）を消して空きを作り、同じ計画で ASan だけ再実行: 74/74 合格・CLI23 合格・サニタイザ指摘なし。
- push 済み（d51a1f8..26cc5ac）。

### ④ 描画道具・線の編集・定規と3D・素材とトーンと効果線・サブビュー（aae4fcc…dd868fd）— 統合済み
- 凍結 dd868fd。計画 `--base 69152b6 --head dd868fd --phase integration`（tier full、Linux 各81試験）。
- linux-release: 81/81 合格・CLI23 合格。linux-asan: 81/81 合格・CLI23 合格・サニタイザ指摘なし。（1回目はコンテナの再起動で途中で止まり、同じ計画で最初から再実行した。）
- 群レビュー後に直した所（Python との差の記録）:
  - Python より守る側: 素材パックの pack.json の "file" はパックの中だけを読む（".."・絶対パス・リンクのフォルダを通る項目は飛ばす。Python 3.12 はパックの外も読む）。素材ライブラリの一覧は 1 MB まで（超える変更は断り、画像を残さない）。スキャナー（WIA）の保存先は Windows の区切りで渡し ' を '' にする。素材フォルダのリンク拒否（M2 の安全検査）は緩めない。
  - Python に揃えた: 置いたガイド線は描かない（ドラッグ中だけ見える。目盛りとガイドの引き出しは台帳上 M4）。定規へ吸着中のペンは描いている間も吸着後の線と対称コピーを見せる。サブビューの枠は最初は隠す（出したままだと 1024×640・1366×768 の 200% で窓がはみ出した）。
  - 見落としの修正: M3④で足した add_shape・smudge・liquify を test_command_bus の登録一覧に入れた（④の対象試験に含めていなかった）。
- push 済み（69152b6..dd868fd）。

### 未決・利用者判断（M3出口で報告）
- 素材一覧のダブルクリック: M2 の振る舞い（ページ中央にすぐ貼る）を維持。Python は「クリックした所に置く」待ちになる（「貼る」ボタンは Python 通り）。
- COMP-04 のネイティブ拡張（Python 以外の別プロセス画像フィルター）: 実行ファイル型プラグインの登録形式は公開契約の追加になるため利用者確認待ち。
- CMYK/Lab の扱い（既出の判断項目）。

## M2出口受入（Linux）— 2026-10-08 記録（この節が最新。下の旧節の「マージ後未実施」はこの節で置き換わる）

- **凍結SHA**: `61e2e2c99857c26c634960f090403e53d339a0c4`（native/integration）。計画は `validation_policy.py --base 5e07b91 --phase milestone`（tier full、Linux各64試験、Windows各41試験＋契約23除外）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Xeon 2.8GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18 / cgroup v1。Qt 6.11.2（aqtinstall 3.3.0）、GCC 13.3。参照 Python 3.12.3＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、`OPENBLAS_CORETYPE=Haswell`・`NPY_DISABLE_CPU_FEATURES` は native.yml と同じ。root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行（書込不能の試験を成立させるため）。3構成を同時実行、各構成内は ctest 1並列。
- **結果**（validation_run.py の result.json）:
  - linux-debug: 64/64 CTest 合格、CLI23 合格
  - linux-release: 64/64 合格（既知の故障注入SKIPのみ）、CLI23 合格
  - linux-asan: 64/64 合格、サニタイザ指摘なし、CLI23 合格
  - `--pair`: 失敗は「構成artifactの欠落」1件のみ＝Windows 2構成が開発中 deferred（VALIDATION §0）。Linux 3構成どうしの照合（Release SKIP↔ASan PASS、source/binary pins、生ログ）は指摘なし。**Windowsを含む --pair 合格ではない。**
- **実xcb（Xvfb 1920×1080）GUI・E2E**: Release で gui_materials 34・gui_color 57・gui_pen 51・gui_actions 14・gui_canvas 76・gui_layout 32、Debug で gui_save 36・app_e2e 8（開く→描く→Undo→保存→終了→別プロセスで再開）、全合格・FAIL/SKIP 0。7a22bda の実行物で実行し、61e2e2c と Release/Debug の66実行物が同一バイトであることを result.json の binary pins で照合。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。試験は表示のみで合否判定なし。測定機は上記コンテナで基準クラスの合否ではない）: 開いて最初の頁 214 ms、残り頁の読込完了 833 ms／入力→ライブ線 p95 1.14・p99 4.05 ms（n=43780）／線確定→表示 p95 18.1 ms／Undo→表示 p95 19.4 ms／頁切替 最初 p95 18.9・精細 p95 92.8 ms（n=4）／保存中のイベントループ遅れ p99 4.09 ms・250 ms超 0／自動保存 p95 2.42 s（複数線の一括保存で、PERF-E の「1線追加の差分保存」は別途未測）。F1頁 350dpi 全描画 1478 ms・1線追加 646 ms・512px部分 31 ms。
- **Windows**: 開発中 deferred（実 build・試験なし）。配布前の Windows 実機受入は必須のまま。
- **利用者判断（2026-10-08）**: M2出口は Linux 分で完了とし、M3以降の改修をすべて終わらせる。Windows の確認（windows-debug/release の実行と、それを含む --pair）は、すべての改修が終わった後に別の環境で行う。受入のための変更（test_precision_merge_controls の上限1200秒、CLI23 の cgroup v1 記録）は残す。

### この候補での変更（c2fd22a→61e2e2c）
- 頁の必要時読込（SPEC PERF-01）: 窓は最初の頁の素材だけ読んで表示・編集を受け付け、残りは Session の worker で読み、読込中の未保存変更を全体へ載せ直す。未読込の頁を持つ本は writer が書かない（"partial"）、CommandBus は未読込の頁に届く操作を拒否（"page_not_loaded"）、描画・サムネイル・キャッシュは未読込の頁を扱わない。
- test_material_preview_cache を QtTest 化・CTest 登録。test_gui_actions の操作数を 60（Bの6操作）に訂正。
- 公開スキーマ（genko schema）を Python 版に戻す（Bの merge_visible.flatten）。flatten / preserve_precision は「無視した未知キー」と報告しない。
- merge_down の strict_gates を Python 版に戻す（利用者判断）。L6-gate 試験は拒否と原稿不変だけを確認。
- 描画契約: ノンブル（Aで移植済み）を参照側でも描いて比べ、参照の文字組は raqm なし（BASIC）に固定。
- CLI23 は cgroup v1 の記録も読む（v1は oom = oom_kill + under_oom）。app E2E は wait_read で全頁読込を待ってジャーナルUndoを確認。test_precision_merge_controls の CTest 上限 1200 秒（ASan で約460秒）。
- レビュー: 独立レビュー（マージ解消箇所＋手順3差分）P1なし・P2 8件→7件修正（P2-7は残課題）、限定再レビュー P1なし・P2 5件→全修正。3周目のレビューは行っていない。

### 残課題
- Windows Debug/Release（deferred）と配布前の実機受入。
- 読込中に閉じると読込完了まで待つ（データは失われない。レビューP2-7）。
- 素材ライブラリの編集GUI（画像を追加・範囲を登録・名前・消す・タグ・素材パック）は台帳上 M3。
- このbuildだけのキー（flatten、preserve_precision）を公開スキーマ外で扱う方針を M5 の MCP/CLI 契約で整理。
- ledger.json / improvements.json の status は未更新のまま。
- 環境メモ: 参照 Python は 3.12（python3 が 3.13 の環境では JSON エラー文言が変わる）と native.yml の数値用環境変数が必要。tools/ci/fetch_qt.py の Linux 版は ICU を取らず使えない（aqtinstall を使う）。
- 次の工程: M3 ①高精度フィルター・消しゴム・選択＋色モデル/チャンネル/補正。

## 再開位置
- 作業場所: `/home/hermes/genko-test/cpp-impl/wt/integration`
- 現在のブランチ: `native/integration`。利用者のマージ指示により、引継ぎ用branchのmerge commit `131c8e7`をfast-forwardで取り込み済み。
- 現在のintegrationは未受入WIPを含む。最後の受入済み基点は `5e07b9102b2768d062825744ecd83cfb44e64f05`であり、履歴に保持。Python stable/mainは変更していない。
- 元A: `native/m2-material-previews` / `9756f25e021e2bb2e2991118a1d2e193cf24ea14`（本体snapshot `2ec250a`、PNG保全 `9756f25`）。元B: `native/m2-user-material-cache` / `835d4dd2c75b0ac036977ee050071c93e56f42e2`。両元作業木は保全し、削除しない。

## 依頼と境界
- 最新依頼は「今の状態をマージして別のセッションで引き継げるようにして」。引継ぎを製品完成/受入済みと扱わない。
- 主担当は接続済みgpt-6.1-solによるHermesの直接実装。別AIは読取専用レビューのみ。Claudeと自己紹介した先の回答は誤りであり、モデル/担当の根拠にしない。
- 新費用/権限/認証変更/CI再有効化/cron/observerを追加しない。契約上限では停止して報告し、自動切替しない。
- 本番原稿/認証は扱わない。元CPU1/RAM2GiB/-j1、fixture/安全検査/画素閾値を維持。Windows開発中deferred、最終配布前native受入必須。waivedは合格ではない。

## 読む正本
1. `genko-cpp-migration` skill
2. `/home/hermes/genko-test/cpp-impl/PROGRESS.md` と `EXECUTION-QUEUE.json`
3. 本branchの `docs/cpp-migration/{SPEC,PLAN,ACCEPTANCE,VALIDATION,ARCHITECTURE}.md` と既存機能/改善/PhotoCraft台帳
4. PhotoCraft固定参照 `/home/hermes/genko-test/references/photocraft-v0.2.0`、commit `ad863217386440ca968fccc9bfff65ba24e61142`（変更禁止）
- `handoff/PROGRESS.snapshot.md` と `handoff/EXECUTION-QUEUE.snapshot.json` は引継ぎ時点の保全コピー。通常の更新正本は上記rootファイルであり、新台帳を増やさない。

## マージした変更と検査状態
- A: 組込み素材preview/検索/配置、FreeType字形基盤、ノンブルのGUI/描画/保存契約、素材cache本体。
- B: 高精度結合/flatten/変換、u16/f32画像↔精密ペン線、保存feature `native.color_stroke_v1`、編集時色保持、安全拒否、素材cache単体試験。
- 明示競合はmain_window.hppとmain_window_actions.cpp。nombre_dialogとlayer_operationの両方を保持して解消。共有render/core/CMakeの自動マージ部分も変更されているため、旧受入をそのまま新候補に拡張しない。
- 元snapshotの全変更パスSHAを照合済み。元ソースは各親branchに完全保全。fixture MANIFESTの70項目は実bytes一致、ノンブルPNG64枚をGitへ追加保全。
- このマージ後のcompile/製品試験/独立レビュー/V2/V3は未実施。マージ保存は受入合格ではない。

## 中断した文字作業
- `native/tests/unit/test_render_page.cpp`の未実行plainTextCases 2slotは稼働試験から外し、`handoff/unfinished-text-tests.patch`へ保存。元A snapshotにも完全に残る。
- 対応するtext-cases.jsonは未生成。RED未確認、renderer未実装。patchを今すぐapplyして全試験を起動しない。
- 元A `build/nombre-reference.py` のtext生成分岐と `build/material-v1.py` のtext phaseは準備のみ。実API/region型/registryを読んでから再開する。build失敗をREDと数えない。

## 証拠の場所（既存結果を再実行しない）
- A `/home/hermes/genko-test/cpp-impl/wt/m2-material-previews/build/`
  - `material-formal-v1/execution.json`: 旧限定Releaseの5suite/95QtPASS。後続18source変更のため現候補全体へ流用不可。
  - `material-stamp-review/`、`material-preview-review/`、`material-stamp-asan/font-glyphs-v2/`: 素材/字形の限定review・sanitizer証拠。
  - `gui-xcb-material-stamp-fixes/execution.json`: 限定GUI passed。
- B `/home/hermes/genko-test/cpp-impl/wt/m2-user-material-cache/build/`
  - `material-cache-review/result.json`: 3source限定合格、GUI library入口は対象外。
  - `m2-shared-color/precision-pen-related-v4/execution.json`: マージ前10suite/347QtPASS、fail/skip0、source不変、OOM0。
  - `m2-shared-color/gui-xcb-precision-pen/{execution.json,visual-review.json}`: マージ前の実xcb限定4QtPASS・画像所見。
  - `m2-shared-color/precision-pen-review/fixes/review.json`: 旧4P1の安全修正合格。filter/高精度pixel編集は明示拒否であり機能未完成。
- 旧ログ・build・失敗attempt・raw/JSON/画像は元作業木に保持。このGit snapshotに全build binaryは含まれない。同一ホストの別セッションで参照できる。

## 残作業と次の具体操作
- 次はこのマージ差分のcompileと関連正常/拒否/保存/画素/GUIの確認。共通境界を変えたので規定に従って対象拡大。先に差分/実APIを読み、driverを新設しない。
- M2残: ユーザー素材libraryのGUI/cache接続、頁の必要時読込、既存StartDialogの1024x640/初期preview不足分、開く→描く→Undo→保存→終了→別process再開とAC-PERF。既存E2E/perf/開始画面を作り直さず現物確認する。
- 再構成手順はrootの単一EXECUTION-QUEUE.json。M2出口の不足を閉じる→変更/競合だけreview→凍結→VALIDATIONの必要構成/実GUI/性能→現integration候補の受入を閉じる。今回の利用者指定マージを受入合格の代わりにしない。
- 続くM3/PhotoCraft: 高精度filter/画素消去・選択、色モデル/チャンネル/補正、紙質/ブラシ/ABR、残描画/アニメ/タイムラプス/plugin。M4写植/ルビ/フキダシ/全出力/PSD、M5制作サービスとMCP/HTTP/CLI、M6両OS配布、M7最終受入は未完。
- 計画/台帳を全件再生成しない。短試験→review→必須修正→凍結→規定重受入→統合の単位を保ち、合格済みの同一source/binary/config/fixture/scopeを再利用する。review1回/受入1回は目安であり、重大不具合や無効化された証拠の再確認を禁止する意味ではない。

## GitHub反映済み
- `native/integration`へマージ済み。GitHubへのpushは再試行で成功し、remote SHA `faf2b01027e319b79c616b554ac70d3496e2e540`を照合済み。本記録更新はその後の文書commit。
- 先行push/REST書込みでのHTTP500は解消し、通常pushで反映した。force push、認証・権限変更、CI再有効化は行っていない。
- 再開先はGitHubの `native/integration` と `docs/cpp-migration/HANDOFF.md`。新規buildや再mergeは引継ぎだけのためには不要。マージ後の製品受入が未実施であることは変わらない。
- `/home/hermes/genko-test/cpp-impl/genko-integration-handoff-20261008.bundle` はGitHubに既存のb4f8fc3を前提とする差分bundle。既存repositoryへfetchでき、ソースと引継ぎ書を別環境へ持ち出せる。旧証拠/binaryは元ホストに残る。

## 他セッションへ渡す文
「Genko C++作業を引き継いでください。`/home/hermes/genko-test/cpp-impl/wt/integration/docs/cpp-migration/HANDOFF.md` を読み、native/integrationの未受入マージ候補から、既存キューのM2出口を継続してください。Python安定版・元作業木・証拠を保全し、主担当が直接実装してください。」
