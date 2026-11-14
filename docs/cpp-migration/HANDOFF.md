# Genko C++ 引継ぎ（2026-10-08）

## 進め方（2026-10-14 の再計画。利用者の指示で、重複する全体試験を省く。これ以降の工程すべてに当てる。VALIDATION.md の範囲・合格値は変えない）
- 前提（VALIDATION.md）: V1 は関係する試験だけ、V2 は統合した候補に一度、V3 は工程の出口。同じソース・実行物・構成・対象で合格した証跡は使い回す。`--pair` は 3 構成が同じ計画（plan_hash）・同じソースであることを求める。
- 漏れ（CMYK・Lab の工程）: 実装とレビューが揃っていた群B・C・D に群ごとの V2 を順に流した（約 20 時間）、凍結前に Release の全体を事前に流した、最後の群の V2 と出口の V3 が同じコードで重なった、V3 の 3 構成を順に流した。2026-10-07 の最速計画（重い V2・V3 は候補・出口に集約、同じ条件の合格は使い回す）の文書がリポジトリに無く、/goal の「群ごとに V2」をそのまま当てたのが原因。
- R1 開発中（V1）は変更に関係する試験だけ（RED→GREEN、関係する契約・GUI）。全体試験と「凍結前の事前の全体試験」はしない。
- R2 一つの工程の群は「実装（関係する試験）→レビュー1回→直し」までを群ごとに。統合は工程の候補 1 本にまとめ、群ごとの V2 は流さない（V3 が V2 を含む）。V2 を流すのは、次の群がその群の合格を前提にしていて、失敗の切り分けが要るときだけ。
- R3 V3 は一つの計画（`validation_policy.py --phase milestone`）で 1 回。ASan と Release を同時に始め、Debug はビルドが済み次第並べる。実 xcb の GUI は Release・Debug の後、性能は ASan の後の静かな機械で、最後に `--pair`。
- R4 失敗したら残りの構成を止める。コードを直したら（全構成のソースが変わるので `--pair` のため）3 構成をやり直す。環境だけが原因なら、その構成だけやり直す。
- **R4 の補い（利用者の決定 2026-11-14）**: 機械の再起動や環境が原因で止まった・落ちた構成は、`validation_run.py --resume` で続きから走らせる（合格済みの試験を引き継ぐ。条件と記録は VALIDATION.md §3、経緯は「試験の「続きから」」）。コードを直したら全部やり直すのは変わらない。
- R5 計画の違う結果は `--pair` で使えないので、最後の群は V2 を流さず、V3 へ直行する。
- R6 M7 の最終候補が M6 の出口の V3 と同じソース・実行物なら、全体試験は使い回し、M7 に固有の受入（最終配布物での起動・保存・出力、2 時間試験、全 PERF、IME の注入、全出力、MCP SDK での外部接続、独立レビュー、別のインストール先への導入）だけを行う。
- R7 統合前の群の commit は、作業ツリーだけに置かず、native/integration ではない作業ブランチへ push して保全する（CI は無効なので試験は走らない）。
- **R3 の置き換え（利用者の決定 2026-11-13）**: 工程の出口は、差分から選んだ範囲（変更分と影響箇所）を ASan と Release の 2 構成で流す。Debug は最終配布の前に 1 回だけ。詳しくは「コマ割り K」の節の「利用者の決定（2026-11-13）」。
- 当て方: CMYK・Lab の工程の出口の V3（e099da5）から。ASan の後に Release・Debug を流す順をやめ、ASan を走らせたまま Release・Debug を並べた（`run.sh` の親だけ止め、ASan の validation_run は続けた）。残る重複は群D の V2 と同じ中身の ASan 1 回分だけ（計画が違うので `--pair` に使えない）。
- 次: M6・M7 は Linux で合格（凍結 962faa4）。PhotoCraft の取り込みは PC1〜PC5 の各工程と PC6（台帳全件の突き合わせ・独立レビュー・配布物を含む V3）が Linux で合格（PC6 の凍結 de0f5e9、下の「PC6 の出口 V3」）。Windows 実機は、下の利用者の決定のとおりマージ後に別の環境で行う。
- **利用者の決定（2026-10-15）: Windows は、すべての実装と Linux での試験が終わり、マージした後に、別の環境で行う。** このセッションでは Windows の実機の build・受入をしない（VALIDATION.md §0 の「配布前の Windows 実機受入は必須」は変えない。Windows の受入が済むまで両 OS の配布完了とは言わない）。同じ日の決定: Windows の実機の作業は M7 まで保留、MCP の `inspect` の説明文は Python と同じ文のまま、Qt のソースの入手先は download.qt.io を案内する。


## フキダシの追加（fukidashi@2。SPEC BALLOON-02）— 2026-11-14

### 利用者の決定（2026-11-14）
- 今風のフキダシに 4 つを足す:「1、2、3足してください」。(1) 筆や G ペンのように太さの変わる線（強弱・入り抜き）、(2) ホラーのしたたる形、(3) 記号の形（ハート・星・花・ひし形。星は `points: 4` で四方のきらめき）、(4) どの輪郭の中にもトーン・グラデーションのトーン・色のグラデーション。
- リードの決め（設計 fukidashi-extra-plan の 13）: 拡張の名前は `fukidashi@2`。記号は 4 つとも。色のグラデーションも入れる（白黒のページではグラデーションのトーンを使うよう手引きとツールチップに書く）。トーンの上の文字にはトーン枠と同じ白い縁を自動で付ける。記号はネームの計画には入れない。
- 群: 群 1 が土台（core・render・service・storage・文言・op の一覧・SPEC と schema・単体とサービスの試験・速さの目安）。群 2 が画面とエージェント（種類のメニュー・パレット・台詞パネル・MCP の説明・手引き・GUI の試験）。この節は群 1。

### 群 1 でしたこと（ブランチ `native/fonts-fukidashi2`。main da6acebd に書体の追加を載せた上）
- 設計: SPEC.md BALLOON-02、schema-v4.md「フキダシの追加」。op の一覧（`genko schema` と docs/ops.schema.json）の `edit_line` に種類 4 つと「style (fukidashi 2)」。
- **まず控え**（設計の手順 0）: 手を入れる前の版で、見本の 2 冊（前の種類の本 4 ページ・今風の見本 3 ページ）の 150・600 dpi の絵の sha256 と、保存した project.json を記録した（tests/data/fukidashi/pins の pages.json・classic_project.json・clean_project.json）。test_fukidashi の aBookWithoutTheAdditionsIsDrawnAsBefore・…IsSavedAsBefore が最後まで通っている。前からの控え（PSD の sha256、tests/data/type/plain_lines.json）も動いていない。
- core: `core/fukidashi.{hpp,cpp}` に `kFukidashi2Feature`・`uses_fukidashi2`・`is_symbol`・`symbol_points`・`star_inner`・`drip_reach`・`body_box`。`letter_box` は本体の箱から始まる（しずくの無い台詞は前と同じ数）。`ops_lines.cpp` に種類 4 つ・鍵 11 個・範囲の確かめ・正規の形、ひし形と星のしっぽの隠れ方。storage の書き手と読み手に `fukidashi@2`。
- render: `render/text/balloon_clean` に記号の輪郭（`heart`・`star`・`flower`・`diamond`）、しっぽの出る楕円（`star_guide`・`flower_guide`・`star_centre`）、文字の場所（`symbol_room`）、しずくの 1 本（`drip`）、ペンの線（`Pen`・`pen_runs`・`pen_plane`。`Run` に形の番号を足した）。雲は「山を並べる」関数に切り出した（同じ式・同じ順。控えで確かめた）。`balloons.cpp` に本体の箱・しずくの並び（`add_drips`）・中のトーン（`tone_bytes`。格子が 512 px 以内で繰り返すときは 1 周期を描いて覚える）・色のグラデーション（`Masks` に blend と ramp、composite で画素ごと）・ペン（`pen_of`）。`lettering.cpp` は記号を「形に収める」種類に足し、トーンの上の文字の白い縁を足した。
- service: `service::letter::measure_styled(breaks, balloon, em, style)`（多角形の辺・星と花の数と深さ・しずくの分の高さを style から読む）、`symbol_room(kind, points, inner)`。`measure`・`fits` は記号を受け取る。文言（service/wording.cpp）に 16 の文（範囲ごとの文 12、トーンとグラデーションの形が違うときの文 2、数でない値の文 2）。
- 見本と試験: tests/support/fukidashi_books.hpp に `additions_book()`（1 ページに 1 つの追加: 記号・ペン・しずく・塗り）。test_fukidashi に 23 の試験（データの行を入れて 284 件）、test_fukidashi_service に 4 つ（41 件）、test_perf_render に速さの目安 1 つ（15 通り）。SKIP は無い。見本の絵は `GENKO_FUKIDASHI_OUT=<フォルダ>` を付けて test_fukidashi を流すと cleanupTestCase で書く（`GENKO_FUKIDASHI_SHEETS=additions` で追加の分だけ）。

### 群 2 が使うもの
- **種類**: `heart`・`star`・`flower`・`diamond`（`add_line`・`edit_line` の `balloon`）。
- **鍵・範囲・無いとき**（どれも `edit_line` の `style`。null で消える）:

| 鍵 | 範囲 | 無いとき | 意味 |
|---|---|---|---|
| `pen_swell` | 0〜0.9 | 強弱なし | 太さ ×（1 ＋ 値 × 法線·太い側の向き）。`border_mm` は平均の太さのまま |
| `pen_angle` | 0〜360 | 45 | 太い側の向き（0 右・90 下） |
| `pen_taper` | 0〜1 | 入り抜きなし | 筆の両端へ細る長さの割合（端は 0.15 倍） |
| `pen_strokes` | 1〜8（整数） | 2 | 角の無い輪郭を描く筆数（角のある輪郭は角から角まで） |
| `drip_mm` | 0〜30 | しずくなし | いちばん長いしずく。箱の高さの 0.6 まで。箱はその分だけ高くする |
| `drips` | 1〜24（整数） | 幅で決まる（3〜16） | しずくの枠の数 |
| `drip_width_mm` | 0.3〜10 | 幅の 7%（1.2〜4 mm） | しずくのふつうの太さ |
| `drip_seed` | 0〜999999（整数） | 0 | 並び。「並びを変える」は 1 足す |
| `points` | 4〜12（整数） | 星 5・花 6 | 星の先・花びらの数 |
| `fill_tone` | `{density 0〜0.9, lpi 20〜85, angle 0〜90, pattern "dot"/"line", to? 0〜0.9, dir? −360〜360, radial?, inset_mm? 0〜5}` | トーンなし | `density` は必ず要る。op が返す形は `density`・`lpi`（60）・`angle`（45）・`pattern`（dot）が必ず入る。パネルはこの形をまるごと書く |
| `fill_gradient` | `{to: [r, g, b], dir? −360〜360, radial?}` | なし | `fill_rgb`（無ければ白）から `to` へ。op が返す形は `to`・`dir`（90） |

  星の深さは前からの `spike_depth`（0.05〜0.6。星は無ければ 0.38。`points: 4, spike_depth: 0.6` がきらめき）、角の丸みは `corner_mm`（ハートの先・星の先と谷・ひし形の角）。
- **パレットが消す鍵**: 上の 11 個を `balloon_look_keys()` に足す（後から当てたプリセットが前の追加を消せるように）。台詞パネルの「既定の設定に戻す」は core::line_style_keys() から作るので、もう 63 個を消す（test_gui_lines の期待値を 52 → 63 にした。下）。
- **関数**: 寸法は `service::letter::measure_styled(breaks, kind, em, style)`。**app/lettering.cpp の measure・box_size・refit_for_kind には記号としずくの場合がまだ無い**（今は記号を箱の種類として測る）。群 2 で、記号の 4 種類を capsule・polygon と同じ道（service の式）へ回し、`own_shape` に足し、`drip_mm` のある台詞は高さにその分を足す。種類の判定は `core::is_symbol(kind)`、しずくの届く長さは `core::drip_reach(line)`、本体の箱は `core::body_box(line)`。見本の絵は今までどおり `render::text::draw_group`（パレットの 56 dpi ではトーンは 2 px の格子で、点ではなく灰色に見える）。
- **隠す行の目安**: しずくは手で描いた輪郭・`dotted_box`・`flash`・`picture`・`sfx`・`none` では描かれない。ペンは `dotted_box`・`flash` では描かれず、破線（`whisper`・`dash_mm`）では強弱だけ。`points` は星と花だけ。トーンとグラデーションは `sfx`・`none`・`picture` では描かれない。op はどれも受け取って持つ（断らない）。パレットがしずくのプリセットを手で描いた輪郭に当てたときの文（設計の 6）:「手で描いたフキダシには、しずくは付きません（線と塗りだけ当てました）」。
- **断りの文**（service/wording。画面でも同じ文が出る）: 「フキダシの線の強弱は 0〜0.9 です」「線の太い側の向きは 0〜360 度です」「線の入り抜きは 0〜1 です」「フキダシの線を描く筆数は 1〜8 です」「しずくの長さは 0〜30 mm です」「しずくの数は 1〜24 です」「しずくの太さは 0.3〜10 mm です」「しずくの並びの番号は 0〜999999 です」「星のとがり・花びらの数は 4〜12 です」「フキダシの中のトーンの濃さは 0〜90% です」「フキダシの中のトーンの線数は 20〜85 です」「フキダシの中のトーンの柄は、点か線から選びます」「フキダシの中のトーンの指定が正しくありません」「フキダシの中のグラデーションの指定が正しくありません」「フキダシの線・しずくの設定は数で指定します」「フキダシの筆数・しずくの数や番号・とがりの数は整数で指定します」。
- **群 2 に残ること**: 種類のメニュー（ハート・星・花・ひし形）と commands.json、パレットのプリセット、台詞パネルの行、MCP の apply_ops・propose_lines の説明、手引き（SKILL.md・drawing.md と integrations/ の写し）、GUI と e2e の試験。手引きには「白黒のページでは `fill_gradient` でなく `fill_tone` の `to`」と書く。

### 変えた既存の試験（意図した変更。緩めていない）
- test_gui_lines theLinesPanelListsReordersAndEdits: 「既定の設定に戻す」が消す鍵の数 52 → 63。この操作は core が受け取る鍵の一覧（core::line_style_keys）から作るので、core に鍵を 11 個足すと数が変わる（設計では群 2 の項目だったが、原因は土台の側にある）。
- docs/ops.schema.json: native の一覧と同じ文に（edit_line の balloon と「style (fukidashi 2)」）。
- ほかの期待値は変えていない。

### 確かめたこと（Linux Release。関係する試験だけ）
- すべてを作り直した（ビルドの全ターゲット 572 手順、エラーなし）うえで、関係する試験 49 本を 2 本ずつ流して 49 本とも合格（2182 件。失敗 0）: test_fukidashi（356 件。前は 72）・test_fukidashi_service（59 件。前は 18）・test_render_page・test_storage・test_contract_storage・test_model・test_command_bus・test_ops・test_studio_ops・test_features・test_type_text・test_type_styles・test_contract_type・test_contract_type_styles・test_contract_type_fonts・test_contract_opentype・test_measure_features・test_ruby・test_ruby_render・test_psd_text・test_psd_smart・test_contract_psd・test_contract_psd_text・test_contract_psd_smart・test_smart_stack・test_tones_effects・test_tone_ops・test_font_catalog・test_font_faces・test_font_service・test_layout_ideas・test_service・test_ledger_service_a/b/c・test_cli_e2e・test_save_e2e・test_ops_e2e・test_mcp_api_e2e・test_gui_ledger_a（どの断りも日本語で読める）・test_gui_lines・test_gui_fukidashi・test_gui_ruby・test_gui_output・test_gui_type・test_gui_type_panels・test_gui_fonts・test_gui_actions・test_perf_render。全体の試験は流していない。
- 記録に SKIP は 17 件あるが、どれも前からのもの（test_save_e2e の障害の差し込み 16 件は Release のビルドには無い、test_perf_render の psd_of_groups_at_350dpi は環境変数が無いとき）。この群で足した試験に SKIP は無い。FAIL・QFATAL は 0。
- 控えの試験（手を入れる前の 2 冊、追加の見本）は、失敗するのを見てから記録して通した。ほかの新しい試験は、それぞれの実装のあとに書いた。書いたときに落ちて実装を直した所: 記号の箱が大きすぎる、星の 1 列が測った寸法で折り返される、下向きのグラデーションの色が行の中で 1 段ずれる。見本の絵を見て直した所: 花の上が平らになる、しずくの付け根で線が切れる（そのあと「線が開かない」試験を足した）。
- 設計の「確かめていない」の項: (1) 雲を切り出しても画素が同じ → 控えで確かめた。(2) 文字を編集できる PSD の白い縁 → トーン枠の自動の縁は、文字の絵に描くのと同じ値から文字レイヤーの境界線（`SetLetters::halo`）として出ていて、`fill_tone` も同じ値を通る（test_fukidashi の theLettersOverAToneHaveAWhiteEdge で幅と色が同じことを確かめた）。(3) ページの端で切れる領域 → aBalloonThePageCutsOffIsTheSame（全部の追加を付けた 4 種類、150・600 dpi で、大きいページの同じ所と同じ画素）。(4) service/atari.cpp は言われた種類をそのまま使い `letter::measure(breaks, balloon)` で測る（記号はその種類の既定の数で測られる）。service/finish.cpp の `tailed` は、記号を話し言葉の種類として扱う（話し手がいればしっぽを向ける。楕円と同じ）。どちらもコードは変えていない。(5) 速さと、小さい円をたくさん描くときの速さ → 下。

### 見本を見て決めたこと
- ハート: 設計の「2 つの円が 1 点で接し、先まで直線」だと切れ込みが幅の無い筋になり、下が三角に見えるので、山を少し重ねて（中心 ±0.485・半径 0.515）切れ込みに角度を付け、下の辺を外へふくらむ曲線にした。左右は点ごとに同じ。
- 星: 先を「短い半径の 0.10 の丸み」で丸めると、細い先（きらめき）が 3 割近く削れるので、無いときは先を短い半径の 0.035 だけ縮める丸みにした（`corner_mm` を書けばその半径）。
- 花: 山を半円にすると円弧の作り方（半円未満）が破れて上が平らになったので、高さを幅の 0.46 にした。雲とはっきり違って見える。
- しずく: 傾いた輪郭（ハートの下・楕円の脇）では、上の辺の付け根に小さな段ができたので、指の形は両方の辺が輪郭を出た所から始め、それより上はまっすぐにした。輪郭が指の角をちょうど通ると継ぎ目が 2 つできて線が切れたので、角を少し下げた。番号によっては本数が少なくなる（4 枠で 1 本だけ、など）。それは「並びを変える」で変えてもらう。
- ペン: 四角は辺ごとに 1 筆で、角で細くなる。しっぽのあるフキダシは先から先までが 1 筆。楕円の 2 筆は左上と右下で細くなる（右下は太い側なので、強弱の山が途中でくびれる。1 筆なら左上だけ）。白黒にしても線は切れない（いちばん細い所で 1 px）。
- トーン: 600 dpi を白黒にすると、点は面積で 3 割以上かかった画素が黒になるので 2 割ほど太る（60 線の 50% では市松に近くなる）。濃さ 10〜30% では点がそろって見える。

### 速さ（test_perf_render。表示だけ。B4・600 dpi（6071 × 8598 px）に 10 個。数字は揺れる: 同じ条件でも、機械が混んだ回は 1.7 倍まで延びた）
- 今日の元の数字（何も足さない 10 個。3 回）: フキダシなし 373〜550 ms、ありを最初から 672〜1168 ms、もう一度 399〜595 ms、形だけ 312〜466 ms（1 個 31〜47 ms）。静かな回は前の記録（680 ms）と同じ。機械が混むと 1.7 倍まで延びる。下の差は同じ回の「何も足さない 10 個」との差。
- ペンの線: 形だけで 1 個 −14〜＋11 ms（揺れの中。差は見えない）。しずく: −7〜＋11 ms（同じ）。記号 10 個: −21〜＋12 ms（同じ）。色のグラデーション: −16〜＋22 ms。
- トーン（形だけ、1 個あたり）: 点 ＋11〜＋12 ms（1 回は揺れで −11）、濃くなる点 ＋4〜＋28 ms、線の内側を白く ＋4〜＋30 ms、濃くなる線 ＋8〜＋25 ms、15° の細かい格子で濃くなる点（最初に 64 枚の周期を作る）＋4〜＋40 ms、繰り返さない粗い格子（20 線・30°。1 個に 1000 個あまりの点を 1 つずつ描く）＋44〜＋72 ms。全部を一度に: ＋18〜＋56 ms。
- **ページ全体では、トーンのフキダシ 10 個で最初の 1 回が ＋2.3〜2.9 秒**。ほとんどが文字の白い縁（1 本 0.25〜0.3 秒）で、トーンを付けずに文字に縁取り（`outline_mm: 0.7`）を付けただけでも同じだけ延びる（＋2.6〜2.9 秒）。前からある縁取りの処理の速さで、この群では変えていない。2 回目からは覚えているので速い。
- 設計の見積もり（1 個あたり ペン ＋1〜4、しずく ＋3〜8、トーン ＋3〜6、グラデーション ＋5〜10 ms）と比べると、ペン・しずくは見積もりの中、トーンとグラデーションは 2〜4 倍。この目安は長いしっぽのせいで 1 個の領域が 2600 × 2400 px あり、トーンの手間は領域の広さで決まる。

### 設計と違う所
- 記号の寸法: 「（塊＋余白）× 倍率を出してから短い辺を広げる」ではなく、その記号らしい縦横の比の中で、塊が入るいちばん小さい箱を探す（13 通りの比を試す）。縦長の言葉で箱が大きくなりすぎないように。
- 記号の中で列を折る長さ: 箱の高さではなく、文字の場所が真ん中を通る長さにした（星の 1 列が測った寸法で折り返されないように）。
- 記号は話し言葉の種類として、文末の「。」を落とす（capsule・polygon は前のまま変えていない）。
- トーン: 点の中心はトーンのレイヤーと同じ所（設計の式に画素の半分を足した）。繰り返しの周期は N ではなく N ÷ gcd(m, n)（60 線 45° は 14 px）。濃さが π/4 を超える所の半径は表で補う（重なりで面積が足りなくなるので）。`fill: none` でもトーンは敷く。`density` は必ず書く。
- しずく: 長さは、そのしずくが出る輪郭の所から測る（本体の下の端からではない）。いちばん長い 1 本だけ箱の下の端まで。
- ペン: 角の無い線の筆の始まりは「周りの 0.875 の所」ではなく、線がいちばん細くなる所（45° の楕円では同じ所）。描き方は帯の分かれ道を作らず、いつも台形とくさびの和集合。つなぐ処理は設計どおりにできた（つなげなかった切れ端を 1 本ずつ描く逃げ道は残してある）。
- app/lettering.cpp には触れていない（設計の 4.1 は群 1 の記号の節に書いていたが、画面の側なので群 2 に残した。上の「群 2 が使うもの」）。

### 残り・気を付けること（群 1 の時点。直したものは下の「レビューの直し」）
- ~~前からの不具合: 四角のしっぽが角へ向かうと線が切れる~~ → レビューの直しで直した（下）。
- トーンの上の文字の白い縁は、前からの縁取り（`outline_mm`・トーン枠）と同じ処理。速さは下の「レビューの直し」で測り直した。
- 繰り返さない粗い格子（たとえば 20 線・30°）のトーンは点を 1 つずつ描く。600 dpi で 1 個 40〜70 ms。1200 dpi ではその数倍。
- Windows のビルドはしていない（`__int128`・`near`/`far`・非コピー型の `?:` は使っていない。新しい角度は libm_float の sinf・cosf。`tones::screen_vector` はトーンのレイヤーと同じく、整数に丸める前に環境の cos を使う）。

### 群 1 のレビューの直し（読み取りだけのレビュー fukidashi2-review1 とリードの決め。同じブランチ、7b3fd9e2〜）— 2026-11-14

レビューは P1 なし、P2 が 4 つ、P3 が 7 つ。リードの決めのとおり全部を直し、それぞれに試験を足した。見本の 2 冊の控え（手を入れる前の版の絵と保存のバイト）は最後まで動いていない。PSD の控え（psd_book_hashes）、tests/data/type/plain_lines.json も動いていない。

- **P2-1 筆の区切りが解像度で変わる**: 角を「点と点で 30° を超えて曲がる所」で決めていたので、丸めた角の切り方（解像度で変わる）で角だったりなかったりした。今は**形を作る関数が自分の角を言う**（`clean::rounded`・`polygon`・`diamond`・`star`・`heart`・`cloud`・`flower` の `corners`、叫び・電子音は輪郭の点すべて、しっぽは `Tail::corners` に先と曲がり目、手で描いた形は 30° を超える点）。丸めた角はその丸みの真ん中の点が角。`rounded`・四角は `corner_mm` が 2 mm 未満（`kPenCornerMm`）のときだけ角を言う（無いとき、四角は角、角丸は丸い 1 本の線）。ほかに、形と形のつなぎ目（しっぽの付け根で継ぎ目が無いとき、しずくの付け根）は、そこから 0.5 mm 前後の向きで 30° を超えれば角（点の並びに依らない）。丸く継いだ継ぎ目は角にならない。角の無い閉じた線は、形の重心から太い側と逆へ引いた線が輪郭を出る所から始める（点の並びに依らない）。筆の長さの決まり（太さの何倍か）は、整数の画素にする前の線の太さで数える。試験: `thePensStrokesAreTheSameAtEveryResolution`（32 通りの形と設定 × しっぽの有無 × 2 つの大きさ: 150・600・1200 dpi で筆の数・始まり（0.34 mm 以内）・太さ（線の太さの 1/8 以内、8 倍より短い筆は 1/4））。
- **P2-2 しずくの並びが解像度で変わる**: しずくの数・ふつうの太さ・並ぶ幅・抜け・長さは、行の mm から（本体を 100 px/mm で作って）`drip_plan` で決め、描くときはその mm を画素にするだけにした（`add_drips`）。数の丸めは「ちょうど半分は上」に 1e-9 の余裕。試験: `theDripsAreTheSameAtEveryResolution`（レビューの 72 mm・38×18・56・88・104・16.8 mm・四角の 49.78 と 35.56 mm など 20 の大きさ × 12 の並び: 150・350・600・1200 dpi で本数・位置（粗い方の 1.5 画素以内）・太さ・長さが同じ）。
- **P2-3 角の間の短い筆に黒い玉**: 筆の両端の細さは、長さが太さの 8 倍以上なら 0.15 倍、4 倍以下なら細らせず、その間はなめらかに。角では、そこで会う 2 つの筆の細い方にそろえる。太さより近い角は 1 つにまとめる。試験: `aShortStrokeBetweenCornersIsNoKnob`（角を切り落とした四角の幾何、レビューのハート 4 通りと細かいトゲ・雲・電子音・十二角形・小さな角丸の四角を 150・600 dpi で: 線の太さが両隣より 1.8 倍以上太い所が無い）。
- **P2-4 二重線のしずくが遅い**: しずくのあるフキダシの二重線の内側の線、`inset_mm` の白、トーン枠の余白は、しずくの指を除いた形（本体・しっぽ・継ぎ目）の縁で作る。試験: `aThickDoubleLineWithDripsIsNotSlow`（レビューの雲 38×13.5 mm・線 2.5 mm の二重線としずく・ペン・トーン: しずく無しの 3 倍＋3 秒以内。前は 55 秒。今は無しとほぼ同じ。二重線は本体だけにあり、指には 1 本の線）。**前からの重さ**: `border_mm` に上限が無く、とても太い線（例: 0.3×60 mm の箱に線 30 mm と二重線）は、しずくが無くても前の版から 25 秒以上かかる。今回は変えていない。
- **P3-1 太いしずくが箱の外へ**: 半分の幅を、枠の 0.45 倍と本体の幅の 0.15 倍までにした。付け根の丸みは、本体の次の角の手前で止める（`clean::drip_joins`）。線の太さの 1.5 倍より細い指には丸みを付けない。**決め**: いちばん長いしずくは必ず描く（その枠で描けなければ輪郭の真ん中から）。だから本体を縮めたのにしずくが 1 本も無い、ということは起きない。試験: `aWideDripStaysInItsBalloon`（レビューのひし形 20×13.5・角丸・小さな星の 12 本・カプセルの 24 本など 10 通り × 32 の並び: 箱の外に何も無い、しずくがある、線が閉じている）。
- **P3-2 白い縁のずれ**: 文字の白い縁は、そのグループで実際に描かれるトーン（最初の行の `fill_tone`）に従う（`render::text::ToneUnder`。draw_group が文字を描く間だけ置く。文字の覚えの鍵にも入る）。何も描かないトーン（濃さ 0 で先も 0、濃さの無いもの）では縁を付けない（`fill_tone_draws`）。試験: `theWhiteEdgeFollowsTheToneDrawn`。
- **P3-3 確かめていない値でページが描けない**: 描く側で、しずくの数・番号・太さ、トーンとグラデーションの中身をすべて範囲に収め、形の違う値は無いものとして読む（グラデーションの先の色は数 3 つのときだけ）。試験: `aMalformedStyleStillDraws`（3 種類 × 11 の鍵 × 間違った型 8 つと極端な数 8 つ、トーンとグラデーションの中身 11 個 × 22 の値で、描く・文字を組む・点検する・保存するのどれも失敗しない。型の違う値は鍵が無いのと同じ絵）。
- **P3-4 白黒のトーン**: 点検に `balloon_tone_light`・`balloon_tone_dark` を足した（formats/checks。白黒のページの、フキダシの中のトーンが、ページの解像度で点（線）が 2 画素より細い・点と点の間の白が 2 画素より細い。グラデーションのトーンは両端で見る。つないだフキダシは最初の行のトーンを 1 回だけ。カラーのページでは言わない）。範囲は `render::text::balloon::tone_print_range(dpi, lpi, lines)`。**600 dpi で印刷に残る濃さ**: 点は 60 線で 4〜90%、85 線で 7〜85%（点が飛ぶ・白がつぶれる）。線は 60 線で 20〜80%。1200 dpi ならその 1/4 から。**残ること**: 60 線・600 dpi の 60〜75% は、隣の点がほとんど触れる所に 1〜2 画素の白い点が残る（網点が点から穴へ移る所。どの網でも起きる。1200 dpi ではほとんど出ない）。点検では言わない（言うとふつうのトーンのほとんどで出てしまう）。試験: `aToneOutsideWhatPrintsIsWarnedOf`。
- **P3-5 文**: レビューの案を入れた。「フキダシの中のトーンの濃さは 0〜0.9（90%）です」「フキダシの線を何筆で描くかは 1〜8 です」「星の先と花びらの数は 4〜12 です」。トーンの角度・濃くなる向き・線から離す幅・濃さが無い、グラデーションの先の色・向きにそれぞれの文。数でない値は鍵ごとの文（「しずくの数は整数で指定します」など 9 つ）。全部で 29 の文（service/wording.cpp）。試験: test_fukidashi_service の `theAdditionsRefusalsReadInJapanese`（51 行）。
- **P3-6**: 星の谷の範囲のコメントを直した（0.2〜0.95、行からは 0.4〜0.95）。しっぽが隠れたか・外へ出す決まりは、しずくのある行は本体の箱で、星は星の中心で測る（`inside_star` の三角関数は `inside_polygon` と同じく環境のもののまま）。`inset_mm` は線の同じ太さから測る（ペンの太い側はその分を食う）ことを SPEC に書いた。試験: `aTipIsHiddenByASymbolItself` に足した。
- **P3-7 白い縁の速さ**: 縁取りの処理は変えていない。測り直し（下の速さの表）: 1 枚を最初から描く試験の中では 10 本で ＋2.6 秒（1 本 0.26 秒）、`genko render`（コマンド）では ＋0.3 秒（10 本）。違いは描き方（コマンドは文字を別のスレッドでも組む）。行と列に分けた最大値フィルター（同じバイトで 1 画素あたり 2 × 大きさ）にすれば速くなるが、今回はしていない。
- **箱の角のしっぽで線が切れる（fukidashi@1 の不具合。リードの決めで直した）**: しっぽの付け根（輪郭の内側にある辺）の端が、箱の角の外に出ると、その外の部分は線の無い縁になっていた。付け根の端が輪郭の外にあるときは、その端だけを中心線の方へ、輪郭のすぐ内側（輪郭までの 0.9）まで寄せる。付け根の端が内側にあるしっぽ（角から離れたしっぽすべて）は前と同じ計算で、同じ点になる。**直す前**（レビューの表、600 dpi の切れ目の大きさ）: 30×24 なし、40×30 は ±3° で 6 px、44×36 は −6°〜+3° で 13 px、50×46 は ±6° で 28 px（約 1.2 mm）、60×40 は −6°〜+3° で 19 px、80×30 は −3° で 4 px。幅 6 mm のしっぽの 24×18 は −11°〜+8°、8 mm は −15°〜+11°。**直した後**: 試験 `aTailAtABoxCornerClosesItsLine`（四角・角 1 mm の四角・角丸の 0・1・3 mm と既定・太い線・ペン × くさび・まっすぐ・ギザギザ × レビューの 8 つの大きさ × 4 つの角 × 角の向きの ±15° を 1.5° ごと = 24 × 672 枚: どれも線が閉じている）。**動いた控え**: 無し。見本の 2 冊（前の種類の本・今風の見本）にも PSD の見本・plain_lines にも角へ向かうしっぽは無く、どの絵の sha256 も変わらなかった（試験で確かめた）。見本の絵: `fukidashi2/fix/fix-box-corner-tail-before.png`・`-after.png`（作業用の場所）。
- **見落とされていた性質の試験**（レビューの 7）: 二つの解像度で同じ筆・同じしずく（上）、角の間の短い筆（上）、二重線と太い線のしずく（上）、本体の幅に近い太さのしずく（上）、描く側の値の収め方（上）、ペンと切れ込み・手で描いた形・消しゴム・消えるしっぽ（`thePensLineWithANotchAPathCutsAndAFade`）、1200 dpi のペンとしずく（`thePenAndTheDripsAt1200Dpi`）、記号の文字の極端な縦横比と 5・6 以外の数（`theLettersFitASymbolOfAnyShape`: 9 つの記号 × 6 つの箱 × 150・600 dpi）、つないだフキダシでトーンの違う行（上）、0.1 より薄い・0.5 より濃い白黒のトーン（上）、スマートオブジェクトの中の文字レイヤー（`theAdditionsInsideContentsAndOnManyThreads`）、スレッドの数（同じ: 8 回、4 つずつ並べて、控えと同じ sha256）。
- **ふるえるハートの切れ込み（自分の見本で見つけた。群 1 の不具合）**: `wave_mm` のハートは、切れ込みの両側で外へ押した波が切れ込みで交わって、線が X の形（長い 2 本の先）になっていた（どの解像度でも。ペンの有無に依らない）。今は、内へ曲がる角（形が言う角のうち、輪郭が内へ折れる所。ハートの切れ込みだけ）から 1 波のあいだで波を 0 まで静める（`clean::waved` の `corners`）。外へ曲がる角（四角・角丸・多角形・ひし形の角、ハートの先）は前と同じ波で、同じ点になる（試験で同じ点を確かめた。だから前の版の本は動かない）。試験: `aTremblingHeartsCleftDoesNotCrossItself`（300・600・1200 dpi × 波 0.2・0.3・0.6 mm × 線 0.3・0.7 mm とペン: 切れ込みのいちばん下 0.6 mm のインクの幅が 1 mm 未満。直す前は 1.4〜4.9 mm。直す前の版で落ちることを確かめた）。
- **ペンの線のいちばん細い所**: 1 画素から 1.5 画素にした（線の太さがそれより細ければその太さ）。2 つの細った端が鋭い角で会う所で、薄すぎる画素が残らないように（300 dpi のギザギザのしっぽで線が切れて見えた）。
- **変わった控え**: 追加の見本（tests/data/fukidashi/pins/additions_pages.json）の 2 ページ（ペン）と 3 ページ（しずく）の sha256（筆の角・しずくの mm の決め）、1 ページ（記号）の sha256（ふるえるハートの切れ込み）。150・600 dpi とも。追加の見本の保存のバイト（additions_project.json）は同じ。4 ページ（塗り）は同じ。手を入れる前の版の控え（pages.json・classic_project.json・clean_project.json）は同じ。
- **変えた既存の試験の期待値（緩めていない）**: test_fukidashi の自分の試験のうち、ペンの最小の太さを 1.5 画素にしたので、強弱の試験の期待値を「1.5 画素か計算の太さの大きい方」に、入り抜きの試験の線を 8 から 12 画素に（0.15 倍が 1.5 画素を超えるように）、四角の角の太さを 1.2 から 1.5 に。test_fukidashi_service の断りの文の期待値を新しい文に（6 行）、足りなかった行を足した。
- **確かめたこと**: 下の「群 2」の「確かめたこと」にまとめた。

### 群 2（画面とエージェント）— 2026-11-14
- **種類のメニュー**（app/lettering.cpp の `kinds()`。5 つのメニュー: ページ → 台詞 → フキダシの形、フキダシの右クリック、台詞パネル、文字ツールの設定、ストーリーエディター）: 多角形の次に「ハート（甘い声）」「星（はしゃぐ声・きらめき）」「花（やわらかい声）」「ひし形（冷たいひとこと）」。commands.json に 4 つ（menu の項目は名前で並ぶ。名前のある操作の数は 823 のまま: 種類の項目は数に入らない）。種類を記号へ（記号から）変えると、同じ取り消し 1 回の中で箱を言葉に合わせて作り直す（`refit_for_kind` の `own_shape` に記号）。
- **寸法**（app/lettering.cpp）: `measure`・`box_size` が style を受け取る。記号は `service::letter::measure_styled`（先の数・深さを style から）。しずくのある台詞は箱をその分だけ高く（`drip_height`: `drip_mm`、30 まで。描かない種類は 0）。文字ツールで置く台詞・打ち直し・種類を変えたときに使う。種類を変えたとき、しずくのある台詞は本体の真ん中を保つ。
- **パレット**（app/balloon_palette.cpp。32 → 49）: 強弱の線・G ペン風・筆風・G ペンの叫び（ペン）、したたる・ベタホラー・溶ける四角・ふるえホラー（しずく）、ハート・星・きらめき・花・ひし形（記号）、トーン・グラデトーン・トーンの叫び・濃いグラデ（中のトーン。どれも白黒で印刷に残る濃さ。色のグラデーションはパレットに入れていない: 白黒の原稿で使われないように）。`balloon_look_keys()` に追加の 11 個（あとで別の形を当てると消える）。しずくのある形を当てると箱がその分高くなり（本体はそのまま）、しずくの無い形を当てると低くなる。手で描いたフキダシにしずくの形を当てると「手で描いたフキダシには、しずくは付きません（線と塗りだけ当てました）」。絵は今までどおり、見せたときに少しずつ描く。
- **台詞パネル**（app/story_panel.cpp。選択ツールの設定の欄）: 線の強弱・太い側の向き（強弱があるとき）・入り抜き・筆の運び（入り抜きがあるとき）、しずくの長さ・しずくの数・しずくの太さ・「しずくの並びを変える」（長さがあるとき）、星の先・花びらの数（星と花）、中のトーン・トーンの線数・トーンの柄・先の濃さ（トーンがあるとき）・濃くなる向き（先の濃さがあるとき）、「中の色のグラデーション…」（設定してあれば「…を外す」）。どれも 1 回の取り消し。しずくの長さは `edit_line` と、箱の高さの `move_line` を 1 回で送る。トーンは `fill_tone` をまるごと書く（角度・`radial`・`inset_mm` は今の値を残す）。見せる行: ペンは点線の囲み・フラッシュ・フキダシの無い種類で隠す。しずくはそれに加えて手で描いた形で隠す。トーンと色のグラデーションはフキダシの無い種類で隠す。星の「トゲの長さ」は星の既定 0.38 を見せる。ツールチップ: 中のトーンに「白黒の原稿では、60 線なら 4〜90%、85 線なら 7〜85% が点として印刷に残ります…点検で知らせます」、色のグラデーションに「カラーの原稿向け…白黒の原稿では…「先の濃さ」を使います」。
- **エージェント**: MCP の `apply_ops` の説明に記号 4 つと追加の鍵（白黒の範囲、`fill_gradient` はカラー向け、記号の箱の目安）、`propose_lines` の説明に記号も。道具の input schema は変えていない（ハッシュの試験は同じ）。手引き（native/resources/studio/guide/SKILL.md の「フキダシの形を選ぶ」の表に 8 行と注意 2 つ、drawing.md の種類と追加の鍵）を integrations/ の 2 か所へ同じバイトで写した。docs/AGENT.md に追加の鍵。
- **試験**: test_gui_fukidashi に 3 つ（`theSymbolsAreKindsAndThePaletteHasTheAdditions`・`aPresetOfTheAdditionsRestylesALine`・`eachAdditionSettingWritesItsKey`）と、見本の画面（パレットの追加・種類のメニュー・追加の行のあるパネル。明るい色と暗い色）。test_mcp_api_e2e に `anAgentsBalloonAdditions`。
- **変えた既存の試験の期待値（緩めていない）**: test_gui_fukidashi の種類の数 17 → 21、パレットの形の数の上限 34 → 52（49 になった）。test_gui_lines の種類の数 17 → 21（2 か所）、既定に戻すと消える鍵の数は群 1 で 52 → 63。commands.json に 4 行（画面のメニューから作り直した。test_gui_prefs の commandsJsonIsTheMenus）。
- **確かめたこと**（Linux Release、関係する試験だけ）: RESULTS2
- **速さ**（静かな機械で 3 回。test_perf_render の ten_balloons_with_the_additions_at_600dpi。B4・600 dpi に 10 個、同じ回の「何も足さない 10 個」との差。形だけの 1 個あたり）: 元（何も足さない）は 1 枚を最初から 777〜921 ms（前の記録 680 ms）、形だけ 37〜45 ms/個。ペン −10〜−5 ms、しずく −7〜−5 ms、記号 −8〜−4 ms（どれも揺れの中で差は見えない）、色のグラデーション −5〜−3 ms、点のトーン ＋4〜＋10 ms、濃くなる点 ＋7〜＋20 ms、線の内側を白く ＋8〜＋22 ms、濃くなる線 ＋2〜＋10 ms、15° の細かい格子 ＋10〜＋29 ms、繰り返さない粗い格子 ＋41〜＋56 ms、全部 ＋37〜＋44 ms。1 枚では、トーンの 10 個で ＋2.1〜2.9 秒。そのほとんどは文字の白い縁で、トーンを付けずに文字に縁取りだけ付けても同じ（＋2.6 秒）。`genko render` で測ると縁は 10 本で ＋0.3 秒。
- **残り**: (1) 白い縁の速さ（P3-7。分けた最大値フィルター）。(2) 60 線・600 dpi の 60〜75% の白い点（上）。(3) Windows のビルド。(4) 見本の絵を利用者に見てもらう（ハートの形、しずくの太さと長さ、筆のくびれ、星の先の丸み、パレットの絵）。(5) `border_mm` に上限が無いことの重さ（前から）。(6) 記号をネームの計画に入れるかは、決めのとおり入れていない。

## 書体の追加（F1: ファイル・ライセンス・一覧）— 2026-11-14

### 利用者の決定（2026-11-13）
- 「書体も足りない気がするのですよね」「フォント追加は、任せます 使えるものであれば可能な限り追加してください」。同梱の日本語書体がまんがには少ない。追加する書体の選び方は任せる。使えるものはできるだけ多く入れる。

### F1 でしたこと（ブランチ `native/fonts`。C++・.qrc・CMake には触れていない。ビルドもしていない）
- `native/resources/fonts/` に書体ファイルを 40 個足した（合計 157.5 MB。既存の 6 個と合わせて 46 個・177.9 MB。画面用の IBM Plex Sans JP は別）。どれも元のファイルのまま（改変なし・名前も元のまま）。
- ライセンス文は書体ごとに `native/resources/fonts/licenses/`（OFL は `<書体>-OFL.txt`、littlelimit の 2 書体は `k8x12-LICENSE.txt`・`misaki-LICENSE.txt`）。`THIRD_PARTY.md` の「同梱フォント・アイコン」に 3 行足した。
- 一覧は `native/resources/fonts/FONTS.json`（genko-fonts@1）。46 書体それぞれに、ファイル・sha256・書体名・太さ・**アプリ用の短いキーの案**・画面に出す日本語名・用途の区分（台詞・ナレーション・叫び・丸ゴシック・かわいい・手書き・筆・ホラー・機械・効果音・その他）・かなが揃っているか・漢字の数（JIS 第 1・第 2 水準、常用）と `kanji: full|partial|none`・縦書きの対応（GSUB の vert／vrt2、vhea・vmtx）・ライセンス・著作権表示・予約フォント名・出典（コミットや ZIP の sha256）・改変の有無。
- 見本（コミットしない。Pillow で描いた。Genko はビルドしていない）: 作業用の `fonts-work/samples/genko-fonts-01.png`〜`04.png`。書体に無い字は赤い×の箱で出す（別の書体で埋めない）。全ページを目で見て、崩れる書体が無いことを確かめた。

### 選んだ決まり
1. ライセンス: SIL OFL 1.1 か、同じだけ自由なもの（アプリへの同梱と、出力の商用利用をはっきり許すもの）。出典に置かれたライセンス文を読んで確かめる。「個人利用のみ無料」や独自の条件、IPA フォントライセンスは入れない。
2. 出典: google/fonts の 1 つのコミット（`b78877b777ed3a18a1b431b0df7d8ee3f84c2e5f`）。取ったファイルは、そのコミットの木の blob の ID と照らした。既存の 6 個も同じコミットのファイルと 1 バイトも違わない。google/fonts に無いものは、作者の配布サイトから取れて、そこでライセンスを読めたものだけ（okoneya.jp の源暎 2 書体、littlelimit.net のドット 2 書体）。
3. まんがでの使い道が違う書体を選ぶ。似た書体は入れない。1 つの書体の太さは 1〜2 個まで。
4. 技術: FreeType 2.14 で開ける静的な TrueType。可変フォントは入れていない（自分で静的に切り出したものも無い）。かな・約物・英数字・漢字の揃い方を数えて `FONTS.json` に書く。かなだけ・漢字が一部だけの飾り書体も入れるが、印を付ける。
5. 大きさ: 12 MB を超えるファイルは入れない（しっぽり明朝 B1・しっぽりアンチック B1 を外した）。

### 入れた書体（キーの案 → ファイル）
- 台詞: `antique_genei` GenEiAntiqueNv6-M（源暎アンチック。アンチゴチ 1 ファイル）、`antique_shippori` ShipporiAntique-Regular、`antigothic_medium`・`antigothic_bold` ZenKakuGothicAntique（アンチゴチ 1 ファイル）、`antique_zen` ZenAntique-Regular、`gothic_regular` ZenKakuGothicNew-Regular、`gothic_seed` LINESeedJP-Bold
- ナレーション: `mincho_shippori` ShipporiMincho-Regular、`mincho_hina` HinaMincho-Regular、`klee` KleeOne-SemiBold（教科書体）
- 叫び: `gothic_black` ZenKakuGothicNew-Black、`gothic_kiwami` GenEiKiwamiGo（源暎きわみゴ）、`mincho_black` ZenOldMincho-Black、`mincho_tokumin` KaiseiTokumin-ExtraBold
- 丸ゴシック: `maru_regular` ZenMaruGothic-Regular、`maru_black` ZenMaruGothic-Black
- かわいい・ポップ: `pop_mochiy` MochiyPopOne、`pop_rocknroll` RocknRollOne、`cute_hachimaru` HachiMaruPop、`cute_kiwi` KiwiMaru-Medium、`mincho_decol` KaiseiDecol-Medium、`cute_cherry` CherryBombOne（かなだけ）、`cute_daruma` DarumadropOne（かなだけ）
- 手書き: `hand_kurenaido` ZenKurenaido、`marker_yusei` YuseiMagic、`hand_slackside` SlacksideOne（かなだけ）
- 筆: `brush_syuku` YujiSyuku、`brush_boku` YujiBoku、`brush_potta` PottaOne
- ホラー: `horror_tegomin` NewTegomin、`horror_chokoku` Chokokutai（かなだけ）
- 機械・ドット: `dot16` DotGothic16、`dot12` k8x12、`dot8` misaki_gothic_2nd、`stick` Stick、`techno_monomaniac` MonomaniacOne（かなだけ）
- 効果音: `sfx_outline` RampartOne（立体の袋文字）、`sfx_inline` TrainOne（漢字は第 1 水準だけ）、`sfx_aoboshi` AoboshiOne（かなだけ）
- その他: `narrow_lubrifont` WDXLLubrifontJPN（長体。10.3 MB）

### 字の揃い方で気を付ける所（どれも `FONTS.json` に書いてある）
- 漢字が無い（`kanji: none`）: cute_cherry・cute_daruma・hand_slackside・horror_chokoku・techno_monomaniac・sfx_aoboshi。漢字が一部（`partial`）: sfx_inline（第 1 水準 2,965 字だけ。常用は 2,106／2,136）。
- 全角の「！」「？」が無い: cute_daruma・hand_slackside・techno_monomaniac（半角の `!` `?` はある）。sfx_aoboshi は「『』」と単独の「゛゜」が無い。
- 「ゔ」が無い: dot12・dot8・narrow_lubrifont（「ヴ」はある）。単独の「゛゜」が無い: 既存の sfx（デラゴシック）、pop_mochiy。
- 「―」（U+2015）と「～」（U+FF5E）: ZEN の各書体としっぽりアンチックなどは持たないが、アプリの似た字への置き換え（fonts.cpp の kLookalike。― → —、～ → 〜）で出る。置き換えた後も無い約物は `punctuation_missing_after_lookalike`。
- 縦書き: dot12・dot8 は vert も vhea・vmtx も無い。cute_daruma は vert はあるが vhea・vmtx が無い。ほかは vert・vrt2・vhea・vmtx を持つ。
- しっぽりアンチックは第 2 水準が 4 字足りない（3,386／3,390。`full` の扱い）。
- dot12・dot8 は輪郭のほかに埋め込みビットマップ（EBDT／EBLC）を持つ。

### いまの読み込み方（F2 の前提）
- 文字入れの 6 書体は `native/resources/genko_text.qrc`（接頭辞 `/genko/text`、別名がキー）を `qt_add_big_resources` で `genko_render` に組み込んでいる（`native/src/render/CMakeLists.txt`）。`render/draw.cpp` の `with_text_font`・`bundled_bytes` が `:/genko/text/<キー>` をまるごとメモリへ読む（上限 256 MB）。ファイルのパスを書けば外のフォントも使える（`fonts.cpp` の `face_of`）。
- キーの一覧はソースに直に書いてある: `render/text/fonts.cpp`（kBundled。合成の antique もここ）、`render/draw.cpp` の `bundled_text_font`、`formats/psd_text.cpp` の `bundled_font`、`render/psd_type.cpp`（PSD のフォント名）、`core/ops_book.cpp`（ops の検査）、`app/lettering.cpp` の `bundled_fonts()`（GUI の一覧と日本語名）、`service/lint.cpp`（名前）、`api/cli.cpp`（doctor の「bundled font missing」）。

### F2 がすること
1. **置き方を決める（勧め: 追加の 40 個はファイルとして導入する）**。157.5 MB を `qt_add_big_resources` で `genko_render` に組み込むと、それを静的に結ぶ実行物（本体・CLI・試験の実行物それぞれ）が同じだけ大きくなり、ビルドの場所も配布物も膨らむ。既存の 6 個と pillow_default はいまのまま組み込み（基準画像・試験が変わらない）、追加分は `share/genko-studio/fonts/`（Windows は実行物の隣）へ `install(FILES …)` し、キー → ファイルは `FONTS.json`（これも導入する）から引くのがよい。開発中・試験では、ソースの `native/resources/fonts/` を見る道（既存の資源の探し方に合わせる）が要る。組み込みを選ぶなら .qrc に 40 行足すだけで済むが、大きさを測ってから決めること。
2. キーを登録する: 上の「キーの一覧」の全箇所と `docs/ops.schema.json`（フォントの説明があれば）。キーは案なので、変えるなら `FONTS.json` も直す。
3. 合成の書体: 既定の `antique`（かな mincho・ほか gothic）は変えない（既存の本の見た目が変わるため）。アンチゴチ 1 ファイルの書体（antique_genei・antigothic_medium・antigothic_bold）は合成が要らない。かなだけ・漢字が一部の書体は、`FONTS.json` の `composite_suggestions`（例: cute_cherry のかな＋maru_black の漢字、horror_chokoku＋mincho_black）のように、漢字側を似た太さの書体にした合成のキーとして出すのがよい。
4. 無い字の代わり: いまの `Fonts::path_for` は「書体の相方 → gothic → sfx」の順。`kanji: none|partial` の書体と、全角「！？」の無い書体で、代わりの書体が黙って混ざる。合成の相方を決めるか、GUI・lint で「この書体は漢字を持たない」と知らせる。全角「！？」→ 半角への置き換えを kLookalike に足すかも決める。
5. GUI: `bundled_fonts()` の一覧を、`FONTS.json` の `categories` の順の区分ごとに分けて出す（47 個を 1 列に並べない）。日本語名は `name_ja`、使い道は `use_ja`。見本の絵を出すなら Genko で描き直す。
6. 配布物と試験: `native/package/sbom.cmake` の `fonts` の行（書体名の列。源暎は OFL-1.1、k8x12・美咲は `LicenseRef-…`）、`native/tests/package/test_package.cpp` の `licenses/fonts` の `*-OFL.txt` の数（いま 7 → **39**。F1 でファイルを足したので、この試験は F2 で直すまで落ちる）と `k8x12-LICENSE.txt`・`misaki-LICENSE.txt` の確認、`THIRD_PARTY.md` の「まだ登録していない」の書き換え、導入するフォントの数・sha256 の試験（`FONTS.json` と照らす）、doctor の検査。
7. 手引き: `docs/AGENT.md`（249 行目あたりの書体の説明）、`native/resources/studio/guide/SKILL.md`（42〜43 行目あたりのジャンルごとの勧め）、`integrations/*/genko-manga/SKILL.md` に、区分ごとの使い分けを足す（例: ホラーの台詞 horror_tegomin、機械の声 dot16・stick、叫び gothic_black・gothic_kiwami・mincho_black、手紙・心の声 klee、時代もの brush_syuku）。
8. 縦書き・ルビ・ふちどりで 46 書体が崩れないかを、Genko の描画で見る（F1 の見本は横組みだけ）。PSD への書き出し（psd_type のフォント名）も足す。

### 入れなかったもの
- **欲しいが取れなかった**: 源界明朝・装甲明朝・廻想体（フロップデザイン。flopdesign.com がこの環境からの取得に 403 を返す。ホラーの本命）、自家製 Rounded M+・源柔ゴシック（jikasei.me に繋がらない）、OSDN に置かれたもの（osdn.net に繋がらない）。
- **ライセンス文を出典で確かめられなかった・条件が合わない**: M PLUS Rounded 1c（google/fonts のその書体の場所に OFL.txt が無い）、IPA フォントライセンスのもの（IPAex、やさしさアンチック、GL-アンチックなど。決まりにより入れない）、独自の「フリー」条件の書体（衡山毛筆フォント、青柳衡山、玉ねぎ楷書激、けいふぉんと、あんずもじ、851 チカラヅヨク、ニコモジ、ラノベ POP、g_コミックホラーなど。条件を読んで確かめていない。入れていない）。
- **技術**: M PLUS 1 Code・Kapakana・Noto Sans JP・Noto Serif JP・Murecho・M PLUS 1／2（可変フォントだけ。M PLUS 1 Code は全角「！？」も無い。Kapakana は「。「」！？」が無い）、源暎ラテゴ・ラテミン（TTC だけ。予約フォント名があるので切り出すなら改名が要る）、Rock 3D・Shizuru・Palette Mosaic（全角の「！？…」などが多く欠ける）。
- **似ているので外した**: Zen Antique Soft、Shippori Antique B1（13.1 MB）、Shippori Mincho B1（15.2 MB）、Yuji Mai、Klee One Regular、Kosugi・Kosugi Maru（Apache-2.0）、BIZ UDGothic・BIZ UDMincho、Kaisei Opti・Kaisei HarunoUmi、M PLUS 1p（第 2 水準が一部）、Sawarabi Gothic・Mincho、Tsukimi Rounded、Otomanopee One、びぜんアンチック、源暎ぽっぷる、棘丸ゴシック、k8x12L・k8x12S・美咲ゴシック・美咲明朝。
- **足りない区分**: 本物の毛筆（行書・草書）と、かすれ・にじみの強いホラー書体は、決まりに合うものが取れる所に無かった。利用者が持つフォントは、いまもファイルのパスで使える。

### F2 群 1（F2-A: 解決と描画。ブランチ `native/fonts-f2`、main da6acebd に F1 の 2 コミットを載せた上）— 2026-11-14

（この節は群 1 を終えたときの記録。その後のレビューの直しで変わった所は、下の「F2 群 1 のレビューの直し」が正しい。ここでは、変わった行に〔直し〕と書いて直してある。）

#### 決めたこと（リードの決定 D1〜D12。利用者の言葉は「フォント追加は、任せます 使えるものであれば可能な限り追加してください」）
- 前からの 6 書体はプログラムに組み込んだまま（.qrc は触らない）。追加の 40 書体はファイルで置き、`FONTS.json` を一覧として `genko_core` に組み込む。
- キーは F1 の案のまま。本に新しい鍵は足さない。追加の書体を使っているあいだだけ `fonts@1` を名乗る。
- 書体ファイルが無いときは、その種類のいつもの書体で描いて知らせる（豆腐にしない）。
- かなだけの書体は、同じキーの中に相方を持つ。全角の「！？」が無い書体は、書体ごとに半角へ置き換える。
- 追加の書体だけ、字の有無を cmap で決める。前からの 6 書体と、パスで指した書体ファイルは 1 ビットも変えない。
- 新しい本の既定の書体は変えない。ノンブルは前からの 7 つのまま。MCP の説明文は変えない。
- **前の本との違いは 1 つだけ受け入れた（D2）**: `fonts@1` を持たない前の本が、たまたま追加のキーと同じ名前（例: `gothic_black`）を `font` に書いていたら、今の版はその書体で描き、次の保存で `fonts@1` を付ける。配布した版が無いので受け入れた。開いたときに 1 度知らせるのは群 3（一覧は `core::fonts::named_unannounced`）。
- 追加の書体の絵の基準に Python・Pillow は使わない（利用者の決まり）。この Linux のビルドが描いた値を固定した。

#### 作ったもの
- `native/resources/fonts/FONTS.json`: `status` を `storage`（`embedded`／`file`）と `feature`（null／`fonts@1`）に替え、`fallback`（相方のキー）、`lookalike`（「！？」の置き換え。3 書体だけ）、`vertical.presentation_forms`（縦のかっこの字形を全部持つか。試験で測った値）を足した。`composite_suggestions` は `fallback` に畳んで消した。`counts` の `existing`・`new`・`bytes_new` は `embedded`・`file`・`bytes_file` に替えた。
- `native/src/core/font_catalog.{hpp,cpp}`（＋ `font_catalog_data.cpp.in`、`core/CMakeLists.txt`）: 一覧を 1 バイトも変えずに組み込む。`core::fonts::` の `entries()`（46）・`categories()`（11）・`find(key)`・`added(key)`・`by_postscript(name)`・`named(doc)`・`uses_feature(doc)`・`named_unannounced(doc)`・`catalog()`・`catalog_text()`・`kFontsFeature`。
- `native/src/render/font_files.{hpp,cpp}`: 追加の書体のファイル。見る場所は `<プログラムのあるフォルダー>/../share/genko-studio/fonts` の 1 か所だけ（環境変数なし）。`dir()`・`set_dir_for_testing()`・`path(key)`・`present(key)`（〔直し〕入っているか: 普通のファイルで、大きさと SHA-256 が一覧と同じ。最初の 1 回がファイルを読む。答えはプロセスの間ずっと同じ）・`file_there(key)`（〔直し〕大きさの合うファイルがあるか。stat 1 回）・`bytes(key)`（読み切って閉じる。共有。最近使った順に 64 MB まで）・`missing()`・`verify(hashes)`・`cached_bytes()`・`clear_cache()`。
- `render/text/fonts.{hpp,cpp}`: `Face` に `fallback` と `own(font)`。`face_of`・`face_exists` は「前からのキー → 追加のキー（入っているとき）→ ファイルのパス → 既定」。追加のキーはパスとしては決して読まない。`path_for` は「その書体 → もう半分 → 相方 → gothic → sfx」。`normalize` は書体ごとの置き換えも見る。`has_glyph` は追加の書体では cmap。
- `render/draw.{hpp,cpp}`: `TrueTypeFont::strict_cmap()`。`TrueTypeFonts`・`font_bytes` は追加のキーを `font_files::bytes` から開く（`with_text_font`・`text_font`＝ノンブルの道は触っていない）。
- `render/text/tategaki.cpp`: 縦のかっこの字形は、追加の書体では cmap にあるときだけ使う（無ければかっこを回す）。相方で描いたルビの字は `font_fallback` にしない。
- `formats/psd_text.cpp`・`render/psd_type.{hpp,cpp}`: 追加の書体は一覧の PostScript 名で書き出し、読み込みではその名前をキーに戻す（書体ファイルは読まない）。相方の字は `font_fallback` にしない。
- `storage/writer.cpp`・`reader.cpp`: `fonts@1`（使っているあいだだけ。読み手は知っている拡張として読む）。
- `formats/checks.cpp`: `font_not_installed`（警告）。〔直し〕文と、見る場所（台詞のほかに文字の属性・ルビ・文字レイヤー・本のいつもの書体）は下の「レビューの直し」のとおり。追加の 40 キーだけ。
- `native/src/api/CMakeLists.txt`: `genko` をビルドした後、ビルドの木に `<genko のフォルダー>/../share/genko-studio/fonts` を作る（Unix は `native/resources/fonts` へのリンク、Windows はコピー）。
- 文書: `docs/cpp-migration/schema-v4.md` の §3 の表に `fonts@1`、末尾に「書体」の節。
- 試験: `native/tests/unit/test_font_catalog.cpp`（95 項目）、`native/tests/unit/test_font_faces.cpp`（452 項目。40 書体それぞれ）、`test_psd_text.cpp` に 3 項目。固定した値は `native/tests/fixtures/render/font-faces-f2.json`（書体ごとに、字のマスクの箱・大きさ・進み幅・SHA-256 と、縦・横・「ルビ＋ふちどり＋極太」の文字組の SHA-256。`GENKO_FONT_FACES_PIN=<ファイル>` で書き直せる）。漢字の数え方の表は `native/tests/data/fonts/kanji-sets.json`（JIS 第 1・第 2 水準と常用漢字。`tools/migration/font_kanji_sets.py` が EUC-JP の表と Unihan 17.0.0 の kJoyoKanji から作る。符号位置の表だけで、描画の基準ではない）。どちらも MANIFEST に sha256 を足した。

#### 群 2・群 3 が使えるもの
- 書体の一覧と区分: `core::fonts::entries()`・`categories()`・`find()`。画面に出す名前は `Entry::label`、使い道は `use`、区分は `category`、漢字は `kanji`（full／partial／none）、相方は `fallback`。前からの 7 つは今までの一覧（`lettering::bundled_fonts()`・`lint::bundled_fonts()`）のまま。
- ファイルがあるか: 〔直し〕下の「レビューの直し」の「いつ何を呼んでよいか」を見ること（画面のスレッドでは `file_there`。`present` は 1 書体を読む。`missing()`・`verify(true)` は 40 個を読むので、画面のスレッドと起動では呼ばない）。
- 試験は `render::font_files::set_dir_for_testing(GENKO_REPO_ROOT "/native/resources/fonts")` を呼ぶ（呼ばない試験の実行物では 40 書体とも「無い」）。フォルダーを替えたら `render::text::clear_layout_cache()` も呼ぶ。
- 見本の絵は、台詞（`style.font` にキー）を `render::text::text_layout`・`render_page` に渡せば、紙面と同じに描ける（相方の漢字も出る）。
- 前の本がたまたま追加のキーを名指ししているか: `core::fonts::named_unannounced(doc)`（開いた直後に呼ぶ）。
- 群 2 に残したもの: `GenkoPackage.cmake`（40 ファイルと `FONTS.json` の導入）・`sbom.cmake`・`THIRD_PARTY.md`（「まだ登録していない」の書き換え）・`test_package` の残り、doctor、`lint` の `unknown_font`（いまは追加の 40 キーを「見つからない」と言う）、`inspect target=fonts`、`wording.cpp`、`headless_server.cpp` の `font_path` の閉じ込め（キーをパスに変えてしまう）、手引き。
- 群 3 に残したもの: 書体のパレット、文字パネル・字形パネル・ルビの書体・見つからないフォントの画面、`note_missing_glyphs`（相方で描く字を「無い字」と言わないようにする）、開いたときの D2 の知らせ、ファイルが無いことの知らせ（画面）。

#### 計画と変えた所
1. **hand_slackside の相方を `hand` から `marker_yusei` にした**。相方は追加の書体に限ることにした（試験で守る）。理由は下の「前からの書体の不具合」: 前からの書体は Pillow の流儀で読むので、縦のかっこが豆腐になる字があり、それを前の本のために変えられない。太さもマーカー同士で合う。
2. **追加の書体の絵の基準は Pillow ではなく、この Linux のビルドの値**（利用者の決まり。計画の D10 の後半のとおり）。`tools/migration/font_faces_reference.py` は作っていない。
3. **`font_not_installed` の検査を群 1 に入れた**（計画では群 2。リードの依頼文が群 1 の目標に挙げていたため）。`lint`・doctor・`wording.cpp` は群 2 のまま。
4. **`uses_feature` は `bible.lettering.*.font` を見ない**。この鍵は project.json に無い（スタジオの bible）。自動の文字入れがそこから台詞の `style.font` に写すので、台詞の側で数える。
5. `FONTS.json` の `counts` の鍵の名前を替えた（上のとおり）。
6. `font_bytes`・`State::file` は追加のキーを名前のまま受ける（`Face.kana`・`other`・`fallback` に入るのはキーそのもの。パスにはしない）。レイアウトの覚え書きの鍵にパスが入らない。

#### 測って分かったこと（計画が「読んだだけで確かめていない」とした所）
- **(a) 字の箱で有無を決める流儀の読み違い**: ドットの書体（dot16・dot12・dot8）では 0 件。代わりに、源暎きわみゴ 1,074 字、Yusei Magic 652 字、Monomaniac One 8 字、Rampart One 6 字、Slackside One 6 字、ほか 5 書体で 1 字ずつ（かな・約物・英数字・JIS 第 1・第 2 水準の 6,700 字ほどを 32 px で見た数）が「無い字」と読まれる。cmap で決めないと、源暎きわみゴのかなの半分ほどが前からのゴシックで描かれていた。試験 `hasGlyphFollowsTheCmap` が数を固定している。
- **(b) 縦のかっこの字形**: 6 書体が一部か全部を持たない（cute_daruma・hand_slackside・techno_monomaniac は ︵︶﹇﹈、sfx_aoboshi は 10 個、dot12・dot8 は 14 個全部）。cmap を見ないと、その 6 書体の縦のかっこは豆腐になっていた。いまは横のかっこを回して描く。
- 40 書体とも U+FFFF の字は持たない。半角の `!` `?` `0` は 40 書体とも持つ（「！？」を 1 マスに並べるのに使う）。
- ドットの書体は、作られた大きさ（k8x12 は 12 px、美咲は 8 px）で、書体が持つビットマップで描く（画素は 0 か 255 だけ）。k8x12 の字の幅は 8 px。ふちどりと太字は輪郭から描く。
- OpenType の文字組（type@1 の記録）の縦: 38 書体で「「」が vert の字形に替わる。k8x12・美咲は cmap のまま。40 書体とも下へ進む（cute_daruma も）。
- ビルドの木の `genko`（リンクの先の書体）で、追加の書体の台詞が描けることを、試験が `genko render` で確かめている。install.sh のリンク越しは群 2 の `test_package` で確かめる。
- **確かめていないもの**: Windows・MSVC のビルド、ASan、ZEN 角ゴシックアンチックの漢字が ZEN 角ゴシック New と同じ輪郭かどうか、配布物の大きさ。

#### 前からの書体の不具合（見つけただけで、直していない。直すと前の本の絵が変わる）
- 前からの `sfx`（デラゴシック）は、縦書きの『』（）()【】［］〈〉《》を「無い字」の箱（□に斜線）で描く。`hand`（よもぎ）は ［］ を箱で描く。縦のかっこの字形を持たないのに、「字を描いた大きさ」だけで有無を決めているため（Python の版と同じ）。「」だけは無事。見本の 1 枚目（`fonts-150-plain-1.png` の左上）に出ている。直すかどうかは利用者・リードの判断（既存の本の絵が変わるので、この群では触っていない）。
- 横組みの最後の行の `_` が切れる（見本の札で気づいた。前からの書体でも同じ）。調べていない。

#### 直した既存の試験（緩めていない）
- `native/tests/package/test_package.cpp`: `licenses/fonts` の `*-OFL.txt` の数を 7 → 39、`k8x12-LICENSE.txt`・`misaki-LICENSE.txt` があること、ファイルは 41 個。F1 でライセンス文を足してから落ちていた検査。ほかの既存の試験の期待値は 1 つも変えていない。
- `test_psd_text.cpp` には項目を足しただけ。

#### 確かめたこと（Linux Release。関係する試験だけ。全体は流していない）
- 新しい試験: test_font_catalog 95／95、test_font_faces 452／452、test_psd_text の新しい 3 項目。
- 関係する既存の試験（どれも合格）: test_storage、test_model、test_command_bus、test_render_page（font-glyphs・ノンブルの絵・MANIFEST）、test_ruby、test_ruby_render、test_psd_text、test_fukidashi、test_fukidashi_service、test_type_text、test_type_styles、test_shaping、test_material_stamp、test_pcraft、test_service、test_ledger_service_a・b・c、test_contract_type・opentype・type_styles・type_fonts・storage・render_cli・psd_text・psd・pcraft・pipeline・export・materials、test_cli_e2e、test_save_e2e、test_migrate_e2e、test_ops_e2e、test_gui_lines、test_gui_ruby、test_gui_type、test_gui_type_panels、test_gui_fukidashi、test_gui_book、test_gui_ledger_a、test_perf_render、test_package。
- SKIP: 自分の試験には無い。前からあるもの: test_save_e2e の 16 項目と test_migrate_e2e の 1 項目（Release に故障の注入が無い）、test_perf_render の 1 項目（`GENKO_PERF_GROUPS_PSD` が無い）。
- 見本の絵（コミットしない）: 作業用の `fonts-f2/`。47 書体（前からの 7 つと追加の 40）を、縦書きの台詞をフキダシの中・ルビ付き・ふちどりの 3 通りで 150 dpi の紙面に（各 3 枚）、8 書体を 600 dpi で、書体ファイルが 1 つも無いパソコンでの同じ紙面を 3 枚。`GENKO_FONTS_OUT=<フォルダー>` を付けて test_font_faces を流すと書き直せる。

#### 残り・気を付けること
- かなだけの 3 書体（cute_daruma・hand_slackside・techno_monomaniac）は、かなが小さめ・細めに作られていて、相方の漢字のほうが大きく見える。書体ごとの大きさの合わせは入れていない（見本を見て決めること）。
- 〔直し〕書体ファイルが同じ大きさの別物に差し替わっていたら、「入っていない」として扱う（レビューの直しで SHA-256 まで見るようにした）。
- 〔直し〕`docs/cpp-migration/SPEC.md` の書体の節は群 2・3 の終わりに足した（FONT-01）。
- Windows のビルドの木では書体をコピーする（157 MB）。

### F2 群 1 のレビューの直し（同じブランチ。リードの 2 コミット bc4de561 の上）— 2026-11-14

読むだけのレビュー（P1 は無し、P2 が 4、P3 が 8）と、リードの決定のとおりに直した。前からの 6 書体と、パスで名指しした書体ファイルの絵は 1 ビットも変えていない。

#### 直したこと（それぞれに試験。どれも `test_font_faces`）
- **P2-1 スタイルの語**（「！？」を半角に置き換える 3 書体で、「！」「？」を含む語に付けたスタイルが黙って外れていた）: 追加の書体のときだけ、スタイルの語も書体の置き換えで読み替えてから探す（`lettering.cpp` の `style_runs_of`）。`aShoutedWordWithMarksKeepsItsStyle`。
- **P2-2 中身の確かめ**（大きさが同じで中身が違うファイルを黙って描いていた）: 「入っている」は大きさと SHA-256 の両方。最初に読むときに確かめ、答えはプロセスの間変えない。違えば「入っていない」（いつもの書体と知らせ）。`aFileOfTheSameSizeButOtherContentIsNotInstalled`。
- **P2-3 知らせる場所**: `font_not_installed` を、台詞の `style.font` のほかに、文字の属性の書体・ルビの書体・文字レイヤーの記録・本のいつもの書体（`font_path`。最初のページに 1 つ）でも出す。相方のファイルだけ無いときは `font_partner_not_installed`。書体ごとにページに 1 つ。`theMissingFaceIsSaidWhereverItIsNamed`。
- **P2-4 文**: 1 つの言い方（「…描いた（アプリを入れ直すと元の書体で描ける）」）、英字 4 字以上の続き・キー・製品名の英字を出さない。何で描いたか（アンチック・極太の書体・本文の書体）を種類ごとに正しく言う。英字の長い 2 つの名前は一覧の `name_ja` を日本語にし、元の名前は `name_original` に残した（gothic_seed「ラインシード B」、narrow_lubrifont「滑油字（長体）」）。`everyLabelAndWarningReadsInJapanese`。
- **P3-1 異体字の印**: 追加の書体で、異体字の印（VS）が付いた字は、元の字で書体を決める。書体に無い印は描かない（豆腐にしない）。`aLetterWithASelectorGoesWhereTheLetterGoes`。
- **P3-2 フォルダーの決め方**: 場所は起動のとき、アプリを作ったスレッドで 1 度だけ決める（`font_files::prime()`。`genko`・画面・印刷の入口で呼ぶ）。追加のキーをファイルの名前として探す道は無い。`theProgramFindsTheFacesBesideItself`。
- **P3-3 読む量と、動かしている途中の変化**: 読むのは一覧の大きさ＋1 バイトまで。覚えは 64 MB まで。読み直すたびに中身を確かめ、違う・消えたときは、その書体をそこから「入っていない」にする（`generation()` が進み、文字組の覚えの鍵に入る。進むまでは鍵は前と同じ）。描いている途中で失った書体は、その 1 回だけ前からのゴシックで描く。`aFileThatChangesWhileGenkoRuns`、`theAddedFacesAreNotReadUntilAsked`。
- **P3-4 `face_index`**: 追加の書体に残っていた `face_index` は読まない（追加の書体は 1 ファイル 1 書体）。`aFaceNumberLeftOnAnAddedFaceIsNotRead`。
- **P3-6 ビルドの木の手順**: `native/cmake/GenkoDevFonts.cmake` と独立したターゲット `genko_dev_fonts`（リンクを張る。張れなければコピー。古いものだけ写し直す）。`genko` のビルドの後に走り、試験の支えのライブラリーがこれに依る。`theBuildTreeStepLinksOrCopies`。
- **P3-7 試験の穴**: 上の各項目のほか、前の本が 1 バイトも変わらずに保存されることを、書体を足す前の版（2026-11-13 の配布物）が書いた `native/tests/data/fonts/book_before/project.json` と比べる（`aBookOfBeforeIsWrittenToTheSameBytes`）。`test_psd_text` は項目ごとに書体のフォルダーを元に戻す。
- **P3-5 は直していない（分かっている限り）**: 1 つの文字（type@1）の中で追加の書体と前からの書体を文字ごとに混ぜたとき、「！？」の置き換えは段落の書体で決まる。文字ごとの書体で決めるには前からの書体の組み方に手を入れることになるので、決定のとおり書き残すだけにした。

#### いつ何を呼んでよいか（`render::font_files`）
- `file_there(key)`: stat 1 回。画面のスレッドで呼んでよい（`inspect` の `available`、`lint`、パレットの「ファイルなし」、開いたときの知らせ）。前に「中身が違う」と分かった書体には false。
- `present(key)`: 入っているか。最初の 1 回はその 1 ファイルを読む（数 MB と SHA-256）。描く道・検査・選んだ書体の確かめに使う。パレットは別のスレッドで呼ぶ。
- `bytes(key)`: 描く道だけ。`missing()`・`verify(true)`: 40 個を全部読む。`genko doctor` と試験だけ。画面のスレッドと起動では呼ばない。
- 試験の実行物は、ビルドの木の `<genko のフォルダー>/../share/genko-studio/fonts` ではなく自分の横を探すので、追加の書体を使う試験は `set_dir_for_testing(<リポジトリ>/native/resources/fonts)` を呼ぶ（替えたら `render::text::clear_layout_cache()` も）。終わりに `set_dir_for_testing(std::nullopt)` で戻す。

#### 変えた期待値（緩めていない）
- `test_font_faces` の `font_not_installed` の文（P2-4 の言い方に）。`replace_fonts` が追加のキーを「見つからないフォント」に挙げる、という群 1 の期待は、群 2 で「挙げない」に変えた（下）。
- 項目の数: test_font_catalog 96、test_font_faces 505（群 1 の終わりは 95・452）。

#### 確かめたこと
- 関係する試験 42 本が合格（1,751 項目。失敗 0。SKIP は前からの 17 項目: test_save_e2e の 16 と test_migrate_e2e の 1、Release に故障の注入が無いため）。

### F2 群 2（F2-B: 配布・サービス・手引き）— 2026-11-14

#### 作ったもの
- **配布物**: `native/cmake/GenkoPackage.cmake` が `FONTS.json` を読んで、`storage: file` の 40 個と一覧を `share/genko-studio/fonts/` に入れる（40 個でなければ構成で止まる）。`native/package/sbom.cmake` に 3 つのパッケージ: `fonts-lettering`（OFL の 36 個）、`fonts-genei`（源暎の 2 個）、`fonts-littlelimit`（k8x12・美咲の 2 個。`LicenseRef-LittleLimit-Free`。ライセンス文を SBOM に入れる）。`THIRD_PARTY.md` の 3 行を「入れてある」に書き換えた。`native/package/README-ja.md` に書体の説明。
- **`genko doctor`**: 40 個の有無・大きさ・SHA-256 を見て、違うものを `problems` に挙げる（`lettering font missing|of another size|changed: <キー> (<ファイル>) in <フォルダー>`。コマンドラインの英語の報告で、画面には出ない）。
- **`lint`**: 追加のキーを知っている書体として扱う。ファイルが無ければ `font_not_installed`（警告。「書体「…」のファイルがこのパソコンに無い（アンチックで描かれる）」、手がかり「アプリを入れ直すと元の書体で描ける」）。
- **`inspect target=fonts`**（`service/briefs.cpp`）: `bundled` は前からの 7 行（前と同じ）＋追加の 40 行 `{key, label, category, use, kanji, fallback?, available}`。`categories` を足した。`note` は前のまま。
- **HTTP の `font_path`**（`api/headless_server.cpp`）: 追加のキーはキーのまま受け取る（作業フォルダーの下のパスに直さない）。
- **`replace_fonts`**（`render/ops_type_fonts.cpp`）: 追加のキーは「見つからないフォント」に数えない。尋ねる op の答えに、そういう書体があるときだけ `not_installed: [{font, label}]`。
- **PSD の書き出し**（`formats/psd_export.cpp`・`psd_text.cpp`）: ファイルが無くても、本が名指しした書体の PostScript 名で書く。`font_warnings` がその名前を挙げる。
- **手引き**: `native/resources/studio/guide/SKILL.md` と、同じバイトの写し `integrations/hermes/genko-manga/SKILL.md`・`integrations/claude-code/skills/genko-manga/SKILL.md`、`docs/AGENT.md`。使い道ごとの書体の表、かなだけの書体と相方、極太の書体の使い方、`available` の見方。
- **`service/wording.cpp`**: 足していない（この群で増えた、投げる英語の文が無い）。
- **MCP**: 道具の入力の形（inputSchema）と説明文は変えていない（`test_mcp_api_e2e` の合計の検査が合格）。承認の流れに触っていない。

#### 配布物の大きさ（Linux x86_64。2026-11-14 に測った。導入したフォルダーを `tar -czf` で固めた値）
- 群 3 まで入れた最後のビルド: tar.gz **162,044,123 バイト**、展開して 333,658,732 バイト、うち `share/genko-studio/fonts` が 157,645,644 バイト（40 個と一覧で 41 ファイル）、`bin/genko` が 86,348,600 バイト。
- 群 2 の終わりに測った値: tar.gz 161,843,212 バイト、展開して 333,534,836 バイト。
- 書体を足す前の記録: tar.gz 75,199,411 バイト。増えたのは 87 MB ほど（書体は固めてもあまり縮まない）。
- Windows の zip と macOS は測っていない。

#### 変えた期待値（緩めていない）
- `test_package`: SBOM の行の表に上の 3 つのパッケージ。ライセンスのファイルの数（OFL 39・LICENSE 2・合わせて 41）は群 1 で直したまま。導入したプログラムで doctor・`inspect fonts` を流す所は、別の利用者のホームで行う（片付けの検査が、試した人のホームに残るものを見るため）。
- `test_psd_text` の `without.fonts`: ファイルが無いときも、名指しした追加の書体の名前が入る（上の PSD の決まり）。
- `test_font_faces`: `replace_fonts` は追加のキーを挙げず、`not_installed` に入れる。

#### 試験
- 新しい試験: `native/tests/unit/test_font_service.cpp`（`inspectListsTheSevenThenTheForty`・`lintKnowsTheAddedFaces`・`replaceAllNeverStripsAnAddedFace`・`psdNamesTheFaceAskedForWithoutItsFile`）、`test_mcp_api_e2e` の `httpApiKeepsAnAddedFacesKey`、`test_package` の `theLetteringFontsAreInstalledAsFetched`・`theInstalledProgramFindsEveryFace`（導入した `genko` が 40 書体を見つけ、描いた絵が変わり、ファイルを消す・中身を変えると doctor が言う。install.sh のリンク越しは別の利用者のホームから）。
- 関係する試験 20 本が合格（496 項目、SKIP 0）: test_font_catalog・test_font_faces・test_font_service・test_psd_text・test_mcp_api_e2e（全部）・test_ledger_service_a・b・c・test_gui_ledger_a・b・c・test_gui_type_panels・test_gui_output ほか。test_package（8）も合格。

### F2 群 3（F2-C: 画面）— 2026-11-14

#### 作ったもの
- **ドック「書体」**（`app/font_palette.{hpp,cpp}`、`app/main_window_fonts.cpp`）: ウィンドウ → 書体、ページ → 台詞 → 書体のパレットを開く（`act_font_palette`。名前のある操作は 822 → 823）、文字ツールの設定の「ほかの書体…」。台詞のタブの隣、初めは閉じている、配置と一緒に覚える。
  - 一覧: 区分の見出し 11 と書体 47（アンチックが最初）。行は「名前 → 知っておくこと（かなだけ（漢字は…）・漢字は一部（残りは…）・ファイルなし）→ 見本」の縦並び（`FaceRow` という行の描き手。ドックが狭いので、見本の横に名前を置くと名前が頭の 3 字で切れた）。
  - 見本: `render::text::text_layout` で「あア漢字！」（4.6 mm、96 dpi）を描いたもの。文字の色は画面の文字の色（明るい・暗いを切り替えると塗り直す）。
  - 遅らせ方: 窓を開いただけでは書体ファイルを読まず、見本も作らない。初めて見せてから、画面の合間に 1 つずつ。追加の書体のファイルは、見本の前に別のスレッド（QtConcurrent）で 1 つずつ読んで確かめる（相方も）。画面のスレッドは組むだけ。測った値: 47 個で 1.3 秒ほど、1 つの見本が画面を止めるのは長くて 40 ms ほど（別のスレッドにする前は 150〜190 ms）。
  - 検索: 名前・使い道・区分・キー・元の名前。空白で区切ると全部を含むもの。
  - 選ぶ: クリックは、このページの台詞を選んでいて文字ツールを持っていなければその台詞に（`edit_line` 1 つ。同じなら何もしない）、そうでなければ次に置く台詞に（本は変えない。文字ツールの「書体」にその 1 つだけ足す）。右クリックで 選んだ台詞・次に置く台詞・選んだルビ・この本のいつもの書体。
  - この本のいつもの書体: `set_meta` の `font_path`。文字ツールの「書体」の最初の行が「いつもの書体（名前）」と言う。
  - ファイルが無い書体: 淡い色と「ファイルなし」、選べない（選ぶと言うだけ）。選ぶときは `present` で中身まで確かめる。
- **開いたときの知らせ**（`MainWindow::note_fonts_on_open`）: ページの上の帯（`fonts_band()`。閉じるまで出ている。「この知らせを閉じる」で消え、その本ではもう出ない。別の本のタブに移ると隠れ、戻ると出る）。言うのは 2 つ: 本が使っている書体のファイルが無いこと、`fonts@1` を持たない前の本が名前で書いていた書体をこの版から付属の書体で描くこと（D2）。ページを読んでいる途中の本は、読み終わってから調べる（後のページの文字レイヤーも数える）。
- **前からの一覧**（レビューの P3-8）: 文字パネルの書体の一覧は、7 つの後に区分ごとの 40 書体を名前で並べる（`type_panels.cpp`。Qt に書体を登録しない）。`qt_family` は、追加の書体をパスの書体と同じに「見せている 1 つだけ登録し、次で外す」。ルビの書体と文字パネルは、キーでなく名前を見せる。`note_missing_glyphs` は相方で描く字を「無い字」と言わない。
- **見つからないフォント**: 「見つからないフォントを解決…」「すべての見つからないフォントを置換」は、付属の書体をファイルが無いだけで置き換えず、そのことを言う。
- `native/src/core/data/commands.json`: 2 行増えた（ウィンドウの「書体」、ページ → 台詞の「書体のパレットを開く」）。

#### 計画と変えた所
1. **開いたときの知らせを、状態行でなくページの上の帯にした**。状態行は 1 行で、名前を挙げた文が窓の幅で切れた（見本の絵で分かった）。帯は読み取り専用の帯と同じ作り。
2. **見本は画面のスレッドで組む**（ファイルを読んで確かめるのだけ別のスレッド）。重いのは読むほうで、組むのは 1 つ 40 ms までだったので、組むのまで別のスレッドには出していない。
3. **行を縦並びにした**（上のとおり）。
4. 文字ツールの「書体」は 7 つのまま。パレットで選んだ追加の書体は、その 1 つだけを 8 行目に足す（次を選ぶと入れ替わる）。

#### 変えた期待値（緩めていない）
- `test_gui_actions`: 名前のある操作の数 822 → 823（`act_font_palette`）。
- `test_gui_lines` の `commandsAsPythonsWindowHasThem`: ページ → 台詞 の並びに「書体のパレットを開く」（フキダシのパレットの次）。
- `commands.json`（`test_gui_prefs` の `commandsJsonIsTheMenus` が比べる表）: 上の 2 行。

#### 試験
- 新しい試験 `native/tests/gui/test_gui_fonts.cpp`（18 項目）: 一覧と見出し・行の作り、見せるまで何も読まない・作らない（別のスレッドで 40 個を読むこと）、見本が紙面の描き方と同じ画素、検索、選んだ台詞に 1 回で、次の台詞に、かなだけの印、ファイルが無い書体（大きさだけ同じ別物も）、前からの一覧、文字パネルが Qt に書体を足さない、本のいつもの書体、ルビ、配置、開いたときの知らせ（何も言わない本・D2・ファイルなし・別の本との行き来・閉じる）、読んでいる途中の本、見つからないフォント、明るい・暗い、読む言葉が全部日本語（英字 4 字以上の続き・キー無し、です・ます）。
- `GENKO_FONTS_GUI_OUT=<フォルダー>` を付けて流すと、画面の絵を書く（明るい・暗いそれぞれ: 窓、パレット、印の付いた行、一覧の終わり、検索、文字ツールの設定、ファイルが無いときの知らせとパレット、前の本の知らせ）。作業用の `fonts-f2/gui/` に 18 枚。紙面の見本 36 枚（`fonts-f2/`）も最後のビルドで書き直した（前からの `sfx`・`hand` のかっこの不具合が出ている 1 枚目もそのまま）。
- **確かめたこと（最後のビルド。Linux Release、2 本ずつ）**: 関係する試験 35 本が合格（1,334 項目。失敗 0、SKIP 0）: test_gui_fonts・test_gui_actions・test_gui_prefs・test_gui_lines・test_gui_type_panels・test_gui_type・test_gui_ruby・test_gui_fukidashi・test_gui_book・test_gui_layout・test_gui_workspace・test_gui_look・test_gui_studio・test_gui_view・test_gui_ledger_a・b・c・test_gui_output・test_gui_file・test_gui_smart・test_gui_tool_presets・test_gui_styles・test_cli_e2e・test_control_channel・test_font_faces（505）・test_font_catalog（96）・test_font_service・test_psd_text・test_contract_prefs・test_mcp_api_e2e・test_service・test_ledger_service_a・b・c・test_package。1 回目に落ちたのは test_gui_lines のメニューの並び 1 つ（上の「変えた期待値」）。
- 同じ最後のビルドで、描画・保存・Python との照らし合わせの 17 本も流した（612 項目。失敗 0、SKIP 0）: test_render_page・test_storage・test_model・test_command_bus・test_type_text・test_type_styles・test_ruby_render・test_pcraft、test_contract_psd_text・psd・type・type_fonts・export・pcraft・storage・render_cli・pipeline。

#### 残り・確かめていないもの
- Windows・MSVC・macOS のビルドと配布物、ASan、Debug。Windows のビルドの木では書体をコピーする（157 MB）。
- 実機の画面（高 DPI、タッチ）での見本の見え方。試験は offscreen。
- P3-5（上）。かなだけの 3 書体のかなの大きさ（合わせていない）。
- 前からの `sfx`・`hand` の縦のかっこの不具合と、横組みの最後の行の `_` が切れること（どちらも前から。触っていない）。
- 全体の試験は流していない（関係する試験だけ。決まりのとおり）。

### 利用者の決定（2026-11-14）: 括弧の四角と、新しい本のいつもの書体
- 利用者の言葉: 「両方おすすめどおりで」。
- **括弧の四角を直す**: これまでの書体のうち、効果音用（`sfx`、デラゴシック）は （）『』【】［］〈〉《》() を、手書き風（`hand`、よもぎ）は ［］ を、字の無い印の四角で描いていた（前からの不具合。書体の見本 fonts-150-plain-1.png の左上）。その書体に無い括弧だけをゴシックで描く。四角が出ていた既存のページは四角が括弧に変わる（利用者が受け入れた違い）。それ以外の字・それ以外の書体は画素も保存も変えない。
- **新しく作る本のいつもの書体を源暎アンチック（`antique_genei`）にする**: 替わるのは新しく作る本だけ。既存の本（`font_path` の無い本を含む）は今のまま（いつもの書体はアンチック）。書体のファイルが無い環境では、知らせを出してこれまでのアンチックで描く（F2 の決まりのまま）。
- 実装はフキダシの追加（群 1 の直しと群 2）の後に、同じ枝で行う。

### F2 2 回目のレビューの直し（ブランチ `native/fonts-fukidashi2`。フキダシの追加 群 1 の 279ed84f の上）— 2026-11-14

読むだけの 2 回目のレビュー（直しの回・群 2・群 3。P1 無し、P2 が 1、P3 が 18）を、リードの決定のとおりに直した。レビューが実行で確かめた「40 書体を使わない本は、2026-11-13 の配布物とバイトも画素も同じ」は保っている（`aBookOfBeforeIsWrittenToTheSameBytes`、Python との照らし合わせの試験、test_render_page）。フキダシのコードと試験には触っていない（`checks.cpp` は書体の所だけ）。

#### 直したこと（番号はレビューのもの。かっこは守る試験）
- **P2-1 いつもの書体がある本でアンチックを選ぶ**: 本に `font_path` があるあいだ、文字ツールの「書体」に「アンチック」（`antique`）の行を足す（`tool_settings.cpp` の `show_usual`・`show_face`）。パレットでアンチックを次の台詞に選ぶとこの行になり、置いた台詞は `style.font: "antique"`。パレットの最初の行は「アンチック」で、「この本のいつもの書体」は、いまのいつもの書体の行に出る（`FontPalette::set_usual`）。（test_gui_fonts `antiqueForTheNextLineInABookWithItsOwnUsualFace`・`theBooksUsualFaceIsSetAndShown`）
- **P3-1 doctor**: 40 個は stat だけ。SHA-256 まで読むのは本が名指しした書体だけ。名指しした書体の問題は `problems`（`ok: false`）、ほかは `installation`（`ok` は変えない。問題があるときだけの鍵）。書体を使わない本の doctor は 0.03 秒（レビューが測った 0.94 秒から）。（test_package `theInstalledProgramFindsEveryFace`）
- **P3-2 検査の穴**: 文字スタイルから来る書体（`core::type_text::resolve_chars`）、ページが見せるスマートオブジェクトの中身の文字レイヤー（入れ子も）、本のいつもの書体の相方。相方の警告は、その言葉に相方が描く字があるときだけ（その書体の cmap で見る）。中身・大きさが違うファイルは「ファイルが無いか、壊れているため」（`font_files::damaged`）。（test_font_faces `theMissingFaceIsSaidThroughStylesContentsAndTheUsualFace`・`everyLabelAndWarningReadsInJapanese`・`aFileOfTheSameSizeButOtherContentIsNotInstalled`）
- **P3-3 見本の先読み**: 別のスレッドは `bytes()` を呼び、見本を組むまで中身を持つ。`font_files` は、覚えから外した中身でも、まだ誰かが持っていれば読み直さずにそれを渡す。画面の倍率が変わったら先読みもやり直す。ドックをしまうと作るのを止める。（test_gui_fonts `aScreenOfAnotherScaleMakesTheSamplesAgain`・`theDockOrTheWindowClosedWhileAFileIsRead`、test_font_faces `bytesAreSharedAndBounded`）
- **P3-4 いつもの書体の表示**: 本が変わるたびに追いかける（`on_book_changed` → `show_usual_face`）。取り消し・やり直し・`set_meta` で変わる。（`theBooksUsualFaceIsSetAndShown`）
- **P3-5 ルビ**: `StoryPanel::set_chosen_ruby_font` は「ルビを選んでいない・同じ書体・変えられなかった・変えた」を返す。変えたときだけ「…にしました」。（`aChosenRubyTakesTheFace`・`aRefusedRubyPickIsNotSaidAsDone`）
- **P3-6 キーが見える所**: 字形パネルの書体の名前、PSD の書き出しの窓の注意書き。（`openingTheFontListAddsNoAddedFaceToQt`・`thePsdNoteNamesTheFaceNotItsKey`）
- **P3-7 名前を 1 つに**: 前からの 6 つは文字ツールの名前（ゴシック・明朝・丸ゴシック・手書き風・極太（効果音）・勢い（効果音））。パレットの行だけ「ゴシック（ZEN角ゴシック B）」「極太（効果音・デラゴシック）」と書体の名前を添える（`lettering::FaceRow::row`、`lettering::face_name`）。（`thePaletteListsTheFacesByCategory`）
- **P3-8 文**: 帯の 2 つの文（「この本が使っている書体…その文字は代わりの書体（台詞はアンチック）で描いています…」「この本が指定している書体…は、前の版には無かったためアンチックで描いていました。この版からはその書体で描くので、見た目が変わります」）、検査の相方の文（「書体「X」の漢字などを受け持つ書体「Y」のファイルが…無いため、その字はゴシックで描いた…」）、`lint`（効果音は「極太の書体で描かれる」）、`FONTS.json` の `use_ja` 3 つから「1 ファイル」を外した。「この原稿」は「この本」に。（`everyWordReadsInJapanese`、test_font_service `lintKnowsTheAddedFaces`）
- **P3-9 状態行で切れる文**: 書体の文は `MainWindow::say_of_fonts` を通る。状態行に収まればそこに、収まらなければ帯に全部出して、状態行は「書体の知らせを、ページの上に出しました」（それも収まらなければ何も言わない）。出した後で状態行が狭くなったときも帯へ移す（`refit_fonts_sentence`）。置き換えの長い文は帯（`tell_of_fonts`）。選んだときの文は短くした。（`noSentenceIsCutInTheStatusLine`: 窓を開いたままの幅 784・1300・760 で、47 書体 × 3 通り）
- **P3-10 帯のボタン**: 帯は `fontsBand`（枠）・`fontsBandText`（文）・`fontsBandClose`（ボタン「この知らせを閉じる」。キーボードで押せる）。（`whatIsSaidWhenABookIsOpened`）
- **P3-11 開いたときの中身の確かめ**: 本が名指しした書体は、開いたときに別のスレッドで `present`（中身まで）。大きさだけ同じ別物も帯に出る。（同上）
- **P3-12 読めなかったファイル**: 開けない・読み切れない・無いは、2 度試して、その 1 回だけ「入っていない」（覚えない）。中身が違うものだけ、プロセスの間ずっと。答えが変わるたびに `generation()` が進み、描いたものを描き直す。読めない間は検査が警告する。（test_font_faces `aFileThatCouldNotBeReadIsAskedForAgain`）
- **P3-13 構成で止める**: `native/cmake/GenkoFontList.cmake`（`genko_font_files`）。一覧の書体ファイルが無ければ、配布物の構成とビルドの木の手順が、ファイルの名前を言って止まる。（`aMissingFontFileStopsTheConfigure`）
- **P3-14 ビルドの木の写し**: 書体は大きさだけで比べ、一覧は中身が違うときだけ写す（時刻は見ない）。（`theBuildTreeStepLinksOrCopies`）
- **P3-15 PSD の相方**: ファイルが無いとき、漢字が無い書体の漢字は相方の名前で書く（`detail::asked_font`）。**漢字が一部の書体（`sfx_inline` の 1 つ）は直せない**: どの漢字を持つかは書体ファイルが無いと分からないので、全部の字がその書体の名前になる。書き出しの窓の注意書きがそう言う。（test_font_service `psdNamesTheFaceAskedForWithoutItsFile`、test_psd_text `aPartnersLettersAreNotWarned`）
- **P3-16 置き換えの答え**: `map` が追加のキーを名指ししたら、答えに `kept: [{font, label, why}]`。（test_font_service `replaceAllNeverStripsAnAddedFace`）
- **P3-17 試験**: `if` 付きの確かめを外した（HTTP の入れ子、字形パネル）。足りなかった場合を足した（上の各項目）。配布物の 2 項目を Linux だけのブロックから出した（中身に Linux だけのものは無い。**Windows では流していない**）。0 ミリ秒のタイマーの数え方に寄りかかる確かめは、画面の出来事を回す前に見るようにした。
- **P3-18**: SPEC の「1 画素も違わない」は、本にいつもの書体が無いときだけ、と書き直した。

#### 前の節から変わった所（上の群 1〜3 の節を読むときに）
- 「答えはプロセスの間ずっと同じ」は、中身が違うファイルだけ。無い・読めないは毎回見る（P3-12）。
- `genko doctor` は 40 個を全部は読まない（P3-1）。`verify(true)`・`missing()` を呼ぶのは試験だけになった。
- 開いたときの知らせの文、選んだときの文、検査の相方の文は、上の P3-8・P3-9 のもの。帯は `QLabel` でなく枠＋文＋ボタン。
- パレットの最初の行は「アンチック（いつもの書体）」でなく「アンチック」＋「この本のいつもの書体」。
- `set_chosen_ruby_font` は `bool` でなく `StoryPanel::RubyFace`。

#### 変えた期待値（緩めていない）
- test_font_faces: 相方の警告の文と、出る数（漢字が一部の書体は、持っている漢字だけの言葉では出ない）。`bytesAreSharedAndBounded`（持っている人がいれば、その中身が返る）。中身が違うファイルの警告の文。
- test_font_service: `lint` に効果音の 1 件。PSD の書体の並びに相方（`MochiyPopOne-Regular`）。
- test_psd_text `without.fonts`: 相方の `ZenOldMincho-Black` が入る（P3-15）。
- test_package: doctor の決まり（P3-1）。
- test_gui_fonts: 文・行の名前・帯の作り（P3-7〜P3-10）。本にいつもの書体があるとき、文字ツールの「書体」は 8 行。
- 前からの本で、いつもの書体が前からの 6 つのキーのとき、文字ツールの最初の行は「いつもの書体（明朝）」のように、その書体の名前になる（前は「（アンチック）」。レビューの P3-4 のとおり、前より正しい）。書体ファイルのパスのときは「いつもの書体（書体ファイル）」。

#### 確かめたこと（最後のビルド。Linux Release、2 本ずつ）
- 関係する試験 58 本が合格（2,424 項目。失敗 0、SKIP 0）: test_font_faces（508）・test_font_catalog（96）・test_font_service・test_psd_text・test_package・test_mcp_api_e2e・test_cli_e2e・test_gui_fonts（24 項目）と、画面の 20 本（lines・type_panels・type・ruby・fukidashi・output・actions・prefs・look・book・layout・workspace・studio・view・file・smart・tool_presets・styles・ledger_a〜c）、test_service・test_ledger_service_a〜c・test_contract_prefs・test_control_channel、描画と保存（test_render_page・test_storage・test_model・test_command_bus・test_type_text・test_type_styles・test_ruby・test_ruby_render・test_pcraft・test_smart・test_psd_smart・test_fukidashi・test_fukidashi_service）、照らし合わせ（test_contract_psd_text・psd・type・type_fonts・export・pcraft・storage・render_cli・pipeline・smart）。
- 配布物（導入したフォルダーを `tar -czf`）: tar.gz 162,123,887 バイト、展開して 333,850,689 バイト、書体のフォルダー 157,645,649 バイト、`bin/genko` 86,540,552 バイト（フキダシの追加 群 1 の分も入った値）。
- 画面の絵（作業用の `fonts-f2/gui/` に 22 枚。`GENKO_FONTS_GUI_OUT`）: 前の 18 枚を作り直し、窓を開いたままの幅での 2 通り（置き換えの長い文が帯に全部出ているもの、選んだときの文が帯に出ているもの）を明るい・暗いで足した。

#### 直していないもの・確かめていないもの
- 漢字が一部の書体の、ファイルが無いときの PSD の名前（P3-15 の残り。上）。
- Windows・MSVC・macOS（配布物の 2 項目、ビルドの木の写し、`QtConcurrent` の先読み、0 ミリ秒のタイマー）、ASan、Debug。全体の試験。
- 読めないファイルの試験（権限を外して開けなくする所）は Unix だけ。
- 状態行で切れるのは、書体の文だけ直した。アプリのほかの文は、窓を開いたままの幅（状態行 286 px）では今も切れる（前から）。

## 今風のフキダシ（fukidashi@1。SPEC BALLOON-01）— 2026-11-13、2026-11-14 に main に統合

### 利用者の決定（2026-11-13）
- 「吹き出しが少しいびつに見える」「いびつにせずに今風の漫画の吹き出しを各種作って置けるように」。続けて「手描き風は消して良いです」、すでにある本のフキダシは「全部を今風に描く」。
- だから: 手描き風の描き方（卵形の楕円・線の強弱・揺れ・ばらついた破線・角のあるしっぽ）を無くし、すべてのフキダシを 1 つの描き方（今風）で描く。**すでにある本のフキダシも開けば今風になる（見た目が変わる）。文字の大きさは変えない。**「すでにある本は同じ画素で描く」という決まりは、フキダシの画素についてだけ、この決定で外れた。フキダシ以外は今までどおり。
- 群: B1 が土台（core・render・service）、B2 が GUI、B3 が AI 向けの道具と手引き。B2・B3 は下の「B2・B3 が使うもの」を使う。

### 群 B1（土台。ブランチ native/fukidashi-core、main 5cf6551a から）
- 設計: SPEC.md BALLOON-01（描き方・種類・鍵・しっぽ・文字の収まり・前の本の読み替え・拡張・試験）、schema-v4.md「フキダシ（fukidashi@1 と balloon_geometry）」。
- 描画: `render/text/balloon_clean.{hpp,cpp}`（輪郭を実数の多角形で作る。＋−×÷と平方根だけ、正多角形の角と波の位相は libm_float の sinf・cosf。4 分の 1 か右半分を作って写す。塗りは形の和集合、線は「和集合の縁から太さ以内」を正確な面積のラスタライザーで出す）。`balloons.cpp` の shape_masks は全部これで描く。手描き風の機械（uneven・swell・角度で切る破線・収縮で出す線・整数の多角形・角のあるしっぽ）は消した。ページへは 1 回の混ぜ方で載せる（透明のあるレイヤーでも縁が暗くならない）。
- 種類: 今ある種類（speech・rounded・box・narration・cloud・thought・shout・electric・whisper・flash・dotted_box・tone_box・fancy_box・手で描いた輪郭）を今風に。新しい種類 `capsule`・`polygon`。新しい鍵 `corner_mm`・`squircle`・`sides`・`wave_mm`・`spike_alt`・`dash_mm`。しっぽの `kind: inward` と `fillet_mm`。しっぽは根元で輪郭の法線に沿って出て先へ曲がり、両側は中心線からのオフセット、根元の幅は短い辺の 0.22（付け根どうしをまっすぐ測った長さ）。付け根は、両側が輪郭と交わる所を小さな曲線（線の 1.5 倍か 0.5 mm、幅の 1/4 まで）でつなぐ。本体の輪郭はその 2 か所のほかは変わらない。つないだフキダシは切れ込みを同じ継ぎ目で、離れていれば首でつなぐ。
- 前の鍵: `hand` は描く形を変えない。今も決めるのは文字を収める楕円だけ（false は箱そのものの真の楕円、ほかは文字の箱のふくらんだ楕円）。`wobble` は波の輪郭（`wave_mm` = 0.03 × wobble × 箱の短い辺）として描く。どちらもエラーにしない。
- 文字: 楕円の種類の文字は「文字の箱」（core::letter_box。箱を真ん中を保って 1 / 1.035。小数 9 桁）の中の、前と同じ「文字の楕円」（箱の 0.955 の、2.6 乗の超楕円）に収める。箱に内接する真の楕円に入る。読み替えた前の本では文字の箱が前の箱とビットまで同じなので、文字はどの解像度でも前と同じ画素になる。capsule・polygon は形そのものに収める（shape_gauge・fit_in_shape）。検査（text_outside_trim・text_outside_frame・text_overlap）も文字の箱で測る（formats/checks.cpp）。
- 前の本: project.json の一番上の `balloon_geometry`（今は 2）。無い本（と 1 の本）は、読むときに 1 回だけ読み替える（storage/reader の read_line → core::migrate_legacy_line。文字レイヤーの記録も）。(1) 手描きの楕円だった台詞の箱を真ん中を保って 1.035 倍（小数 9 桁）。type@1 の箱の文字・ポイント文字、`hand: false`、`wobble`、手で描いた輪郭は大きくしない。(2) その台詞の消しゴムの跡（style.cuts）と文字の道（style.text_path）を、箱の角が動いたぶんずらす（ページの上で同じ場所）。しっぽ（ページの座標）と text_dx/dy（文字の箱から）はそのまま。(3) `border_mm: 0` は 0.01 mm（core::kHairlineMm。1 px の細い線のまま）。保存で鍵が付き、2 度は読み替えない。鍵は楕円の台詞か border_mm 0 の台詞がある本に書く。2.0 も読む。読めない値は broken_value（読み取り専用）で、読み替えない。前の版の Genko は知らない鍵として残す（拡張ではない）。
- 拡張 `fukidashi@1`: 新しい種類・鍵・しっぽを使っているあいだだけ書く（core::uses_fukidashi）。
- 自動の文字入れ: 楕円の箱は前の 1.035 倍。**種類を書いた台詞は変えない。** 種類が無く、1 列で 5 字以上の台詞だけ `capsule`（service::letter::shape_for(breaks, given)。ネームの文字入れの計画の行と propose_lines で balloon が無いとき）。
- 設計からの変更（利用者の決定で）: `style.draw`・本の `balloon_draw`・`authoring_draw`・op `restyle_balloons`・既定の書き込みは作らない（描き方が 1 つになったので要らない）。「前の本は同じ画素」の試験も置かない（意図して変わる）。

### B2・B3 が使うもの
- 種類の名前 `capsule`・`polygon`、鍵 `corner_mm`・`squircle`・`sides`・`wave_mm`・`spike_alt`・`dash_mm`、しっぽの `kind: "inward"`・`fillet_mm`。どれも今の `add_line`・`edit_line`・`move_line` で渡す（新しい op は無い）。範囲と断りの文は SPEC BALLOON-01、`genko schema`（edit_line の「style (fukidashi)」）。
- 寸法: `render::text::hug_power`・`hug_keep`（app/lettering.cpp の measure はこれを呼んでいるので、楕円はもう 1.035 倍になっている）、`service::letter::measure(breaks, "capsule")`、`service::letter::shape_for`。
- 見本の絵（パレットのサムネイル）: `render::text::draw_group` をそのまま使う。
- B2 に残ること: 種類のメニューに「縦長（カプセル）」「多角形（機械の声）」、パレット、台詞パネルの新しい鍵、`hand`（手描き風）と `wobble` をメニューから外す、commands.json。**app/lettering.cpp の measure（320〜350 行のあたり）には capsule と polygon の場合が無い**（今は箱の種類として測るので、カプセルに打った文字の自動の箱が合わない）。service::letter::measure(breaks, "capsule") と同じ式（幅は列＋ふち、高さは文字＋幅ぶんの半円 2 つ）を入れること。
- B3 に残ること: MCP の道具の説明と手引き（SKILL.md・drawing.md と integrations/ の写し）から手描き風の説明を外し、新しい種類・鍵を書く。ネーム計画（studio/schemas.cpp の panels[].lines[].balloon）に capsule・polygon を足すかどうか。

### 直した既存の試験（どれも意図した変更。緩めていない）
- test_storage v4RoundTrip: 楕円のある本は `balloon_geometry` を書く（鍵の並びに 1 つ足した）。
- test_gui_lines: 楕円（speech・thought）の箱の期待値を 1.035 倍に（aLineIsTypedWhereTheTextToolClicks、aLineIsTypedOverDeletedTurnedAndReshaped の 2 か所、theLinesPanelListsReordersAndEdits の 2 か所、theStoryEditorPoursAScriptAndEditsEveryLine、selectedCharactersAreStyledAsTyped の box_size・measure）。「既定の設定に戻す」が消す鍵は 46 → 52（鍵が 6 つ増えた）。
- test_ledger_service_c: main と同じに戻した（最初の版では、種類を書いた 1 列 5 字の台詞がカプセルになるので書き換えていた。種類を書いた台詞を変えなくなったので要らなくなった）。
- tests/support/psd_book_hashes.hpp: PSD の見本（画像の道 4 つ、文字を編集できる道 4 つ）の sha256 を取り直した（フキダシの画素が変わるので。しっぽを作り直したのでもう一度）。レイヤーを重ねると台詞になること・2 つの道の統合画像が同じことは、前の試験のまま通る。main の版の PSD と比べた結果は下の「PSD の見本を main の版と比べた」。
- test_save_e2e savedBookReadsBackWhole: v3 の見本の手描きの楕円（30 × 60 mm）は、v4 に変換した写しで 31.05 × 62.1 mm（真ん中は同じ）になり、`balloon_geometry: 2` が付く。
- test_psd_text imageWayIsAsBefore: 4 ページ分の sha256 をまとめて言うようにした（検査は同じ）。test_render_page: コメントだけ。
- docs/ops.schema.json: native の一覧と同じ文に（edit_line の balloon・tails・style の説明）。
- test_contract_opentype linesOfBeforeAsBefore（tests/data/type/plain_lines.json）: フキダシのある 14 ページの sha256 を記録し直した（フキダシの画素が変わるので）。フキダシの無い 3 つ（latin・sfx・text_only）は前のまま。文字は、17 ページを main 5cf6551a の実行ファイルと今の版で PSD（文字を編集できる道）に書いて文字レイヤーを比べた: 字の大きさは 17 個とも同じ。置き場所は 11 個が同じで、op で作った楕円の 6 個（leant・tcy・no_features・down_features・text_gamma・across_ruby_spans）は 150 dpi で 1 px ずれる（楕円の文字は文字の箱の真ん中に置く。文字の箱はフキダシの箱の 1 / 1.035 で、画素への丸めが箱とは別になる）。保存してあった本は読み替えで文字の箱が前の箱と同じになるので、ずれない。**この試験も最初の版（09099074）のときから落ちていた。**
- test_gui_output checksPanel: 点検の「台詞が仕上がり線の外」をクリックしたときに印が付く箱は、台詞の文字の箱（楕円はフキダシの箱の 1 / 1.035。指摘の小数 2 桁で −2.49, 40.34, 28.99 × 19.32）。前はフキダシの箱（−3, 40, 30 × 20）。指摘の件数・文・選ばれる台詞は同じ。
- test_layout_ideas submitNameSavesAsBeforeK1: demo4 のネームを道具で提出して保存した project.json の sha256 を取り直した（7c1284b4… → bf8114d5…）。計画の input_hash 4 つは前のまま。main 5cf6551a の実行ファイルと今の版で同じネームを MCP から提出して、2 冊の project.json を鍵ごとに比べた: 違いは 18 個の値だけ — 道具が写植した楕円 5 本の w_mm・h_mm（1.035 倍）と x_mm（その大きさの置き場所。3 本は別の空きへ移る）、しっぽの先 1 つ、`balloon_geometry: 2`。コマ・計画・id・ほかの台詞は同じ。**この試験は最初の版（09099074）のときから落ちていた**（関係する試験の一覧に入れていなかった）。今回、台詞を足す試験を全部流して見つけた。

### 確かめたこと（Linux Release。関係する試験だけ）
- 関係する試験 103 本: 最初の版の 59 本（test_render_page・test_gui_lines・test_psd_text・test_smart_stack・test_ruby_render・test_contract_type 3 本・test_ledger_service_a/b/c・test_service・test_storage・test_model・test_features・test_command_bus・test_ops_e2e・test_cli_e2e・test_mcp_api_e2e・test_gui_ledger_a・test_fukidashi・test_fukidashi_service と、台詞に触れる契約・GUI・e2e の試験）に、台詞を足す試験・点検を流す試験・台詞のある本を読む試験を全部足した（test_layout_ideas・test_contract_opentype・test_gui_output・test_gui_effects・test_contract_annotations/layer_comps/measure/pcraft/slices/smart/video・test_gui_ai/align/anim/color/paths/shapes/smart/smart_contents/transform/variables/workspace/prefs・test_contract_align/anim/color_model/image_ops/ink/layers/paths/transform/prefs/retouch・test_brushes_model・test_color_model・test_groups・test_page_geometry・test_page_size・test_pyre・test_spell・test_variables・test_adjust・test_ai_link・test_strokes・test_perf_render）。試験の関数 2411 件が合格、不合格 0、スキップ 33（前からのスキップと、見本の書き出し 1 件）。test_soak_app（3 時間の耐久。台詞の期待値は無い）、全体の試験、ASan は流していない（群の出口で）。
- 新しい試験: test_fukidashi 72 件（輪郭の鏡像・線の太さ・破線・しっぽが輪郭の法線から出る・先が法線の上なら曲がらない・本体はしっぽのほかは変わらない・継ぎ目・線より小さい泡・同じフキダシは同じ画素・角 0 は四角・タイル＝ページ・白黒の線の太さ・レイヤーを重ねるとページ・ベタに白抜き・トーン枠・前の鍵・拡張の出入り・前の本の読み替え（箱・消しゴムの跡・文字の道・細い線・世代の鍵の読み方）・文字の大きさと画素・検査の指摘・2 度は読み替えない・今の形の文字の収まり）、test_fukidashi_service 18 件（op の受け取りと日本語の断り・切れ込みのしっぽ・capsule と polygon の tail_hidden・楕円の寸法・capsule の選び方（種類を書いた台詞は変えない）・ネームの文字入れ）。前の本の見本は tests/data/fukidashi（old_book・old_book2 は main 5cf6551a が書いた project.json。old_letters.json・old_letter_layers.json・old_issues.json はその版での文字の em と大きさ・文字だけの絵の sha256・検査の指摘）。台詞 92 本（77 ＋ 15）すべてが 150・350・600・1200 dpi で同じ em・同じ大きさ、文字だけの絵は同じ sha256。
- 速さ（test_perf_render の ten_balloons_at_600dpi。表示だけ。ほかの試験と並べて流したときの数字）: B4・600 dpi（6071 × 8598 px）に 10 個。フキダシなし 354 ms、ありを最初から 1008 ms、もう一度 494 ms、形だけ 449 ms（1 個 44.9 ms）。
- 見本: 今風の種類としっぽ（150 dpi の全体 3 ページ、600 dpi の原寸の切り出し）、前の本 2 冊を main と今の版で描いた並び。作業用の場所（リポジトリの外）に置き、報告に道を書いた。

### 前の本を開いたとき・取り消し・ジャーナル（確かめたこと。コードは変えていない）
- GUI で前の本を開いても、**本は変更ありにならず、ファイルも書き換えない**。読み替えは読むときにメモリーの上で起きるだけで、セッション（app/session.hpp）は op を当てるまで「保存済み」のまま。画面のフキダシは今風で、文字は前と同じ。何もせずに閉じれば、ディスクの本は前のまま（次に開くとまた同じように読み替える。結果は毎回同じ）。
- 利用者が何か 1 つ編集して保存すると、その保存の取引で、読み替えた箱と `balloon_geometry: 2` が一緒に書かれる。保存の差分には、その編集のほかに楕円の箱と鍵の変更が入る。
- 取り消し・やり直し: メモリーの上の取り消しは、読み替えた後の文書どうしを行き来するので、境目をまたがない。ジャーナル（.genko/journal）から前の状態（鍵が無い状態）を戻したときは、それをまた読むときに同じ読み替えがかかる（毎回同じ結果）。鍵のある状態を戻せば読み替えない。どちらでも箱が 2 度大きくなることは無い。
- 落ちた後の復旧: 保存していない op は、読み直した（読み替えた）文書の上に当て直される。op が書いている箱の数字はそのまま使う（op は読み替えない）。前の版で書かれて当て直されていない op（楕円を足す add_line で w・h を書いたもの）が残っていた場合だけ、その楕円は 1.035 倍にならず、文字が 1 段小さく収まることがある（下の「op の列を今の版でやり直したとき」と同じ）。
- 版をまたぐ: 鍵の付いた本を前の版で開いて保存しても、鍵は知らない鍵として残る。

### PSD の見本を main の版と比べた（main 5cf6551a の実行ファイルと今の版で同じ本を書いて、PSD を開いて比べた）
- 見本: tests/data/psd_text_book.json（4 ページ、フキダシのある台詞 12 本）。150 dpi、画像の道と文字を編集できる道。2 通りで比べた: (A) main の版が保存した本を今の版で開いて書き出す、(B) 同じ op を今の版で当て直して書き出す（試験の sha256 はこちら）。
- どちらも結果は同じ: 8 ファイルとも、レイヤーの並び（深さ・種類・名前）とキャンバスの大きさが同じ。フキダシ以外のレイヤー（紙・コマ・絵・文字レイヤー）の画素は全部同じ。文字レイヤーの本文・行列（置き場所と向き）・字の大きさ・フォント・ランの長さは全部同じ（違い 0 件）。
- 違うのはフキダシの画素を持つレイヤーだけ: 画像の道は台詞のレイヤー 12 本（p1 が 5、p2 が 3、p3 が 3、p4 が 1）、編集できる道は「フキダシ」レイヤー 12 本。画像の道の台詞レイヤーで違う画素は 1 本につき 4886〜9517 px で、そのうち文字の上にあるものは 0 px（フキダシの輪郭だけが変わっている）。
- 箱を数字で書いた台詞で、main より文字が小さく組まれたもの: 0 本（(B) でも）。
- この見本にはしっぽが無いので、しっぽの作り直しでは sha256 は変わらなかった（最初の版で取り直した 8 つのまま）。

### 残り・気を付けること
- 同じ build フォルダーで main の版を作って比べた（2 つ目の build フォルダーは作らない決まり）。main の実行ファイルは作業用の場所に置いただけで、リポジトリには無い。
- 版をまたぐ動き: `balloon_geometry` の付いた本を前の版で開いて楕円を足して保存すると、その楕円は読み替えられない（文字が少し小さく収まることがある）。前の版は公開前の v0.1.0 の下書きだけ。
- op の列を今の版でやり直したとき（ジャーナルの再生、試験の見本の op）は、箱は書かれた数字のまま真の楕円になる。文字の箱はその 1 / 1.035 なので、前の版で同じ op を当てたときより文字が 1 段小さく収まることがある（保存した本を開く道は読み替えるので変わらない）。
- 回したフキダシの紙の大きさ（turned_sheet の py_hypot）は前のまま（環境の libm）。しっぽの先の回し方は libm_float。今風の形の計算には環境の libm は入っていない。
- 鋭い切れ込み（本体に沿って寝たしっぽ）は、継ぎ目が小さいので小さなくびれとして残る。`fillet_mm` で大きくできる。
- ギザギザのしっぽの先の小さな目は、群 B3 で直した（下の「群 B2・B3」）。ごく短いギザギザのしっぽ（1 段が線 2 本より短い）は、歯がつぶれて黒い塊に見える（16 段のまま。前から）。
- Windows の実機のビルドは未確認（near・far の名前、GCC の拡張、__int128 は使っていない）。

### 群 B2・B3（GUI と、AI 向けの道具・手引き。同じブランチ native/fukidashi-core、d48c3f60 の上）— 2026-11-13
#### 作ったもの（B2。GUI）
- 種類: `lettering::kinds()` に「縦長（カプセル）」`capsule`（楕円の次）と「多角形（機械の声）」`polygon`（四角の次）。ページ → 台詞 → フキダシの形、フキダシの右クリック、台詞パネル、文字ツールの設定、ストーリーエディターの 5 つに出る。15 → 17 種類。
- 手描き風の設定を外した: 台詞パネルの「線の揺れ」（`wobble`）。`hand` の設定は画面には元から無い。残したもの（フキダシの手描き風ではないので）: 「フキダシを手で描く」（自分で輪郭を描く）、「手で描いたフキダシを曲線にする」、書体の「手書き風」、コマの枠線の「手描き風」、ブラシ。`hand`・`wobble` を持つ本はそのまま開き、ほかの設定を変えても鍵は残る（パネルはもう `wobble` を書かない）。前の `wobble` の波は「線のふるえ」に mm で出て、そこを変えると `wave_mm` に書いて `wobble` を外す（楕円の台詞は `hand: false` を足し、文字を収める楕円を変えない）。「既定の設定に戻す」は前の鍵も消す（前から）。
- 台詞パネルの新しい設定（app/story_panel。選択ツールのオプションに出る）: 角の丸み、角をふっくら丸くする、角の数、線のふるえ、トゲの長短、破線の長さ、しっぽの形（くさび・まっすぐ・ギザギザ・消える・泡・内向き）、しっぽの付け根の丸み。効く種類のときだけ行を出す（描画の body_of・no_tail と同じ分け方）。どれも `edit_line` 1 つ。フキダシの右クリックの「しっぽの形」にも「まっすぐ」「内向き（切れ込み）」を足した。内向きにすると先をフキダシの中へ（真ん中から先への線の上、縁までの 0.55）、外向きに戻すと外へ（1.6）移す。
- ドック「フキダシ」（app/balloon_palette、main_window_fukidashi）: プリセット 32 個（下）。絵は draw_group が描く（40 × 34 mm の紙を 56 dpi × 画面の倍率で。線なしだけ灰色の紙）。初めて見せたときに 4 枚ずつ描き、プロセスの中で使い回す。キー（矢印・Enter・Space）、ツールチップ、右クリック（選んだ台詞に当てる／次に置く台詞に使う）。ウィンドウ → フキダシ、ページ → 台詞 → フキダシのパレットを開く（`act_balloon_palette`）。台詞のタブの隣、初めは閉じている。写植の並び（作業の段階）に入れた。配置は saveState に入る（objectName「フキダシ」）。
- 選んだときの動き: このページの台詞を選んでいて文字ツールを持っていなければ、その台詞に当てる（`edit_line` 1 つ）。そうでなければ次に置く台詞の形（文字ツールに持ち替え、type_new_line・balloon_drawn が読む。別のプリセットか文字ツールの「フキダシ」を選ぶまで続く）。台詞パネルの「コマに追加」とストーリーエディターは、自分の種類の欄があるので、パレットの形は使わない。
- 寸法: app/lettering の measure は capsule・polygon を service::letter::measure で測る（式は 1 つ）。polygon の寸法は service に足した（polygon_room）。polygon の角の数は place_at・refit がスタイルから読む。
- プリセット（鍵 / 名前）: oval 普通、oval_tail しっぽつき、capsule 縦長、capsule_wide 横長、rounded 角丸、squircle ふっくら角丸、box 四角、narration ナレーション、cloud もくもく、thought_cloud 心の声（雲）、thought 心の声（泡）、shout 叫び、shout_alt 叫び（長短）、shout_sharp 叫び（鋭い）、electric 電子音、flash フラッシュ、whisper ささやき、long_dash 長い破線、wave ふるえ声、double 二重線、thick 太い線、no_line 線なし、polygon8 八角形、polygon6 六角形、black_oval ベタ白抜き、black_shout ベタの叫び、black_flash ベタフラ、dotted_box 点線の囲み、tone_box トーンの囲み、fancy_box 飾り枠、inward_tail 内向きしっぽ、two_tails しっぽ 2 本。
- 作らなかったもの: つないだ 2 つ（`group`）のプリセット。つなぎは台詞 2 本の鍵なので、1 本を置く・1 本に当てるプリセットでは作れない（右クリック「次の台詞のフキダシとつなげる」は前のまま）。
- commands.json: 4 つ足した（ウィンドウ → フキダシ、フキダシのパレットを開く、フキダシの形の 2 つ）。消した命令は無い。保存してあるショートカットは、ウィンドウの命令ごとに `shortcuts/<名前>` を引くだけなので、知らない名前の設定が残っていても読まれず、何も起きない（preferences.cpp）。

#### 作ったもの（B3。AI 向け）
- MCP の道具の説明（api/mcp_tools.cpp）: `apply_ops` に、フキダシの種類（capsule・polygon を含む）・形の鍵と範囲・しっぽの kind と fillet_mm を書いた。`propose_lines` に、balloon を省いたときの決まり。手描き風は「無い」とだけ書いた。`inspect` の説明は変えていない（2026-10-15 の決定）。道具の inputSchema は変えていない（test_mcp_api_e2e の sha256 は前のまま）。
- op の一覧（core/data/ops_schema.json と docs/ops.schema.json）: `corner_mm` の 0 は四角、`add_line` の balloon・tails・style は edit_line と同じ、`wobble` を style の並びから外し、`hand`・`wobble` は「前の鍵。受け取るが新しい台詞には使わない」と書いた。
- 手引き: SKILL.md に「フキダシの形を選ぶ」の表（会話・1 列の台詞・説明・心の声・叫び・ささやき・機械の声・ナレーション・ふるえ・コマの外の声・ベタ）、drawing.md に種類・形の鍵・しっぽの説明（`wobble`（手描き風の揺れ）を外した）、manga_rules.md の balloon の並び。integrations/hermes と integrations/claude-code の写しは同じ内容。
- ネーム計画: `panels[].lines[].balloon` の種類に `capsule`・`polygon` を足した（studio/schemas.cpp）。計画の input_hash は計画の中身のハッシュなので、今ある見本のハッシュは変わらない。
- docs/AGENT.md の種類・しっぽ・鍵の説明。OTHER_AGENTS.md・AI_PIPELINE.md には直す所が無かった。CLI のヘルプに種類の並びは無い（`genko schema` は ops_schema.json）。
- ギザギザのしっぽの先の「目」（B1 の残り）: balloon_clean.cpp の zigzag で、くびれの白が線の太さより細くなる所から先は外へ振らずに細らせる（TailSpec.line）。16 段のままなので、ごく短いしっぽ（1 段が線 2 本より短い）は前と同じく歯がつぶれる。

#### 直した既存の試験（どれも意図した変更。緩めていない）
- test_gui_lines commandsAsPythonsWindowHasThem: ページ → 台詞 の並びに「フキダシのパレットを開く」が入る（11 → 12 項目）。フキダシの形のメニューと文字ツールの設定の種類は 15 → 17。
- test_gui_lines theLinesPanelListsReordersAndEdits: 台詞パネルの設定を変えたときの edit_line の style から `"wobble": null` が無くなった（線の揺れの欄を外したので、パネルは `wobble` を書かない）。
- test_gui_actions theMenusHoldPythonsCommands: 名前つきの命令 821 → 822（`act_balloon_palette` の 1 つ）。
- core/data/commands.json: 4 つ足した（test_gui_prefs commandsJsonIsTheMenus が作り直したもの）。
- docs/ops.schema.json: native の一覧と同じ文に（add_line・edit_line の説明）。
- 期待値を変えていないもの: test_mcp_api_e2e の道具の inputSchema の sha256、PSD・opentype の画素の sha256（見本にギザギザのしっぽが無い）、ネームの見本の input_hash と test_layout_ideas の project.json の sha256。

#### 確かめたこと（Linux Release。関係する試験だけ）
- 152 本の実行ファイル（test_gui_* 全部・*_e2e 全部・service・ledger_service_a/b/c・fukidashi・fukidashi_service・layout_ideas・studio_files・studio_ops・mcp_connection・storage・model・strokes・command_bus・ops・features・render_page・psd_text・smart_stack・smart_render・ruby・ruby_render・type_text・contract の type 3 本・opentype・psd_text・psd・storage・render_cli・pipeline・export・export_cli・annotations・layer_comps・measure・pcraft・slices・smart・video・align・anim・color_model・image_ops・ink・layers・paths・transform・prefs・retouch・guides・copy・open・variables・artboards・page_size・brushes_model・color_model・groups・page_geometry・page_size・pyre・spell・variables・adjust・ai_link・perf_render・package・control_channel・session・migration・tiles・web_export・pcraft・prefs_table・json・buildinfo）: 試験の関数 2986 件が合格、不合格 0、スキップ 25（Release では動かない障害注入 22 件、見本と画面の絵の書き出し 2 件、PSD の速さの見本 1 件）。3 本を並べて流した。全体の試験・ASan・test_soak_app は流していない。
- 新しい試験: test_gui_fukidashi 9 件（メニュー 5 つ、手描き風の設定が無い・前の鍵の台詞の編集、形の設定の鍵と取り消し、パレットと絵、選んだ台詞に当てる、次に置く台詞、capsule・polygon の寸法、画面の絵の書き出し）、test_fukidashi aZigzagTailClosesWithAPoint（直す前は落ちることを確かめた）。
- パレットの絵 32 枚を描き直す時間（表示だけ。書体を読んだ後、1.5 倍の画面で）: 49 ms。配置（saveState・restoreState）で開閉が戻ることも試験で見る。
- 画面の絵（作業用の場所。GENKO_FUKIDASHI_OUT にフォルダを指定して test_gui_fukidashi を走らせると、最後に書く）: パレット（明・暗）、台詞パネルの設定（明・暗）。日本語は字で出ていて、切れている所は無い。

#### 残り・気を付けること（B2・B3）
- Windows の実機のビルドは未確認。
- 「次に置く台詞の形」は、文字ツールの「フキダシ」を選び直すか別のプリセットを選ぶまで続く。画面の印は、パレットの選ばれた項目（太字）と文字ツールの設定の種類だけ。
- 選んだ台詞に capsule のプリセットを当てても箱は変えない（言葉・場所・大きさを保つ決まり）。横に広い箱では、両端の丸い横長になる。
- 手で描いた輪郭（path）の台詞に当てると、形は変わらず線と塗りだけ変わる（そう知らせる）。type@1 のポイント文字・段落文字には当てない（知らせる）。
- パレットの絵は 56 dpi（× 画面の倍率）なので、飾り枠の角飾りやトーンの点は実際より粗く見える。
- polygon の寸法は角を丸める前の多角形で測る。奇数の角（5・7・9・11）は 2% の余裕を足している（試験は 5〜12 角で文字が 5 mm のまま収まることを見る）。
- 内向きのしっぽにしたときの先の置き場所（縁までの 0.55）は、箱の楕円で測る（四角や多角形でも中に入る）。

#### レビュー（d48c3f60..14a425c5 の読むだけのレビュー。P1 なし・P2 4 件・P3 10 件）と直したこと
- P2-1 切れ込み（inward）がフキダシに置いていかれる: core の `move_line` が、しっぽを言わずに箱を動かす・大きさを変えるとき、inward のしっぽだけ箱と一緒に動かす（動かしただけなら同じだけ、大きさを変えたら箱の中の同じ割合へ。`to`・`via`・`vias`）。ほかのしっぽは前のまま（ページの上で指す所。隠れた先は外へ）。同じ op をもう一度当てても同じ結果（op は文書だけから決まる）。画面: 内向きの先（●）はドラッグしても中に留まる（縁までの 0.9）。「しっぽをまっすぐにする」はしっぽの種類を保つ（内向きが見えないくさびにならない。ギザギザ・消えるも保つ）。
- P2-2 つないだフキダシ（`group`）にパレットの形: つないだ台詞の全部に種類と見た目の鍵を当てる（1 回の apply_ops＝取り消し 1 回）。しっぽは選んだ台詞だけ。文字の色の決まりは 1 本ずつ。
- P2-3 前の `wobble` を外すと文字が小さくなる: パレット（preset_style_change）と線のふるえ（set_wave）の両方で、`wobble` が効いていて、結果が楕円で、`hand` が false でなければ `hand: false` を書く（`hand: true` のときも）。
- P2-4 カプセルは箱で形が決まる: AI 向けの文（apply_ops・propose_lines の説明、SKILL.md の表とネームの項、drawing.md、integrations の写し、docs/AGENT.md）を直した（箱の長い辺が長い辺、大きさを省いた add_line は 40×20 mm、edit_line で変えたら move_line で箱を合わせる、目安の寸法）。画面: 種類のメニュー（ページ → 台詞 → フキダシの形、右クリック）とパレットで capsule・polygon へ／から変えると、同じ取り消しの中で箱を言葉に合わせて作り直す（真ん中を保つ。lettering::refit_for_kind）。ほかの種類どうしは箱を変えない。台詞パネルの「台詞を直す」とストーリーエディターは前から作り直している（右上を保つ）。core の edit_line はそのまま。
- P3-1 白い文字: ベタの白い文字は、文字の色が無いか黒い台詞にだけ。白い文字を戻すのは前の塗りが黒かったときだけ（白抜き文字は触らない）。
- P3-2 効かない行: 点線の囲みに角の丸み・線のふるえを出さない、飾り枠に角の丸みを出さない、心の声にしっぽの形を出さない。
- P3-3 触っていない欄が書く: 新しい欄は、出している値と同じ（表示の刻みの半分以内）なら op を出さない（story_panel の left_as_shown）。しっぽの付け根の丸みは、1 本目の値が同じなら出さない（しっぽごとの違いをそろえない）。前からの「しっぽの幅」は、同じなら move_line（しっぽ）を出さない。ただし前からの欄がどれもそうであるように、欄を離れるとスタイルの edit_line は出る（test_gui_lines が見ている前からの動き。変えていない）。
- P3-4 何も変わらない選択: op も取り消しの 1 回も作らず、「もう「◯◯」です」と知らせる。
- P3-5 内向きと外向きが混じったしっぽ: しっぽのプリセットでは、向きの違うしっぽが 1 本でもあればプリセットのしっぽに置き換える。
- P3-6 文の正しさ: propose_lines（balloon を省いて形が言葉で決まるのは、x_mm・y_mm だけを出したとき。box01 は speech のまま）、docs/AGENT.md（add_line の既定は speech）、入らないときの目安（service::letter::fits）を capsule・polygon の寸法で数える。
- P3-7 試験: 下の「試験」。
- P3-8 画面の倍率: QEvent::DevicePixelRatioChange で絵を描き直す。
- P3-9 次に置く形が見えない: 文字ツールのオプションに「パレットの形: ◯◯」と「外す」。文字ツールの「フキダシ」を選び直しても外れる（前から）。
- P3-10 文言: パレットの「普通」、種類の「ささやき（破線）」「フラッシュ（放射状の線）」（前の名前で付けたショートカットは preferences::renamed で引き継ぐ。commands.json の old_names）、「元に戻す で取り消せます」、ドックの説明。

#### レビューの直しで変えた既存の試験の期待値（どれも意図した変更。緩めていない）
- test_fukidashi_service aNotchStaysInsideItsBalloon → aNotchGoesWithItsBalloon: 箱を 40×30 → 60×50 にしたとき、切れ込みは「元の場所のまま」ではなく箱と一緒に動く（[58, 50] → [67, 56.67]）。P2-1 の決まり。ほかのしっぽ・検査の指摘は前のまま。
- test_gui_lines aBalloonsOwnMenu（しっぽをまっすぐにする）: ギザギザのしっぽをまっすぐにしても種類が残る（`{"to": …}` → `{"to": …, "kind": "zigzag"}`）。P2-1。
- core/data/commands.json: 種類 2 つの名前（id も名前）が変わり、old_names に前の名前が入る。
- docs/ops.schema.json: native の一覧と同じ文（move_line の tails）。
- test_gui_fukidashi（群 B2 の試験）: 種類を capsule・polygon へ変える op が 2 つ（edit_line と move_line）になる、ベタは利用者の赤い文字を白にしない、など上の決まりに合わせた。
- 変えていないもの: 画素・PSD・opentype・project.json の sha256、道具の inputSchema の sha256、ネームの見本の input_hash。

#### レビューの直しの後に確かめたこと（Linux Release。関係する試験だけ）
- 前と同じ 152 本の実行ファイル（上の一覧）を、全部を作り直してから 3 本並べて流した: 試験の関数 2989 件が合格、不合格 0、スキップ 25（内訳は前と同じ）。sha256 を取り直した試験は無い。全体の試験・ASan・Debug・test_soak_app は流していない。
- 試験: test_gui_fukidashi は 12 件（つないだフキダシに当てる aPickOnAJoinedBalloonTakesEveryMember、切れ込みとフキダシ aNotchGoesWithItsBalloon を足した。手描き風の試験は、wobble を外すと文字の組み方が変わる箱を探して使い、`hand: false` が入ること・組み方が変わらないことを線のふるえとパレットの両方で見る。寸法の試験は手で計算した数と比べる。パレットの項目の名前とツールチップも「無くなった言葉」の検査に入れた。右クリックのメニューは item_menu から試す）。test_fukidashi_service は 19 件（aNotchGoesWithItsBalloon: 動かす・大きさを変える・同じ op をもう一度・しっぽを言った move_line。aPlanSaysCapsuleAndPolygon: ネーム計画の capsule・polygon が写植まで届く・目安の字数）。
- 画面の絵（作業用の場所）: gui-palette.png・gui-panel.png・gui-window.png（パレットを台詞の隣に開き、次に置く形を選んだ所。文字ツールのオプションに「パレットの形: 二重線」と「外す」）・gui-armed.png と、それぞれの -dark。日本語は字で出ていて、切れている所は無い（右のタブの見出しは、タブが多いと「フキ…」と省略される。前からのタブの動き）。

#### 残り・気を付けること（レビューの後）
- フキダシをドラッグしている途中の絵では、切れ込みは離すまで元の場所に見える（離したときに動く）。確かめたのは離した後の op と結果だけ。
- `move_line` 以外で台詞を動かす op（整列・変形・用紙の変更など）で切れ込みがどう動くかは見ていない。
- つないだフキダシで capsule・polygon へ／から変えると、1 本ずつ真ん中を保って箱を作り直す。箱が小さくなって離れることがある（短い半径の 1.2 倍までは首でつながる）。
- 「しっぽをまっすぐにする」が種類を保つのは、全部の種類（前はくさびに戻していた）。
- 種類 2 つの名前を変えたので、`genko commands` の id も変わる（前の名前は old_names）。

### 今風のフキダシの出口 — 凍結 a72bd1e8 で Linux の ASan・Release が合格（2026-11-14）
- **凍結SHA**: `a72bd1e8`（native/fukidashi-core。B1〜B3 とレビューの直し b6cb225d に、出口で見つけた試験の直し 6b2f5411 と、試験の「続きから」a72bd1e8 を足した版）。計画は `validation_policy.py --base 5cf6551a --head a72bd1e8 --phase milestone`（tier full。理由: CI 自身の変更〔tools/ci〕、ビルド・登録の共通境界、共有の描画経路、共通モデル・保存、工程出口、性能試験の変更〔test_perf_render に表示だけの測定〕、未分類の native 変更）。
- 実行環境（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Xeon 2.3GHz）/ RAM 16GB（プロセスは 14 GB の memory cgroup の中）/ Ubuntu 24.04.5 / kernel 6.18.44 / cgroup v1。root から CAP_DAC_OVERRIDE・DAC_READ_SEARCH・FOWNER を外して実行。
- 結果:
  - linux-asan: 267/267 合格（2 並列、3 時間 1 分。QtTest の項目 5,502、SKIP 0、CLI23 合格）。1 回で走り切った（`--resume` を付けて呼んだが、引き継ぎは起きていない）。
  - linux-release: 267/267 合格（2 並列、18 分。項目 5,428、既知の SKIP 32 はどれも ASan の同じ項目の PASS と対になる。CLI23 合格）。
  - 実 xcb の GUI（Release、Xvfb 1920x1080）: 65 本すべて合格（項目 1,198。SKIP は既知の TestGuiSave::initTestCase の 1 つ）。
  - 性能（静かな機械で）: test_perf_app 2 回（開くまで 501 ms・432 ms、入力から画面まで p95 1.68 ms・1.31 ms、p99 3.20 ms・2.55 ms）、test_perf_render 合格（フキダシ 10 個の B4 600 dpi: フキダシ無しのページ 316 ms、一から 680 ms、描き直し 401 ms。表示だけで合否は付けていない）、test_perf_pipeline 合格（PERF-F 8 ページの校正刷り p95 5120.8 ms）。
  - `--pair`: 「構成artifactの欠落・重複」で不合格（Debug と Windows 2 構成が無い）。**3 構成の --pair 合格ではない。**
  - **流していないもの**: linux-debug（最終配布の前に 1 回。利用者の決定 2026-11-13）、配布物（この工程は配布物の中身と作り方を変えていない。次の書体の工程で配布物が変わるので、そこで作って確かめる）、WebAssembly のホスト（変えていない）、Windows（deferred）。
- 合格までの回（どれも合格に数えていない）:
  - 凍結 b6cb225d で 3 回、コンテナの再起動で止まった（ASan のビルド中、試験の 166 本目、192 本目）。背景の見張りが 2 時間の上限で切れた後に 1 回、基盤の入れ替え（kernel の版が変わった）で 1 回。
  - ASan の試験を 4 並列にした回: test_cli_e2e が落ちた。原因は memory cgroup の上限（14 GB）で、試験が起こした genko の子プロセスを OOM killer が止めた（dmesg の記録あり。プロセスを起こす e2e の試験が同時に走り、ASan の genko が 14 個、各 1 GB で並んだ）。製品の不具合ではない。**ASan の試験は 2 並列まで**（4 並列は使わない）。
  - 2 並列に戻した回は lead が止めた: test_fukidashi の sampleSheets と test_gui_fukidashi の screenshots（見本の絵の書き出し）が、GENKO_FUKIDASHI_OUT が無いと QSKIP していた。監査は ASan の SKIP を一つも認めないので、走り切っても不合格になる。4 並列の回のログを「続きから」の読み方（試験ごとの結末）で読んで分かった。絵の書き出しを試験の項目から外し、フォルダを指定した回の cleanupTestCase で書く（6b2f5411。test_gui_color の画面の絵と同じやり方。監査は変えていない）。ASan で 2 本を、指定なし（72・11 項目、SKIP 0）と指定あり（絵 40 枚が書かれる）で確かめた。
- 次から気を付けること: 開発中の関係する試験（V1）は ctest の合否しか見ていなかったので、足した QSKIP に気付かなかった。試験に QSKIP を足したら、出口の前に関係する試験のログで `SKIP` を見る。
- test_fx_lens は ASan で 1 本 51〜75 分かかっていた（今回 3,054 秒。ASan の所要時間の 3 割前後）。**直した（2a998875、利用者の決定 2026-11-14）**: 遅いのはフィルターではなく試験の補助だった。`pixel()` が呼ぶたびに ColorRasterView を作り、view は作るときに float の画像の全サンプルを検査するので、画像を回るループが画素の数だけ全体の検査を繰り返していた（theSameLookAtEveryResolution が Release で 79 秒のうち 73 秒）。ループの外で view を 1 つ作って読む。読む値・比べ方・許容値は同じ。Release で 79 秒 → 3.5 秒。実行ファイルは分けていない。RUN_SERIAL と制限時間はそのまま。ASan での時間は次の出口で測る。ほかのフィルター系の重い項目（test_fx everyKindAsASmartFilter、test_smart_filters likeRasterizeAndFilterRaster、test_precision_merge_controls a4HighPrecision600dpi など）は実際に描いている時間で、同じ無駄ではない。

### 試験の「続きから」（validation_run.py --resume）— 利用者の決定 2026-11-14
- 利用者の決定: 「「続きから」を入れてください」「他の試験でも同様のようなテストがあれば変更してください」。機械の再起動と環境が原因の失敗で、ASan の全体の試験を最初からやり直すことが 1 日に 3 回続いた（合わせて約 5 時間）。
- 何をするか（正本は VALIDATION.md §3・§4）: 同じ `--output` のまま `--resume` を付けると、前回の `result.json` と計画・ソース・実行物・構成が全部同じときだけ、合格済みの試験を引き継いで残りと落ちた試験だけを走らせる。違えば最初から走らせ、理由を `resume.reason` に残す。各回のログは残し（`ctest-output.log`、`ctest-output.attempt-N.log`）、`--pair` は全部を読み直す。前の回で落ちて後で合格した試験は `audit.rerun_after_failure` と `--pair` の `resumed` に出る。**その原因（機械のメモリ不足など、コードの外のもの）をこの引継ぎに書けないときは、合格に数えない。** コードを直せばソースが変わるので全部走らせ直しになる（R4 のまま）。
- `--resume` を付けない実行の監査、合否の基準、SKIP の扱い、試験の中身は変えていない。足したのは、走らせた機械（OS・CPU 数・メモリ）と回のログ名の記録、記録を横に書いてから置き換えること、出力先を一つの実行だけが使う印（`.running`。ctest と試験にも渡すので、道具だけが落ちて ctest が残っているあいだは次の回が始まらない）、実行物の指紋にビルドの中の共有ライブラリを入れること（sanitizer の構成では製品のコードが libgenko_*.so にある）。
- レビュー（読取専用 1 回）と直し:
  - P1: 最初から走らせ直す回が、記録を書いてからログを開くまでの間に落ちると、前のソースのログが残り、次の回が今の合格として引き継ぐ。→ 引き継がないログは、記録を書く前に消す。
  - P1: `\r` の後ろの診断と、試験の番号の付かない診断の行を、試験ごとの読み方が見落とす（一度に走り切る監査は見つける）。→ 行は `\n` だけで切り、どの試験のものか分からない診断がある回は不合格にする（落ちた試験の出力の写しだけは認める）。最後の回には、始めた試験を全部合格で走り切ったという CTest の行を求める。
  - P2・P3: 落ちる前に自分の失敗を出していた試験を「落ちた」として残す、合格の記録を書き換えない、判定の前に落ちた回が合格の記録を残さない、ログの指紋とビルドの場所を確かめる、記録の型を確かめる、CLI23 のログを回ごとに残す、`--pair` が記録にないログを不合格にする、ほか。中身のない試験 3 本を直し、届いていなかった分岐の試験を足した。
- 確かめたこと: tools/ci/tests 137 本（test_validation_run 57 → 93 本。ctest と cmake の呼び出しだけを差し替え、途中で止まった回を通しで再現する）。過去の出口（k-v3・pc6-v3）の実ログで、新旧の読み方の結果が同じ。本物の ctest（ASan のビルド、小さな 4 本）: 3 本合格の後に道具だけを止める → ctest が残っているあいだは断る → 消えた後は 3 本を引き継いで 1 本だけ走り合格 → `--pair` 合格 → 合格の記録に付けても何も走らない。
- 似た流れ（出口の手順。作業用のスクリプトで、リポジトリには無い）: 性能の関門は段（描画・アプリ 2 回・書き出しの流れ）ごとに印を残し、済んだ段は測り直さない（再起動の後は暖機だけやり直す）。実 xcb の GUI は試験ごとに印を残す。ビルドと構成ごとの合否は前から続きから進む。**出口の手順で、出力先を `rm -rf` してから validation_run.py を呼ぶ行（これまでの run.sh にあった）は写さない。**
- 残り: Windows 構成は続きから走らせない（QtTest のファイル記録を回ごとに残していない）。CI（native.yml）は機械ごと作り直すので付けない。ビルドの置き場は 1 つしか入らない（Release 約 14.6 GB、ASan 約 11.7 GB）ので、ASan の後に Release を作り直す時間（約 40 分）は残る。分類の粗さ（未分類の変更を全体へ広げる）と重い試験の分割は、別に利用者に相談する。

## コマ割り K（komawari@1。SPEC LAYOUT-01）— 2026-11-12

### 利用者の決定（2026-11-12）
- 生成 AI がコマ割りをすると無難な格子しか出ない。これを直すため、次の 4 つを全部入れる。
  - **A 書ける形を広げる**: ネーム計画（submit_name の tiers・template）で、コマの中のコマ（差し込み・重ね）、はみ出し（枠で切らない）、枠なし、コマごとの間の広さ、自由な割り線を書けるようにする。
  - **B 手引き**: 手引きを、定型の名前でなく緩急で教える形に書き直す（別の群）。
  - **C 単調さの検査**: 提出したネームに単調さの警告を返す（止めない）。
  - **コマ割りの案の束**: 利用者が参考に挙げたコマ割りジェネレーター（tools.oyasumi-gamedev.com/komawari。乱数で束を出す）の考え方を Genko に作る。そのコードは読まない・写さない。考え方だけを使い、一から作る。コマ数をちょうど指定できる（1〜8。kMaxPanels=8 は変えない）、段数の範囲・プリセット（会話・標準・アクション・見せゴマ／扉）・つまみ（斜め・断ち切り・コマ内分割・大ゴマ率・重ね・はみ出し・枠なし）、**12 案の束**（数は引数、既定 12）、seed で同じ束、1 案か今のページを元にした変異（変異度）、振るい（無難すぎる格子・読み順の崩れ・小さすぎるコマ・狭すぎる間・束の中で似た案を除く）、当てると本物のコマ（apply_layout の道）。
- 群: K1 が土台（core・service）、K2 が GUI、K3 が MCP の道具。K2・K3 は K1 の API（SPEC LAYOUT-01「GUI・MCP 向けの API」）を使う。

### 群 K1（土台。ブランチ komawari-core、main 8362d5fc から）
- 設計: SPEC.md LAYOUT-01（骨組みのキー・生成の手順・プリセットとつまみ・振るい・変異・警告の code と文言・API・互換）、schema-v4.md「コマの重ね（komawari@1）」。
- A（ネーム計画の拡張。どれも任意のキーで、書かなければ前と同じ）:
  - 段・列・行の `gap_mm`・`tilt_mm`（その後ろの境目の間と斜め）、panels の `border_mm`（0 で枠なし）・`overflow`（はみ出し）、計画の `insets`（差し込み・重ね）。name_plan@1 の schema にも足した（strict のまま、どれも null 可）。
  - check_tiers の検査と apply_layout の当て方を両方直した。当て方は今の op の組み合わせ（split_frame の gutter_mm・tilt_mm、add_frame、set_frame）。
  - core は重ね・差し込みをそのままでは表せなかった（add_frame は根を描いたコマの節にするので、差し込んだコマがページ全体の後に読まれ、下のコマの枠線も重なって見える）。そこで拡張した:
    - コマの `over`（true だけ保存、`komawari@1` の拡張として features に載る）。
    - `add_frame` の `into`（宿主のコマの場所を描いたコマの節にして、宿主と並べる。読み順は宿主のそばで位置の順）と `over`。
    - `set_frame`・`set_layout` の木の `over`。
    - 描き方: over のコマの下では、ほかのコマの枠線と、ほかのコマに置いた絵が隠れる（render/panels.cpp の draw_frames、render/placed.cpp）。
  - submit_name は、fill_nulls が足した拡張のキーの null を `studio::unfill_extensions` で外してから計画を持つ。拡張のキーを書かない計画は、計画の保存・input_hash まで前と同じ。
- 生成（service/layout_ideas.{hpp,cpp}）と、測り方を 1 か所に置いた service/komawari_shape.{hpp,cpp}（形・振るいの基準・違いの点数・今のページの骨組み・単調さの警告）。乱数は xoshiro256**（splitmix64 で種を混ぜる）で、std の分布は使わない。
- C: submit_name に単調さの警告を足した（止めない）。code は layout_repeats・layout_flat・reveal_beat_small・book_without_accent。review.html にも、本の今のコマについて「コマ割りの緩急」として並べる。
- 試験: unit/test_layout_ideas 17 本（同じ seed で同じ束の sha・コマ数ちょうど 1〜8×4 プリセット・振るい・見せ場・当てて保存・読み戻し・今のページからの骨組み・変異・速さ・要求の断り・前と同じ op とバイトの sha・新しいキーの形と検査・add_frame into・重ねの下の枠線が消える描画・単調さの警告・submit_name の道）。12 案の束は描かずに約 1 ms（4 束 48 案で 3 ms）。関係する試験（R1）: test_model・test_storage・test_ops・test_studio_ops・test_command_bus・test_placed・test_contract_storage（参照 Python が無い 3 本は前からの SKIP）・test_service・test_ledger_service_a・b・c・test_gui_ledger_a の test_every_error_reads_in_japanese・test_gui_ledger_c、すべて合格。
- 直したもの（関係する試験で見つけた）: 公開の op の一覧 docs/ops.schema.json に add_frame の into・over、set_frame の over を足した。単調さの警告は、ほかの issue の後に足す（承認済みの断り name_approved が先頭のまま）。
- 直さなかった物（K2・K3 か後の群へ）:
  - GUI（K2）: 案の束の窓、変異、当てる操作。app/templates.cpp は描いたコマの節（free）を持つ「自分のテンプレート」をまだ当て直せない（差し込みのあるページを残すとこれに当たる）。
  - MCP（K3）: 案の束の道具。submit_name の説明文と手引き（B）に新しいキーを書くこと。→ 群 K3 で済んだ（下）。
  - 重ねの下で隠すのは、枠線と、コマに置いた絵（clip_to frame・bleed）だけ。スマートオブジェクトをコマで切った絵と、ページ全体の層の絵は、重ねのコマの下でも見える。over のコマ同士の上下は決めていない。
  - 自由な割り線は直線（位置と傾き）まで。曲がった線・折れ線は今の手の操作（set_frame curves・poly）のまま。

### 群 K1 のレビュー（読取専用 1 回）と直し
- P2（4 件）はすべて直した:
  - 承認済みのページで、人でない者が set_frame の over を変えることを、strict_gates で止める。over はコマ割りの変化（same_layout）にも数える。
  - apply_idea の関門を `core::can_approve(agent)` にした（ai: でない名前も止める）。骨組みは check_tiers で先に検査し、写しの上で当ててから入れ替える。断ったとき work は元のまま。
  - 差し込みの宿主を、変異を通して変わらないコマの番号で持つ。変異の後、差し込みは宿主と一緒に動く。振るいに「差し込みの箱の 4 分の 1 以上が宿主に掛かる」を足した。
  - 壊れた base は LayoutError("/base…", 日本語) で断る。
- P3:
  - over はコマだけが持つ（割れば子へ移る、set_layout の節には付かない、features は葉だけを見る）。読み手は真偽値でない over を broken_value にする。
  - book_monotony は本の番号のまま返し（Json）、位置で前後を見る。monotony も例外を出さない。
  - reading_slots と lint の読み順は、本の基本枠の大きさで測る。
  - 描いたコマ 1 つだけのページ、基本枠でない根は、骨組みにしない。
  - 要求の検査を足した（panels と範囲の両方、min_tiers>コマ数、focus>コマ数、空の seed、64 bit を超える整数）。大文字の 16 進の seed も受ける。Request を直接作ったときの NaN と範囲の外は、generate が範囲に収める。
  - 自動の seed は new_txn_id（システムの乱数）から作り、new_id を食わない。
  - 振るいに flat（layout_flat と同じ基準）を足した。
  - draft_of は、panels の slant、template の bleed と slant、計画の tier_gap_mm・col_gap_mm を引き継ぐ。
  - merge_into_plan は、骨組みに無い slot の panels を外して知らせる（dropped）。panels の slant も外す。
  - apply_layout は、差し込みの後で選択を元に戻す。
  - 文書（列の間・set_layout の over）。
- 互換の試験を、8362d5fc のコードで取った値に替えた。8362d5fc を scratchpad の別の build で、試験の対象 1 つだけ組み、demo4 のネーム 4 ページを submit_name した project.json（book_id を 0 にしたもの）の sha と、各ページの input_hash を固定した（submitNameSavesAsBeforeK1）。新しいコードでも同じ値になった。その build は消した。
- 試験: test_layout_ideas は 22 本になった（足した物: 8362d5fc と同じ保存・関門（strict_gates の set_frame over、ai: でない名前の apply_idea）・over と features の外れ方・割った over・読み手の検査・変異した案（差し込みあり）の当て・K1 より前の計画を元にした変異・壊れた base と骨組みと要求）。すべて合格。関係する試験（test_model・test_storage・test_ops・test_studio_ops・test_command_bus・test_placed・test_contract_storage（前からの SKIP 3 本）・test_service・test_ledger_service_a・b・c・test_gui_ledger_a の test_every_error_reads_in_japanese・test_gui_ledger_c）もすべて合格。
- 直さなかった物: 台詞の自動配置（letter）は、宿主の台詞が差し込みの上に乗ることを避けない（未確認のまま）。束の sha は Linux でだけ取った（Windows では M7 で確かめる）。`"over": false` を明示した本は、保存でその鍵が消える（false は既定なので、見た目と意味は同じ）。

### 群 K2（GUI。ブランチ komawari-gui、komawari-core 692d212d から。K1 のレビューの直し e9601687 を取り込み済み）
- 窓「コマ割りの案を出す…」（app/komawari_dialog.{hpp,cpp}、命令 `act_komawari`）: ページ → コマ の「テンプレートでコマを割る…」の次と、コマ割りツールの設定（「案を出す…」）。形と操作は SPEC LAYOUT-01「窓『コマ割りの案を出す…』」。
- 当てる: `idea_plan` が clear_page と apply_idea の op を 1 つの変更にまとめる（service の apply_ops は batch ごとに job id を 1 つ引くので、op ごとに同じ ids で当て直して job id を外し、最後に 1 batch で同じコマになることを確かめる）。主窓はテンプレートの窓と同じ確かめの後 apply_ops で当てる（人の操作。関門はテンプレートの窓と同じ）。
- app/templates.cpp: 自分のテンプレートを、当てるページの基本枠に合わせて置く（今までは紙の比だけで伸ばしたので、反対側のページでは描いたコマと斜めのコマの輪郭が基本枠からずれた）。差し込み・重ねの節（宿主 1 つと over 1 つ）は add_frame の into で作り直す（今までは描いたコマとして set_layout で丸ごと置いた）。
- service の API に `preset_defaults(Preset)` を足した（窓のつまみの初めの値。K3 も使える）。ほかの API は変えていない。
- 試験: gui/test_gui_komawari（窓と 12 案の絵・同じ seed で同じ束〔コピー・貼り付け・Enter〕・コマ数ちょうど 1〜8・段数・見せ場・変異とガチャに戻る・当てて Undo・Redo・保存と読み戻し・確かめ・今のページを元に変異・承認済みのページ〔strict_gates〕に人は当てられ、同じ op を AI は断られる・テンプレートに残して反対側と同じ側と元のページに当て直す〔差し込み・描いたコマ〕・速さ: 12 案の束は 1 ms、絵 12 枚は約 50 ms、その間のイベントループの待ちは最長約 30 ms）。test_gui_layout の画面の大きさの検査に窓を足した（21 → 22 窓）。test_gui_actions の命令の数を 820 → 821。commands.json を作り直した。ヘルプの「2. コマを割る」に一節。
- 直さなかった物: 窓の設定（プリセット・つまみ・束の数）は窓を閉じると忘れる（覚えない）。描いたコマを複数持つページは、今までどおり「今のページのコマ割り」を変異の元にできない（skeleton_of_page が無し）。
- レビュー（rev-K2）の直し（komawari-ai 576dd5d6 を取り込み済み）:
  - P1 テンプレート: 「今のコマ割りをテンプレートに残す…」が元のページの基本枠を `"frame"` として残す。`"frame"` のあるテンプレートだけ、その枠から当てるページの基本枠へ写す（同じ枠なら 1e-3 mm 以内で写さない。描いたコマの木は根が紙全体なら根だけ紙のまま、割った木は斜めのコマの輪郭だけを写す）。`"frame"` の無い今までのテンプレートは e9601687 と全く同じに置く。試験 templatesLayAsBefore が、e9601687 の templates.cpp で取った ops（ids 込み）と project.json の sha で、描いたコマ（add_frame。根が紙全体）を元のページへ、割った斜めの木を元のページへ、内蔵のテンプレート 2 つを固定する（今残したテンプレートも同じ値）。反対側のページでも根は紙のまま、コマは基本枠の中の同じ位置。
  - P2 窓: Enter はリストか「このページに当てる」にフォーカスがあるときだけ当てる（窓を show した試験）。描いたコマが 1 つのページ（白紙で根が描いたコマの節）にも当てられる（idea_plan・絵・K1 の apply_idea で clear_page。確かめも出す）。絵はコマだけを描き、紙と基本枠の外形・枠なしの破線・断ち切りの塗り・はみ出しの矢印を重ねる。
  - P2 案の質（layout_ideas.cpp・komawari_shape.cpp の生成と振るいだけ。merge_into_plan は触っていない）: プリセットの既定を強め、段の境目の斜めを 12〜(12+40×斜め) mm、列を 6〜(8+20×斜め) mm に深くし、振るいに strips（帯の積み重ね）・accents（目立つ所が足りない案は 12 案に 2 案まで）・tiers（段数の範囲）を足し、似ているかを見た目（look_alike）で見る。単調な元からの変異には目立つ所を足す（add_accents。K3 の報告: 格子のページの base_page が 0 案）。束の sha（test_layout_ideas）を作り直した。数字は SPEC LAYOUT-01 と manga_rules.md・layout_ideas の説明文に合わせた。
  - P3: 見せ場・段数の上限をコマ数（指定なしはプリセットの上限）に、誤りの言葉から API の鍵を外す、誤りの後も seed を保つ、「コピー」は束の seed、設定を変えた 250 ms の間は当てる・変異を止める、足りないときは外した理由を言う、案の言葉を人の言葉に（describe_for_people）、シード・変異の元・ヘルプの言い回し、moved は 1e-3 mm で比べる、反対側でも割った木の間は前の値（写すのは輪郭だけ）、AI の拒否の試験は ids の台本の下で関門の言葉まで、絵を描いている間に閉じる試験、承認済みのページは注記で言う、裏の絵は ScopedIdsApart、act_komawari の言葉・ツールチップ・場所を test_gui_actions で、内容タブで灰色を test_gui_smart_contents で。

### 群 K3（AI 向けの道具と手引き。ブランチ komawari-ai、komawari-core 692d212d から）
- 設計: SPEC.md LAYOUT-01「AI 向けの道具（群 K3）」。K1 のレビューの直し（e9601687）を取り込んだ上で合わせた（merge_into_plan の dropped、振るいの flat）。K1 の API（request_from_json・generate・batch_to_json・merge_into_plan、komawari::skeleton_of_page・shape_of_page・difference・describe）をそのまま使い、service の API は変えていない。
- MCP の道具 `layout_ideas`（service/studio_komawari.cpp、道具の表 kAgentTools・tool_table・api/mcp_tools に足した。MCP は 42 道具）:
  - 引数: project・page（必須）、panels か min_panels・max_panels、min_tiers・max_tiers、preset、knobs、seed、count（既定 12）、base（案の skeleton）か base_page（今のページ）、variation、focus、plan（下書きのネーム計画）。
  - 返り値: {page, request, seed, ideas[{skeleton, summary, panels, pattern, largest, reading_order, like_pages, plan?}], tried, dropped, around, name_approved}。plan を渡すと各案に merge_into_plan した plan が付き、そのまま submit_name に出せる（案に無い slot のコマは外れて plan_dropped、警告 plan_panels_dropped）。コマ数を書かずに plan を渡すと plan のコマ数ちょうど。
  - 読むだけ（MCP の read_only に入れた: presence と book_in_use は無く、道具の記録だけが書かれる）。人が承認したページは警告 name_approved（変更は submit_name が断る）。断りは issues の code layout_ideas_invalid・base_twice・base_unusable・no_page、束が足りないときは警告 layout_ideas_few。
  - MCP の inputSchema: ToolParam に schema（プロパティの説明・範囲を JSON の文で足す）を足し、layout_ideas の各引数と submit_name の plan に使った。ほかの道具の schema は前のまま。submit_name の説明文と MCP の instructions に拡張のキー・単調さの警告・layout_ideas を書いた。
  - CLI: `genko studio layout-ideas BOOK --page N [--panels N] [--preset …] [--knobs JSON] [--seed S] [--count N] [--base FILE | --base-page] [--variation V] [--focus N] [--plan FILE] …`。`genko studio call BOOK layout_ideas ARGS.json` も通る。HTTP の API（genko serve）には studio の道具の入口が無いので足していない（MCP の Streamable HTTP には同じ道具が出る）。
- B 手引き: manga_rules.md に「コマ割りの緩急」（beat の重さで大きさ・めくり・視線・前のページと変える・斜め／断ち切り／重ね／差し込み／はみ出し／枠なし／間の使いどころ・定型は例）、「コマ割りの案の束（layout_ideas）」、「単調さの警告」（code ごとの直し方の表）を足し、段組の節に拡張のキーを書いた。SKILL.md の plan_page を緩急と案の束で教える形に書き直した（integrations の Hermes・Claude Code の写しも同じ）。drawing.md（と写し）に add_frame の into・over、set_frame の border_mm・clip・over。lint の reveal_small・emphasis_small・finale_small の hint を、定型を例として示す文にした（code と severity は同じ）。docs/AGENT.md、OTHER_AGENTS.md（42 個）、ARCHITECTURE.md、integrations/hermes/config.example.yaml（ネーム担当に layout_ideas）。
- 試験: test_mcp_api_e2e に layoutIdeasThroughMcp（tools/list に出る・schema・submit_name の説明文のキー、同じ seed で同じ束、コマ数ちょうど 1〜8、断り、本を変えない、下書きの計画から選んで submit_name、今のページの変異、拡張のキーを全部含む計画、単調な 3 ページで layout_repeats と layout_flat がほかの issue の後に警告で出る、承認済みのページは name_approved の警告で、submit_name は断り本は変わらない）。onlyAnAgentsTools の道具の表に layout_ideas を足した（42）。test_cli_e2e に studioLayoutIdeas（同じ seed で同じ出力・コマ数・knobs・plan のファイル・call・使い方の誤り exit 2・count 49 の断り・本を変えない）。手引きの写しの試験（test_ledger_service_b の skillCopyMatchesThePackagedOne・claudeCodeSkillCopyMatchesThePackagedSkill）と unit/test_layout_ideas 17 本も合格。PERF-F（test_perf_pipeline）は道具の数を固定していない（pipeline_harness も固定していない）ので変えていない。
- レビューの直し（rev-K3）:
  - P2-1 core: `Page::merge_frame` で `split` も消し、split_frame のまっすぐの道で `remember_split` の前に葉の古い `split` を消した。試験 aSplitAfterAMergeRemembersItsOwnLine（斜めの split → merge → まっすぐの split、前の版の本の葉に残った線、submit_name の replace の後の割り線・skeleton_of_page・gutters・move_gutter、undo で前の斜めの線に戻る）。layoutIdeasThroughMcp は、案で出したページを格子で作り直す順に戻した（直す前は layout_repeats が出ずに落ちる）。submitNameSavesAsBeforeK1 の sha は変わらない。
  - P2-2: plan を name_plan@1 で確かめて `plan_invalid`（path 付き）。merge_into_plan はオブジェクトでない要素を外して dropped に入れる。
  - P2-3: 読み取り専用（別の会話からの呼び出しで book_in_use にならず、presence.json と、logs を除く本のファイルが同じで、tools.jsonl だけ 1 行増える）、like_pages の中身（difference と同じになる、unit/likePagesAreTheNeighboursAlike）、全道具の inputSchema の sha、base_twice・no_page・plan_invalid・plan_too_large・MCP からの count 49 の試験。
  - P3: plan は 256 KiB まで（plan_too_large）。隣のページと今のページの測りを try で受ける。plan_page_changed・variation_unused の警告、plan から決めたコマ数の断りの文、request.seed は実際の seed。SPEC の「41 のまま」を直し、integrations の drawing.md の写しを本体とそろえて試験 drawingCopiesMatchThePackagedOne を足した。
- 直さなかった物:
  - （済み。上の「レビューの直し」）core の既存の不具合（見つけた）: 斜めに切った（cut_frame）コマを merge_frame で戻すと、親のコマの `split`（割り線の記録）が残り、その後のまっすぐの split_frame は remember_split が前の記録を上書きしない。submit_name の replace で斜めの段組から斜めの無い段組に作り直したページは、`split` に前の斜めが残る（コマの形は正しい）。そのため skeleton_of_page・shape_of_page がそのページを「斜めあり」と読み、layout_repeats が出ないことがあり、inspect page の gutters（move_gutter）も前の線を返しうる。K3 のレビューで直した。
  - 案の束の絵（12 案を並べた縮小画像）は返さない。AI は summary・pattern・largest と、submit_name のプレビューで見る。
  - GUI（K2）は別の群。

### コマ割りの工程の出口 V3 — 凍結 108ae80 で Linux の ASan・Release が合格（2026-11-13）
- **凍結SHA**: `108ae80c`（native/komawari。K1・K2・K3 とレビューの直し 2df678c1 に、V3 の 1 回目で見つけた試験の直しを足した版）。計画は `validation_policy.py --base 8362d5f --head 108ae80 --phase milestone`（tier full。理由: 共通モデル・保存〔model.hpp の Frame に over、reader・writer〕、共有の描画経路〔panels.cpp〕、登録〔commands.json・ops_schema.json〕。各構成 264 試験）。
- 実行環境（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Xeon 2.1GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18 / cgroup v1。root から CAP_DAC_OVERRIDE・DAC_READ_SEARCH・FOWNER を外して実行。
- 1 回目（凍結 2df678c、2026-11-12）: ASan で test_layout_ideas だけが落ちた（263/264 合格）。どちらも試験の側の誤りで、製品のコードは変えていない。
  - overAndItsFeature: set_frame がページの枠を置き換えた後も、前の枠の子を指したまま回していた（heap-use-after-free）。子の id を先に取ってから回す。
  - aBatchIsQuick: 1 束 50 ms の上限を ASan（-O0）で超えた（4 束で 303 ms）。上限は最適化ビルドではそのまま。サニタイザ・Debug のビルドでは、48 案の時間が 12 案の 10 倍以内（案の数に比例して伸びること）を確かめる（support/regex_timing.hpp の kOptimizedBuild と同じ考え方。ASan で 239 ms と 926 ms）。
  - R4 に従い直した版（108ae80）で凍結し直して全体をやり直した（途中でコンテナが再起動し、ASan を最初から流し直した）。
- 結果（2 回目、凍結 108ae80）:
  - WebAssembly のホスト（cargo test と本体）: 合格。
  - linux-asan: 264/264 合格（2 並列、約 3 時間 16 分）。
  - linux-release: 264/264 合格。実 xcb の GUI 64 本すべて合格。
  - 性能（静かな機械で）: test_perf_app 2 回（開くまで 531 ms・544 ms、入力から画面まで p95 1.48 ms・1.51 ms、p99 3.30 ms・3.12 ms）、test_perf_render 合格、test_perf_pipeline 合格（PERF-F 8 ページの校正刷り p95 5264.6 ms）。
  - 配布物: `genko-studio-0.1.0-linux-x86_64.tar.gz`（sha256 `df7a07794ade39b9ec078eff9d31cecc8e3aaff0d0e93dfbc3e25bc057cf5ce5`、75,199,411 バイト）。素の Ubuntu 24.04（ネットワークなし、管理者でない利用者）で導入・足りないライブラリなし・版・窓で描いて保存・開き直して同じ画素・16 形式の書き出し・MCP（42 道具）・プラグイン・アンインストールまで ALL OK。
  - **linux-debug: 流していない（利用者の判断 2026-11-13、下）。** `--pair` は「構成artifactの欠落・重複」で不合格（Debug と Windows 2 構成が無い）。**3 構成の --pair 合格ではない。**
- 残り: 文字の自動配置が差し込みのコマを避けない、束の sha の固定は Linux だけ、窓の設定を覚えない、斜めの深いページで似た 2 つのコマの大きい方を skeleton_of_page が取り違えうる、Windows は deferred。

### 利用者の決定（2026-11-13）: 試験の量と実行ファイル
- 「変更分と影響箇所だけを見る」でよい。これからの工程の出口は、`validation_policy.py` が差分から選ぶ範囲（変更分と影響箇所）を、ASan と Release（範囲の試験と配布物・性能・実 xcb の画面）の 2 構成で流す。**Debug は最終配布の前に 1 回だけ。** 同じ試験を 3 構成で重ねて流さない（R3 の「Debug を並べる」をこれで置き換える）。範囲・合格値・安全検査は変えない。流さなかった構成は合格に数えない。
- コマ割りの工程は、ASan・Release の合格で Debug を省いて main に入れる。
- マージのときに Windows 用と Linux 用の実行ファイルを作る。Windows は GitHub Actions で一度だけ（手動でだけ動く `.github/workflows/build-executables.yml`。自動の CI〔native.yml〕は戻さない）。試験は「動くかだけ」（版・本を作る・調べる・描き出す）。できた物は下書きの Release に添付し、公開はしない。流した後はワークフローを `.github/disabled-workflows/` に戻す。Windows の実機での受入（配布前に必須）は残る。

### 実行ファイル（Windows・Linux）と Windows のビルドの直し — 2026-11-13
- `.github/workflows/build-executables.yml`（手動でだけ動く）を一度流して、下書きの Release **v0.1.0** を作った（公開していない）。流した後は `.github/disabled-workflows/` に戻した（`.github/workflows/` は空。native.yml も無効のまま）。
  - genko-studio のリポジトリ設定で Actions が「Disable actions」になっていて、最初の実行（run 43368010783）は始まらないまま残った（取り消せない状態）。利用者が「Allow all actions」に変えた。ワークフローが 1 本も無いので、この設定のままでも何も走らない。
  - 合格した run: 43400526839（main 74882ad3）。Linux（ubuntu-24.04）と Windows（windows-2022、MSVC 19.44）でビルドし、展開した配布物で `--version`・`new`・`inspect`・`export --format png`（8 ページ）が通った。
  - 添付: `genko-studio-0.1.0-linux-x86_64.tar.gz`（sha256 `b37d4f9d70bb06073e0bcb7116e9c52b4ffa8ba857e3bbdc419e74ea89c79f02`）、`genko-studio-0.1.0-windows-x64.zip`（sha256 `dae21e4917e5d9675da2b27d30ebd66ccfda3df0de9b1c24f114fe71e950a59c`）、SHA256SUMS.txt。
  - Linux の tar.gz は GitHub 上で作り直した物（同じソース。V3 で全試験・素の Ubuntu への導入まで通した物は sha256 `df7a0779…`、上の節）。
- 10 月 5 日以来 Windows でビルドしていなかったので、MSVC・vcpkg との違いを直した（Linux の動きは変えていない。直したファイルは Linux で -Werror のコンパイルを確かめた）:
  - vcpkg: harfbuzz を既定の機能なし（hb-ft を使わない）にし、使っていない freetype を外した（自前の FreeType 2.14.3 の Freetype::Freetype と、vcpkg の freetype の包みがぶつかって構成が止まっていた）。nlohmann-json を Linux と同じ 3.11.3 に固定した（overrides）。
  - imaging.hpp: libImaging の ImPlatform.h が持ち込む <windows.h> の near・far マクロを取り消す。
  - fx_rank: MSVC に __int128 が無いので、64 ビット 2 つの Wide（_mul128、double への丸めは 128 ビットと同じ。手元で 200 万回照らし合わせて一致）。
  - color_canvas・proof: Little CMS 2.16 以降は cmsGetToneCurveSegment(0, …)->Params（vcpkg 2.19、Ubuntu 2.14）。
  - optional<Json> を返す所を明示の Json に（ops_studio・state・brushes・selection・psd_effects）、psd_type は文字列で比べる、pcraft_variables は path::native() を文字で比べる、page.cpp の InkCanvas を ?: でなくラムダで、automate_merge の actor を波括弧で（most vexing parse）。
  - GenkoPackage: Windows だけ windeployqt に `--no-system-dxc-compiler`（dxcompiler.dll・dxil.dll を入れない。SBOM の検査は緩めていない）。
- **Windows で確かめたのはビルドと上の 4 つの操作だけ。** Windows の試験（windows-debug・windows-release の全試験と --pair）と実機での受入（配布の前に必須）は残る。費用は Actions の込みの枠の中（Windows 約 8 回のビルド）。

### ネームのラフは線で描く（案 1）— 2026-11-13、main に統合
- 利用者の報告: AI にネームを頼むと、手描きでなく画像を貼ってくる。原因は手引きと道具の側（「ネームでは絵を描かない」「作画＝生成した絵を置く」「ラフは purpose=draft」）で、AI は教えたとおりに画像を貼っていた。利用者の決定: 案 1（教え方と関門）を先に入れ、案 2（設定から描くネーム・ポーズ集 30〜40）は後で設計する。
- 入れたもの（native/name-lines、凍結 217683d5）:
  - 手引き（SKILL.md・drawing.md・manga_rules.md と integrations の写し、AGENT.md・OTHER_AGENTS.md・AI_PIPELINE.md）: ネームは「コマ割り・台詞の配置と、線で描くラフ」。ラフは人形（add_figure・add_mannequin）を置いて trace_prims の鉛筆の線にし、物は add_shape、線は add_stroke。構図を変えるときは線を消して人形を動かし、引き直す。SKILL.md の例は試験がそのまま流す。
  - MCP の説明、断りの文（「ラフは purpose=draft」をやめた）、plan_page の案内。
  - 関門（core::refuse_unnamed_picture、core::kept_off_print）: AI と進める本で、ネームが未承認のページでは、AI はネーム・下描き・印刷に出さないレイヤーに画像を置けない（place_asset・smart_place・adopt_candidate・画像の stamp_material・画像を含む paste。下描き用の generation_request と take_panel_art は name_not_approved）。断る前に何も書かない。人は今までどおり。承認の後も今までどおり。ファイル形式は変えていない。
  - アタリの印（kind atari）を付け外しできるのは人だけ。AI は import_name（人から渡された手描きのスキャン）でだけ下描きに置ける。ネームのレイヤーにはアタリも置けない。
- レビュー 1 回（P1×2: アタリの偽装、paste の画像。P2×5、P3×4）と直し。
- 残る穴（手引きで禁止と書いた）: AI が自分で作った画像を import_name で取り込めば下描きには置ける（人のスキャンの取り込みを残すため）。
- 試験（利用者の判断 2026-11-13: Release の全試験だけ。ASan・Debug は流していない）: `validation_policy --base 304d5564 --phase milestone`（tier full）、linux-release 264/264 合格（1 回目は test_gui_ledger_a が、日本語の断りの文に入れた操作の名前を英語と数えて落ちた。人に見せる文から名前を外し、AI には英語の detail で届ける形に直して流し直した）。実 xcb の画面・性能・配布物は流していない。

## PhotoCraft の全機能の取り込み（PC 工程）— 2026-10-16 計画

### 利用者の決定
- （2026-10-16）PhotoCraft の台帳 `docs/cpp-migration/photocraft/feature-ledger.json` の **全 961 行**を、この移行で取り込む（Windows の作業の前）。2026-10-06 の決定（DECISIONS.json: 漫画原稿へ統合し C++ 版の原稿形式を広げる、原本は変えない、未対応の版では編集を断る。カラー制作 COLOR-01）はそのまま。

### 出発点
- 参照: PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT OR Apache-2.0。移植する所は MIT を選び表示を残す。ArtCraft の商標・ブランドは使わない）。SOURCE.md の参照コピーの場所はこの機械にないので、同じ pin を作業用に取得して読む（変更しない）。
- 台帳は既存の feature-ledger.json を正本とする（新しい台帳は作らない）。2026-10-06 に当時の C++（f9bb7a1）と比べて作ったもので、930 行が「未照合」、実装に手を付けたのは 2 行（露光量の調整層・色深度のモデル）。その後の工程（M3〜M7・CMYK/Lab）で C++ が持った機能は、まだ台帳に反映されていない。
- 行の内訳: 操作 892（layer 181・filter 122・edit 77・view 77・type 70・image 64・window 61・file 53・tool 47・select 32・channel 21 ほか）、道具 42、形式 13、横断の要件 14。

### 照合の結果（PC0、2026-10-16、C++ 962faa4 と上流 ad86321。feature-ledger.json の各行の pc0 欄）
- 961 行: 同等 73・一部 299・なし 589。作業の大きさ S 592・M 207・L 75・XL 14（目安で S 0.5・M 1.5・L 4・XL 10 人日として約 1,050 人日）。同等はすべて C++ の試験が確かめているもの。
- 大きな土台: 色深度 32 ビット・色モデル・チャンネル（PC1）、スマートオブジェクト・スマートフィルター、レイヤースタイルの効果、ベクトルマスク・シェイプ・ベジェのパス、ラスタのブラシエンジン・修復ブラシ、文字スタイル・HarfBuzz の字形、フィルターギャラリー、自動処理・Photomerge・HDR、WASM のプラグイン、RAW。

### 利用者の決定（2026-10-16、照合の後）
- 統合は**群ごと**: PC1〜PC5 をそれぞれ一つの工程にし、工程の出口で V3 を 1 回流して native/integration へ push する。
- 台帳の macOS とブラウザ（WebAssembly）の 2 行は**加えない**（SPEC・DECISIONS のとおり対象外。台帳に利用者の判断として記録）。
- カメラの RAW は **LibRaw** を使う（LGPL-2.1 / CDDL-1.0。動的リンク、差し替えの案内とライセンス文を配布物に足す）。
- 新しい外部ライブラリ（OpenEXR か tinyexr・libavif＋libaom・zstd・BLAKE3・WASM の実行系）は**入れてよい**（要る群で入れ、THIRD_PARTY.md・SBOM・ライセンス文に記録）。

### PC1 の設計と利用者の決定（2026-10-16）
- 設計: 原稿の拡張 `color-model@1`（ページ・本の色モデル: RGB のプロファイルと 16/32 bit、グレー・2 階調・インデックス・ダブルトーン・マルチチャンネル、CMYK/Lab の 32 bit、黒点補正の指定。GKIR 版 2）と `channels@1`（アルファ・特色チャンネル）。RGB 8 bit・sRGB のページは Python と同じバイト。小群 PC1a（形式・深度・グレー・RGB プロファイル・変換・inspect）→ PC1b（カラー設定・校正・色域外警告）→ PC1c（チャンネル）→ PC1d（インデックス・2 階調・ダブルトーン・マルチチャンネル）。新しい外部ライブラリは要らない（Little CMS）。仕様は実装とともに SPEC.md（COLOR-03）・schema-v4.md に書く。
- 利用者の決定: (1) インデックス・2 階調・マルチチャンネルへの変換と特色の合成は**上流どおり統合する**（画素レイヤーを 1 枚に。要素は要素のまま）。(2) 高精度の画素の本の上限（GKCR の予算）を **256 MiB → 1 GiB**。(3) 文字のガンマは画素に効かせる・カラー設定の方針は開いた原稿にも当てる・作業用 CMYK の既定は coated-cmyk・チャンネルの分割/統合は元のページを置き換える（**上流に合わせる**）。既定の設定のままの RGB 8 bit・sRGB のページは Python と同じ。

### PC1 の出口 V3 — 凍結 af06f45 で Linux 合格（2026-10-17）
- **凍結SHA**: `af06f454149d571cd51f7e9e16cf70d34978c63f`（native/wip-pc に push 済み）。計画は `validation_policy.py --base 962faa4 --head af06f45 --phase milestone`（基点は M6・M7 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release）。4 回目の実行 2026-10-17 03:58〜08:43 UTC。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v139。Qt 6.11.2、GCC 13.3、参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。R3 のとおり Release と ASan を同時に始め、Debug を並べた（各構成内 ctest 1 並列）。Debug は空き容量のため一から `-gz`（デバッグ情報の圧縮。コードとデバッグ情報は同じ。validation_run が確かめる構成の値 — build type・故障注入・-Werror・サニタイザー — は preset のまま）でビルドした。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-release: 139/139 合格（03:58–05:16）。故障注入が Debug・ASan だけのための SKIP はこれまでの出口と同じ組で、合格に数えない。
  - linux-debug: 139/139 合格（04:00–05:50）。
  - linux-asan: 139/139 合格、サニタイザの指摘なし（03:58–08:32）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 29 本すべて合格（新しい test_gui_channels を含む。save は故障注入の SKIP）。Debug で test_gui_save・test_app_e2e（開く→描く→Undo→保存→終了→別プロセスで再開）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線、始まりの負荷 1.37。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6）: 開いて最初の頁 1065 ms・全頁 1234 ms／入力→ライブ線 p95 1.79・p99 5.33 ms（n=43780）／線確定→表示 p95 21.2 ms／Undo→表示 p95 32.6 ms／頁切替 最初 p95 37.2・精細 p95 120 ms（n=4）／自動保存 p95 2.16 s（n=31）／保存中（計 31.4 s）のイベントループ: 5 ms ごとの刻みの遅れ p99 9.81 ms、250 ms を超える停止 0、4 ms を超えた停止の分布 p99 74.1・最大 89.1 ms（n=110）。走行全体の 4 ms を超えた停止の最大 638 ms（1 回、保存の外）。test_perf_render: F1 の頁 350 dpi 全体 861 ms・線を足した後 485 ms・512×512 の部分 31 ms、線 200 本の小さな絵 5 ms。**M6 の出口（962faa4）より遅い**: 最初の頁 626→1065 ms、全頁 805→1234 ms、自動保存 p95 1.64→2.16 s、保存中の停止 p99 54.4→74.1 ms、走行全体の停止の最大 54→638 ms（負荷 1.04→1.37 の違いもある）。合否のない表示の値。**調べた結果（2026-10-17）: PC1 による遅れではない。** 静かな時間に 962faa4 と同じ実行物を交互に流すと全項目が揺れの範囲（例 最初の頁 HEAD 735・587／M6 704・480 ms、自動保存 p95 1754・1735／1731・1736 ms）、callgrind の命令数も同じ（全体 27.16 G 対 27.10 G。PC1 で増えたのは開くときの約 4 ms だけ）。V3 の値は、計測の 3 秒前に Debug・ASan のオブジェクト約 5 GB を消した（discard 付きの ext4）ことと負荷 1.37 によるもの。次の V3 から、オブジェクトの削除は計測の後に回し、sync して静まるのを待ち、1 回温めてから 2 回計り、生ログ（GENKO_PERF_LOG）を残す。638 ms の停止は生ログが無く特定できない（キャッシュが冷えると M6 の版でも開く途中に 344 ms の停止が出る）。PC1 より前からある停止の元（線ごとの色の履歴の設定の書き込みと fsync、窓の影と枠の大きな塗り）は PC2 で直す。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 61.0 秒・88 作業。全呼び出し n=314 p95 805 ms／情報取得 p95 680 ms／編集の確定 p95 86.0 ms／候補の採用 p95 1.44 s／頁の仕上げ p95 821 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 6.80〜7.20 s（各 30 秒以内）。**全項目合格**。
- **台帳**: feature-ledger.json の PC1 の 56 行のうち 55 行を実装済み（PC1a 11・PC1b 18・PC1c 20・PC1d 6）。document:color-depth-model は、高精度・インキのページでまだ断る処理（トーンの層・描画モードの補正層・形を動かす補正・書式を持つ高精度の線など）が残るため、PC2 以降まで開けたまま（行の note）。
- **群の経過**: PC1a（形式・深度・グレー・RGB プロファイル・変換・問い合わせ）→ PC1b（カラー設定・色の校正・色域外警告・文字のガンマ）→ PC1c（アルファ・特色チャンネル）→ PC1d（インデックス・カラーテーブル・2 階調・ダブルトーン・マルチチャンネル・色版の削除）。どれも 実装（関係する試験）→読取専用の独立レビュー 1 回→直し。指摘: PC1a 21（P1 2）、PC1b 約 18（P1 1）、PC1c 約 17（P1 2）、PC1d 7（P1 1: 統合で残すネーム・下描きの層の画素を寄せていた）。すべて直した。群を重ねた所の食い違い 2 件（文字のガンマだけを持つ本が読み取り専用になる・16 bit のページの方針の試験）も直した。
- **V3 の中で起きたこと（R4: 1〜3 回目の記録は使わない。記録は scratchpad の pc1-v3/attempt1〜3）**:
  - 1 回目（凍結 ac43bde）: Release 合格。実 xcb で test_gui_actions noTwoCommandsShareAKey（PC1b で足したキーの重なりの試験）が落ちた。**製品の不具合**: 「選択範囲を消す」が X11 の標準の Delete のキーを全部取り、その中の Ctrl+D が「選択を解除」と重なっていた（Qt はあいまいなキーをどちらも動かさない。PC1 より前からの違い）。Python と同じ Delete の最初のキーと Backspace にした（66f07ba）。
  - 2 回目（凍結 66f07ba）: Release 合格。実 xcb で test_gui_color theProofAsItComesIsTheWorkingCoatedCmyk が落ちた（試験の書き方: 窓を前に出さずにショートカットを送っていた）。activateWindow と qWaitForWindowActive で待つ形に直し、やり直す前に Release の GUI 試験を全部実 xcb で流して確かめた（b4fafbf）。
  - 3 回目（凍結 b4fafbf）: Release・Debug・実 xcb の GUI 31 本は合格。ASan で test_color_model だけが落ちた（試験の確保の数え方が nothrow の new を置き換えず、ライブラリの一時領域が置き換えた delete で返された alloc-dealloc-mismatch。製品のコードではない）。test_image_split_oom と同じく nothrow の形も置き換えた（af06f45、ASan で合格）。
- **工数の較正**（利用者の問い「1,050 人日は人が行う場合の工数？」への答え）: 1,050 人日は人が行う場合の目安。PC1 の 56 行の目安は 78 人日（S 33・M 13・L 8・XL 1）。実際は、実装の始め（10-16 10:21 UTC）から最後の凍結（10-17 03:38）まで約 17 時間（作業役の延べ約 16 時間、並行 3〜4）、V3 は 4 回で約 13 時間（合格した回は 4 時間 45 分）、合わせて約 1 日。
- **残課題**: Windows（deferred、配布前の実機受入は必須）。上の停止の元の直し（PC2）。document:color-depth-model の残り（PC2 以降）。マルチチャンネル・特色・インデックスの PSD は psd-tools では確かめたが Photoshop の実機では確かめていない（SPEC COLOR-03 の既知の制限）。

### PC2 の出口 V3 — 凍結 5ebcbe6 で Linux 合格（2026-10-18）
- **凍結SHA**: `5ebcbe61bc9c74437ab0f4e499bca6dad953bebc`（native/wip-pc に push 済み）。計画は `validation_policy.py --base af06f45 --head 5ebcbe6 --phase milestone`（基点は PC1 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release、plan_hash cdf04ff2c9aa…）。3 回目の実行 2026-10-17 22:25〜10-18 03:29 UTC。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v139。Qt 6.11.2、GCC 13.3、参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。R3 のとおり Release と ASan を同時に始め、Debug を並べた（各構成内 ctest 1 並列）。Debug は `-gz` で組み、試験の実行物はリンクの後にデバッグ情報を外した（`objcopy --strip-debug`。1 本約 190 MB・計 13 GB で空き容量に入らなかったため。コードは同じで、validation_run が確かめる構成の値 — build type・故障注入・-Werror・サニタイザー — は preset のまま）。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-release: 152/152 合格（22:26–23:46）。故障注入が Debug・ASan だけのための SKIP はこれまでの出口と同じ組で、合格に数えない。
  - linux-debug: 152/152 合格（22:36–00:29）。
  - linux-asan: 152/152 合格、サニタイザの指摘なし（22:26–03:12）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 33 本すべて合格（save は故障注入の SKIP）。Debug で test_gui_save・test_app_e2e（開く→描く→Undo→保存→終了→別プロセスで再開）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6）。計り方は PC1 の調べのとおり: 計測の前に sync して静まるのを待ち（始まりの負荷 0.48）、Debug・ASan のオブジェクトは消さず、1 回温めてから 2 回計り、生ログ（GENKO_PERF_LOG）を残した。2 回の値（1 回目／2 回目）: 開いて最初の頁 518／491 ms・全頁 714／669 ms／入力→ライブ線 p95 1.13／1.05・p99 3.28／3.27 ms（n=43780）／線確定→表示 p95 19.1／19.1 ms／Undo→表示 p95 32.4／30.7 ms／頁切替 最初 p95 23.5／40.9・精細 p95 108／139 ms（n=4）／自動保存 p95 1.51／1.54 s（n=31）／保存中のイベントループ: 刻みの遅れ p99 6.89／7.24 ms、250 ms を超える停止 0、4 ms を超えた停止 p99 51.6／47.4 ms／走行全体の 4 ms を超えた停止の最大 51.6／76.3 ms。test_perf_render: F1 の頁 350 dpi 全体 782 ms・線を足した後 422 ms・512×512 の部分 28 ms、線 200 本の小さな絵 4 ms。PC1 の出口（af06f45）の値（最初の頁 1065 ms・自動保存 p95 2.16 s・走行全体の停止の最大 638 ms）より速い。PC2d で直した停止の元（線ごとの色の履歴の設定の書き込み、窓の影と枠の大きな塗り）と、計り方の直しによる。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 57.0 秒・88 作業。全呼び出し n=314 p95 752 ms／情報取得 p95 663 ms／編集の確定 p95 80.2 ms／候補の採用 p95 1.31 s／頁の仕上げ p95 772 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 6.44〜7.00 s（各 30 秒以内）。**全項目合格**。
- **台帳**: feature-ledger.json の PC2 の 57 行を実装済み・受入済みに（PC2a 27・PC2b 15・PC2c 8・PC2d 7。PC1 で開けたままだった document:color-depth-model も PC2d で閉じた）。
- **群の経過**: PC2-0（補正の土台: 種類の表・`native`・adjustments@1・3 つの道・op と窓と PSD の枠）→ PC2a（階調の補正・3D LUT・パネル）・PC2b（色の補正）・PC2c（局所の補正・画像の演算・トラッピング・フェード）・PC2d（色管理と高精度の合成の残り・停止の元）を並行 → 統合のときの直し（C++ 版だけの設定の警告、PSD の hue2・notes・記述子、利用者の決定 8 のコマンドの名前）→ 読取専用の独立レビュー 2 回（A: 補正・LUT・PSD・画面、B: 高精度の合成・局所の補正・色管理）→ 直し（A: P1 3・P2 4・P3 9、B: P1 1・P3 4。すべて直した）。
- **V3 の中で起きたこと（R4: 1・2 回目の記録は使わない。記録は scratchpad の pc2-v3/attempt1・attempt2）**:
  - 1 回目（凍結 cda6a36）: Release で 2 本が落ちた。どちらも試験の期待が利用者の決定 5（PSD の調整レイヤーはいつも書く）に追いついていなかった（群の関係試験に入っていなかった）: test_contract_studio の配置画像の本の PSD、test_contract_export_cli の PSD（61787b0・50572e2。調整レイヤーを除いた層・統合画像が同じで、調整レイヤーが 1 枚以上あることを確かめる形に）。Debug はビルドの途中で空き容量が尽きた（上の strip で対処）。
  - 2 回目（凍結 50572e2）: Release・Debug・実 xcb は合格。ASan で test_contract_adjust の everyPageThroughTheCommandLine が QtTest の 1 関数 5 分を超えた（指摘なし。Release では 21 秒）。ほかの長い契約試験と同じ QTEST_FUNCTION_TIMEOUT を足した（1b62552）。やり直しの前に、PC2 で触った試験と GUI・E2E の残りを ASan で先に流し、レベル補正・トーンカーブの窓のメモリの漏れ（状態とその表示の関数が shared_ptr で互いを持つ。913b9ec で weak_ptr に）と、test_gui_color の ctest の上限（ASan で約 270 秒、上限 300 秒。止まった試験の見張りなので 900 秒に。5ebcbe6）を見つけて直した。
  - 3 回目（凍結 5ebcbe6）: 上のとおり合格。
- **工数の較正**: PC2 の 57 行の目安は 94.5 人日。実際は、設計と利用者の決定（10-17 08:45〜09:20）の後、実装の始め（09:30 ごろ）から V3 の凍結候補（16:05）まで約 6.5 時間（並行 4〜5）、V3 は 3 回で約 11.5 時間（合格した回は 5 時間 4 分）。
- **残課題**: Windows（deferred、配布前の実機受入は必須）。インキのページで、写した画素のかけらを貼るとはみ出し off でもコマで切らない（PC1 からの動き。レビュー B で残した）。PSD の調整レイヤーは psd-tools とこのビルドでは確かめたが Photoshop の実機では確かめていない（SPEC COLOR-04 の既知の制限）。

### PC3 の出口 V3 — 凍結 e9ee13c で Linux 合格（2026-10-22）
- **凍結SHA**: `e9ee13ca9ea51cea81f63ea399308a794ad3d459`（native/wip-pc に push 済み）。計画は `validation_policy.py --base 5ebcbe6 --head e9ee13c --phase milestone`（基点は PC2 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release、plan_hash 64aa42d08b4b…）。5 回目の実行 2026-10-21 12:02〜10-22 01:20 UTC（Release・Debug は 12:37〜14:53、ASan は流し直しの 22:18〜01:01）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v139。Qt 6.11.2、GCC 13.3、参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。空き容量（約 40 GB の割り当て）で 3 構成のビルドが同時に置けないので、Release（ctest 1 並列）と Debug（ctest 3 並列）を並べて流し、Debug のビルドを消してから ASan をビルドして流した（ctest 3 並列。result.json の ctest_parallel）。ASan の流し直しだけ、QTEST_FUNCTION_TIMEOUT=1700000・M3A_FLOOD_DEADLINE=5 を環境に置いた（下の 5 回目）。Debug は空き容量のため、デバッグ情報なし（`-g0`。GCC は -g の有無で作るコードを変えない。validation_run が確かめる構成の値 — build type・故障注入・-Werror・サニタイザー — は preset のまま）でビルドした（2 回目に、-gz と試験の実行物の strip でも 11 GB になり、空き容量が尽きたため）。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-release: 197/197 合格（12:37–14:53、ctest 1 並列）。故障注入が Debug・ASan だけのための SKIP はこれまでの出口と同じ組で、合格に数えない。
  - linux-debug: 197/197 合格（12:37–13:46、ctest 3 並列）。
  - linux-asan: 197/197 合格、サニタイザの指摘なし（10-21 22:18–10-22 01:01、ctest 3 並列。下の 5 回目の流し直し）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 43 本すべて合格（save は故障注入の SKIP）。Debug で test_gui_save（36）・test_app_e2e（開く→描く→Undo→保存→終了→別プロセスで再開、8）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6。）。計り方は PC1 の調べのとおり: 計測の前に sync して静まるのを待ち（始まりの負荷 0.46）、1 回温めてから 2 回計り、生ログ（GENKO_PERF_LOG）を残した。2 回の値（1 回目／2 回目）: 開いて最初の頁 743／810 ms・全頁 771／810 ms／入力→ライブ線 p95 1.28／1.88・p99 5.28／7.56 ms（n=43780）／線確定→表示 p95 23.6／21.0 ms／Undo→表示 p95 43.3／42.7 ms／頁切替 最初 p95 32.6／36.7・精細 p95 153／170 ms（n=4）／自動保存 p95 1.85／1.80 s（n=31）／保存中のイベントループ: 刻みの遅れ p99 8.68／10.0 ms、250 ms を超える停止 0、4 ms を超えた停止 p99 57.7／83.2 ms／走行全体の 4 ms を超えた停止の最大 71.2／254 ms。test_perf_render: F1 の頁 350 dpi 全体 1143 ms・線を足した後 545 ms・512×512 の部分 35 ms、線 200 本の小さな絵 1 ms、深い SO の 6 タイル 1 枚ずつ 7.7 s・4 枚同時 2.5 s（デコード 1 回、最大 699 MB）。**PC2 の出口（5ebcbe6）より遅い**（最初の頁 518／491 ms、自動保存 p95 1.51／1.54 s、350 dpi 全体 782 ms）。同じ凍結の 4 回目の計測（c06c5cd、製品のコードは同じ）も同じくらい（最初の頁 854／837 ms・350 dpi 全体 1381 ms）。**調べた結果（10-22、PC2 の凍結 5ebcbe6 の Release を同じ機械で組み、交互に 2 回ずつ計った）: 描画・入力・保存・PERF-F は PC2 と同じ**（350 dpi 全体 PC2 1192／1207・PC3 1175／1194 ms、入力→ライブ線 p95 1.2／1.0 と 1.1／1.6 ms、自動保存 p95 1.79／2.09 と 1.78／2.08 s）。遅く見えたのは機械の日による違い（PC2 の凍結もこの日は同じだけ遅い）。**開いて最初の頁だけは PC3 が遅い**（開くだけをさらに 6 回ずつ交互に計り、8 回ずつの中央値で PC2 720・PC3 827 ms、約 0.1 秒）。PC3 で増えた窓・パネル・メニューを開くときに組む分だった。PC4 の P0 で、パネルを先に dock・scroll に入れてから中身を足し（足すたびに大きさを測り直さない）、メニューの道筋を 1 回だけたどるように直した（窓を開くまでの命令数 PC3 21.0 億 → 15.6 億で PC2 の 17.5 億を下回り、壁の時間で約 0.1 秒縮んで PC2 と +20 ms ほど。PC4 の出口の計測で 689／675 ms）（記録は scratchpad の perf-ab）。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 107.5 秒・88 作業（PC2 の出口は 57.0 秒。この V3 の 4 回目は 84.3 秒）。全呼び出し n=314 p95 1.42 s／情報取得 p95 739 ms／編集の確定 p95 107 ms／候補の採用 p95 1.52 s／頁の仕上げ p95 848 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 7.61〜8.18 s（各 30 秒以内）。**全項目合格**（合格値は変えていない）。工程全体と全呼び出しが PC2 の出口より遅く見えたのは機械の日による違い（同じ機械で交互に計ると F3 は PC2 67.8／70.5 秒・PC3 66.5／72.5 秒、各 p95 も同じ）。
- **台帳**: feature-ledger.json の PC3 の 250 行を実装済み・受入済みに（A0 9・A1 12・A2 10・A4 15・A5 3・B1 27・B2 25・B3 14・B4 15・C-a 26・C-b 28・C-c 23・C-d 31・C-e 12。A3 は行を持たない PSD の SO の読み書き）。edit.fill は、パターンの塗りが PC4（パターンのライブラリの後）なので一部実装のまま開けておく（行の note）。
- **群の経過**: 土台 PC3-0（選択・マスク・パス・変形・判型の土台と、選択・編集の名前の変更）・A0（スマートオブジェクトの土台と絵の取り込み）→ 波: A1（中身とリンク）・A2（スマートフィルター）・A3（PSD の SO）・A4（スタックモードと台詞のラスタライズ）・B1（選択の基本と選択範囲の編集）・C-a（パスとベクトルマスク）・C-e（判型）・B2（編集・マスク・レイヤーの選択）・B4（修復）・C-b（シェイプ）・C-c（変形）・B3（賢い選択・選択とマスク・マッティング）・C-d（移動・整列・吸着）・A5（SO の変形とワープ系のスマートフィルター）。読取専用の独立レビュー 3 回（節 A＋C-d: P1 2・P2 5、節 B: P2 8・P3 約 12、節 C: P1 2・P2 8・P3 13）→ 直し 1〜5（保存・判型・ページの写し／選択範囲の格子・クリップボード・マスクの描き方・修復／変形・PSD・道具の状態／SO・保存・重さ／調整役の判断の残り）。P1・P2 はすべて直した。P3 は直したか、SPEC の既知の制限（COLOR-06・COLOR-07）に書いた。取り込みのときに群の重なり（同じ id・同じ言葉・メニューの並び・クリップボードと形の部品・リンクしたマスクの変形）を直した。直し 4 の報告から、復旧点が SO の素材を持たない不具合と、保存していない SO を持つ復旧点を採用すると本が無い素材を指す不具合（原稿の欠損）を見つけて直した（d85063f）。
- **V3 の中で起きたこと（R4: 合格に使うのは 5 回目の Release・Debug と、5 回目の ASan の流し直しだけ。ほかの記録は scratchpad の pc3-v3/attempt1〜attempt5）**:
  - 1 回目（凍結 d85063f）: 15:18 に機械が再起動して止まった（Release は 85 本まで失敗なし、ASan はビルドの途中）。
  - 2 回目（同じ凍結）: Release で test_contract_ops（C-b がグラデーションの形を足して、断る言葉が変わった。この言葉だけ期待を C++ 版に。新しい言葉に日本語の読み替えが無かったので足した: e131935）と test_gui_adjust（ファイル メニューの並びが C-a の「パスを Illustrator へ…」に追いついていなかった）。ASan で試験のコードの誤り 2 つ（test_psd_smart が一時の文字列の中を読む heap-use-after-free、test_retouch の int のあふれ。製品のコードではない）。Debug のビルドで空き容量が尽き、ほかの試験が巻き込まれた（4711b5f で直し、Debug を -g0 に）。
  - 2 回目と 3 回目の間: 2 回目の ASan が届かなかった試験 118 本を ASan で流すと、test_retouch の 1 関数が QtTest の 5 分を越えた（試験が層を 1 画素ごとに丸ごと読み直していた。Release で 83 秒 → 2.7 秒: 7055484）。重い負荷の下で test_contract_mcp が 1 回落ち、MCP の initialize の前に読んだ要求を、作業スレッドが遅れて動くと initialize の後の状態で答える製品の誤りを見つけて直した（受け取った時点の状態で断る。test_mcp_connection を足した: 8a89e43）。
  - 3 回目（凍結 8a89e43）: 01:18 と 06:49 に機械が再起動して 2 度止まった（記録は attempt3・attempt3b。止まるまでに失敗はない。Release は 3b で合格まで流れたが、ASan が止まったので使わない）。再起動で背景の処理が消えるので、ここから ASan・Debug は 1 構成の中で ctest を 3 本並べた（validation_run.py に `--jobs` を足し、result.json の ctest_parallel に残す: c06c5cd）。
  - 4 回目（凍結 c06c5cd）: Release・Debug は合格。ASan で、3 本並べて遅くなった分だけ見張りを越えたもの（test_contract_adjust の genko 1 回の 300 秒、test_smart_filters の ctest の 900 秒）と、test_mcp_api_e2e のトークンの渡し方（URL 安全な base64 の先頭が「-」だと `--id -…` がオプションと読まれる。負荷と関係なく 64 回に 1 回落ちる。`--id=…` に）。見張りの 75〜89% を使った試験の ctest の上限も 2 倍に（e9ee13c。止まった試験の見張りで、性能の合格値ではない）。CLI23 の flood_termination は、ASan の起動（約 1.2 秒、計装した共有ライブラリ約 860 MB を読む）だけで 1 秒の締め切りを越えた（断り自体は即座）。
  - 5 回目（凍結 e9ee13c）: Release（ctest 1 並列）・Debug（3 並列）は合格。ASan（3 並列）は見張りの 2 つだけで落ちた: test_smart_stack の精密・インキの頁の階調が QtTest の 1 関数 5 分を越えた（4 回目は試験全体で 753 秒）ことと、CLI23 の flood の 1 秒。ソースを変えずに（R6 で Release・Debug の結果はそのまま使う）、QtTest の 1 関数の上限を 1,700 秒（ctest の各試験の上限は変えない＝止まったものはそこで捕まる）・M3A_FLOOD_DEADLINE を 5 秒（その検査自身の設定）にして ASan だけ流し直した。1 度目の流し直しは 20:44 に容器が回収されて止まり（31 本まで失敗なし、記録は attempt5/validation-linux-asan-2-stopped）、2 度目（22:18 から）で 197/197 合格（CLI23 も合格）。見張りの上限をそろえる直し（遅い 26 本の QtTest の 1 関数の上限を ctest の上限から 100 秒引いた値に）は、統合の後に入れる。
- **工数の較正**: PC3 の 251 行の目安は約 264 人日（pc0 で約 280、重ね数えを直した見込み）。実際は、実装の始め（10-18 03:30 ごろ）から最後の凍結まで、API の週の上限で止まった約 42 時間（10-18 12:30〜10-20 06:00）を除いて約 17 時間（並行 4）、V3 は 5 回で約 35.5 時間（10-20 13:46〜10-22 01:20。直しと、容器の再起動・回収で止まった 3 回を含む。合格した記録は Release 2 時間 16 分・Debug 1 時間 9 分・ASan 2 時間 43 分）。
- **残課題**: Windows（deferred、配布前の実機受入は必須）。開いて最初の頁が PC2 より約 0.1 秒遅い（→ PC4 P0 で直した）。遅い試験の QtTest の 1 関数の上限を ctest の上限にそろえる直し（→ 統合の後に 0e7c7c0 で直した）。tools/ci の test_validation_policy が M6（c287a75）からの test_gui_look の QSKIP で落ちる（製品の試験ではなく、V3 の対象外。→ PC4 P0 で参照 Python の確かめを契約試験 test_contract_materials へ移して直した: 3c7c1d7）。SPEC COLOR-05〜07 の既知の制限（200 dpi のまま残る所、PSD のフォルダーのマスク、変形のプレビューの重さ、表紙のある本の用紙、網の位相、自由変形の数の欄など）。PSD の SO・パス・シェイプ・ベクトルマスクは psd-tools とこのビルドで確かめたが、Photoshop の実機では確かめていない。

### PC4 の出口 V3 — 凍結 7fb07f8 で Linux 合格（2026-10-28）
- **凍結SHA**: `7fb07f8298fb4afd44004491c1254ffec831239f`（native/wip-pc に push 済み）。計画は `validation_policy.py --base e9ee13c --head 7fb07f8 --phase milestone`（基点は PC3 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release、plan_hash 954219e66707…）。3 回目の実行 2026-10-28 03:48〜15:47 UTC。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon。05:30 の再起動の前は @ 2.10GHz、合格に使った実行はすべて再起動の後の @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v180。Qt 6.11.2、GCC 13.3、参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。PC4 から要るもの: `GENKO_LIBRAW_ROOT`（LibRaw 0.22.x を組んだ場所。構成のときに要る）、`GENKO_PCRAFT_CHECK`（test_contract_pcraft の上流の読み手。tools/migration/pcraft_fixtures を上流 PhotoCraft v0.2.0 に当ててオフラインで cargo で組んだ。無いと SKIP になり V3 は落ちる）、apt の libopenexr-dev・libavif-dev・libaom-dev・libzstd-dev。空き容量（約 40 GB の割り当て）で 2 構成のビルドが同時に置けないので、1 構成ずつ組んで流し、流し終えたビルドを消した（ASan → Release → Debug。ctest の並列は ASan 3・Release 2・Debug 3、result.json の ctest_parallel）。ASan だけ M3A_FLOOD_DEADLINE=5（PC3 と同じ。CLI23 の flood の検査自身の設定）を環境に置いた。Debug は PC3 と同じく `-g0`。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-asan: 241/241 合格、サニタイザの指摘なし（05:32–10:11、ctest 3 並列）。test_contract_pcraft は上流の読み手で 21 件すべて確かめた（SKIP なし）。
  - linux-release: 241/241 合格（11:06–12:03、ctest 2 並列）。故障注入が Debug・ASan だけのための SKIP はこれまでの出口と同じ組で、合格に数えない。
  - linux-debug: 241/241 合格（14:03–15:45、ctest 3 並列）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 51 本すべて合格（save は故障注入の SKIP）。Debug で test_gui_save（36）・test_app_e2e（開く→描く→Undo→保存→終了→別プロセスで再開、8）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6）。計り方は PC1 からのとおり（sync して静まるのを待ち、始まりの負荷 0.46、1 回温めてから 2 回計る）。2 回の値（1 回目／2 回目）: 開いて最初の頁 689／675 ms・全頁 846／763 ms／入力→ライブ線 p95 1.52／1.33・p99 5.17／5.40 ms（n=43780）／線確定→表示 p95 21.1／21.3 ms／Undo→表示 p95 39.7／45.6 ms／頁切替 最初 p95 31.0／29.2・精細 p95 116／152 ms（n=4）／自動保存 p95 1.89／1.98 s（n=31）／保存中のイベントループ: 刻みの遅れ p99 8.00／9.18 ms、250 ms を超える停止 0、4 ms を超えた停止 p99 52.3／70.9 ms／走行全体の 4 ms を超えた停止の最大 61.1／71.1 ms。test_perf_render: F1 の頁 350 dpi 全体 926 ms・線を足した後 434 ms・512×512 の部分 31 ms、線 200 本の小さな絵 1 ms、深い SO の 6 タイル 1 枚ずつ 5.75 s・4 枚同時 2.22 s（デコード 1 回、最大 702 MB）。**開いて最初の頁は PC3 の出口（743／810 ms）より速く**、P0 の直し（パネルを先に dock・scroll に入れてから中身を足す・メニューの道筋を 1 回だけたどる。窓を開くまでの命令数 PC3 21.0 億 → 15.6 億で PC2 の 17.5 億を下回る）が効いた。ほかの値も PC3 の出口と同じか速い。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 64.6 秒・88 作業。全呼び出し n=314 p95 803 ms／情報取得 p95 704 ms／編集の確定 p95 110 ms／候補の採用 p95 1.53 s／頁の仕上げ p95 862 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 7.06〜7.94 s（各 30 秒以内）。**全項目合格**（合格値は変えていない）。
- **台帳**: feature-ledger.json の PC4 の 261 行を実装済み・受入済みに（D0 5・D1 25・D2 25・D3 48・D4 13・D5 1・D6 3・E1 9・E2 18・E3 16・E4 18・E5 16・E6 25・F-a 13・F-b 13・F-c 9・F-d 1・F-e 2・F-f 1。D7・E-0・F-0・P0 は行を持たない土台・対応表・後始末）。PC3 で開けておいた edit.fill は E3 のパターンの塗りで閉じた。
- **群の経過**: 土台 PC4-0（F-0 形式の土台・P0 PC3 の後始末・E-0 描く道具の土台・D0 フィルターの土台）→ 波 1: D1（ぼかし・シャープ・ノイズ・その他・ビデオ）・D3（フィルターギャラリー）・E1（点を打つブラシと鉛筆）・F-b（ファイルの入口・画像を開く）→ 波 2: D2（変形・ピクセレート・表現手法）・E3（パターン）・E4（塗りつぶし・グラデーション・描画色と背景色）・F-a（コーデック 13）→ 波 3: D6（レンズ補正・広角補正・消点）・E5（覆い焼き・焼き込み・スポンジ・ぼかし・シャープ・指先・混合ブラシ・背景消しゴム・色の置き換え）・F-d（PSD/PSB の全精度と知らない塊の保持）・F-f（.pcraft の読み書き）→ 波 4: D4（描画・ぼかしギャラリー）・D5（Camera Raw フィルター）・E2（プリセットとパネル）・F-c（書き出し形式・クイック書き出し・コピーを保存）→ 波 5: D7（PSD の filterFX と .pcraft のスマートフィルターの対応表）・F-e（カメラ RAW、LibRaw 0.22.x 動的リンク）・E6（レイヤースタイル 17 行）と F-c の上書きの守り。群ごとに読取専用のレビュー 1 回（23 回: P1 は F-b 1・E3 3・E4 1・F-a 1・F-d 3・F-f 3・E5 1・E2 2・E6 1・D0 1 の 17、P2 は約 90、P3 は約 300）→ 直し。P1・P2 はすべて直した。P3 は直したか、SPEC の既知の制限に書いた。取り込みのときに群の重なり（fx.hpp の Kernel::prepare と D4 の大きなぼかしの準備＝halo が全体のときだけ準備する、フィルター メニューと窓の一覧、op の一覧、メニューの項目数、環境設定のタブ）を直した。
- **調整役の判断（推奨どおり・設計と利用者の決定による。詳しくは SPEC と台帳の各行）**: 重いフィルターは断らずに裏で進み、窓を閉じたら止める。SO に確かめずにスマートフィルターとして足す（SO が Photoshop の絵やフィルターマスクを持つときだけ確かめる）。Lab の明るさの向きのフィルターは L だけ。輪郭のトレースは「輪郭のトレース…」。8 bit の層の通常のモードのグラデーションは線を焼かずにパッチとして置く（描画モードのときだけ画素に）。インキのページの色混ぜは線を含めて読みインキにだけ書く。透明な画素の見えない色はフィルターで混ぜない。書き出しは本のファイル・フォルダー・配置の元・開いた元の画像の上には書かない（クイック書き出しは同じ名前なら番号を足す、すべてのページは 1 回尋ねる。CLI は上書きのまま）。写せないフィルターを持つ .pcraft の SO・隠した写せないフィルターは外して SO を残す、PSD の書き戻しはフィルターが同じなら SoLd をそのまま。RAW を中身に持つ SO は PSD では Photoshop の shown を残す。描画レイヤーに変換はスタイルを残して描き込まない。16・32 bit のページの新しい層（panel_clip の既定）は 8 bit のまま（公開の op の既定）。30000 px を超える PSD は断って PSB を勧める。
- **V3 の中で起きたこと（R4: 合格に使うのは 3 回目だけ。ほかの記録は scratchpad の pc4-v3・pc4-v3b・pc4-v3c/attempt1）**:
  - 1 回目（凍結 9527394、10-27 14:21〜）: ビルドの間に機械の再起動 2 回と容器の回収 1 回で止まり、その後 Release と Debug を並べて組んで空き容量が尽きた（Release・Debug は使わない）。ASan で落ちたのは 11 本: **製品の不具合 4 つ**（RAW の現像が、高精度の面がその場で読む f32 の画素を先に放していた use-after-free、Camera Raw の窓が一時の文字列の上の色の見方を持っていた use-after-free、ぼかしギャラリーの作業と欄の間の shared_ptr の輪による漏れ、Camera Raw の編集の窓の親の無い欄によるフォントの漏れ）、**試験の誤り 3 つ**（test_contract_copy が一時の値の中を回る、test_gui_pen が放した後のページの dpi を読む、test_gui_brush_settings の new/delete の不一致）、3 本並べた ASan での見張りの越え 4 本（test_fx・test_fx_render・test_contract_paint_tools・test_fx_lens）。df1382f・…・ebedbd4 で直した（見張りは ctest の上限と QtTest の 1 関数の上限で、性能の合格値ではない）。
  - 2 回目（凍結 ebedbd4、10-27 22:40〜）: 1 構成ずつ組んで、途中から再開できる形に。00:15 に機械が再起動（ASan の途中、失敗なし）。ASan で、test_fx_d2・test_fx_lens が QtTest の 1 関数の上限を越え、test_contract_pcraft が GENKO_PCRAFT_CHECK の無い SKIP（合格に数えない）、CLI23 の flood_termination が ASan の起動（約 1.1 秒）だけで 1 秒を越えた。7fb07f8 で 2 本の上限を上げ test_fx_lens を 1 本ずつに、上流の読み手を組んで環境に、M3A_FLOOD_DEADLINE を 5 秒に。
  - 3 回目（凍結 7fb07f8、10-28 03:48〜）: 05:30 に機械が再起動（ASan の 57 本まで失敗なし、attempt1）。流し直した ASan・Release・実 xcb の GUI・性能が合格。Debug は、私の流す手順が PC3 の `-g0` を書き漏らして `-g` で組み、試験の前に空き容量を使い切った（1005/1706 で止まった。試験は 1 本も流れていない、attempt2）。`-g0` で組み直して合格（ソースと ASan・Release の実行物は同じなので R6 でそのまま使う）。
- **工数の較正**: PC4 の約 260 行の目安は約 293.5 人日（pc0 で約 270）。実際は、実装の始め（10-20 21:30 UTC）から最後の凍結（10-28 03:48）まで、API の週の上限で止まった約 96 時間（10-23 05:55〜10-27 06:00）を除いて約 64 時間（並行 4）、V3 は 3 回で約 25.5 時間（10-27 14:21〜10-28 15:47。直しと、機械の再起動・容器の回収で止まった 5 回を含む。合格した記録は ASan 4 時間 39 分・Release 57 分・Debug 1 時間 42 分）。
- **残課題**: Windows（deferred、配布前の実機受入は必須。PC4 の宿題: LibRaw の DLL を同梱して開けること、PSD の filterFXList の並びとフィルターマスクの id を Photoshop の実機の PSD で確かめること、Windows の静的なライブラリ（libdeflate・openjph・libyuv）のライセンス文と SBOM）。（PC6 の台帳の照合で訂正: レイヤー効果・Blend If・塗りの不透明度は PC4 E6 で PSD から読んで描く。残るのはラベルの色だけ）.pcraft は RAW を中身に持つ SO の shown を持てない。32 bit の sRGB のページの PSD は値をそのまま書く（Photoshop は 32 bit を線形として読む）。インキのページの色混ぜで、透明が混じる縁の色が前の黒への寄りなしになった（これから描く筆だけ）。E4 の make_precise は層全体を予算の外で作る。F-b のレビューの P3 のうち直さなかったもの（ほかの書き手の変更も戻ることの知らせ・色の表の画像の ICC・最近使用したファイルほか）。genko review の HTML の「書き出し…」と ops_schema.json の「glow (光彩拡散)」の言葉（公開の出力・契約なので残した。利用者に報告）。SPEC COLOR-05〜09 の既知の制限。tools/ci の test_validation_policy（Windows で流す試験は参照 Python が無いことで SKIP しない）が、E6 の test_layer_styles::psdWrittenAndRead の psd-tools の確かめの QSKIP で落ちる（製品の試験ではなく V3 の対象外。直すと凍結と違うソースになり 3 構成をやり直すので、PC3 の前例どおり PC5 の最初の群で契約試験に移す。Windows の実機受入の前に要る）。（PC5 で test_contract_psd_smart の layerStylesReadByPsdTools に移して済み）

### PC5 の出口 V3 — 凍結 98b180b で Linux 合格（2026-11-05）
- **凍結SHA**: `98b180bee5cef076bc3d58441b7164bc8611b9e4`（native/wip-pc に push 済み）。計画は `validation_policy.py --base 7fb07f8 --head 98b180b --phase milestone`（基点は PC4 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release、plan_hash 9060b596267d…）。4 回目の実行 2026-11-05 04:45〜20:44 UTC（JST 13:45〜翌 05:44）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.10GHz）/ RAM 16GB（cgroup の上限も約 16GB）/ Ubuntu 24.04.5 / kernel 6.18.44-fc-v236。Qt 6.11.2、GCC 13.3、参照 Python 3.12＋Pillow・numpy・psd-tools（PC4 と同じ）、GENKO_MCP_PYTHON、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。PC4 から要るもの（`GENKO_LIBRAW_ROOT`＝LibRaw 0.22.2、`GENKO_PCRAFT_CHECK`、apt の libopenexr-dev・libavif-dev・libaom-dev・libzstd-dev）はそのまま。PC5 から要るもの: Rust（cargo・rustc 1.97.0、`GENKO_CARGO`。J-f の `genko-plugin-host` を native/wasm-host から組む。wasmi 2.0.0 などは crates.io の固定の版と Cargo.lock の checksum）。空き容量（ビルドの無いときの空き約 21 GB）で 2 構成のビルドが同時に置けないので、PC4 と同じく 1 構成ずつ組んで流し、流し終えたビルドを消した（ASan → Release → Debug。ctest の並列は ASan 2・Release 2・Debug 3、result.json の ctest_parallel）。ASan だけ M3A_FLOOD_DEADLINE=5（PC3 からと同じ）。Debug は `-g0` と `-DGENKO_SHARED_LIBRARIES=ON`（下の 3 回目。試験の実行物が静的だと 1 本約 140 MB×約 290 本で約 35 GB になり容量に入らないため。この option の既定は OFF で、製品の Release・Debug は今までどおり静的、ASan は前から共有）。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-asan: 293/293 合格（試験の関数 5,500、SKIP なし）、サニタイザの指摘なし（11:05–16:36、ctest 2 並列）。test_contract_pcraft は上流の読み手で確かめた（SKIP なし）。
  - linux-release: 293/293 合格（17:28–18:14、ctest 2 並列）。SKIP 33 件は故障注入が Debug・ASan だけのためのもの（test_transaction・test_contract_save・test_save_e2e・test_migrate_e2e・test_session・test_gui_save・test_app_e2e。これまでの出口と同じ組で、合格に数えない）。
  - linux-debug: 293/293 合格（試験の関数 5,500、SKIP なし。19:20–20:42、ctest 3 並列）。
  - 試験の数は PC4 の出口の 241 本から 293 本に（PC5 の群の新しい試験と、test_contract_plugins から分けた test_contract_wasm_plugins）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred（PC4 と同じ）。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 63 本すべて exit 0（関数 1,167 合格、save は故障注入の SKIP）。Debug で test_gui_save（36）・test_app_e2e（開く→描く→Undo→保存→終了→別プロセスで再開、8）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6）。計り方は PC1 からのとおり（sync して静まるのを待ち、始まりの負荷 0.42、1 回温めてから 2 回計る）。2 回の値（1 回目／2 回目）: 開いて最初の頁 657／1062 ms・全頁 748／1149 ms／入力→ライブ線 p95 1.29／1.42・p99 2.97／3.54 ms（n=43780）／線確定→表示 p95 13.4／13.8 ms／Undo→表示 p95 28.2／29.8 ms／頁切替 最初 p95 23.4／21.6・精細 p95 102／96.8 ms（n=4）／自動保存 p95 1.56／1.87 s（n=31）／保存中のイベントループ: 刻みの遅れ p99 5.40／6.45 ms、250 ms を超える停止 0、4 ms を超えた停止 p99 33.9／55.0 ms／走行全体の 4 ms を超えた停止の最大 37.7／98.7 ms。test_perf_render: F1 の頁 350 dpi 全体 800 ms・線を足した後 454 ms・512×512 の部分 23 ms、線 200 本の小さな絵 1 ms、深い SO の 6 タイル 1 枚ずつ 4.32 s・4 枚同時 1.99 s（デコード 1 回、最大 703 MB）。グループの PSD の頁（I-b で足した計測）は GENKO_PERF_GROUPS_PSD を渡していないので SKIP（任意の計測）。**test_perf_app の 1 回目はどの値も PC4 の出口（開いて最初の頁 689／675 ms ほか）より速い**。2 回目は開くのが約 0.4 秒遅く、走行全体の停止の最大も大きかった（同じビルド・同じ機械の続けての実行。原因は調べていない、合否判定のない値）。test_perf_render は線を足した後だけ PC4（434 ms）より少し遅く、ほかは速い。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 48.1 秒・88 作業。全呼び出し n=314 p95 651 ms／情報取得 p95 511 ms／編集の確定 p95 73.7 ms／候補の採用 p95 1.09 s／頁の仕上げ p95 615 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 5.02〜5.43 s（各 30 秒以内）。**全項目合格**（合格値は変えていない）。
- **台帳**: feature-ledger.json の PC5 の 334 行を実装済み・受入済みに（G-a 18・G-b 16・G-c 20・G-d 20／H-a 17・H-b 19・H-b・J-b 1・H-c 25・H-d 14・H-d・H-e 1・H-e 12・H-f 18／I-a 23・I-b 1・I-c 9・I-d 6・I-e 9・I-f 6・I-g 5・I-h 2／J-a 15・J-b 14・J-c 8・J-d1 15・J-d2 7・J-e1 9・J-e2 14・J-f 7・J-g 3。節ごとに G 74・H 105・I 61・J 92、2 群にまたがる行 2。Z0・G-0・H-0・I-0・J-0 は行を持たない土台、G-e は G-a〜G-d の行の PSD・.pcraft の分で行を持たない）。render:GPU-CPU-parity は下の利用者の決定（2026-10-29）の許しを書いたまま。最後の edit.preferences.integrations は J-g の「連携」の面で閉じた（cf21ef8）。台帳の残りは platform:Windows・platform:Linux（PC6 の配布、未着手）と対象外 2 行だけ。
- **群の経過**: 設計（4 節を読取専用で、10-28 21:40〜22:19、利用者の決定 1〜12）→ 土台 PC5-0（Z0 PC4 の出口の決定 4 つと tools/ci・G-0 文字の土台・H-0 メニューの骨組みと名前と環境設定の表・I-0 自動処理の土台、続けて J-0 拡張 6 つの枠と重ね描きと PSD・.pcraft の口）→ 波 1: G-a（文字レイヤーと文字ツール）・H-a（表示と画素・% の意味）・I-b（グループとフレーム）・J-a（レイヤーカンプ）→ 波 2: G-b（組版・OpenType・書体・右から左、SheenBidi）・H-b（ガイド・グリッド・エクストラ・単位）・I-d（変数とデータセット）・J-c（注釈）→ 波 3: G-c（文字スタイル・段落スタイル）・H-c（窓の区画とパネル）・I-e（バッチ・ドロップレット・スクリプト・イベント）・J-d1（ものさし・カウント・計測）→ 波 4: G-d（パネル・スペルチェック・検索と置換・書体の置き換え、SCOWL 50）・H-d（ワークスペース・キーの組・メニュー・ツールバー）・I-a（レイヤーの基本の残り）・J-b（スライス）→ 波 5: G-e（PSD・.pcraft の文字）・H-e（環境設定と API）・I-c（アートボード）・J-d2（計測ログ・目盛りマーカー）→ 波 6: H-f（取り消し・メモリをクリア・画素の縦横比）・I-f（プリント・コンタクトシート・画像アセット）・J-e1（タイムライン）・J-e2（ビデオレイヤー）→ 波 7: I-g（自動整列・自動合成・Photomerge・スタック・統計）・I-h（HDR Pro・写真の切り抜き）・J-f（WASM のプラグイン）・J-g（制御チャンネルとアクション）。実際は枠が空き次第次の群を始め、波 5〜7 は重なった（多いときに 6 群が並行）。群ごとに読取専用のレビュー 1 回（33 回）→ 直し。P1・P2 は直した（H-a の GPU と CPU の一致の許しは利用者の決定で）。P3 は直したか、SPEC の既知の制限に書いた。取り込みは群ごとに native/wip-pc へ merge し、合わせた確認（w0〜w7a: 関係する単体・契約・GUI と tools/ci）を挟んだ。取り込みで直した群の重なり: メニューの表示・非表示 ▸（menu_show）と読み込み ▸（menu_import）を探して無ければ作るに一本化、is_query のあいまいさ（command_bus.hpp）、命令の数（最後は 820）と commands.json の作り直し、環境設定の面（G-d のテキスト・J-f のプラグイン・J-g の連携を H-e の作りに）、test_contract_studio の配置画像の PSD の回帰（H-b の 1032 のガイドの書き方、aefcf6c）。新しい外部の物: SheenBidi（Apache-2.0、third_party に源）・SCOWL size 50（同梱）・wasmi 2.0.0 ほかの Rust の crate（MIT/Apache-2.0）。新しい拡張: type@1・groups@1・image-assets@1・layer-comps@1・slices@1・annotations@1・measure@1・timeline@1・video@1・pixel-aspect@1・layer-locks@1・blend-channels@1・plugin-filters@1。
- **調整役の判断と利用者の決定（設計の決定 1〜12 は上の「PC5 の設計と利用者の決定」の節。詳しくは SPEC と台帳の各行）**:
  - 利用者の決定（PC5 の途中）: H-0 で、Photoshop の組で Genko だけの道具（選択・効果線・3D 操作・線の修正・塗りつぶしツール）に修飾キー付きのキー（Shift+N・K・D・R・Q）を付け、初めての起動でキーの組の変更を知らせる。H-a（2026-10-29）で、GPU と CPU の表示の一致は、再標本しない表示（今のまま・反転・画素の段・色の校正）は 1/255、回転・画面に合わせた倍率は 2/255 を超える画素 2‰ まで・差 12/255 まで（Mesa のテクスチャと Qt の双一次の重みの丸めの違い）。
  - **決定 13（10-29、キー）**: Photoshop の組の Shift+Ctrl+M は「計測値を記録」に。「コマを結合」（act_merge）は Photoshop の組で Ctrl+Alt+M に移した（Genko の組は今のまま）。
  - **決定 14（11-03、WASM）**: 「PhotoCraft に合わせて実装するだけで良い」。設計の WAMR は原本がこのセッションの GitHub の範囲外で取れず、J-f を止めて利用者に聞いた。調整役の読みで WAMR をやめ、上流と同じ wasmi 2.0.0（Rust）で上流 crates/plugins の作り（ABI v1・manifest・燃料・記憶の上限・帯）をそのまま写し、別の実行ファイル `genko-plugin-host`（genko と同じフォルダー）で動かす。プラグインのフィルターはスマートフィルターにもなる（plugin-filters@1）。既定無効・明示選択・sha256 の同意、入れる・選ぶ・同意の入口は MCP・HTTP・制御チャンネルに作らない。
  - **決定 15（11-03、余白の無いページの辺）**: render::mm_to_px の最小 1 のため、余白・塗り足しの無いページ（画像を開いた本・Webtoon の型など）を仕上がり／裁ち落としで切る書き出しが上と左の 1 列を落として 1 列ずれていた前からの誤りを直した（書き出し・EPUB・Kindle など、見開き・表紙の背と袖・下見の枠も）。当たる本の書き出しのバイトは変わる。余白のある本・paper・原稿の保存は変えない。置いた画像の切り抜きの箱（rect_px）は余白のあるページのバイトを変えるので直さない（調整役、SPEC に書いた）。
  - 調整役の判断（推奨どおり）: 文字・グループ・アートボード・カンプ・スライス・注釈・画像アセットなど PC5 の新しい op は MCP の agent_op に入れない（PC3・PC4 と同じ。CLI・窓・HTTP の apply からは通る）。MCP・HTTP から自動処理は使えない。古いアクションが承認・ロック・取り消しを持てば、どの op かを言って断る（全入口で同じ）。新しい op は strict_gates の関門に入れる。J-e2 の 16 bit の素材は 16 bit の PNG（スマートオブジェクトの画像の道にそろえる）。ドロップレットの起動ファイルは Windows では .lnk。スライスの道具 2 つは余分なツールの側。メモリをクリアの名前は場所つき（取り消し（編集）・ヒストリー（ウィンドウ）、決定 8 の決まり）。V3 の中で、キーをどの平台でも同じにした（下の 3 回目）。
- **V3 の中で起きたこと（R4: 合格に使うのは 4 回目だけ。ほかの記録は scratchpad の pc5-v3/attempt1〜3・attempt4-asan、直しの報告は pc5-v3/asan-fix-report.md・xcb-debug-fix-report.md）**:
  - 1 回目（凍結 cf21ef8、11-04 09:04〜）: ASan のビルドの間に機械の再起動 3 回（09:30・10:05・10:36。run.sh は途中から再開できる形なので流し直した）。待ちの間に容器が回収されるらしいので、見張りの背景の Bash を常に動かして容器を起こしておいた。ASan のビルドの後の空きは 3.6 GB で、取り込み済みの worktree 35 個を消して 6.3 GB に。ASan で 292 本中 72 本が落ちた（14:02）: **製品の不具合**（UBSan の破棄中の呼び出し＝H-c の修飾キーの Latch が `~MainWindow` の後の子の破棄の event で窓を呼ぶ 61 本、同じ種類の G-a のその場の文字の編集の callback と J-a・J-c・J-d1・J-0 ほかのパネルの `visibilityChanged` の lambda、LSan の fontconfig の漏れ（test_gui_layout ほか）＝G-d が窓を作る途中で同梱の書体を足してキャッシュを捨てていた→起動のときに一度だけ足す、16 bit のページのスマートフィルターに 0..1 の外の値が来て描画ごと例外＝Release でも落ちる物）、**試験の誤り**（一時の値の中を回る heap-use-after-free 6 本、利用者の決定 2026-10-28 で変えた add_layer の断りの言葉の読み替えが 2 本の契約試験で漏れ、プラグインのスマートフィルターを許した J-f の後の古い期待、test_type_styles の時間の上限を最適化しない build では比で見る＝regex_timing の前例どおり）。86c16b0 で直した。
  - 2 回目（凍結 86c16b0、11-04 17:13〜）: ASan の ctest は 292/292 合格だが、validation_run の「Totals は 1 本に 1 行」を test_contract_plugins（1 本に 2 つの QtTest）が満たさなかった。WebAssembly の組を test_contract_wasm_plugins に分けた（5f7283f、確かめる中身は同じ）。
  - 3 回目（凍結 5f7283f、11-04 20:15〜）: ASan（23:10）・Release（01:37）は合格。Release の途中で容器の再起動 2 回（00:06・00:34、28 分おき）で Release を流し直し、待ちの間の回収を防ぐ見張りの作業役を出した。実 xcb の GUI で 2 本落ちた: test_gui_prefs commandsJsonIsTheMenus（いくつかの命令のキーを QKeySequence::StandardKey で作っていて、offscreen と xcb で違った: やり直し Ctrl+Y／Ctrl+Shift+Z、閉じる、終了 Ctrl+Q）と test_gui_workspace theMenusHideAndColourTheirItems（試験の待ち方の誤り: xcb では窓が前に出る前にキーを押していた）。Debug は `-g0` でも静的な試験の実行物が容量を使い切った（02:45、空き 40 MB）。f55e812 でキーを全部明示してどの平台でも同じに（どこかの平台で使えたキーはどこでも使える: Genko の組のやり直しは Ctrl+Y・Ctrl+Shift+Z、終了は両方の組で Ctrl+Q。SPEC VIEW-01 に書いた）、試験は `QApplication::activeWindow()` を待つ。98b180b で `GENKO_SHARED_LIBRARIES`（既定 OFF）を足し Debug を共有ライブラリで組む（約 3 GB）。共有の Debug で、PSD の書き出しが PSD から何も持たないページで空の optional を読む未定義の動作（test_contract_studio の SIGSEGV）が見つかり直した。
  - 4 回目（凍結 98b180b、11-05 04:45〜）: ASan（3 並列、05:45–10:12）で 293 本中 1 本、test_contract_m3_mixed の 1 手で genko が exit -1。dmesg で cgroup のメモリ不足による OOM killer（genko、anon-rss 約 1 GB）と分かった。単独で流し直すと合格。コードは変えず、ASan の並列を 3→2 にして ASan だけ流し直し（11:05–16:36）合格、続けて Release・実 xcb の GUI・性能・Debug・--pair（20:43）。4 回目に機械の再起動・容器の回収は無かった。
  - ディスク: ビルドの無いときの空き約 21 GB。ASan のビルドで約 12 GB、Release で約 15 GB、共有の Debug で約 3 GB。
- **工数の較正**: PC5 の 338 行（PC5 の 334 行と行を持たない土台）の目安は約 306.5 人日（設計）。実際は、設計の始め（10-28 21:40 UTC）から V3 の合格（11-05 20:44 UTC）まで、API の週の上限で止まった約 99 時間（10-30 03:10〜11-03 06:00）を除いて約 92 時間。うち実装（設計から最初の凍結 cf21ef8 の 11-04 09:04 まで）が約 56.5 時間（並行 4、多いときに 6。容器の再起動 5 回で作業役を再開した）、V3 は 4 回で約 35.5 時間（11-04 09:04〜11-05 20:44。直しと、機械の再起動・容器の回収で止まった 5 回と OOM の 1 回を含む。合格した記録は ASan 5 時間 31 分（2 並列）・Release 46 分・Debug 1 時間 22 分）。
- **残課題**:
  - Windows（deferred、配布前の実機受入は必須）。PC5 で増えた Windows の宿題: 制御チャンネルの名前付きパイプの相手の確かめ（GetNamedPipeServerProcessId・ClientProcessId と SID、UserAccessOption。Linux ではコンパイルされない）、native/wasm-host の cargo（MSVC）での組み方と genko-plugin-host.exe の置き場所・SBOM の対応、印刷（プリンターの一覧・ICC・`genko print --printer`）、ドロップレットの .lnk（% の入ったパス・エクスプローラーの引数）、GPU の表示の実機のドライバー（区画を移すときの GL の文脈）、OS に依る環境設定の項目（UI の拡大・縮小、カラーピッカーの窓、ビープ音、クリップボードを残す）、32 bit の PSD を Photoshop で 1 枚開いて見ること。PC4 からの宿題（LibRaw の DLL を同梱して開けること、PSD の filterFXList の並びとフィルターマスクの id、静的なライブラリのライセンス文と SBOM）は残る。
  - MCP の stdio・HTTP の深い JSON の読み方（J-g のレビューで見つかった、J-g より前からの「読んでからコピー」の形。制御チャンネルは J-g で直した）。（PC6 のレビューで閉じた: MCP の stdio・MCP の HTTP・genko serve は読むときに深さを数え、255・255・1000 で断る。genko serve の深さ 1000 の再帰は Windows のスタックで未確認）
  - Photomerge の大きな写真の見積もりは安全側: 2400 万画素の写真 2 枚（半分ずつ重なる）は読み込むときの縮小をおおよそ 45 % 以下にしないと断る（全体を記憶の中で作るため、2 GiB の見積もり）。WASM のプラグインは 1 つのホストで帯を順に流す（上流は並べる。重いプラグインは遅い）、余白 256 のプラグインは B4・600 dpi で約 155 MiB を借りる。
  - HDR Pro の位置合わせ（MTB、上流・Photoshop と同じ算法）は、一方向の傾きだけの絵や、いちばん明るい画像の半分近くが白く飛ぶ場面で 2 px ずれることがある。
  - PSD は Photoshop の実機で確かめていない: 文字レイヤー（TySh・EngineData。Photoshop が書いた実物の見本が無く、この版の書き手・psd-tools・手で組んだ EngineData で確かめた）、アートボード、カンプ（1065）・スライス（1050）・計測（1074）・ガイド（1032）、32 bit の PSD の線形の値。
  - 日本語の項目名・窓の欄の名前（文字・段落スタイル、パネル・環境設定、窓の並べ方、スクリプトイベント、アートボード、変数・データセット、自動処理・画像プロセッサー、プリント・コンタクトシート・画像アセット、自動整列〜統計、HDR Pro・写真の切り抜き、計測ログの列）は Photoshop の実機と照合していない。
  - そのほか: 高精度のページでグループを通して見える層の結合・統合は断る（言葉で逃げ道を言う）。RGB の 32 bit のページの PSD の層は 8 bit の絵から（統合画像と中身の PSD だけが全精度）。コンタクトシートは 1 枚 1 レイヤー、画像アセットの png8 はフルカラーの PNG で書いて知らせる。テキストレイヤーの文字のガンマは色の文字・色の下地で台詞と少し違う。LSan の件で、字形パネルでパソコンの書体ファイルを見せるときの書体の追加・削除は今も実行中に起きる（古い QFontEngineData を持ち続ける Qt の中の物は突き止めていない）。16 bit のスマートフィルターに 0..1 の外の値が来るようになった commit は突き止めていない。SPEC の各節の既知の制限。
  - 次の V3 のための覚え: 命令やキーを足したら commands.json を作り直す（`GENKO_WRITE_COMMANDS=1 ./test_gui_prefs commandsJsonIsTheMenus` → genko_core・genko・test_gui_prefs・test_contract_prefs を組み直す）。この機械（16 GB）では ASan は 2 並列、Debug は `-g0`・`-DGENKO_SHARED_LIBRARIES=ON`。実 xcb の GUI は offscreen と違う所（キー・窓の前に出る時）を見るので省かない。
  - 次は PC6（台帳全件の同等性、独立レビュー、V3（Linux 3 構成・実 xcb の GUI・性能・配布物））。

### PC2 の設計と利用者の決定（2026-10-17）
- 設計: 補正・色管理（PLAN 工程 3）の 57 行（pc0 の目安 94.5 人日）。中心は image.adjustments.*（23）・layer.newAdjustmentLayer.*（16）・setAdjustment・自動トーン/コントラスト/カラー、色管理の残り（プロファイルの不一致・情報、取り込む画像の埋め込みプロファイル、モニター表示）、画像操作・演算・トラッピング・フェード、補正・属性・ヒストグラム・情報のパネル、PC1 で開けたままの document:color-depth-model（高精度・インキのページでまだ断る合成: トーンの層・描画モードの補正層・形を動かす補正・フォルダーの中・線や絵や書式・効果を持つ高精度の層）。今の補正 10 種（Python の 9 種と露光量）は作り直さずに広げ、C++ 版だけの設定は補正の中の `native` 1 か所に、新しい種類 8 つ（色相・彩度、自然な彩度、カラーバランス、白黒、レンズフィルター、チャンネルミキサー、特定色域の選択、カラールックアップ）。原稿の拡張 `adjustments@1`。新しい外部ライブラリは要らない。補正以外の image.* 23 行は PC3（判型・回転・切り抜き 11）と PC5（計測・複製・変数 12）へ。小群: PC2-0（補正の土台）→ PC2a（階調の補正・3D LUT・補正のパネル）・PC2b（色の補正）・PC2c（局所の補正と画像の演算）・PC2d（色管理と高精度の合成の残り）を並行。
- 利用者の決定: (1) 範囲は 57 行すべて。(2) 補正レイヤーの描画モードは補正の中に持ち、どのページでも効かせる。(3) 画面で新しく作る補正は上流（Photoshop 型）の形を既定に（今ある層は元の形のまま直せる）。(4) レベル補正・トーンカーブは上流の式も選べるようにする。(5) **PSD の調整レイヤーはいつも読み書きする**（補正レイヤーを持つ原稿・PSD では Python とのバイト一致と取り込みの結果の一致をやめる。補正レイヤーの無いものは今と同じバイト）。(6) 高精度の層の効果も高精度で作り直す。(7) ほかの原稿を使う操作は同じ原稿の中だけ（色の一致だけ、ほかの原稿の色の統計を画面が計算して渡す）。 (8)（PC2a の後）レイヤー → 色調補正 のコマンドとレイヤーパネルの補正レイヤーの項目は Photoshop と同じ名前にし（「レベル補正…」など）、コマンドを探す窓では置き場所で区別する（GUI 試験の「名前は重ならない」の決まりをこの決定で変える）。
- PC1 の出口で遅く見えた AC-PERF の表示の値は、調べた結果 PC1 によるものではなかった（上の PC1 の出口の記録）。PC1 より前からある停止の元（線ごとの色の履歴の設定の書き込み、窓の影と枠の塗り）を PC2 で直し、V3 の計り方を直す。

### 利用者の指示（2026-10-17、PC2 の V3 の間）
- 旧 Python 版は今後消す。**取り込み済みの機能は Python 版を比べる手本にしない**（手本は、取り込み済みの機能は今の C++ 版、新しい機能は上流 PhotoCraft）。仕様・ヘルプ・HANDOFF などの文書に Python 版との違いを書かない。
- **Python 版からの引き継ぎ（設定・データの移し替え）と、Python 版と同時に動かす考えは要らない**。C++ 版ができたら、すべて新しく入れて載せ替える。
- 当て方（PC3 から）: 新しい機能の設計に、Python とのバイト・画素の一致・Python が読める形・引き継ぎ・同時に動かすための仕組みや費用を入れない。今の原稿は今と同じに開けて同じに描ける（壊さない）。新しい機能を持つ原稿は今までどおり拡張（features）に。公開の契約（op の引数・MCP の道具表・CLI の出力）は使い手との約束として変えない（足すのはよい）。src/genko は変えない。Python と比べる今の試験は、振る舞いを変えない所の見張りとして流し続け、決定で振る舞いを変える所は期待を C++ 版の正の値に置き換える（理由は commit に）。Python と比べる試験を C++ の正の値に移す作業と、今ある同時に動かすための仕組みを消す作業は、Python 版を消すときの別の作業にする。

### PC3 の設計と利用者の決定（2026-10-17）
- 設計: PLAN 工程 4 の 251 行（pc0 の目安で約 280 人日、重ね数えを直した見込み約 264 人日）を 3 つの節に。A スマートオブジェクト・スマートフィルター（49 行）、B 選択・マスク・修復（82 行）、C パス・ベクトル・変形・判型（120 行。PC2 から回した判型・回転・切り抜きを含む）。ほかの工程へ: PC4 にパターンの塗り・背景消しゴム・色の置き換え・レンズ補正・広角補正・消点、PC5 にアートボード・レイヤーを自動整列・スライス。土台 2 つ（PC3-0: 選択・マスク・パス・変形の土台と名前の変更、A0: スマートオブジェクトの土台）を先に並行し、その後 4 つずつ並行。
- 利用者の決定: (1) コンテンツを編集は同じ原稿の別のタブで開く。(2) 「画像を読み込む…」もスマートオブジェクトにする（スタジオの候補の採用・op `place_asset` は配置画像のまま）。(3) PSD のスマートオブジェクト・パス・シェイプ・ベクトルマスク・マスクのリンクはいつも読み書きする（今ある塗りの層は画素のまま）。(4) 選択範囲は今のまま窓の状態（クイックマスクだけ原稿のチャンネル）。(5) 選択の道具・編集の項目は、今あるものも全部 Photoshop の名前にする（項目の id は変えず、利用者のショートカットなどの読み替えを足す）。(6) 賢い選択（被写体・空・オブジェクト・クイック選択・境界の調整）は上流どおり古典的な算法（学習モデルなし）。(7) シェイプはペンの層を広げて持つ（形の部品、拡張 `pen-shapes@1`。部品の無いペンの層は今と同じ）。(8) 判型は本全体（画像解像度・カンバスサイズ）とページごと（切り抜き・トリミング・回転、拡張 `page-spec@1`）の両方。
- 調整役の判断（推奨どおり・上の指示による）: 範囲は 251 行すべて。リンクした SO は原稿に写しを持つ。スマートフィルターは描く解像度で掛ける。高精度・インキのページの選択範囲とマスクはページの画素の細かさ（600 dpi まで）。新しく作るレイヤーマスクはリンクを既定（今のマスクはページに固定のまま）。回転・反転で台詞の文字は立てたまま。切り抜いた画素は残さない。整列・分布は層と台詞を同じ op で。

### PC4 の設計と利用者の決定（2026-10-20、PC3 の V3 の間に読取専用で）
- 設計: PLAN 工程 5（ブラシ・フィルター群・RAW・形式の互換）の約 260 行（pc0 の目安で約 270 人日、見込み約 293.5 人日）を 3 つの節に。D フィルター（120 行: 新しい フィルター メニュー・ぼかし・シャープ・ノイズ・変形・ピクセレート・表現手法・フィルターギャラリー・描画・ぼかしギャラリー・Camera Raw フィルター・レンズ補正・広角補正・消点）、E 描く道具・プリセット（84 行＋レイヤースタイル 17 行: 上流の点を打つブラシ・鉛筆・覆い焼き・焼き込み・スポンジ・ぼかし・シャープ・指先・混合ブラシ・背景消しゴム・色の置き換え・塗りつぶしツール・グラデーションツール・描画色と背景色・パターン・各プリセットとパネル）、F 形式（39 行: コーデック 13・PSD/PSB の全精度と知らない塊の保持・カメラ RAW（LibRaw、動的リンク）・.pcraft の読み書き・ファイル メニューの入口と書き出し）。ほかの工程へ: file.automate.lensCorrection は PC5。文字・表示・窓・自動処理・タイムライン・プラグイン・レイヤーカンプ・スライス・注釈・計測・アートボードの 359 行は PC5。土台 3 つ（D0・E-0・F-0）を先に並行し、その後 4 つずつ 5 つの波。新しいライブラリ: OpenEXR（BSD-3）・libavif＋libaom（BSD-2）・LibRaw（LGPL-2.1/CDDL、動的リンク: 2026-10-16 の決定）・zstd（BSD-3）・BLAKE3（CC0/Apache-2.0、同梱）。GPL・ML のモデル・外部の素材・新しい費用は要らない。新しい拡張: filters@1・vanishing-point@1・brushes@1・patterns@1・file-info@1・image-grid@1・psd-keep@1・formats@1・camera-raw@1。
- 利用者の決定: (1) フィルターは新しい フィルター メニュー（Photoshop の名前）に一本化する。今の 22 種は op・スマートフィルター・今の原稿でそのまま効くが、画面からは新しく選べない（Genko だけの 5 つは フィルター → 漫画）。(2) 描く道具の名前も Photoshop にそろえる（ペン → ブラシツール など 13 か所。項目の id とキーは変えず読み替え。層・線・ブラシの名前は変えない）。(3) ブラシの線は今どおりベクトルの線として持ち、上流の点を打つ描き方を線の描き方の 1 つとして足す。(4) 画像・PSD・RAW・.pcraft を開くとメモリの中の名前の無い 1 ページの本になり、保存で .genko の置き場所を尋ねる（元のファイルは変えない）。
- 調整役の判断（推奨どおり）: PSD の Photoshop のスマートフィルターは対応表にあるものを読む（書き出しは画素）。レンズのプロファイルは上流の一般のもの（lensfun は配布の条件が変わるので使わない）。重いフィルターは断らず画面は裏で進み具合とやめる。プリセットは機械の設定のライブラリ、原稿は使ったものの写し。レイヤースタイル 17 行を PC4 に入れる。Adobe の形式は ABR・GRD・PAT・PSD の Patt・ASL。画像から開いた 8 bit のページは一番細かい層の格子を作業の格子に。RAW を開くときは Camera Raw の窓を出す。LibRaw は Linux・Windows とも同じ 0.22.x を組んで同梱。.pcraft の Genko が持てない中身は画素にするか外して知らせる。

### PC6 の出口 V3 — 凍結 de0f5e9 で Linux 合格（2026-11-11）。PhotoCraft の取り込み（PC1〜PC6）は Linux で完了
- **凍結SHA**: `de0f5e94444c55a3451b89d4911dadaa6087726e`（native/wip-pc に push 済み）。計画は `validation_policy.py --base 98b180b --head de0f5e9 --phase milestone`（基点は PC5 の出口の凍結。tier full、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release、plan_hash 063ed23139b9…）。1 回で通った: 2026-11-10 13:23〜22:04 UTC（JST 22:23〜翌 07:04）。この節と台帳の platform:Linux の行は、凍結の後の文書だけの commit。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.10GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v338。Qt 6.11.2、GCC 13.3、cargo・rustc 1.97.0、参照 Python 3.12＋Pillow 12.3.0・numpy・psd-tools、GENKO_MCP_PYTHON、GENKO_LIBRAW_ROOT（LibRaw 0.22.2）、GENKO_PCRAFT_CHECK、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。配布物の試験は Docker 29.9.2。PC5 と同じく 1 構成ずつ組んで流し、流し終えたビルドを消した（ASan → Release → Debug。ctest の並列は ASan 2・Release 2・Debug 3。ASan だけ M3A_FLOOD_DEADLINE=5。Debug は `-g0` と `-DGENKO_SHARED_LIBRARIES=ON`）。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - WebAssembly のホスト（tools/ci/build_wasm_host.sh: cargo test と genko-plugin-host）合格。
  - linux-asan: 293/293 合格（試験の関数 5,537、SKIP なし）、サニタイザの指摘なし（14:18–18:49、ctest 2 並列）。
  - linux-release: 293/293 合格（関数 5,462、19:10–19:53、ctest 2 並列）。SKIP 33 件は故障注入が Debug・ASan だけのためのもの（test_transaction・test_contract_save・test_save_e2e・test_migrate_e2e・test_session・test_gui_save・test_app_e2e。これまでの出口と同じ組で、合格に数えない）。
  - linux-debug: 293/293 合格（関数 5,537、SKIP なし。20:54–22:02、ctest 3 並列）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred（PC5 と同じ）。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 63 本すべて exit 0（関数 1,175 合格、SKIP 1 は save の故障注入）。Debug で test_gui_save（36）・test_app_e2e（8）合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6）。始まりの負荷 0.48、1 回温めてから 2 回（1 回目／2 回目）: 開いて最初の頁 536／658 ms／入力→ライブ線 p95 0.90／1.28・p99 2.77／2.75 ms（n=43780）／線確定→表示 p95 13.8／13.6 ms／Undo→表示 p95 28.7／28.0 ms／頁切替 最初 p95 25.0／21.6・精細 p95 111／97.6 ms（n=4）／自動保存 p95 1.19／1.23 s（n=31）／保存中のイベントループ: 刻みの遅れ p99 5.44／5.19 ms、4 ms を超えた停止 p99 36.9／43.0 ms／走行全体の 4 ms を超えた停止の最大 66.2／43.0 ms。test_perf_render: F1 の頁 350 dpi 全体 717 ms・線を足した後 393 ms・512×512 の部分 25 ms、線 200 本の小さな絵 1 ms、深い SO の 6 タイル 1 枚ずつ 3.74 s・4 枚同時 1.53 s（デコード 1 回、最大 704 MB）。どの値も PC5 の出口と同じ程度か速い。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり）: F3 の工程 42.0 秒・88 作業。全呼び出し n=314 p95 552 ms／情報取得 p95 496 ms／編集の確定 p95 63.2 ms／候補の採用 p95 944 ms／頁の仕上げ p95 569 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回 4.91〜5.23 s（各 30 秒以内）。**全項目合格**（合格値は変えていない）。
- **配布物**（platform:Linux。Release のビルドから `cpack`）: `genko-studio-0.1.0-linux-x86_64.tar.gz`（74,896,106 バイト、sha256 `bf7246ce5a87edb1c2a8eec24dbca49e6073417136060a9382bb3682ed9eaebe`）。まっさらな Ubuntu 24.04.5 の容器（ubuntu:24.04 に README-ja.md の実行時の apt の物と xvfb・xdotool・x11-utils・x11-apps・fonts-noto-cjk だけを足した像、sha256:d3cf6e6023…。Python・CMake・コンパイラ・qmake・uv が無いことを確かめた）に、root でない人（u）がネットワークなし（`--network none`）で:
  - install.sh で `~/.local/opt/genko-studio-0.1.0` に入れ、`~/.local/bin/genko-studio` と .desktop ができる。ldd で genko・genko-plugin-host・xcb と offscreen のプラットフォームに欠けなし（任意の Qt のプラグインが要る物は一覧に書くだけ）。`--version` に genko 0.1.0・LibRaw 0.22.2・Qt 6.11.2。
  - `genko new` の本を窓（Xvfb・xcb）で開き、マウスでブラシの線を描いて Ctrl+S、終える → 開き直して終える。inspect が同じで、8 ページの画素も同じ（PNG のバイトは ICC のプロファイルに作った時刻が入るので比べない）。
  - 16 形式（png tiff pdf strip psd psb epub pack webtoon sns cmyk layers kindle timelapse animation pcraft）を書き出せる（113 ファイル）。MCP stdio で initialize・tools/list（41 道具）。WASM のプラグインを入れて一覧に出る（人が選ぶまで動かない）。
  - 書いたファイルは本・`~/.config/genko-studio`・入れた先（と fontconfig のキャッシュ）だけ。uninstall.sh で本体が消え、本と設定は残る。結果「ALL OK」。
- **PC6 の進め方**（plan: 台帳全件の突き合わせ → 独立レビュー → 直し → V3）:
  - 台帳の突き合わせ（読取専用、5 組並行、HEAD 6d75518）: 957 行（961 行から platform の 2 行と対象外の 2 行を除く）のうち OK 944・証跡の古さ 9・欠け 4（どれも証跡の誤りか試験の不足で、機能の抜けは無かった）。古さは 549dcbb で直した。
  - 独立レビュー（読取専用、3 観点、PC 工程 962faa4..6d75518 の横断）: A 入口と承認（P1 1・P2 1。MCP の agent_op と道具の表は 962faa4 から 1 字も変わっていない）、B 原稿の完全さ（P1 なし・P2 5）、C 頑丈さ（P1 1・P2 5）。
  - 直しは 5 群（群ごとに 実装→関係する試験→読取専用のレビュー 1 回→直し、R1・R2）。A 承認の関門（9fd6bde）・B 原稿の整合（c801d35・e2f19df、追加分を 2 回目のレビュー）・C 記憶の予算と大きな入力（9ba1b9d）・D 長い作業とスレッド（d5bb6ac）・E .pcraft のスタイルと GUI（85e8746）。統合のたびに関係する試験を流した。台帳は群 A〜E の直しに合わせて 156 行を直した（de0f5e9）。
  - 群 A: ネームの承認の後、承認できない actor（ai:・legacy:unknown・agent の権限）のページの大きさの op がコマ割りを動かすのを断る。ネームの承認の前に、PC の画素の op（40 個、`core::strict_drawing_ops`）で印刷される層に描くのを人も含めて断る（今の raster_edit_op と同じ規則。未公開の機能）。SO の中身を替える op は印刷される SO の絵が変わるなら断る。exportable の判定に高精度・インキの層。actions.json が読めないときは退けてから書く。
  - 群 B: ビデオレイヤーとアニメのカメラを、判型・回転・反転・任意の角度・角度付きの切り抜き・縦横の倍率が違う画像解像度で写す（video@1 の行に任意の `transform`。恒等や畳み込める形なら書かないので、回していない本は形も保存のバイトも前と同じ。古い版は transform のある本を読み取り専用で開く。schema-v4.md）。ほかの本から来る文字のスタイルを StyleCarry に一本化（.pcraft の中・配置も、群 E）。記録したアクションのバッチで 2 ページ以上の本のページを作る・消す手を断る。duplicate_layers でビデオの行を写す。
  - 群 C: Photomerge・スタック・統計・HDR・画像を開く道で、本の高精度の上限（1 GiB）を作る前に確かめる。SO のスタックの作り方を置き場の予算に入れる。データセットの画像を素材にする。CSV の外の画像の確かめ。
  - 群 D: 長い作業を窓のスレッドの外へ、中止の数と一覧、窓の破棄、ffmpeg とプラグインは親が終われば止まる（Linux は PR_SET_PDEATHSIG）、リンクの大きなファイルを読まずに確かめる。
  - 群 E: .pcraft のレイヤースタイルは効果ごとに読めない値を外して知らせる（全体が読めないときは前の振る舞い）、カンプの状態ごとに捕まえる、照らし直し、psd_raw・lmfx の扱い。
- **台帳**: platform:Linux を閉じた（上の配布物）。台帳の残りは platform:Windows（deferred、配布前の実機受入は必須）と対象外 2 行だけ。
- **利用者の決定（PC6）**:
  - 決定 16（2026-11-10、strict_gates）: 承認できない actor（HTTP の `/v1/apply` の ai: のトークン、`genko apply` の legacy:unknown・`--agent ai:…`）が `set_meta` で本の strict_gates を外せ、外した後は名前の承認の関門が当たらない。前からの振る舞いで、Python と比べる契約試験 test_contract_ops の randomSequences もこれを合格の条件にしている。**今のまま、既知の制限として記録する**（SPEC AUTO-01 と台帳の関門の行の limitations）。MCP と制御チャンネルからは set_meta の strict_gates に届かない。
- **既知の制限・残課題（PC6 の出口の時点）**:
  - Windows（deferred、配布前の実機受入は必須。PC4・PC5 の宿題はそのまま。ffmpeg とプラグインの「親が終われば止まる」は Linux だけ。Windows の 6 GiB 級の疎なファイルの試験は 257 MiB の形に替えた）。
  - SO の中身（"contents"）の道は strict_gates を外して当てる（前から。fill なども同じ）。
  - features だけの横書きの台詞（native_type なし）を基本段落の違う本へ持ち込むと、持ち込んだ本の基本段落で組む（見え方を保つには描き手の側の直しが要る。SPEC）。
  - 細かい物（P3、直していない）: アートボードの箱がフッテージの箱を見ない、smart_convert が祖先をたどらない、置き場の予算と 1 枚の上限の間の大きさのスタックが 2 つあると交互に追い出す、部分の道の結果を WorkHold に数えない、needs_whole_page の中身で帯ごとに全体を作る、中を指す絶対パスの CSV も断る、終了で進み具合の窓が先に閉じる、コンタクトシートの途中で窓を閉じると作りかけのシートを開いてから本ごとに保存を聞く、bytes の無いリンクで窓（読む）と CLI（modified）が違う、浮動小数の完全一致で比べる所がある、カンプのスタイルで "value" 以外の core::Error はカンプの読みが断る、グレーの PSD の Blend If の名前 "gray" が合成と重なる。
  - 保存していない原稿の 変数データセット… は、画像が合わせて 256 MiB を越えると断る（保存すれば 4 GiB まで）。
- **次**: native/integration へ統合（この節の commit の後）。Windows の実機の build・受入（windows-debug・windows-release と、それを含む `--pair`）と配布は、利用者の決定（2026-10-15）のとおりマージ後に別の環境で。

### main への統合と Python 版の削除（2026-11-11）
- **利用者の決定 17（2026-11-11）**: Python 版は安定版として扱わない。native/integration を main にマージし、今の版（C++）を最新の版とする。README には Python 版から移行したことだけを書く（c12298b）。main の CI を止めた形（.github/disabled-workflows）は保ち、native.yml も同じ場所に置いた。
- **利用者の決定 18（2026-11-11）**: Python 版のソースを main から消す。Python 版と突き合わせる試験もやめる。コマンドと文書は今の版（`genko …`）に書き換え、Python 版に合わせることはもうしない。
- したこと（ブランチ remove-python）:
  - C++ 版が使う資源（UI の書体・本文の書体とライセンス・lucide のアイコン・studio の layouts.json・vocab.json・手引き）を native/resources へ移した。
  - src/・tests/（Python の試験）・pyproject.toml・uv.lock・Python の試験の workflow、Python 版を import する tools/migration の harness・tools/acceptance の確認を消した。
  - Python 版を動かして比べていた契約試験 33 本と比較の関数を外した。比較に埋もれていた C++ 版だけの検査は、元の期待値のまま戻した。
    - 新しい試験: e2e/test_refusals_e2e（数でない値・壊れた本の断り 64 件、人だけの操作、壊れた絵）、unit/test_ai_link。
    - 既存の試験への追加: test_save_e2e・test_migrate_e2e（固定の v2 の本）・test_cli_e2e（studio の 101 段）・test_gui_ledger_a・test_contract_abr・render_cli・plugins・anim・export_cli・pipeline。
  - 外の道具の試験（Pillow・psd-tools・MCP の SDK・上流 PhotoCraft の pcraft の検査）と、前に記録した固定データとの比較は残した。
- 確かめたこと:
  - Release を一から組んで、ctest 268 本すべて合格（test_soak_app 7,106 秒を含む）。
  - tools/ci の試験 68 本合格。
  - 配布物 genko-studio-0.1.0-linux-x86_64.tar.gz（sha256 `da608443691715d6d407b9f96d865a0e47e6c0c4d3aa94651d57e9159f31b66e`）のファイルの一覧は、消す前と同じ。
  - まっさらな Ubuntu 24.04 の容器への導入（PC6 の出口と同じ手順）は「ALL OK」。開き直した 8 ページの画素も同じ。
  - 製品のコード（native/src）の変更はコメントだけ。
- 残り:
  - wording::error が日本語にしなかった文言 102 件は、利用者の指示（2026-11-11）ですべて自然な日本語の表示にした（wording.cpp の add_this_builds_refusals ほか）。許す一覧 errors_not_in_japanese.json は外し、test_every_error_reads_in_japanese は 1 件でも英語が残れば落ちる元の厳しさに戻した。CLI・MCP・HTTP の JSON の code と英語の error の文は変えていない。あわせて「ふつうの数」を「有限の数値」、「ふつうのファイル」を「通常のファイル」にそろえた。
  - render_memory_check（cgroup の上限での描画）と raster の put_raster の段の断り（壊れた JPEG・PSD の zip）は戻していない。
  - contract の試験は Windows ではまだ走らない（Windows の実機の受入と一緒に）。

### PC5 の設計と利用者の決定（2026-10-28、PC4 の出口の後に読取専用で）
- 設計: 台帳の残りの 338 行すべて（PC5 が最後の工程で、行を後へ回せない）を 4 つの節に。G 文字（74 行: 文字レイヤー・文字ツール・組版と OpenType・右から左・文字／段落スタイル・パネル・スペルチェックと表記の点検・検索と置換・書体の置き換え・PSD と .pcraft の文字、約 69.5 人日）、H 表示・窓・環境設定（約 111 行: 画素の段と % の意味・ピクセルグリッド・32 bit のプレビュー・手のひら・スクリーンモード・ガイドとグリッドと単位・窓の区画と並べ方・パネル・ワークスペース・キーの組・メニュー・ツールバー・環境設定と API・取り消しとメモリをクリア・画素の縦横比・GPU と CPU の表示の一致、約 61 人日）、I レイヤー・アートボード・自動処理（61 行: レイヤーの基本の残り・グループとフレーム・アートボード・変数とデータセット・バッチ／ドロップレット／スクリプト／イベント／画像プロセッサー・プリント・コンタクトシート・画像アセット・自動整列／自動合成／Photomerge／スタック／統計・HDR Pro・写真の切り抜き、約 73.5 人日）、J 制作の補助（92 行: レイヤーカンプ・スライス・注釈・ものさし／カウント／計測・計測ログ・タイムライン・ビデオレイヤー・WASM のプラグイン・制御チャンネル・アクション、約 99.5 人日）。PC4 の出口の決定 4 つと tools/ci の直し（Z0、約 3 人日）。計約 306.5 人日。土台 PC5-0（Z0・G-0・H-0・I-0、続けて J-0）の後、4 つずつ 7 つの波。新しいライブラリ: WAMR（Apache-2.0 WITH LLVM-exception）・SheenBidi（Apache-2.0）・英語の辞書 SCOWL。GPL・FFmpeg・ML のモデル・新しい費用は要らない。新しい拡張: type@1・groups@1・image-assets@1・layer-comps@1・slices@1・annotations@1・measure@1・timeline@1・video@1・pixel-aspect@1。
- 利用者の決定（PC4 の出口の報告で）: (1) genko review の HTML の「書き出し…」を今のメニュー名に直す。(2) ops_schema の glow の説明文を今のフィルター メニューの名前に（op・引数は変えない）。(3) 32 bit の PSD は線形の値で書く（読むときも対に）。(4) 16・32 bit のページで新しく作る層はページの深さ（add_layer の既定が変わる）。
- 利用者の決定（PC5 の設計で）: (1) 文字レイヤーは層の種類「文字」を足し、中身と組版は台詞と同じ（縦書き・ルビも効く。台詞と行き来できる。台詞は今のまま）。(2) 新しく作るグループと PSD・.pcraft から読んだグループを Photoshop のグループとして描く（今の原稿のフォルダーは今の見え方のまま、1 手で切り替えられる）。(3) アートボード＝ページ。(4) 動画ファイル（MP4・MOV・WebM）は読まない（連番と動く画像だけ。MP4 の書き出しは今のまま）。(5) スクリプトは操作（op）の手順だけ。(6) 制御チャンネルは見る＋MCP と同じ関門の op＋キャンバスへのペンの入力（窓・メニュー・承認・パネルには届かない。既定は切、持ち主だけのローカル接続）。(7) 複数の原稿は 1 つの窓の中の区画に並べる。(8) 既定のキーを Photoshop の組にする（今のキーは「Genko」の組として残して選べる）。(9) 右から左の文字は横組みで並べ替える（SheenBidi）。(10) 画素の縦横比は表示だけの補正を作る。(11) 表示倍率の % はページの画素の倍率（100%＝画素等倍）。(12) 表示の「目盛り」を「定規」に、描く定規の表示は「描画定規」に。
- 調整役の判断（推奨どおり）: 横書きの OpenType の台詞だけ OpenType で組む（今描けていない文字が描ける。縦書きは今のまま）。スペルチェックは英語の辞書と利用者の表記の表。台詞パネルの書式の欄は文字・段落パネルへ。メモリをクリアは記憶の中だけ（原稿の取り消しは残る）。合成は CPU が正本、GPU は表示だけ。AI（MCP）は環境設定を読むだけ。画素を仮想記憶ディスクに退避しない。単位は定規・ガイド・グリッドなどに効き、漫画の道具の mm の欄はそのまま。MCP・HTTP から自動処理は使えない。WASM のプラグインは別プロセスで WAMR、既定無効・明示選択・sha256 の同意。設計の控えは scratchpad の pc5/design.md と節のファイル。

### 群（R1〜R7 のとおり: 群の中は 実装（関係する試験）→レビュー 1 回→直し。群ごとに一つの工程とし、その出口で V3 1 回・統合）
- PC0 照合: 961 行を今の C++ と上流の実装（crates/…）で突き合わせ、行ごとに「同等（C++ のソースと試験を示す）／一部（足りないものを書く）／ない」を feature-ledger.json の既存の欄に書く。以後の群の範囲はこれで決める。
- PC1 色・精度の土台（PLAN 工程 2。台帳の中心 56 行: image.mode.*・channel.*・document.*・プロファイルの指定と変換・カラー設定・色校正・色域警告・document:color-depth-model）: 8/16/32 ビット・float/HDR、色モデル（Gray・Indexed・Bitmap・Duotone・Multichannel を含む）、任意のチャンネル、文書のプロファイル・ソフトプルーフ・色域警告、保存・Undo・復旧・GC。
- PC2 補正・色管理（工程 3）。
- PC3 非破壊（スマートオブジェクト・スマートフィルター）、選択・修復、パス・変形（工程 4）。
- PC4 ブラシ・フィルター群、RAW・形式の互換（工程 5。.pcraft の読み書きもここ）。新しい外部ライブラリが要る形式は、ライセンスを THIRD_PARTY.md・SBOM に記録して入れる（GPL など配布の条件が変わるものは止めて確かめる）。
- PC5 制作の補助・バッチ・タイムライン・プラグイン・JSON の制御（工程 6。MCP の承認を迂回する別の入口は作らない）。
- PC6 出口: 台帳全件の同等性、独立レビュー、V3（Linux 3 構成・実 xcb の GUI・性能・配布物）。Windows は従来どおりマージ後に別の環境で。


## M6 導入・ヘルプ・制作導線・配布候補と M7 の受入（Linux）— 2026-10-14〜16 記録

### 利用者の決定
- （2026-10-14）設定は旧版と分け、初回に尋ねる: この版は自分の設定のフォルダーを持ち、初めての起動で旧版の設定があれば「引き継ぎますか？」と一度だけ聞き、「はい」のときだけ写す。旧版の設定は変えない。
- （2026-10-15）Windows の実機の作業は M7 まで保留。MCP の `inspect` の説明文は Python と同じ文のまま。Qt のソースの入手先は download.qt.io を案内する。
- （2026-10-15）**Windows は、すべての実装と Linux での試験が終わり、マージした後に、別の環境で行う。** このセッションでは Windows の実機の build・受入をしない（VALIDATION.md の「配布前の Windows 実機受入は必須」は変えない。両 OS の配布完了とは言わない）。

### 群（作業ブランチ native/wip-m6。8cc498c・84c2b49・07656c7・c287a75・0794797・b72f851〜f96c119・a7b5be7）
- 群B1 画面の仕上げ: コマンドを探す・クイックアクセス・コマンドバー・ワークスペース、メニューの残り、最初の一歩と次の作業の案内（I11）、パネルの配置、レイヤーパネルの見た目、選択範囲ランチャー、書き出しのプレビューは初めからページ全体（I08）、書き出し先の全文・コピー（I12）。
- 群B2 環境設定と快適さ: ショートカット・筆圧の試し書き・サイドボタン・見た目・カーソル・修飾キー・道具ごとの修飾キー・押している間だけ持ち替え・円形メニュー・休憩・動きを減らす・GPU 表示・バックアップ（zip64 も書ける）・保存の間隔、原稿だけを表示、システムのガラス、数値欄の入力。
- 群B3 持ち越しの機能: オートアクション、画像を読み込む、マスクを編集する、枠線の間隔・長さ・揺れ、本の形でプレビュー、TIFF・WebP の読み込み（Pillow 12 と同じ手順。色を面ごとに分けた TIFF は未移植と言う）。
- 群B4 ヘルプ（I10）: 使い方・キー一覧・よくある質問・バージョン情報を実際のメニューの経路から作る（help.cpp）。
- 群C 設定の分離と引き継ぎ（UX-01）: core::config_dir（~/.config/genko-studio・%APPDATA%\GenkoStudio・settings.ini）、migration.cpp（初回に一度だけ聞く、写さないもの、リンクの規則、ロック、失敗は次回また聞く）、引き継ぐ前はコマンドラインが知らせる。
- 群D 配布物（I03・AC-PACKAGE）: cmake --install と cpack（GenkoPackage.cmake）、SBOM（SPDX 2.3、SHA1・SHA256）、README-ja・QT-LGPL-ja、Linux の install.sh・uninstall.sh、THIRD_PARTY.md v1.1（ICU・xcb-util・MSVC ランタイム・PhotoCraft）、ライセンス文。
- レビュー（群ごと 1 回、4 本並行）の指摘は c287a75 で直した。

### 関係試験（R1）で直した所（a7b5be7）
- 31 本（関係する GUI・契約・単体・配布）のうち 10 本が落ちた。製品の直し: 最初の一歩・次の作業の案内の帯が窓を画面より広げていた（横にスクロールする 1 行の帯に）、クイックアクセスをスクロールの中に（最小の高さ）、ごく小さい画面のキャンバスの最小幅の下限 160→100px、窓の組み立て中に題名の帯を外す前のレイアウトで窓が高く広がったまま残る（最後に決め直して画面に収め直す）、ブラシの詳細の窓を共通の部品で（UI-G）、キャンバスだけ表示で端に出したパネルが離れても隠れない（タブの裏のパネルを除いて端を測る。Python も同じ書き方）、レイヤーの目のアイコン、PA の画像のパレット、配布物に offscreen のプラグイン。
- 試験の前提の誤り（合格の基準は変えていない）: パネルが見えているかを visibleRegion で見ていた（題名の帯を外すと中身が覆うので空）→ 窓の中にあるか、短い窓のパレットの試験を Python の test_every_tool_is_within_reach_on_a_short_screen と同じ 1366×700 に、メニューの並び・ツールチップの形・コマンドの数を Python に、ほか。
- 31 本すべて合格（アニメの試験は 5 回続けて合格）。

### M7 の 2 時間試験を M6 の候補に入れた（R6）
- M7 は M6 の出口の V3 と同じソースなら全体試験を使い回す（R6）。M7 に固有の「2 時間の連続制作（入力の注入）」と PERF-G（メモリと安定性）の試験 `test_soak_app`（perf の印。V3 の全体試験 132 本には入らない）を、M6 の凍結の前に入れた。F1 を主の窓で開き、Xvfb の上で G ペンの線・取り消し・やり直し・ページ送り・拡大縮小・台詞を実時間で注入する。最初の 20 分で全ページを回ってキャッシュを満たし、その後 100 分を 2 分ごとの巡回（90 秒の作業、描画のキャッシュの解放、30 秒の休み、私用メモリ（Private_Clean＋Private_Dirty）の読み取り）にする。判定: VmHWM 8 GB 以内、最後の 10 分の平均が最初の 10 分の平均から 128 MiB 以内、最小二乗の増え方 1 MiB/分 以下、描いた線・取り消し・やり直し・台詞の数がページごとに窓の本と一致し、閉じて読み直した本とも一致。
- 試しで見つけた漏れ（製品の直し）: 取り消しの履歴は、メモリに置く量を「精密なカラー画像の分で 1 GiB まで」と数えていて、線の変更（その前のページの版を持つ）を 0 と数えていた。線を描く作業では履歴がいつまでもメモリに残り、暖機の後も 1 分あたり約 7.5 MiB 増えた（F1、40 分の試し）。Python は保存した変更をメモリに置かない（取り消しは日誌を通す）。C++ はメモリに置く変更を直近 100 件まで（Session::Options::history_changes）にし、古いものは既存の仕組み（trim_history、日誌を通す取り消し・やり直し）へ回す。試験: test_session aLongLineHistoryLeavesMemory（上限 3 件で 6 本の線を引き、保存の後メモリに 3 件、6 回の取り消しが日誌を通って本と画面が 1 本ずつ戻り、6 回のやり直しで元に戻る）。
- 履歴の上限の後も 1 分あたり約 2.2 MiB 増えた（40 分の試し: 巡回 25 分で 29.8 MiB）。巡回ごとに C ライブラリの割り当て器の内訳を記録すると、プログラムが使っている分は 172→175 MiB でほぼ一定、増えたのは「解放済みで割り当て器が手元に残している分」（99→124 MiB）だった（漏れではなく、描画の作業スレッドなどが返した領域を OS に返していない）。製品の直し（app/memory.cpp、画面のアプリだけ。コマンドライン・MCP は変えない）: (1) `genko app` の始まり（スレッドを作る前）に一度、glibc の割り当て器を arena 2 つまで（M_ARENA_MAX。描画の作業スレッドが領域を分け合う）、256 KiB 以上は mmap（M_MMAP_THRESHOLD。タイル・絵は使い終われば OS に返る）にする（memory::tune）、(2) 入力が 10 秒止まったら一度、割り当て器の空きを OS に返す（Linux は malloc_trim、Windows は _heapmin。memory::watch_idle）。キャッシュ・履歴は捨てない。(2) だけでは水準は下がった（305→223 MiB）が増え方は残った。(1) を主の窓を作るときに設定すると、作業スレッドがすでに arena を作った後で効かず（傾き 6.1 MiB/分）、同じ値を GLIBC_TUNABLES（プロセスの始まりから効く）で与えると 19 分の試しで傾き 0.80 MiB/分・増分 6.3 MiB だったので、プログラムの始まりで設定する形にした。性能の試験（test_perf_app・test_soak_app）も、アプリと同じく始まりで設定する。

- 2 時間の試し（凍結前、試験の木 fab4aea＋直し、2026-10-15 09:39〜11:43 UTC）は不合格だった。暖機の後の巡回 30 分までは「メモリに置く変更」が上限の 100 件で私用メモリ 258〜375 MiB だったが、巡回 34 分（開始から 54 分）から変更が 1 巡回あたり約 58 件ずつ溜まり（98 分で 668 件）、私用メモリは 531 MiB、VmHWM 633 MiB まで増え、最後の「保存済みになるまで 5 分待つ」で落ちた。原因（製品の漏れ）: 保存の 1 歩ごとに原稿の journal（studio/journal.jsonl。試しの終わりで 38 MB・6,736 行）を 2 回（修復と取引の確認）全部読み直して JSON を解いていた（1 回 1〜2 秒。`genko doctor` で 38 MB の読みに 2.3 秒）。journal が 15〜20 MB を超えると保存が描く速さ（1.5 秒に 1 本）に追いつかず、保存を待つ変更がメモリに残った（履歴の上限は保存済みの変更だけを外す）。Python の journal.entries は読んだ行を覚えていて、ファイルが伸びた分だけを解く（_PARSED。先頭と前回の終わりの 64 バイトで同じファイルか確かめる）。直し: `storage::journal::read_lines_without_ops`（同じ確かめ方で、伸びた分の行だけを解いて覚える。覚えるのは各行から ops（変更の操作。journal の大部分）を除いたもの: 修復・取引・やり直しの積み・版の番号はそれを読まない）を、保存のたびに読む所（修復、Saver の取引の確認、stacks、Session の committed_revision・読み直し）で使う。ops を読む所（履歴パネル・gc・migrate・doctor）は今までの read_lines。試験: test_journal readLinesWithoutOps（ops を除く・伸びた分だけ・書き直されたら全部・短くなったら全部）。
- journal の直しの後の 60 分の試し（暖機 20 分＋巡回 40 分、試しの木の Release、2026-10-15 13:12〜14:12 UTC）: メモリに置く変更は終わりまで 100 件、保存の遅れはなくなった。私用メモリは巡回 30 分まで 219〜232 MiB で横ばいだったが、巡回 32 分で 296 MiB に上がってそのまま残り、傾き（1 MiB/分以下）で不合格。arena ごとの内訳（試験に GENKO_SOAK_TRACE を足して malloc_info と 16 MiB 以上の無名の割り当てを巡回ごとに記録した 2 回目の 60 分の試し）: 作業のスレッドの arena は確保 106 MiB・空き 62 MiB で一定のまま、その 2 つの領域（63 MiB・49 MiB）が巡回 15・16・18・20 では丸ごと常駐し、17 では元に戻っていた（漏れではない）。入力が 10 秒止まったときに一度だけ空きを返していたので、その後に終わる裏の仕事（キャッシュを消した後のページの精細な描き直し — 線が増えた後半ほど長い）が汚した空きが、次の入力の後の休みまで残っていた。直し（app/memory.cpp）: 入力が止まっている間は、10 秒の後も 5 秒ごとに空きを返す（止まっている間だけ。描く間は何もしない）。あわせて、journal の覚えは解いた値（小さな割り当て数万個が保存のスレッドの arena に散らばり残る）でなく、ops を除いた行の文字列 1 本で持つ（読むたびに解く。2 時間の終わりで約 1.7 MB）。
- 原稿のフォルダーの大きさ: 2 時間で 4.3 GB（assets 4.2 GB・6,300 ファイル）。描くたびに、その層の線の全部（strokes.json、1 本で約 1.6 MB）を新しい資産として書き、取り消しのために残す。Python の v3 と同じ作り（変更ごとの資産、gc は手動）で、この版の不具合ではない（知られた性質として記録する。`genko gc` で減らせる）。

### M7 群（移植の漏れ・試験の漏れ・独立レビューの直し・PERF-G）の R2 レビュー（1 回、2 並行、読取専用）と直し
- app・配布の側: P1×2・P2×1・P3×2。
  - P1 採用した復旧用コピーを、原稿に書く前に取り消すと（頁の読み込み中、または別の書き手が原稿を持っていて保存が断られた間）、今回の「やり直しの欠落」の直しが取り消しを journal に書く側に回し、保存で復旧の内容が原稿に書かれ、その取り消しは journal が断り、読み直しで復旧の内容が戻っていた → 復旧の採用（recover）は書く前なら必ずメモリだけで取り消す。試験 test_session anAdoptedRecoveryPointUndoneBeforeItIsWrittenIsNeverWritten。
  - P1 復旧用コピーを別のスレッドにした（バックアップ中も止まらない）ため、保存が先に終わって「原稿がすべてを持った」後に、書き終わった復旧用コピーが残り、次に開いたとき原稿より新しいとして勧められ得た → 保存（別名保存・閉じるときも）が終わったとき書き中の復旧用コピーがあれば、その後に消す（復旧のスレッドは順に動く）。その書き終わりは「書いた」と数えない。試験 aRecoveryPointWrittenBehindASaveIsRemoved（20 回、両方の順番）。
  - P2 最初の一歩のボタンのキーが起動のときの既定のまま（環境設定のキーの前に作られる）→ 案内を出し直すたびと環境設定の適用のときに今のキーを出す。
  - P3 ストックのメニューの子のメニューが開くたびに残る → 作り直す前に消す。復旧の採用の所で「元に戻す」が押せる → 押せない。
- core・storage・描画・API の側: P1×1・P2×4・P3×9。
  - P1 journal の読みの覚え（PERF-G の直し）の鍵が Windows で ANSI にない文字のパスで例外 → UTF-8 の鍵。
  - P2 フォントの代わり（G5）を汎用の読み込み（ImageFont.truetype 相当）に入れていたため、render._font（次の候補へ進む）とフォントの一覧（壊れたファイルを飛ばす）が変わった → 代わりは genko.fonts.truetype 相当（text::Fonts）だけ。
  - P2 面ごとの TIFF で、Pillow が除く「意味の決まっていない追加の面」も読んでいた（色がずれる、大きな数の面でメモリを使い切る）→ 画像の帯の数だけ読む。YCbCr（JPEG の面ごと）は Pillow と同じく libtiff の RGBA の読み、1 つの面だけのものは続いた形と同じに読む。
  - P2 絵柄カタログのトークンが転送（リダイレクト）先の別のホストに付いて行き得た → トークンを付けるときは転送を自分でたどり、別のホストへはトークンなしで（最大 50 回、https から http へは行かない）。
  - P2 `genko render --out 新しいフォルダー/p1.webp` が失敗 → 書く前にフォルダーを作る（Python と同じ順）。
  - P3: .dib はファイルの見出しなし（_dib_save）、BMP の 4 GiB 超は ValueError、Pillow だけの形式の見分けを各プラグインの _accept に合わせた（netpbm P0〜P6・Pf・Py、CUR、AVIF の mif1・msf1）、文字の大きさを測る所のフォントの上限も 256 MB、journal の読みの覚えは解き終わってから入れ替える（例外で半端に残らない）・解く間は鍵を持たない、無圧縮 TIFF は 1 つのストリップ（Pillow が自分で書くとき）。
  - 直さずに記録: doctor の問題の並び（同梱のフォントがないときだけ。Python は資産・フォント・パスの順、この版はフォントが最後）。無圧縮 TIFF のバイトの並び（画素とストリップは同じ。libtiff はデータを目次の前に書く）。

### M7 群の関係試験（R1、作業の木の Release、4 CPU・15 GB・Ubuntu 24.04.5）
- 43 本（変えた所に関係する単体・契約・E2E・GUI・配布と、新しい試験 6 本）: 36 本合格、7 本不合格。
- 製品の直し:
  - (1) 自分自身を指すリンク（ループ）を `--root` に与えた `genko serve` が断らずに動き続けた（今回の resolve_path の直しの退行。Python 3.12 の Path.resolve はループで RuntimeError）→ HTTP の場所の確かめはループを断る（core::symlink_loop）。
  - (2) お知らせを出している間、回転・反転の表示（表示 46% ・ 回転 +30° ・ 左右反転）とコマンドの状態が更新されなかった → お知らせの間も更新する（Python の _refresh_zoom と同じ）。
  - (3) 1024×640 で台詞パネル（フキダシの種類の選択が 211 px）と文字の道具の設定がパネルの幅（230 px）からはみ出した。Python は右のパネルとツールの設定のすべてに tool_settings.fit_narrow を掛ける（選択は 6 字ぶんに縮んで開くと広い、長いラベルは折り返す、入力行は狭ければ欄を名前の下に、枠の注記は題の下に、ボタンは文字の幅）が、C++ にはなかった（移植の漏れ）→ app::fit_narrow を移し、同じパネルに掛ける。
- 試験の前提の誤り（合格の基準は変えていない）:
  - test_placed: 読めない形式の画像の見本を PPM にしたが、PPM は配置のときに断る（大きさが読めない。記録した制限どおり）→ 配置は image_format で断り、Python が置いた本の PPM は描かずに知らせる、の 2 つに分けた。
  - test_ops_e2e: 下地混色を「この版ではしない」例にしていた → 移したので、適用して本と inspect が一致することを確かめる。
  - test_gui_workspace: 2 つ目の窓を作った後に環境設定を確かめ、答えの記録が 2 つ目の窓に移っていた → その前に確かめる。
  - test_gui_actions: 台詞のあるページへのテンプレートは「コマと台詞を消して、テンプレートで割り直します」と先に尋ねる（Python と同じ）→ はいと答える。
  - test_gui_ledger_a: 読み直した本の余白（Python の読み込みと同じく 16.0）と既定（16）を JSON の文字で比べていた → Python の == と同じく値で比べる。
- test_session theAutosaveWaitsForAPauseAndAtMostTenSeconds は重い並行の中の 1 回だけ「20 秒以内に保存済み」にならなかった。単独で 4 回・4 本同時でも合格。凍結後の V3 の 3 構成の全体試験で合格（合格の基準は変えていない）。

### 台帳の突き合わせ（LEDGER.md の M6 の 20 行）
- gui_action 11（act_prefs・act_help_keys・act_help_guide・act_help_faq・act_about・act_find_command・act_edit_commandbar・act_edit_quick・act_save_workspace・act_tool_names・act_quick_mask）、gui_action_group 1（launcher_actions）、cli 1（genko register-links。src/api/main.cpp の入口）、python_module 7（comfort・dialog_look・glass・help・links・preferences・workspace）: すべて C++ にあり、試験（test_gui_actions・test_gui_carried・test_gui_workspace・test_gui_look・test_gui_select・test_gui_studio）がある。ledger.json の status は手で変えない（生成物。M7 の機械集計で）。

### V3（M6 の出口、M7 は R6 で使い回す）— 凍結 962faa4 で Linux 合格
- **凍結SHA**: `962faa4f87dd7dbe05908937e2e54d3ff3488e29`（native/wip-m7-freeze に push 済み）。計画は `validation_policy.py --base e099da5 --head 962faa4 --phase milestone`（基点は CMYK・Lab の工程の出口の凍結。tier full、plan_hash `deb05491…`、構成は linux-debug・linux-release・linux-asan・windows-debug・windows-release）。2026-10-16 01:11〜07:38 UTC。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44-fc-v139。Qt 6.11.2、GCC 13.3。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON（mcp 2.2.0）、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。R3 のとおり Release と ASan を同時に始め、Debug はビルドが済み次第並べた（各構成内 ctest 1 並列）。
- **結果**（validation_run.py の result.json。3 構成とも status finished・passed true・failures 空、除いた契約なし、CLI23 合格、同じ plan_hash）:
  - linux-release: 132/132 合格（01:13–02:33 UTC）。故障注入が Debug・ASan だけのための SKIP 33（test_transaction 10・test_save_e2e 16・test_session 3・test_contract_save 1・test_migrate_e2e 1・test_gui_save 1・test_app_e2e 1。これまでの出口と同じ組）は合格に数えない。
  - linux-debug: 132/132 合格、SKIP 0（01:28–03:14）。
  - linux-asan: 132/132 合格、サニタイザの指摘なし、SKIP 0（01:13–05:25）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で test_gui_* 28 本すべて合格（actions 14・ai 62・anim 8・book 32・brushes 8・canvas 78・carried 11・color 60・colours 4・effects 15・guides 7・layers 12・layout 32・ledger_a 34・ledger_b 39・ledger_c 27・lines 20・look 42・materials 36・output 23・paint 11・paper 14・pen 51・ruby 18・save は故障注入の SKIP・select 13・studio 29・workspace 10）。Debug で test_gui_save 36・test_app_e2e 8（開く→描く→Undo→保存→終了→別プロセスで再開）。IME の注入（QInputMethodEvent: test_gui_ruby・test_gui_layout・test_gui_lines・test_gui_studio）もこの中で合格。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線、静かな機械: 始まりの負荷 1.04。試験は表示のみで合否判定なし。測定機は基準クラスではない＝ACCEPTANCE §6、基準クラスでの合否とは呼ばない）: 開いて最初の頁 626 ms・全頁 805 ms／入力→ライブ線 p95 1.06・p99 3.65 ms、入力→描画 p95 1.79 ms（n=43780）／線確定→表示 p95 18.9 ms／Undo→表示 p95 38.2 ms／頁切替 最初 p95 32.3・精細 p95 113 ms（n=4）／自動保存 p95 1.64 s（n=31）／保存中（計 25.3 s）のイベントループ: 5 ms ごとの刻みの遅れ p99 5.71 ms、250 ms を超える停止 0、4 ms を超えた停止だけの分布では p99・最大とも 54.4 ms（n=65。前の出口の記録はこの分布で 46.1 ms）。test_perf_render: F1 の頁 350 dpi 全体 796 ms・線を足した後 431 ms・512×512 の部分 27 ms、線 200 本の小さな絵 4 ms。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり。測定機は基準クラスではない）: F3 の工程 59.4 秒・88 作業。全呼び出し n=314 p95 810 ms・p99 1360 ms（MCP の道具 306 件 p95 777 ms、人の `genko studio` 8 件 p95 5.45 s、書き出し 4 件 最大 7.34 s を含む）／情報取得 n=100 p95 681 ms／編集の確定 p95 87.9 ms／候補の採用 p95 1.40 s／頁の仕上げ p95 799 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回: 呼び出しから PDF の完成まで 6.82〜7.33 s（各 30 秒以内。仕事の記録は呼び出しから開始まで最大 6.2 ms）。**全項目合格**（ctest exit 0）。
- **PERF-G（M7、2 時間の連続制作、test_soak_app）**: 実 xcb で 118.2 分（暖機 20 分＋2 分ごとの巡回 49 回、30 秒の休み）。線 3729 本・取り消し 372・やり直し 186・台詞 62、問題 0、窓の本と閉じて読み直した本の違い 0 頁。VmHWM 407 MiB（≤ 8 GB）、私用メモリ 最初 218 → 最後 248 MiB、増分 29.8 MiB（≤ 128）、傾き 0.365 MiB/分（≤ 1）。**合格**。
- **配布物（AC-PACKAGE、M7 の最終配布物での確かめ）**: 同じ Release のビルドから cmake --install → genko-studio-0.1.0-linux-x86_64.tar.gz（58 MB）。docker の素の Ubuntu 24.04.5（python・cmake・gcc・qmake なし、管理者権限なしの利用者）で: 導入（~/.local/opt）→ --version → 引き継ぐ前はコマンドラインが知らせる → 起動してマウスで描いて終了 → 線が保存された → 開き直して同じ → PNG・PDF の書き出し → MCP（stdio の initialize・tools/list、道具 41 件）→ **全 14 形式**（png・tiff・pdf・strip・psd・epub・pack・cmyk・layers・webtoon・sns・kindle・timelapse・animation、31 ファイル）→ 設定は ~/.config/genko-studio だけ → 外す（原稿・設定・旧版の設定は残る）: すべて合格。別の場所に展開した配布物の `genko --version` 合格、その genko に対する test_contract_mcp（公式 MCP SDK のクライアントで stdio・HTTP、11 項目）合格。
- **台帳**: `build_ledger.py --check` 一致（763 行: ported 757・in_progress 3・not_applicable 3。in_progress は Pillow だけの画像形式の 3 モジュール。下の利用者の判断待ち）。
- **V3 の中で起きたこと（R4: 1 回目・2 回目の記録は使わない）**:
  - 1 回目（凍結 108b515、2026-10-15 16:24〜19:13 UTC）: Release 132/132・CLI23 合格、ASan 49 本・Debug 52 本まで失敗なし。実 xcb の GUI（Release 28 本）で test_gui_workspace の pythonsMenus だけが落ちた（Ctrl+G でフォルダができない）。試験の書き方の誤り: activateWindow の直後にキーを送っていて、実 xcb では窓が作業中になる前のキーがショートカットに届かない（offscreen ではすぐ作業中になる）。ほかの GUI 試験と同じく qWaitForWindowActive で待つ形に直した（4f04b86。製品・合格の基準は変えていない）。直した試験は実 xcb で 3 回・offscreen で 1 回合格。R4 に従って ASan・Debug を止め、3 構成を初めからやり直した（1 回目の記録は使わない）。
  - 2 回目（凍結 4f04b86、2026-10-15 19:16〜10-16 01:00 UTC）: Release 132/132・CLI23、Debug 132/132・CLI23、実 xcb の GUI（Release 28 本・Debug の保存の GUI と E2E）は合格。ASan で 4 本が落ちた（M6・M7 で入った GUI・配布の試験は、これまで ASan で流していなかった）。
    - test_gui_carried（解放後使用、試験のコード）: 読み直した本の一時オブジェクトの部分を範囲 for で回していた → 本を変数に持つ。
    - test_gui_look（LeakSanitizer、416 バイト）: OpenGL の文脈を作れない画面（offscreen）で、Qt の QOpenGLWidget は大きさの変化・表示のたびに文脈を作り直し、前のものを解放しない（QOpenGLWidgetPrivate::initialize）。製品の直し（app/canvas_gpu.cpp）: 失敗した後は作り直さない（1 回だけ試す）。製品は gpu_available() で文脈が作れるときだけ GPU 表示にするので、利用者の動きは変わらない。漏れの抑止（suppression）は使っていない。
    - test_package（ASan の構成だけ）: サニタイザーの構成では Genko のライブラリが共有ライブラリで（GenkoFlags.cmake）、Qt の配布の道具が lib/ に写したそれにビルドのフォルダーの探し先（RUNPATH）が残り、libpillow_imaging.so をビルドのフォルダーから読んでいた。SBOM もそれらを数えていなかった。直し: 導入のとき写した libgenko_*.so の RUNPATH を $ORIGIN に（GenkoPackage.cmake）、SBOM は libgenko_*.so を Genko Studio、libpillow_imaging.so を Pillow libImaging の部品として数える（package/sbom.cmake）、test_package はその構成でだけそれらを lib/ に許す（Release・Debug と配布物の確かめは今までどおり Qt・ICU・xcb だけ）。配布物（Release、静的）は変わらない。
    - test_gui_actions（解放後使用、製品）: 自分のテンプレートの割り付け（app/templates.cpp の plan）が、根のコマの id を作業中の本の中の文字列への参照のまま build に渡していて、最初の操作で本が置き換わった後にそれを読んでいた（Release では読めないコマの id・落ちることがあり得た）→ id を写してから渡す。
    - 直した 4 本は ASan で合格（test_package は Release でも合格）。凍結 962faa4。R4 に従い、3 構成を初めからやり直した（2 回目の記録は使わない。やり直しの前に機械が再起動した: kernel 6.18.44-fc-v139）。

### M7 の受入（R6: 上の V3 を使い回す。同じソース・実行物）— Linux の分は合格
- 最終配布物での起動・保存・出力: 上の配布物の確かめ（素の Ubuntu 24.04.5、管理者権限なし）で合格。
- 2 時間試験（PERF-G）: 合格（試験の判定、上）。PERF-F: 合格（試験の判定、上）。PERF-A〜E: この開発機で測って記録した（上。表示のみで、基準クラスでの合否とは呼ばない: ACCEPTANCE §6）。
- IME の注入: 実 xcb の GUI（QInputMethodEvent の注入を含む 4 本を含む 28 本）で合格。全出力: 14 形式を配布物で合格。
- MCP SDK での外部接続: 配布物の genko に対して公式 SDK のクライアント（stdio・HTTP）で合格。
- 独立レビュー: M7 群の R2 レビュー（上、2 並行・読取専用）と、凍結前の独立レビューの直し（P1×2・P2・P3）。直さずに記録したのは下の P3 の 2 件だけ。
- 別のインストール先への導入: ~/.local/opt（install.sh）と、別の場所に展開した配布物の両方で合格。
- **Windows の実機の build・受入（windows-debug・windows-release と、それを含む `--pair`）は未実施**（利用者の決定: マージ後に別の環境で）。それが済むまで、両 OS の配布の完了とは言わない。
- **AC-RELEASE（最終判定）はまだ**: Windows の受入と、台帳の in_progress 3 行（Pillow だけの画像形式。下の判断待ち）が残る。

### 既知の制限（M6・M7 の出口の時点）
- Windows の実機の build・受入は未実施（利用者の決定: マージ後に別の環境で）。
- 色を面ごとに分けた TIFF（PlanarConfiguration 2）のうち、1 画素の深さが 8 ビットの倍数でないものは未移植（置けるが描かずに知らせる）。8・16 ビットの面ごと、YCbCr は Pillow と同じに読む。
- Pillow だけが読む画像の形式（netpbm・ICO・CUR・icns・QOI・DDS・SGI・JPEG 2000・AVIF）は置くときに image_format で断り、Python が置いた本のその画像は描かずに知らせる（利用者の決定待ち: 既知の制限として記録を推奨）。
- Lab のページの編集可能な文字の色は RGB で書く。書いた PSD の Photoshop での確認はしていない。
- Qt が中に含む第三者のソフトウェアの帰属表示の全文は配布物に入れていない（QT-LGPL-ja.md で案内）。
- アプリのアイコンはまだない。
- MCP の `inspect` の説明文のプラグインの置き場所の例は旧版の場所のまま（利用者の決定）。
- `genko render --out` の Pillow だけが書く形式（GIF・ICO・PPM・TGA・JPEG 2000 など）は NotYetPorted format:<ext> と言う（PNG・JPEG・TIFF・PDF・WebP・BMP・DIB は書く）。
- 原稿のフォルダーは描くたびに変更ごとの資産で大きくなる（2 時間で 4.3 GB。Python の v3 と同じ作り、`genko gc` で減らせる）。
- R2 レビューで直さずに記録した P3: doctor の問題の並び（同梱のフォントがないときだけ順が違う）、無圧縮 TIFF のバイトの並び（画素とストリップは同じ）。

### 利用者の判断（2026-10-16）
- コマンドラインからの設定の引き継ぎ（`genko settings import`）: **作らない**。引き継ぎはアプリの初回の問いだけ（README-ja の既知の制限に書く）。
- Pillow だけが読む・書く画像の形式: **既知の制限にする**。台帳の 3 行（__main__・materials/__init__・studio/importer）はその理由を注記して閉じる（README-ja にも書く）。
- PhotoCraft: **全 961 行をこの移行に入れる**（Windows の作業の前。下の「PhotoCraft の全機能の取り込み」の節）。
- 既定の起動: **手順書だけ**（README-ja の「この版をふだん使う版にする（Linux）」。配布物・install.sh の既定は変えない）。

### 次
- PhotoCraft の全機能の取り込み（上の節）。そのあと native/integration をマージし、別の環境で Windows（windows-debug・windows-release の全体試験、実機の GUI、配布物の導入、`--pair`）。


## CMYK・Lab の編集（color-mode@1、COLOR-02）— 2026-10-13〜15 記録（M5 の後、M6 の前の独立した工程）

### 工程の出口 V3（e099da5）— Linux で合格
- **凍結SHA**: `e099da5a09ecc1653c1caa4da6f09bafdd921cc8`（群D の統合の記録）。V3 は main tree の HEAD 2404922（その上は docs/ の再計画だけ。固定するフォルダー native・src・tools は凍結と同じことを確かめてから流した）。計画は `validation_policy.py --base 9f97117 --head e099da5 --phase milestone`（基点は M5 出口の凍結。tier full、各 120 試験、性能は別に必須）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44。Qt 6.11.2、GCC 13.3。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、GENKO_MCP_PYTHON（mcp 2.2.0）、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。R3 のとおり ASan を走らせたまま Release・Debug を並べた（各構成内 ctest 1 並列）。
- **結果**（validation_run.py の result.json。3 構成とも failures は空、除いた契約なし、CLI23 合格）:
  - linux-release: 120/120 合格（10/15 00:19–01:47 UTC）。故障注入が Debug・ASan だけのための SKIP 33（test_transaction 10・test_save_e2e 16・test_session 3・test_contract_save 1・test_migrate_e2e 1・test_gui_save 1・test_app_e2e 1。M4・M5 出口と同じ組）は合格に数えない。
  - linux-debug: 120/120 合格、SKIP 0（03:30–05:37）。
  - linux-asan: 120/120 合格、サニタイザ指摘なし、SKIP 0（03:05–08:55）。
  - `--pair`: 失敗は「構成artifactの欠落・重複」1 件のみ＝Windows 2 構成が開発中 deferred。Linux 3 構成どうしの照合は指摘なし。**Windows を含む --pair 合格ではない。**
- **実 xcb（Xvfb 1920×1080）GUI・E2E**（凍結のビルド）: Release で gui_actions 14・gui_ai 62・gui_anim 8・gui_book 32・gui_brushes 8・gui_canvas 78・gui_color 60・gui_colours 4・gui_effects 15・gui_guides 7・gui_layers 12・gui_layout 32・gui_lines 20・gui_materials 36・gui_output 21・gui_paint 11・gui_paper 14・gui_pen 51・gui_ruby 18・gui_select 13・gui_studio 29、Debug で gui_save 36・app_e2e 8、全合格・FAIL 0。Release の gui_save は故障注入が Debug だけのため SKIP 1（Debug で 36 合格）。
- **PERF-F**（Release、test_perf_pipeline、ctest 経由。合否判定あり。測定機は基準クラスではない）: F3 の工程 80.2 秒・88 作業。全呼び出し n=314 p95 1034 ms・p99 1919 ms（除外なし。MCP の道具 306 件 p95 1020 ms、人の `genko studio` 8 件 p95 7.03 s、書き出し 4 件 最大 9.62 s を含む）／情報取得 n=100 p95 862 ms／編集の確定 p95 149 ms／候補の採用 p95 1782 ms／ページの仕上げ p95 991 ms（合格値は各 p95 2 秒）／8 ページの校正出力 10 回: 呼び出しから PDF の完成まで 8.27〜9.32 s（各 30 秒以内。仕事の記録は呼び出しから開始まで最大 8.1 ms）。**全項目合格**（Totals 3 passed、ctest exit 0。08:55 以降、load 1.1〜1.9）。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし）: 開いて最初の頁 1536 ms・全頁 1536 ms／入力→ライブ線 p95 1.56・p99 7.06 ms（n=43780）／線確定→表示 p95 21.8 ms／Undo→表示 p95 37.2 ms／頁切替 最初 p95 26.9・精細 p95 113 ms（n=4）／自動保存 p95 2.34 s／保存中のイベントループの遅れ p99 11.8 ms（250 ms 超 0）。F1頁 350dpi 全描画 1359 ms・1線追加 682 ms・512px部分 68 ms。M5 出口とほぼ同じ。
- **V3 の中で起きたこと（製品の失敗ではない。R4 の環境だけの原因として、その構成だけやり直した）**: (1) ディスクが満杯（Errno 28）になり、Release の test_contract_raster_ops が書けずに失敗し、Debug のビルドが止まった。空きを作って Release を流し直した（上の値）。(2) 機械が再起動し（10/15 03:00 UTC ごろ）、走っていた ASan（70/120）と Debug（53/120）が止まった（失敗なし）。2 構成を初めから流し直した（上の値。止まった分の記録は使わない）。Debug は空きのため、試験の実行物のデバッグ情報を `objcopy --compress-debug-sections=zlib` で圧縮してから流した（同じコード、ninja は作り直すものなしと確認）。
- **CMYK・Lab の工程の出口は Linux で合格**（Windows は開発中 deferred。上の利用者の決定のとおり、マージ後に別の環境で受入）。

### 進め方
- 群ごとに「実装（対象試験）→レビュー1回→凍結→V2（Release＋ASan）→統合」。工程の出口だけ V3。群A（CL-A: 色モードの土台）→群B（描画道具・色パネル・スポイト）→群C（補正・フィルター）→群D（文字・トーン・効果・編集可能な文字の PSD・レイヤースタイル）→出口 V3。
- M6（Windows 実機）は着手前に止めて利用者に確かめる。

### 利用者の決定（2026-10-13）
- 色モードはページごと（RGB・CMYK・Lab。新しいページは本の既定 `color`）。
- この工程で CMYK・Lab のページで全部を使えるようにする（群に分けて順に統合）。
- ICC は原稿に埋め込む（`.icc` の素材、16 MiB まで。CMYK にするとき選ぶ）。
- 精度は 8bit と 16bit（32bit は RGB だけ）。

### V2（群D）— 統合済み（Release は出口の V3 で）
- **凍結SHA**: `a686c8010ea7a86aa3252dcc6d71fec17fc8e040`（native/integration へ早送り。8d7a2a3 の上に 66e5fac・51104ab・26c7336・a686c80）。計画は `validation_policy.py --base 8d7a2a3 --head a686c80 --phase integration`（tier full、各 120 試験、性能は別・必須でない）。
- **実行環境**: 群B と同じ。
- **結果**（validation_run.py の result.json。ASan は failures が空、除いた契約なし）:
  - linux-asan: 120/120 合格、サニタイザ指摘なし、CLI23 合格、SKIP 0（10/14 17:36–23:01 UTC）
  - linux-release: 流さずに止めた（10/14 23:20 UTC、120 試験中 46 まで失敗なし）。利用者の決定（2026-10-14）: 最後の群の V2 と出口の V3 は同じコード（違いは HANDOFF の記録だけ）なので、Release は V3 で確かめる。V2 の Release の result.json は無い（止めたため）。

### 群D（CL-D。66e5fac・51104ab・26c7336・a686c80）
- レイヤースタイル: インキのペイントレイヤーの効果は 8 bit の層と同じ形（効果の届く分だけ広く描いて切る）でインキに掛ける（`render/composite` の `ink_layer_effects`）。水彩の縁は縁の α だけ Genko の墨へ寄せ（層の α はそのまま）、フチはその色の規則のインキを下に敷く（形は 8 bit の絵の α から）。表示の色（`color`）は画面でその色の規則のインキ（印刷では `color_prints` のときだけ）。トーン化（`screen`）は網点にするときだけ、インキが意味する灰色を 8 bit で網点にして規則で（K）。ノイズのトーン化は分けて描くときも上の帯から同じ粒で作る（全体と同じ画素）。
- 編集可能な文字の PSD（PSD-01）を CMYK・Lab のページでも書く（`psd_text_ink` の断りを外した）。文字の層の画素はインキ、グループの記録はページの版の数。CMYK のページの文字の色は規則のインキ（EngineData の FillColor Type 2: α・C・M・Y・K）、縁取り（lfx2）の色は CMYC（%）。Lab のページの縁取りは LbCl、文字の色は RGB（Type 1）。8・16 bit。
- 試験（QtTest の Totals: test_ink_filters 55・test_psd_text 32・test_contract_ink 6）: test_ink_filters inkLayerStyles・stylesInPartsAsWhole（範囲を分けて描いた結果が全体と同じ。フチとノイズのトーン化）、test_psd_text editableOnInkPages（このビルドの読み手）、test_contract_ink editableTextKeepsTheInks（psd-tools で CMYK 8・16 bit と Lab の文字の色・縁取り・版の数・精度）。`tools/migration/ink_check.py` に文字の層の色を読む `texts`。
- レビュー（1回。P2×1・P3×2）で直した所: ノイズのトーン化を分けて描くとき上の帯を表示の RGB で作っていた（P2。インキが意味する灰色で作る）、古い説明（P3）、16 bit の編集可能な PSD の試験が無かった（P3）。
- 凍結の前に作業ツリーで Release の全体（性能を除く 120 試験）を 1 回流し、全合格を確かめてから凍結した（群B の V2 の ASan と並べて最低優先度で。その記録は残さない。上の V2 が証跡）。

### 既知の制限（群D の時点）
- Lab のページの編集可能な文字の色は RGB（Type 1）で書く。Lab の Type の値を確かめる手段（Photoshop・仕様）が無いため。
- 書いた PSD は psd-tools とこのビルドの読み手で照合した。Photoshop での確認はしていない。

### V2（群C）— 統合済み
- **凍結SHA**: `d592657710e81365b6ba0537dd033d950b232152`（native/integration へ早送り。24d60dd の上に ea85cf0・186785e・d592657）。計画は `validation_policy.py --base 24d60dd --head d592657 --phase integration`（tier full、各 120 試験、性能は別・必須でない）。
- **実行環境**: 群B と同じ（4 CPU / RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44、Qt 6.11.2、参照 Python 3.12＋Pillow 12.3.0、CAP を外して実行。ASan（ctest 1並列）→ Release）。
- **結果**（validation_run.py の result.json。failures は空、除いた契約なし）:
  - linux-asan: 120/120 合格、サニタイザ指摘なし、CLI23 合格、SKIP 0（10/14 10:01–15:49 UTC）
  - linux-release: 120/120 合格、CLI23 合格（10/14 15:52–17:31 UTC）。故障注入が Debug・ASan だけのための SKIP（群B と同じ組）は合格に数えない。

### 利用者の決定（2026-10-14、群C）
- 補正は「画面の色に掛ける」: RGB の式をインキが意味する色に掛け、ページの規則でインキに戻す（灰色は K だけ、変えなかった画素はインキのまま）。
- フィルターはインキの版ごとに 16 bit のまま掛け、K だけの線は K だけのまま。

### 群C（CL-C。ea85cf0・186785e・d592657）
- 補正（レベル・トーンカーブ・色相/彩度・反転・ポスタリゼーション・2階調化・グラデーションマップ・明るさ/コントラスト・露出。補正レイヤーと `filter_raster` の両方）は利用者の決定どおり「画面の色に掛ける」: インキが規則で意味する RGB（K だけは灰色、色の表のインキは表の RGB、ほかは表示の sRGB）に RGB のページと同じ式を掛け、ページの規則でインキに戻す（`render/ink` の `meaning_of`・`inks_by_rules`・`correct`、`ink::Rules` で色の表を一度だけ引く）。変えなかった画素はインキのまま（16 bit も）。補正レイヤーはページの合成で描く（`InkCanvas::recolour`。不透明度・マスク・クリップはインキの値で。描画モードのある補正レイヤー・形を動かす補正・plugin の補正は高精度と同じく省いて記録）。
- フィルター 21 種はインキの版ごとに層の精度のまま（`render/ink_filters`）。動かす・ぼかすは α を掛けた値で、暗くするのは K を足す・明るくするのはインキを減らす（Lab は L）、新しく置く色は規則のインキ。K だけの線はどのフィルターでも K だけ。ペンの線・絵は先に規則でインキに描き（透明保護の層は自分の画素の中だけ）、掛けた層はペイントレイヤー。plugin のフィルターは群B の「表示の絵を貸す」方式。
- 群B の取りこぼしを直した: 高精度の線（`color_rgb`）を持つ層の結合は、op が高精度の画素で返してもインキで合成し直す。透明保護のインキの層に置いた絵は自分の画素の中だけ。
- 画面: レベル・カーブの窓のヒストグラムは、インキの層では意味する色の明るさで数える。
- 試験: test_ink_filters 53（CMYK・Lab の全種で K だけの線は K だけ・16 bit のまま、ぼかし、補正の規則、補正レイヤー、ペンの線、高精度の線、透明保護、掛けた層の種類）、test_ink（断りの試験を補正レイヤーの省きに）、test_ink_edit、test_contract_plugins（CMYK のページの plugin）。
- レビュー（1回。P1×1・P2×2・P3×2）で直した所: 光彩が透明の画素（インキなし＝白）を光と数えて線が薄くなっていた（P1。8 bit と同じく透明は暗い）、モザイクの透明の見本が不透明な線の画素にインキなしを入れていた（P2。透明の見本は黒）、線・絵の焼き込みが透明保護を無視していた（P2）、フィルターを掛けた層の種類・色の表の引き直し（P3）。
- 凍結の前に作業ツリーで Release の全体（性能を除く 120 試験）を 1 回流し、全合格を確かめてから凍結した（群B の V2 の ASan と並べて最低優先度で。その記録は残さない。上の V2 が証跡）。
- 私の判断: 補正の「変えなかった画素」は往復の丸め 1e-9 以内で決める（8 bit の ±2 ではない。補正は画素ごとの式なので、変えない補正なら意味する RGB はちょうど同じに戻る）。

### 既知の制限（群C の時点）
- 補正で変わった画素は、意味する RGB（float）からページの規則で戻す（色の表の色は 8 bit の値ちょうどのときだけ表のインキ）。リッチブラックなど表に無い色は「変えない補正」でだけインキが保たれ、色を変えれば意味する RGB から規則で戻す（灰色なら K だけ、ほかはプロファイル）。
- 描画モードのある補正レイヤーは高精度のページと同じく省く（画面は記録、書き出しは not_yet_ported）。
- レイヤースタイル（水彩の縁・フチ・表示の色・トーン化）のあるインキの層は描かずに `ink_layer_style` と記録し（書き出しは not_yet_ported）、インキのページの編集可能な文字の PSD は `psd_text_ink` で断る（どちらも群D）。

### V2（群B）— 統合済み
- **凍結SHA**: `994238a44d97117aa0fd30d081bff2ca078cfd54`（native/integration へ早送り。d1ca8e8 の上に 6e8f65e・99dff23・411c8e4・994238a）。計画は `validation_policy.py --base d1ca8e8 --head 994238a --phase integration`（tier full、各 119 試験、性能は別・必須でない）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU / RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44、Qt 6.11.2、参照 Python 3.12＋Pillow 12.3.0（lcms 2.19。C++ 側は lcms 2.14）。root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。ASan（ctest 1並列）→ Release。ASan の間、別の作業ツリー（群C・群D）のビルドと事前の全体試験を最低優先度（nice 19）で並べた。
- **結果**（validation_run.py の result.json。failures は空、除いた契約なし）:
  - linux-asan: 119/119 合格、サニタイザ指摘なし、CLI23 合格、SKIP 0（10/14 02:43–08:01 UTC）
  - linux-release: 119/119 合格、CLI23 合格（10/14 08:18–09:54 UTC）。故障注入が Debug・ASan だけのための SKIP（test_transaction 10・test_save_e2e 16・test_session 3・test_contract_save 1・test_migrate_e2e 1・test_gui_save 1・test_app_e2e 1。これまでと同じ組）は合格に数えない。ASan では SKIP 0。

### 群B（CL-B。6e8f65e・99dff23・411c8e4・994238a）
- 画素を消す・動かすだけの操作はインキの値のまま（`render/ink_edit`。高精度の層の color_edit と同じ形）: 消しゴム（ぼかし・ざらつきは 8 bit と同じ形で α を減らす）・範囲の削除・変形とワープ（α を掛けたインキで再標本化、触れない画素は 16 bit のまま）・判型の変更（`relayout_inks`、200 dpi・バイリニア）・インキの断片の貼り付け（画面のコピーは範囲のインキを `gkir` の断片として持ち、同じ色モード・精度のペイントレイヤーへの回さない貼り付けならインキのまま置く。`gkir` は C++ の画面だけの鍵で保存しない。ほかは表示の絵として貼る）。
- 結合と変換（merge_down・merge_layers・merge_visible・convert_layer のペイントへ）は、op の前の層をページの合成と同じ形でインキで合成し直す（`render::composite_ink_layers`。下地・不透明度・描画モード・クリップ・紙は 8 bit の結合と同じ取り方）。リッチブラックの上に Genko の墨の線を結合しても、線は K だけ、下はリッチブラックのまま。
- 新しい色を描く・混ぜる操作（色混ぜ・ゆがみ・塗りつぶし・グラデーション・図形・塗り残し・定規から・ライトテーブル・3D・高精度の画素）だけ、対象の層（ページ全体の操作はそのページのインキの層すべて）を表示の絵にして 8 bit の操作に渡し、インキに戻す（変えない画素はインキのまま、色の表 → 貸した層の色 → 墨・白・規則の色 → 灰色は K → 2 以内の近い色 → プロファイル）。`page` の無い op は CMYK・Lab のページごとに貸す。flood_fill は Python と同じく役割の層を塗るので、貸すのも役割の層。
- op がペイントレイヤーに絵として置いたもの（表示の絵として貼った絵・素材・塗り（fill 系）のパッチ）は、高精度の層と同じくその場でインキに描く（`ink_edit::bake_marks`。色はページの規則）。core の消しゴムがインキの層を「消す画素なし」で終えていたのを、続けてインキを消すように直した。
- 画面: スポイトはその点のインキ（見えている色はページを描いた値、描く先のレイヤーはその画素）を拾う。色の窓にインキの値（CMYK %・Lab）と色域外の印、値で選べる（値を変えたときだけ。欄から出ただけでは何もしない）。選んだインキの表示の RGB は、規則がそのインキを当てる灰色・墨・白を先に探し、無ければ表示の色を墨・白・表のほかの色から 1〜数段ずらす。規則が別のインキを当てるときは同じ色空間・同じプロファイルのページすべての色の表に組を入れる（一つの取り消しで戻る）。知らせは スポイト「色を拾いました」・色の窓「色を選びました」。
- 試験: test_ink_edit 34（変えない画素・消しゴム・移動・結合・全 op・リッチブラック・判型・高精度の画素・貼った絵・flood_fill の層・フィルターの断り・スポイトと色の窓の値）、test_gui_book inksInTheColourPanel、test_ink（断りの試験を filter_raster に）。
- レビュー（1回。P1×2・P2×2・P3×2）で直した所: 結合と絵の焼き込みが不透明な紙から合成して空いた所を埋めていた（P1。透明から合成）、色混ぜなどが貸した層に置いた絵を規則で焼いて混ぜなかった画素のインキを失っていた（P1。貸した絵に描いてから戻す）、消しゴムの「線全体」「交点まで」・トーンの層で画素を消していた（P2）、色の窓がほかのページの色の表の組を上書きできた（P2）、値の欄の印・インキの断片のプロファイル（P3）。
- 1 回目の V2 の ASan で test_ink_edit が heap-use-after-free（貼った絵の試験が一時のインキの画素を指したまま読んでいた。製品の誤りではない）。試験を直して（994238a）凍結し直し、V2 をはじめからやり直した（上の結果）。
- 私の判断: 最初の版（全部の画素の操作を表示の絵を通す方式）は、結合でリッチブラックが墨と同じ表示色になるため線と区別できない欠陥があった（自分の見直しで P1 相当）。動かす・消す・結合はインキのまま扱う方式に作り直した（利用者に伝えた方針どおり）。

### 既知の制限（群B の時点）
- 新しい色を描く操作で変わった画素は 8 bit の表示の色から戻す（16 bit のページでも）。塗りつぶし（flood_fill）は 8 bit の操作と同じく 72 dpi。
- フィルター（filter_raster）と補正レイヤーは CMYK・Lab のページで not_yet_ported（群C）。
- 回した・裏返した貼り付けのインキの断片は表示の絵として貼る（色はページの規則）。
- 本を開いた直後でまだ読み込んでいないページがあると、色の窓で選んだ組の色の表への追加は断られる（ペンの色だけ変わる）。
- 高精度の線（`color_rgb`）を持つ層を結合して op が高精度の画素で返したときは、インキで合成し直さずにプロファイルで戻す。透明保護のインキの層に置いた絵は、自分の画素の外にもインキを置く（どちらも群C で直す）。

### V2（群A）— 統合済み
- **凍結SHA**: `276a799173f346e615a6fefd20a5a9b443e62ff2`（native/integration へ早送り。9318f03 の上に 2fe5986・db695a8・ec075d5・e801ece・276a799）。計画は `validation_policy.py --base 9318f03 --head 276a799 --phase integration`（tier full、各 118 試験、性能は別・必須でない）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU / RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44、Qt 6.11.2、参照 Python 3.12＋Pillow 12.3.0（lcms 2.19。C++ 側は lcms 2.14）。root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。ASan（ctest 1並列）→ Release。ASan の間、別の作業ツリー（群B）のビルドを最低優先度（nice 19）で並べた。
- **結果**（validation_run.py の result.json。failures は空、除いた契約なし）:
  - linux-asan: 118/118 合格、サニタイザ指摘なし、CLI23 合格（10/13 15:03–20:41 UTC）
  - linux-release: 118/118 合格、CLI23 合格（10/13 21:08–22:38 UTC）。故障注入が Debug・ASan だけのための SKIP（test_transaction 10・test_save_e2e 16・test_session 3・test_contract_save 1・test_migrate_e2e 1・test_gui_save 1・test_app_e2e 1。これまでと同じ組）は合格に数えない。ASan では SKIP 0。
- 統合の前に Release の全体（性能を除く）を 1 回流して 5 件の古い期待（Lab の PSD を断る前提、op の一覧、メニューの並び）を見つけ、直してから凍結した（その記録は残さない。上の V2 が証跡）。

### 群A（CL-A。2fe5986・db695a8・ec075d5・e801ece・276a799）
- 形式: schema-v4.md「色モード（color-mode@1）」。ページと本の `color`、レイヤーの `ink_raster`（.gkir）、機能 `color-mode@1`。読み込みは ICC・GKIR を確かめ、合わない原稿は断る（RGB のページの絵はインキのページで not_yet_ported）。
- 合成と表示: render/ink_canvas（インキの値で合成。CMYK は版を反転して、Lab は 8 種の合成だけ）、8bit で描く要素（文字・コマ枠・トーン・効果・ノンブル）は黒と白に描いて取り出す（不透明な画素は完全一致だけを表・墨・白に寄せる）。表示は sRGB（相対・BPC）。
- 操作: `set_color_mode`（RGB に戻すとき depth 8 は PNG、16 は精密 u16）、`set_inks`。まだの画素の操作はインキのページで not_yet_ported（群B・C で順に）。
- 書き出し: PDF・TIFF はインキのまま（auto、CMYK のページの cmyk）、PSD はいつもインキのまま（モード 4・9、ICC は資源 1039）。RGB だけの形式は `notes`（CLI の JSON にも）。
- 取り込み: put_raster・import_psd（CMYK は絵の ICC からページの ICC へ黒を保って、Lab はそのまま）。Lab の PSD は Pillow の "LAB" として開く（RGB への変換は Pillow と同じ Lab V2・4 バイト目をそのまま）。RGB のページへの import_psd は Python と同じく断る。
- 画面: 「ページの色モード…」、ステータスの「CMYK 8 bit」、タイル・サムネイル・レイヤーの小絵の再描画。
- 試験: test_ink 12、test_contract_ink 5（Pillow の ImageCms・psd-tools・poppler と照合。Little CMS の版が違うので ±1。Pillow は 16bit の PSD を開けないので psd-tools で、16bit の CMYK の TIFF は上位バイトで読む）、test_gui_book colourModeOfPages、raster_cases の「put_raster PSD lab」は Python と照合（C++ の断りの印を外した）。Lab の PSD が開くことに合わせて test_contract_psd（Pillow の "LAB" の画素とバイトで照合。RGBA は lcms の版の差で照合しない）・test_placed・test_contract_studio（配置した Lab の PSD は Pillow と同じく透明で、ページと書き出しは置く前と同じ）・test_command_bus（op の一覧に set_color_mode・set_inks）を直した。色モードのメニューは Python の項目の後ろ（Python の並びを崩さない）。
- レビュー（1回。P1 なし、P2×2・P3×7）で直した所: 塗り・トーンの参照（参照レイヤー・このレイヤーだけ）にインキの層の線を入れる（P2）、sRGB にして書いた書き出しは CLI だけでなく書き出しの窓・MCP の export（`notes`）・HTTP の /v1/export・正式な書き出し・動画の書き出しでも知らせる（P2。入稿セットは「PNG だけ変換、TIFF はインキのまま」）、まだの op のページを ops と同じ探し方で照合（1.0・true でもすり抜けない）、読み込みで validate_ink_document（Lab で使えない描画モードなど、どの op も作らない原稿は読み取り専用と理由）、インキのページは補正レイヤーで全頁描画にしない（画面は省いて記録、書き出しは not_yet_ported）、RGB に戻す窓は 8 bit を既定に、LAB からほかの形式への変換の文言を Pillow と同じ `conversion from LAB to RGB not supported` に。
- 私の判断: 高精度の線の色（`color_rgb`）は RGB のページの精度で、インキのページでは線の `rgb`（8 bit）を規則に当てる（schema-v4 を直した。インキの精度は色の表とプロファイル）。インキのページのトーンは RGB のカラーのページと同じく網点にしない濃さ（SPEC を直した）。

### 既知の制限（群A の時点）
- まだの画素の操作（消しゴム・範囲・貼り付け・色混ぜ・ゆがみ・塗り・結合・変換・3D・定規・判型の変更・フィルター）は CMYK・Lab のページで not_yet_ported（群B・C）。
- 二色の境の画素はプロファイルで写す（表の色にはしない）。
- スポイトは表示の RGB を拾う（群B）。描いている途中の線の表示は RGB。
- インキのページの編集可能な文字の PSD は not_yet_ported（群D）。
- CLI の `--color` に auto が無いので Lab のページは CLI では RGB と notes（CMYK は `--color cmyk` でインキのまま）。
- 16bit の PSD の取り込みは 8bit で読む。

### 残課題・次
- ~~工程の出口 V3~~ 済み（上の「工程の出口 V3」）。
- Windows は開発中 deferred（配布前の実機受入は必須。利用者の決定（2026-10-15）: マージ後に別の環境で）。

## M5出口受入（Linux）— 2026-10-13 記録

- **凍結SHA**: `9f97117d478f1877c304a8e9e93a3f28ac57fd68`（native/integration。M5②③ の上に M5④ a8e6e67・0c4aae4・3567cd1、出口の準備 ad0f49e、出口のレビュー 9f97117）。計画は `validation_policy.py --base 65f8333 --head 9f97117 --phase milestone`（基点は M4 出口の凍結。tier full、Linux 各116試験、性能は別に必須）。
- **実行環境**（資源上限は固定しない＝利用者決定）: Claude Code クラウドコンテナ、4 CPU（Intel Xeon @ 2.80GHz）/ RAM 16GB / Ubuntu 24.04.5 / kernel 6.18.44。Qt 6.11.2、GCC 13.3。参照 Python 3.12＋Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9、MCP の参照 Python は GENKO_MCP_PYTHON（mcp 2.2.0）、native.yml の数値用環境変数、root から CAP_DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER を外して実行。ASan（各構成内 ctest 1並列）→ ASan のビルドを消して Release と Debug を同時に。
- **結果**（validation_run.py の result.json。failures は空、除いた契約なし）:
  - linux-asan: 116/116 合格、サニタイザ指摘なし、CLI23 合格（10/13 00:54–06:15 UTC）
  - linux-release: 116/116 合格、CLI23 合格（07:13–08:49）。故障注入が Debug・ASan だけのための SKIP（test_transaction 10・test_save_e2e 16・test_session 3・test_contract_save 1・test_migrate_e2e 1・test_gui_save 1・test_app_e2e 1。M4 出口と同じ組）は合格に数えない。Debug・ASan では SKIP 0。
  - linux-debug: 116/116 合格、CLI23 合格（07:13–09:18）
  - `--pair`: 失敗は「構成artifactの欠落・重複」1件のみ＝Windows 2構成が開発中 deferred。Linux 3構成どうしの照合は指摘なし。**Windowsを含む --pair 合格ではない。**
- **M5 の出口（PLAN.md）の証拠**（3構成とも合格。件数は QtTest の Totals）:
  - 公式 MCP SDK のクライアント（stdio・HTTP）、CLI、HTTP から同じ原稿へ安全に操作: test_contract_mcp 11（公式 `mcp.Client` の stdio・Streamable HTTP と生の JSON-RPC・HTTP API を C++ と Python のサーバーで照合。初期化・道具一覧・4リソース・成功・失敗・取消・画像・ジョブ）、test_mcp_api_e2e 22（承認・ページロック・チャット承認の切替・人が固定した欄・人の変更の Undo をエージェントが MCP・HTTP で通れない、41 の道具に人の道具が無い、HTTP の token・Host・Origin・root 外のパス・悪い絵・巨大な要求・chunked・接続の上限、外部コマンドの引数）、test_contract_service 11・test_service 32・test_contract_cli 9・test_cli_e2e 10・test_http_server 6。
  - GUI 案内から初回クライアント設定・再接続・原稿と会話の選択・依頼・承認・結果反映: test_gui_ai 62（firstSetupToResult は代わりのサーバー、firstSetupWithGenkoMcp はこの版の `genko mcp`。変更案→バックアップ→確認→テスト接続（initialize と tools/list だけ）→原稿と会話→クライアントの再起動→二つの原稿 A・B へ別の会話から依頼→A の承認箱で確認待ち→承認と2行の返事→切断・再接続の後に AI へ結果、B は変わらない。tools.jsonl・presence は Python と同じ）、test_gui_studio 29、test_contract_studio_gui 7（参照 Python と照合）。利用者のクライアントでの初回導入は実施不要（ACCEPTANCE §6。waived、合格に数えない）。
  - 合成画像によるオフライン 8 ページ工程: test_contract_pipeline 3（eightPagesLikePython。FakeAgent を公式 MCP SDK で `genko mcp` に、人役は `genko studio`。Python と全呼び出しの返事・絵・最後の本・監査・承認を照合し、書き出し（PDF・TIFF 8枚・校正 PDF）は最後の本を Python が書き出したものとバイトで照合。ASan・Debug でも合格）。
  - 速度（PERF-F）: 下の計測。権限・保存は上の試験と test_save_e2e・test_session（Debug・ASan で故障注入まで）。
- **PERF-F**（Release、test_perf_pipeline、F3 の工程。合否判定あり。測定機は基準クラスではない）: F3 の工程 80.1 秒・88 作業（人の PDF と TIFF 8 枚）。全呼び出し n=314 p95 1030 ms・p99 1825 ms（除外なし。MCP の道具 306 件 p95 996 ms、人の `genko studio` 8 件 p95 7.76 s、書き出し 4 件 最大 9.60 s を含む）／情報取得 n=100 p95 845 ms／編集の確定 p95 150 ms／候補の採用 p95 1751 ms／ページの仕上げ p95 989 ms（合格値は各 p95 2 秒。間の人の承認 p95 164 ms）／8 ページの校正出力 10 回（新しい出力先 studio/proofs/r<版>）: 呼び出しから PDF の完成まで 8.25〜9.88 s（各 30 秒以内。仕事の記録は呼び出しから開始まで最大 13.9 ms、開始から終了まで最大 9.87 s。MCP の export_proof は最大 40 秒仕事を待つので、返事は仕事の終わりと同じ時刻）。**全項目合格**（Totals 3 passed・0 skipped、ctest exit 0。10/13 09:36–09:44 UTC、load 1.3〜1.6）。
- **V3 の手順の誤り（私の誤り、製品の失敗ではない）**: V3 のスクリプトが test_perf_pipeline を ctest を通さずに実行したため、CMake に登録した `QTEST_FUNCTION_TIMEOUT=3500000`（と TIMEOUT 3600）が渡らず、QtTest の既定 300 秒で productionApi が打ち切られた（exit 134、計測の途中）。打ち切りで親を失った harness と `genko mcp` が残ったので止めた。同じ凍結のビルドで `ctest -R '^test_perf_pipeline$'` からやり直した（上の値）。
- **実xcb（Xvfb 1920×1080）GUI・E2E**（凍結SHAのビルド）: Release で gui_actions 14・gui_ai 62・gui_anim 8・gui_book 30・gui_brushes 8・gui_canvas 78・gui_color 60・gui_colours 4・gui_effects 15・gui_guides 7・gui_layers 12・gui_layout 32・gui_lines 20・gui_materials 36・gui_output 21・gui_paint 11・gui_paper 14・gui_pen 51・gui_ruby 18・gui_select 13・gui_studio 29、Debug で gui_save 36・app_e2e 8、全合格・FAIL 0。Release の gui_save は故障注入が Debug だけのため SKIP 1（合格に数えない。Debug で 36 合格）。
- **AC-PERF 注入計測**（Release、Xvfb、F1＝32頁×1500線。表示のみで合否判定なし）: 開いて最初の頁 641 ms、全頁 757 ms／入力→ライブ線 p95 1.23・p99 5.88 ms（n=43780）／線確定→表示 p95 20.5 ms／Undo→表示 p95 33.0 ms／頁切替 最初 p95 28.8・精細 p95 105 ms（n=4）／自動保存 p95 2.41 s／保存中のタイマーの遅れ p99 11.5 ms・イベントループの停止 p99 44.6 ms（最大 49.6 ms、250 ms 超 0）。F1頁 350dpi 全描画 1227 ms・1線追加 583 ms・512px部分 37 ms。M4 出口とほぼ同じ。基準機での測定は M7。
- **M5 の出口は Linux で合格**（Windows は開発中 deferred。配布前の Windows 実機受入は必須）。

### M5④（a8e6e67・0c4aae4・3567cd1）
- app/review_model・ai_link・client_config・mcp_probe・reply_box・viewer・studio_widgets・ai_dialog・links・main_window_studio・style_picker: Python の画面（承認箱、工程、コマの詳細、資料、AI と作る、絵柄を選ぶ、genko:// のリンク）。
- I07: 接続の状態（未設定・設定済み・未接続・接続済み・処理中・人の確認待ち・失敗）と事実（設定・テスト接続・依頼先・最後の呼び出しと誤り・作業・確認待ち）。テスト接続は initialize と tools/list だけ（生成・課金・原稿の変更をしない）。I13: 複数行の返事（Ctrl+Enter で送る、IME の変換中は送らない）、送る前の確認、下書き（原稿の studio/reply_drafts/、genko-reply-draft@1）。
- クライアントの設定（Claude Desktop の JSON・Hermes Agent の YAML）は変更案・写し・確認・取りやめ・元に戻す。旧版の `genko gc` が v4 の古い履歴を消しても壊れない（Undo/Redo は history_missing で断り、doctor が報告）。
- レビュー（P1・P2）で直した所: 確認窓の間に原稿・ページ・コマ・依頼・下書きが変わったら送らない（キーを値で取り、答えの後に確かめる）、Hermes の YAML の書けない形は書かずに理由を出す・コメントと空行と BOM を残す、正式な書き出しは保存が終わってから、doctor の判断を Undo と同じに、下書きのリンクのフォルダー・知らない版・gc、shared_ptr の循環、試験の子プロセスに一時の HOME・APPDATA。

### M5 出口の準備と出口のレビュー（ad0f49e・9f97117）
- test_contract_pipeline（F3、上）と test_perf_pipeline（PERF-F）。FIXTURES.md F3 に書いた。
- 1 回目の PERF-F の計測（V2 と同時、load 2〜3）で候補の採用の p95 が 3.07 秒で 2 秒を超えた。合格値は緩めず、Pillow の拡大・アンシャープマスク・ぼかし・配置画像の縮小を行の帯ごとに並列にし（出力のバイトは同じ）、Genko がこのプロセスで今書いた絵（sha256 一致）だけは採用時の全展開の確かめを省く（外から来た絵は今までどおり。SPEC に記載）。
- GUI の選択（select_frame）は、AI がページを並べ替えても ID で付け直す（消えたら外す）。中身を変える変更は競合として未保存で残す（既知の制限、SPEC）。
- ASan の test_gui_studio の LeakSanitizer（ViewerDialog のページ送りの部品）を直した。
- 出口のレビューで直した所: ページ一覧・コマ・台詞の右クリックのメニューはページの id で答えの後の番号を探して当てる、ファイル → 書き出し の正式な書き出しは保存を待つ、Hermes の YAML で genko の値にアンカー・別名・タグがあれば書かない、帯の並列のスレッドはプロセス全体で CPU 数 − 1 本まで（作れなければ呼んだ側で、失敗は逐次でやり直す）、描画の覚えに入れるのは sha256 が合うときと adopt のときだけ。

### 利用者の決定（2026-10-12、M5④）
- 返事の下書きは原稿に残す（studio/reply_drafts/<sha256>.json、Undo・journal の対象外、v1〜v3 には書かない）。
- クライアント設定のバックアップは設定ファイルの隣（<名前>.genko-backup-<日時>）。
- 旧版（Python 版）の `genko gc` が v4 の原稿の古い履歴を消す件は、C++ を強くして記録（既知の制限）。

### 既知の制限（M5 出口の時点）
- `genko studio` の --agent で人として振る舞える（Python と同じ。利用者決定）。書体のパスは root に閉じない（利用者決定）。
- 画面の中身を変える変更は、保存待ちの間に AI がページの番号と ID の対応を変えると ID で付け直さず、競合として未保存で残す（SPEC）。
- C++ 版の原稿（v4）に旧版の `genko gc`・`genko undo` を使わない（SPEC・schema-v4 §8）。
- 下書きの排他はない（二つのアプリで同じ依頼に書くと後勝ち）。
- Windows: プラグインの子の子の停止（Job Object）、MSIX 版の Claude の設定の場所、コンソールの窓は M6 の実機で確かめる。
- cli の手順 42 の一度だけの失敗（M5②）は原因不明のまま（以後再現なし）。

### 残課題・次
- CMYK/Lab 編集の工程（M5 の後、M6 の前の独立工程。Lab の PSD を含む）。
- M6 は Windows 実機の実行場所が要るため、着手前に止めて利用者に確認する。
- Windows 実機受入（配布前に必須）。

## M5②（Studio のサービスと genko studio）・M5③（MCP と HTTP）— 2026-10-12 記録

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
- M5④（画面）と M5 出口の準備（F3・PERF-F）は上の M5 出口の節で統合済み。
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

## M2出口受入（Linux）— 2026-10-08 記録（下の旧節の「マージ後未実施」はこの節で置き換わる）

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
