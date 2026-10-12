# Genko C++ 引継ぎ（2026-10-08）

## M5②（Studio のサービスと genko studio）・M5③（MCP と HTTP）— 2026-10-12 記録（この節が最新）

### V2 — 統合済み
- 凍結 `17a22bf7ebb6660db7838b72a677d2c7d3727b75`。計画 `validation_policy.py --base 3192b93 --head 17a22bf --phase integration`（tier full、Linux 各 112 試験）。
  - linux-asan: 112/112 合格・CLI23 合格・サニタイザ指摘なし（10/12 11:24–16:47 UTC）。result.json の failures は空。
  - linux-release: 112/112 合格・CLI23 合格（10/12 17:25–18:55 UTC）。result.json の failures は空。
- 1 回目（凍結 ef14b1d）は ASan で test_contract_mcp の jobs（client_stdio）が落ちた。取り消した export_status がサーバーで最大 3 秒動き続け、SDK の stdio クライアントは会話の終わりの 2 秒後にサーバーを止めるので、その呼び出しが道具の記録に残るかが速さで変わっていた。照合を緩めず、取り消しの後は記録に書かれるまで待つ形に直し（17a22bf）、ASan から V2 をやり直した。
- その前の試み（凍結 6b0d6b7）は、ASan のビルドが 23 GB（874 段の 861 段目）に達して空きが尽きかけたので止めた（結果は使わない）。サニタイザーのビルドに限り Genko のライブラリと同梱の Pillow を共有ライブラリにした（ef14b1d。全体で約 4 GB。genko_render_ops は共有のときだけ genko_render に入れる。zlib を直接使う 2 試験の DEPS を書いた）。デバッグ情報も行の表だけ（-g1、6b0d6b7）。Release・Debug・Windows は静的のまま。
- 参照の MCP の Python は GENKO_MCP_PYTHON（tools/migration/mcp_reference_requirements.txt、mcp 2.2.0）。無いと test_contract_mcp は SKIP になり、validation_run の監査が不合格にする。
- Windows は開発中 deferred（配布前の Windows 実機受入は必須）。

### M5②（1998525・6e5a24b）
- native/src/service/: StudioService と HumanService の全メソッド（エージェントの道具 39 と projects・create_project・upscalers を Python と同じ引数・ToolResult で `StudioService::call` から呼ぶ）、絵の道具、ネームの取り込みと解析、点検・書き出し・仕事（jobs）・正式な書き出し。api/studio_cli.cpp に `genko studio` の全命令。
- test_contract_service（参照 Python と flow・style・drafts・art・ship・atari・cli・palette を照合。cli は同じプロセスで本・監査・ファイルまで厳密に、別のプロセスでも stdout と終了コードを照合）と test_service。
- レビュー（P1×2・P2×12）で直した所: for_pages の内側の op にもエージェントの op 一覧と path の閉じ込め（Python にもある穴。AC-API の root 外パス）、プレビューのファイル名の id を安全に、消えたページを参照しない、v1〜v3 の本は副作用の前に needs_migration（genko studio は終了コード 3）、request_approval の人物を 1 回で保存、チャット承認をロックの中で確かめ直す、正式な書き出しは承認の保存までロックを持つ、アプリの正式な書き出しを service::run_export に、画素を展開する前にヘッダーで 64 MP、PDF の数をロケールに依らず、confine を call のキーワードから外す、パレットの絵（モード P）を Pillow と同じバイトで書く、ほか。
- 一度だけの失敗（cli の手順 42、stdout が空で exit 1）は 3300 回で再現せず、原因は不明。リトライは入れず、試験の Run が crashed・stderr などを出すようにした。

### M5③（f65dd12・4d930f9）
- api/mcp_*: Python と同じ道具 41 と 4 リソース、JSON-RPC（初期化・道具・リソース・取り消し・ジョブ）、stdio と Streamable HTTP（`genko mcp --transport streamable-http --host --port`。既定は stdio。名前と既定値は公式 SDK）。api/headless_server・http_server・tokens: `genko serve`（server.py の HTTP API）、`genko token`。
- test_contract_mcp（公式 mcp.Client の stdio・HTTP と生の JSON-RPC・HTTP API を C++ と Python のサーバーで照合）、test_mcp_api_e2e（エージェントが人だけの操作を通れない、人の道具が無い、外部コマンドの引数、HTTP の検査）、test_http_server。
- レビュー（P1×1・P2×3・P3×13）で直した所: /v1/apply の import_pages の from を root の中に（Python にもある穴）、ヘッダーの段階で Host→Origin→Content-Type→token を確かめて断る要求の本文は読まない、許可した Origin への断りにも ACAO・Vary、token は SHA-256 を全件と同じ手間で比べる、chunked を厳しく、tokens.json は要求ごとに読み直す、スレッド 64・1 アドレス 32 接続・ヘッダー 10 秒の上限、ほか。

### 利用者の決定（2026-10-12）
- 正式な書き出しの screen は指定どおり使う（Python は真偽値で上書きして使わない）。CLI の裏の仕事は終わるまで待ってから終える。C++ だけの扱い（承認依頼の人物・下書きの採用は全部確かめてから 1 回で保存、作品はロックの下で作る、v3 の本への書き込みは needs_migration、CLI の失敗 JSON に code）を確定。
- `genko studio` の --agent（human:x などで人として振る舞える）は Python と同じにする（制限しない。既知の制限）。
- `genko mcp --transport streamable-http --host --port` の形を確定（MCP の HTTP は SDK と同じく認証なし）。
- 書体のパス（style.font・set_nombre.font）は、システムの書体を指す正当な使い方があるので root に閉じない（あるかどうかだけが分かる。既知の制限）。

### 残課題
- M5④（画面）と M5 出口の準備（F3・PERF-F）は m5-4・m5-exit で作業中。M5 出口の V3 はその後。
- Windows 実機受入（配布前に必須）。

## M5 と M3・M4 の残り群E — 2026-10-11 記録

### 進め方
- 群ごとに「実装（対象試験）→レビュー1回→凍結→V2（Linux ASan＋Release、validation_run.py）→native/integration へ統合」。群E（M3・M4 の残り、worktree `m4-rest`）と M5①（worktree `m5-dev`）は並行して作り、それぞれ独立レビュー1回の指摘を直した後、M5① を群E の上へ載せ直して、2群をまとめて1つの候補に凍結し V2 を1回行った。
- 群E だけで先に回した V2（凍結 6a8d1b0）は、ASan で test_pyre が落ちた（正規表現の探す手間の上限の試験が Release の実測で決めた 10 秒で、-O0＋ASan では 62 秒かかる）。試験を「同じプロセスで測った単価×予算×3」（最適化した構成では 10 秒・15 秒も残す）に直し（5a5f650）、ASan の段の途中で止めた。結果は使わない。直した版で群E に関わる 15 本を ASan で先に確かめた（15/15 合格）。
- 実行環境（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU / RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44。Qt 6.11.2、参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9。

### V2（群E＋M5①）— 統合済み
- 凍結 `13a425d25fbece4e173191ae7229d4b23c9a3b06`。計画 `validation_policy.py --base b95eaf9 --head 13a425d --phase integration`（tier full）。
  - linux-asan: 107/107 合格・CLI23 合格・サニタイザ指摘なし（10/11 15:57–20:48 UTC）。result.json の failures は空。
  - linux-release: 107/107 合格・CLI23 合格（10/11 21:20–22:40 UTC）。result.json の failures は空。
- Windows は開発中 deferred（計画の windows-debug/release は実行しない。配布前の Windows 実機受入は必須）。

### 群E（M3・M4 の残り。a6342f2・6a8d1b0・5a5f650）
- replace_text の `regex: true` と台詞の検索・置換の窓を、CPython 3.12.3 の `re`（sre）の移植で行う（`native/src/core/pyre*`、Unicode の表と名前は生成。CPython・Secret Labs AB・Unicode License v3 の許諾文を `native/third_party/cpython/` に同梱）。参照 Python と照合（手書き約 770 件・乱択 3 万件の型、置換テンプレート、エラー文と位置）。
- COMP-04 のネイティブ拡張: `<設定>/plugins/<名前>/plugin.json`（`docs/PLUGIN_NATIVE.md`）。既定で無効、同意は plugin.json と本体の sha256 に結び付け、同意したバイトを設定フォルダーの非公開の写しへ書き出して動かす（差し替えたものは動かない）。シェルなし、環境変数は許可リスト、時間・出力の上限、POSIX ではプロセスグループごと止める（Windows は子の子を止められない＝限界として記載）。
- PSD を Pillow 12.3 と同じに一枚の絵として開く（`render/psd_image`）。Lab の PSD は Little CMS を通すため未移植として断る。
- 素材ライブラリの一覧（library.json・folders.json）の上限 16 MB。素材一覧のダブルクリックは Python と同じく「貼る」（置く所をクリックで選ぶ）。
- レビュー（P1×1・P2×4・P3）で直した所: 正規表現の手数の数え漏れ（印の写し・選択肢・文字集合）、数字のあふれ（Python と同じ OverflowError・invalid group reference に。正規表現でない置換テンプレートの退行も）、同じ名前の .py と plugin.json の同意を取り消せない、確かめてから動かすまでの差し替え、ライセンス表示、読めない本体・リンク・リンクのフォルダー、標準エラーの末尾、同意の問いに本体と引数、folders.json の上限。

### M5①（Studio の状態と制作の操作。1a3f544・d6d4039・13a425d）
- 制作の操作 46 種（`core/ops_studio.cpp`。絵の大きさが要るものは render から差し込む）、`native/src/studio/`（presence・toollog・claims・依頼のフォルダー・道具の登録・入力の型・本から読む状態）、配置画像と白黒の仕上げの描画（`render/placed`・`render/screentone`）。頁・レイヤー・PSD・書き出しの NotYetPorted（placed・finish）を外した。群E の上へ載せ直して、PSD の配置画像も Python と同じ画素で描く。
- 試験データの誤り（M4 出口の残課題）を直した: 「大きな声で約束する」のルビを「声」[3,4)・「約束」[5,7) に（psd_text_book.json・F4）。PSD の 1 頁目の hash が Python の PSD と同じことを照合で確かめる。
- test_contract_studio（457 件の操作、保存・Undo/Redo と監査、studio のファイル、配置画像 48 頁・83 ファイル・48 タイル）を参照 Python と照合。
- レビュー（P1×1・P2×4・P3）で直した所: 巨大な有限値の配置で頁全体が描けなくなる、SPEC の書き方が実装より強い、C++ だけの拒否の試験、claim の一時ファイル名、O(n²)、picture_size はヘッダーだけ、`:` を含む参照、数のあふれ、変えない操作で頁を複製しない、網が粗すぎるときの拒否を SPEC に、配置画像のキャッシュの解放。

### 利用者の決定（2026-10-11）
- 群E: Esc で素材の配置待ちを取りやめる（C++ の追加。SPEC に記載）を残す。plugin.json の形式を確定。Lab の PSD は CMYK/Lab 編集の工程（M5 の後、M6 の前）で扱う。正規表現の C++ だけの上限（1 億ステップ／戻る場所 100 万・データ 400 万／入れ子 200 段）を確定。
- M5①: 操作は core に置き、絵の大きさは render から差し込む配置で ARCHITECTURE.md を直す。C++ だけの拒否（Inf/NaN、set_bible の characters、assets/ の外の参照、読めない絵・256 MiB 超、数でない仕上げ、presence/claims の壊れた項目）を確定。tools.jsonl は Python と同じ（整数の ms は整数、Windows は CRLF）。
- M5①: AI（human: でない actor）は、人が描いた範囲（set_panel の regions）、人物の承認（locked の付け外し・承認済みの人物の設定画と顔。set_bible・upsert_character）、承認済みの人物の削除、承認依頼のチケット、絵柄の固定を変えられない（Python は通す。ACCEPTANCE AC-API・SPEC の「固定欄・承認を維持」に合わせた C++ だけの拒否）。承認済みの人物が消えたことを audit.jsonl に残す（Python は残さない）。
- M5①: 読めない絵（形式を問わず）は配置のとき（place_asset・adopt_candidate・候補のない set_placement）に全部展開して確かめて断る。大きさだけのときはヘッダーだけ読む。

### 残課題
- Windows 実機受入（配布前に必須）。Windows でのプラグインの子の子の停止（Job Object）。
- Lab の PSD（CMYK/Lab 編集の工程で）。
- 配置画像の仕上げが 4000 万画素を超えるとタイルごとに作り直す、白黒の頁の色のレイヤーの仕上げをタイルごとに頁全体で作り直す（性能。正しさは同じ）。
- 編集可能の PSD の書き出しが縁取りのある台詞で遅い（M4 から）。
- M5②〜④（StudioService・CLI、MCP・HTTP、画面）と M5 出口。M5② は m5-2 で作業中。

## M4出口受入（Linux）— 2026-10-11 記録

- **凍結SHA**: `65f833353c548bc5f28c9e0f4c775c96f5fa4635`（native/integration。M4 の群A・群B・群C と M3 の残り BRUSH-01 を統合した後の記録の版）。計画は `validation_policy.py --base 9b344f5 --head 65f8333 --phase milestone`（基点は M3 出口の凍結。tier full、Linux 各100試験）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz。M3 出口とは別の機種）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44 / cgroup v1。Qt 6.11.2、GCC 13.3。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。ASan（各構成内 ctest 1並列）→ ASan のビルドを消して Release と Debug を同時に。
- **結果**（validation_run.py の result.json）:
  - linux-asan: 100/100 合格、サニタイザ指摘なし、CLI23 合格（10/10 22:54–10/11 02:55 UTC）
  - linux-release: 100/100 合格、CLI23 合格（03:17–04:30）
  - linux-debug: 100/100 合格、CLI23 合格（03:17–04:50）
  - `--pair`: 失敗は「構成artifactの欠落・重複」1件のみ＝Windows 2構成が開発中 deferred。Linux 3構成どうしの照合は指摘なし。**Windowsを含む --pair 合格ではない。**
- **実xcb（Xvfb 1920×1080）GUI・E2E**（凍結SHAのビルド）: Release で gui_actions 14・gui_anim 6・gui_book 29・gui_brushes 8・gui_canvas 78・gui_color 60・gui_colours 4・gui_effects 14・gui_guides 7・gui_layers 12・gui_layout 32・gui_lines 20・gui_materials 35・gui_output 20・gui_paint 11・gui_paper 14・gui_pen 51・gui_ruby 18・gui_select 13、Debug で gui_save 36・app_e2e 8（開く→描く→Undo→保存→終了→別プロセスで再開）、全合格・FAIL 0。Release の gui_save は故障注入が Debug だけのため SKIP 1（合格に数えない。Debug で 36 合格）。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。試験は表示のみで合否判定なし。測定機は基準クラスではない）: 開いて最初の頁 548 ms、全頁 795 ms／入力→ライブ線 p95 1.56・p99 4.38 ms（n=43780）／線確定→表示 p95 18.3 ms／Undo→表示 p95 30.5 ms／頁切替 最初 p95 23.7・精細 p95 93.8 ms（n=4）／自動保存 p95 2.43 s／保存中のタイマーの遅れ p99 9.40 ms・イベントループの停止 p99 46.1 ms（最大 46 ms）。F1頁 350dpi 全描画 1114 ms・1線追加 598 ms・512px部分 45 ms。空いた状態での再計測もほぼ同じ（入力→ライブ線 p95 1.50 ms、停止 p99 52.8 ms、タイマーの遅れ p99 9.71 ms）。M3 出口（別の機種）と比べ、入力→ライブ線は約3倍・タイマーの遅れは約2倍、停止の最大は 309→46 ms。どれも合格値（PERF-B 33/50 ms、PERF-E 50 ms）より小さいが、基準機での測定は M7。
- **AC-PSD（§6 の代替検査）**: 編集可能な PSD（`native/tests/data/psd_text_book.json`、150 dpi、文字レイヤー19・ルビのレイヤー13、画像にした台詞 0、書体の注意 0）を Krita 5.2.2（Ubuntu の 1:5.2.2+dfsg-2build8、Xvfb）で開いて PNG に書き出せた（4頁とも 1240×1754 px、Genko の PNG との差は 0.01〜0.08% の画素で見た目は同じ、効果文字の縁取りあり）。Krita の .kra に書き出した層の構造でも「台詞 …」のグループの中に「フキダシ」「本文 #<id>」「ルビ「…」 #<id>-r<n>」、効果文字の本文にレイヤースタイルがある。Krita 5.2 は文字レイヤーを画素の層として読む（文字として直せることは求めない）。Photoshop・CLIP STUDIO PAINT での再編集は waived（未確認として既知制限に書く）。構造は test_psd_text（自前の読み手）と test_contract_psd_text（psd-tools）で検査。
- **AC-TEXT の目視**: 同じ本の PNG（150 dpi）と PDF（pdftocairo で 150 dpi に戻した頁）を拡大して見た。文字・句読点・ルビの欠けはない（縦書き・横書き、縦中横の AI・12、禁則、異体字セレクタの 葛󠄀・辻︀、ルビの書体・揃え・距離・字間・大きさ、二列にまたがるルビ、回転・斜体・効果文字）。書体は ZenKakuGothicNew-Bold・ZenOldMincho-SemiBold・DelaGothicOne-Regular・ZenMaruGothic-Bold。**試験データの誤り（残課題）**: psd_text_book.json と test_ruby_render の F4 の本の「大きな声で約束する」の ruby_spans が [4,5)「こえ」・[6,8)「やくそく」で、書記素で数えると「で」「束す」に付く（意図は「声」[3,4)・「約束」[5,7)）。描画はデータの通りで製品の誤りではない。M5 の最初の群で直す。IME の Windows での確認は deferred。
- **M3 の出口**: 下の M3 出口受入（2026-10-09）で欠けていた BRUSH-01（紙質を別PCへ持ち出して再現できる）は、この凍結で合わせて受け入れた（test_paper・test_gui_paper・test_contract_paper が3構成とも合格、GUI は gui_paper 14）。**M3・M4 とも Linux の出口受入は合格**（Windows は deferred）。
- **残課題・利用者判断**: Windows 実機受入（配布前に必須）、上の試験データの誤り、編集可能の PSD の書き出しが縁取りのある台詞で遅い（部分ごとに文字組を計算し直す）、replace_text の正規表現（not_yet_ported、利用者判断）、COMP-04 のネイティブ拡張の登録形式（公開契約の追加、利用者確認待ち）、CMYK/Lab の扱い、素材ライブラリ一覧の 1 MB 上限、素材一覧のダブルクリック、PSD を画像として開く（Pillow の平坦化、NotYetPorted）。置いた絵（place_asset）と正式な書き出しは M5。

## 利用者の判断（2026-10-11）

保留していた判断項目への回答。M3・M4 の残りの群として M5 と並行して実装する（CMYK/Lab は別工程）。
- replace_text の正規表現（regex: true）: Python の re を全部移す（Python と同じ結果。通常の置換は既に Python と同じ）。
- COMP-04 のネイティブ拡張: plugins フォルダーの plugin.json（名前・実行ファイル・引数・設定項目）で登録する形を設計して入れる。既定は無効、選んだものだけ同意の上で実行。
- PSD を 1 枚の画像として開く・読み込む: Python と同じ（Pillow の PsdImagePlugin の平坦化）に入れる。
- 素材ライブラリの一覧（library.json）の上限: 1 MB → 16 MB。
- 素材一覧のダブルクリック: Python と同じ（ページでクリックした所に置く）。
- CMYK・Lab での編集: 足す。M5 の後、M6 の前の独立した工程（設計・実装・検証）。

## M4 — 2026-10-09 記録

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

### 群C ルビの詳細組版と範囲追従（TEXT-01、ruby-layout@1、改善 I04）・編集可能な文字の PSD（PSD-01、改善 I05）（…c91288a）— 統合済み
- PSD-01 は TEXT-01 のルビの位置（render::text::ruby_places）に依るので1群にした。凍結 c91288a。計画 `validation_policy.py --base 2b5fe41 --head c91288a --phase integration`（tier full、Linux 各100試験）。
  - linux-asan: 100/100 合格・CLI23 合格・サニタイザ指摘なし（10/10 16:52–21:11 UTC）。
  - linux-release: 100/100 合格・CLI23 合格（21:34–22:53 UTC）。
  - 前の凍結 8629b46 は ASan で test_psd_text が QtTest の1関数 300 秒を超えて止められた（試験の画素の読み方が遅い。製品の不具合ではない）。時間の上限は変えず、試験を速くして関数を分けた（c91288a、ASan で test_psd_text 134 秒）。ほかの 99 試験は合格していた。
- TEXT-01: 台詞の ruby_spans（拡張書記素クラスタの [start,end)、Qt 6.11.2 の Unicode 17.0）にルビごとの書体・大きさ・揃え・本文との距離・字間。追従の規則（前は動かす、後ろ・末尾は変えない、親文字にかかる編集は要確認、全削除・全置換は外して Undo で戻る、長い台詞は前後両方から比べ、別の語に載せない）。要確認がある間は正式な書き出しを止め、点検に ruby_review。Python の台詞は Python と同じ結果の間は Python の形のまま。画面は記法を正とし、ルビの一覧で振り直す・外す・ルビごとの設定。
- PSD-01: genko export --format psd --psd-text editable で台詞ごとのグループ（フキダシ・本文・ルビごとの文字レイヤー、TySh/TxLr/EngineData、縦横・run・字間・揃え・変形・縁取り）。既定（image）は Python とバイトまで同じ。未対応（グラデーション・画像の塗り・パスの文字・円弧・ワープ・傍点・組み方の変わる旧形式のルビ）は厳密で断り、--rasterize-unsupported の時だけその台詞を画像に。書体は PostScript 名だけで埋め込まない。対応表 docs/PSD_TEXT_SUPPORT.md。受取アプリでの再編集は §6 で waived（psd-tools と自前の読み手で構造を照合。Krita での読込は M4 出口で行う）。
- 利用者の決定（10/09–10）: ルビの設定はルビごと、親文字の全置換は外す（Python と同じ）、ルビの公開契約の追加を確定。PSD の CLI、未対応に円弧と傍点、層の名前と対応 ID を確定。
- レビュー: TEXT-01 は群レビュー（P1×3・P2×7）と修正の再確認3回（別の語に載る・黙って外れる不具合が無くなるまで）。PSD-01 は群レビュー（P2×4: 縁の太さ、縦書きの立てた半角、旧形式のルビ、試験の弱さ）。
- 既知の制限: 編集可能の PSD は縁取りのある台詞の文字組を部分ごとに数回計算し直し、効果文字の多い頁は画像の書き出しより数倍遅い。
- push 済み（2b5fe41..c91288a）。

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
