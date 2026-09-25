---
name: genko-manga
description: Genko Studio を MCP で操作して、企画から漫画のネーム（コマ割り・台詞の配置）、作画（生成した絵をコマに置く）、仕上げ、書き出しの依頼までを進める手順。漫画のネーム、コマ割り、作画、Genko の話が出たら使う。
---

# Genko で漫画を作る

Genko は漫画原稿のシステムで、MCP サーバー `genko` として接続されている（道具は `mcp_genko_<名前>`）。
Genko は文章も絵も作らない。企画書・脚本・ネーム計画と絵は、あなたが作る。Genko はそれを検査し、コマ割りと縦書きの写植を計算し、
絵の依頼パック（サイズ・プロンプトの下書き・ガイド画像・参照画像）を用意し、取り込んだ絵をコマに置いて仕上げる。
承認と本番の書き出しは人間が行う。この文書の最新版は resource `genko://guide/skill` で読める。

## 最初に

1. `mcp_genko_projects` でプロジェクトを確かめる。無ければ `mcp_genko_create_project`（例: name `summer.genko`、pages 8）。
2. `mcp_genko_inspect` を `target: "rules"` で呼び、ネームの規則を読む。`target: "schemas"` で入力の形を読む。

## ループ

1. `mcp_genko_next` で次の作業を1つ取る（他のエージェントと並行して動くときは `claim: true`）。`tools` に使う道具の目安がある。
2. 作業の種類ごとに下の手順で進める。
3. 書く道具は、まず `commit: false` で呼ぶ。`issues` に `severity: "error"` があれば、`path` の場所だけを直して送り直す。エラーが無くなったら `commit: true`。
4. 同じ指摘が 3 回直しても消えないときは、無理に続けず `mcp_genko_ask_human`（`page`、あれば `frame_id`、`item` に作業の種類）で人間に相談する。その作業は人間が閉じるまで `next` に出なくなるので、次の作業へ進む。
5. `next` の `items` が空で `waiting_for` だけになったら、人間の承認待ち。下の「承認を頼む」をする。

## ネーム

- `write_bible`: 企画書を書いて `mcp_genko_set_bible`。登場人物の `tokens_en`（英語の見た目の記述）は、絵の依頼にそのまま入るので丁寧に書く。
- `write_script`: 脚本を書いて `mcp_genko_set_script`。beat ごとに `page` を決める。見せ場（reveal）は偶数ページの先頭、引き（hook）は奇数ページの最後。
- `plan_page`: `mcp_genko_inspect`（target `page`）でそのページの beat を読み、ネーム計画を書いて `mcp_genko_submit_name`。
- `review_name`: `mcp_genko_render` で画像を見て、読み順の迷い・窮屈なコマ・弱いめくりを確かめる。直すなら `submit_name` を `replace: true` で送り直す。よければ `mcp_genko_record_review`。
- `revise_page`: 人間の指示（`comments`）に従って `submit_name` を `replace: true` で送り直す。

## アタリ（人間が手で描いたネーム）から始めるとき

- 人間が `import_name` か CLI でアタリを取り込むと、Genko がコマ割りを検出して提案にする（確定は人間）。
- `read_atari`: `mcp_genko_render`（`kind: "atari"`）でアタリを見て、手書きの台詞を読み、`mcp_genko_propose_lines` で提案する。
  位置は `box01`（アタリ画像の中の 0..1 の `[x, y, 幅, 高さ]`）で渡せる。縦書きの列の区切りは `\n`。台詞が無いページは
  `record_review`（`kind: "atari_lines"`）で知らせる。
- `atari_layout`: コマ割りの提案が無い（見つからなかった・却下された）。`mcp_genko_analyze_name` を `params` を変えて試すか、`ask_human`。
- `brief_panels`: 確定したコマごとに、アタリを見て指示（shot、angle、人物、動き、表情）を `apply_ops` の `set_panel` で書く。
- 提案を出したら `review_page` の場所を人間に知らせて、確定を待つ。

## 作画

作画はパイロットページ（ふつうは 1 ページ目）から始まる。パイロットページの作画が承認されると、絵柄（参照画像）と使う画像ツールが固定され、残りのページの依頼パックに入る。それまで他のページの作画は `next` に出ない。

- `make_sheet`: `mcp_genko_generation_request`（`character_id`）で設定画の依頼パックを受け取り、画像生成で作る。
  顔が正面を向いたアップを必ず入れる（承認時に顔の参照として切り出される）。画像を返された `inbox` のフォルダに保存し、
  `mcp_genko_import_images`（`request_id`、`images: [{file, origin}]`）で取り込む。取り込んだら人間に選んでもらう（承認を頼む）。
- `gen_panel`: `mcp_genko_generation_request`（`page`, `frame_id`）で依頼パックを受け取る。
  - `request.prompt` は下書き。書き直してよいが、登場人物の見た目の記述（`characters[].tokens_en`）は言い換えない。
    `avoid` にあるもの（文字・フキダシ・効果音・署名・枠線、モノクロのページでは色も）は描かせない。
    `request.color` が true のページはカラーで、それ以外はモノクロ（グレースケール）で作る。
  - 参照画像（`files.references`: 設定画・顔・場所・`refs/style_pilot.png`）は、画像ツールが受け付けるなら必ず添える。
    `files.composition`（構図）と `files.pose`（人物の位置と向き）も参考として添えてよい。線をなぞらせる必要はない。
    `notes_for_agent` にマネキンの注記があるときは、`files.pose` の棒人形がポーズの指定。体の向きと手足の角度を合わせる。
  - サイズは `size.suggested_px`。画像ツールが決まったサイズしか出せないときは `size.tool_sizes` の先頭を使う（切れる方向が書いてある）。
  - `keepout` の場所（台詞が入る）は静かに空けておく。プロンプトの下書きにも書いてある。
  - 1 つの依頼で 2〜4 枚作り、全部 `inbox` に保存して一度に `import_images` する。
  - `origin` には `tool_id`（例 `openai:gpt-image-1`）、`model`、実際に使ったプロンプト（`prompt`）、`params`、添えた参照（`refs_used`）を書く。
- 複数人物のコマ（依頼パックに `steps` がある）: まず全員の構図で作って採用候補を決め、`report_regions` で人物ごとの領域を報告し、
  似ていない人物だけを `generation_request`（`mode: "inpaint"`、`parent` に採用候補、`focus_character` に人物 id）で一人ずつ直す。
- 顔だけ直す: `generation_request`（`mode: "inpaint"`、`parent`、`regions: ["face:<人物 id>"]`）。マスクの白い所だけを描き直させる。
- 全体を少し直す: `mode: "edit"` と `parent`、`instruction` に直す点。`source.png` を元画像として画像ツールに渡す。
- `import_pending`: 依頼済みで画像がまだのコマ。`inbox` に画像を置いて `import_images`。
- `review_candidates`: `mcp_genko_candidates`（metrics の `rank` が小さいほど目安が良い）と `mcp_genko_render`（`kind: "compare"`、`candidate_id`）で比べる。
  赤い線がネームの構図。構図がネームに合うか、人物が設定画に似ているか、手足の破綻、画内の文字、台詞の場所が空いているかを見て、
  `review_candidates` で点数（0〜1）とメモを残し、良いものを `mcp_genko_adopt`。どれも駄目なら直しの依頼を作る。
  1 コマ 8 枚・直し 2 巡を超えると、そのコマは人間の判断待ちになる。
- `fix_panel`: 人間の指示（`comments`）どおりに直しの依頼を作る（`instruction` に指示を入れる）。
- `report_regions`: 採用した絵の顔と人物の位置を `mcp_genko_report_regions` で報告する（`box01` は画像の中の 0..1 の `[x, y, 幅, 高さ]`）。
  人物がいない絵なら `apply_ops` の `record_review`（`kind: "regions"`、`frame_id`、`input_hash` に採用中の候補 id）。
- `upscale_panel`: 画像ツールに高解像度化があれば `generation_request`（`mode: "upscale"`）で作り直して取り込み、採用し直す。
  無ければ `record_review`（`kind: "upscale"`、`input_hash` に採用中の候補 id）で理由を残す。
- `finish_page`: `mcp_genko_finish_page` を `commit: false` で見て（顔にかかる台詞の移動、効果）、よければ `commit: true`。
- 線をくっきりさせたいコマは `mcp_genko_derive`（`kind: "lineart"`）で採用中の絵から線を抜き出し、`adopt`（`to: "ink"`）で絵の上に置く。
  モノクロの原稿では、グレーの絵は印刷のときに Genko が網点に変える。画像はグレースケールで、トーンを描き込みすぎずに作る。
- 効果音はネーム計画の台詞で `balloon: "sfx"` にする（Genko が大きな縁取り文字で描く。画像に描かせない）。
- 台詞の見た目は `apply_ops` の `edit_line` で直せる: `balloon`（speech・rounded・box・cloud・thought・shout・flash・whisper・narration・sfx・none）、`style`（`font`: antique・gothic・mincho・maru・hand・sfx・sfx_pop、`size_mm`、`outline_mm` など）、`tails`（`[{to, via}]`、曲がったしっぽ・複数）。人間が直した見た目は変えない。

## 人と同じ道具で描く・直す

人が画面でできることは、`mcp_genko_apply_ops` の op で全部できる（一覧は resource `genko://ops`）。まず `commit: false` で試す。

- ページ: `add_page`・`delete_page`・`duplicate_page`・`reorder`・`set_spread`（見開き）・`set_page_spec`（原稿用紙を変えるとコマや台詞も合わせて動く）。
- 調べる: `mcp_genko_inspect` の `target` で `snapshot`（レイヤーの名前・種類・不透明度・合成・マスク・表示色・フォルダ・参照、台詞の書式とフキダシ、3D）、`materials`（貼れる素材の id）、`fonts`（`style.font` に使える書体）、`brushes`（`kind` に使えるブラシ）。
- 見る: `mcp_genko_render` の `mode: print` は印刷と同じ見え方、`layer_id` はそのレイヤーだけ。
- レイヤー: `add_layer`・`duplicate_layer`・`merge_down`（ペン同士は線のまま）・`delete_layer`・`set_layer`（`exportable: false` で下描き＝書き出さない、`color` で画面だけの表示色、`reference: true` で参照レイヤー）。
  マスクは `set_layer_mask`（`area` の所だけ見せる・`fill`・`invert`・`enabled`・`delete`）と `paint_mask`（`show: true` で見せる、`false` で隠す）。
- 線と塗り: `add_stroke`（`kind` はブラシ。自作のブラシは `define_brush` で定義してから）、`erase`、`fill`・`fill_area`（`fill` の `reference: "reference"` は参照レイヤーの線だけを見て塗る）、`gradient_fill`（`from`・`to`・色・不透明度、`shape: radial` で円）、`filter_raster`（`levels`・`curve`・`hue`・`blur`…）。
- ブラシ: 入っているものは `inspect` の `brushes`（G ペン・筆・スプレー・点描・点線・破線・レース・草むら・木の葉・ハート・星・カリグラフィ・水彩など）。`define_brush` で `tip`（`round`・`flat`・`image`＋`tip_png`）・`pattern`・`spacing`・`scatter`・`stamp_size`・`size_jitter`・`turn_jitter`・`count`・`speed`・`post_smooth`・`aa` も決められる。色を混ぜる・ぼかすのは `smudge`（`mode`: `blur`・`smudge`・`blend`）。線を丸ごと消すのは `erase` の `mode: "whole"`。
- 線の編集: `vector_edit`（`action`: `move_point`・`add_point`・`delete_point`・`connect`・`cut`・`recolor`・`delete`。線の id は `inspect` の `snapshot` か `render`）。塗り残しは `fill_gaps`。
- レイヤー: `add_layer` の `kind` に `fill`（ベタ塗り・`rgb`）・`gradient`（`gradient`）・`adjust`（色調補正・`adjust: {kind, …}`、下の絵の色を変える。あとから `set_layer` で直せる）。`set_layer` の `effect`（`border` フチ・`water_edge` 水彩境界）と `color_prints`（表示色を印刷にも）。合成モードは比較（暗・明）・焼き込み・覆い焼き・ソフトライト・差の絶対値・色相・輝度なども。まとめて: `merge_layers`・`merge_visible`（`copy`）・`group_layers`・`move_layers`・`set_layers`・`convert_layer`（`to`: `paint`・`pen`）。用紙の色は `set_paper`。
- 変形: `liquify`（`mode`: `push`・`pinch`・`bloat`・`twirl_cw`・`twirl_ccw`）、`transform_area` の `interp`（`nearest` でドットをぼかさない）。フィルターは移動・放射・ズームぼかし、ノイズ、波形、渦巻き、線画抽出、反転、階調化、しきい値、グラデーションマップも。
- コマ: `set_frame` の `curves`・`bow`（辺を曲げる）と `line`（枠線: `solid`・`double`・`dashed`・`dotted`・`rough`、`rgb`）。
- 文字の `style`: `scale_x`（長体・平体）・`gradient`・`fill_png`（画像で塗る）・`warp`（4 隅で遠近・ゆがみ）・`text_path`（パスに沿わせる）・`features`（字形）・`yakumono`（約物の詰め）。異体字はテキストに異体字セレクタを入れる。フキダシは `picture`（画像のフキダシ）、`spike_jitter`・`bumps`、しっぽの `kind`（`zigzag`・`fade`・`bubbles`）。
- 効果線: 流線の `path`・`spread_mm`、集中線の `inner_path`・`twist`。トーン: 柄（`check`・`brick`・`wave`・`grid`・`hatch`・`star`・`sand`・`image`）、レイヤーのトーン化は `set_layer` の `screen`、点検の `tone_moire`。
- 定規: `parallel_curve`・`multi_curve`・`radial_curve`、`layer_id`（レイヤー専用）、パースの `lock_horizon`・`horizon_y`・`fixed`、定規ペンは `ruler_to_layer`。描き文字の素材は `stamp_material`（kind `lettering`）。
- 図形: `add_shape`（`shape`: `line`・`polyline`・`curve`・`rect`・`ellipse`・`polygon`、`line`・`fill` で線と塗り）。
- 範囲（`area`）: どの op の `area` にも、`poly`・`mask` のほかに `rect`・`ellipse`・`layer`（そのレイヤーの描いてある所）・`color`（その色の所）・`all`・`saved`（`store_area` で残した範囲）と、`union`・`intersect`・`subtract` の組み合わせ、`invert`・`grow_mm`（負で縮める）・`feather_mm` が使える。ガイド線は `add_ruler` の `kind: "guide"`（`axis`・`at`）。
- 範囲の変形: `transform_area` の `matrix`（移動・拡大・回転・反転）か `warp`（`perspective` で 4 隅、`mesh` で 3×3 の点）。レイヤーを丸ごと動かすのは、ページ全体を `area` にした `matrix`。
- 台詞: `add_line`・`edit_line` の
  - `ruby_runs`（ルビ）、`emphasis_runs`（傍点）、`style_runs`（一部を大きく・小さく・太く・色を変える: `[["本当", {"scale": 1.4, "weight": "heavy"}]]`）。
  - `style`: `rotate_deg`（フキダシごと回す）、`skew_deg`・`arc`（描き文字の傾き・弓なり）、`weight`（`normal`・`bold`・`heavy`）・`italic`、`outline_rgb`（フチの色）、`latin`（`rotate` で 4 文字以上の英数字を寝かせる／`upright`）、`emphasis_mark`（`sesame`・`dot`）、`wobble`（手描き風の揺れ）・`double`（二重線）・`spikes`・`spike_depth`（叫びのトゲ）。
  - `balloon` の形に `electric`（電子音: 電話・テレビの声。角のあるギザギザの縁と稲妻のしっぽ）。
  - `path`（手で描いた形のフキダシ、`set_balloon_path` でも）。
- 3D: `add_prim3d` の `kind` は `box`・`cylinder`・`stairs`・`floor`（パースの格子）。背景は `add_scene`（`kind`: `room`・`classroom`・`corridor`・`street`）で、壁・床・窓・机・建物をまとめて置き、`edit_prim`・`delete_prim`・`trace_prims` は id 1 つで効く。人形は `add_mannequin`。`trace_prims` で線にする。
- 点検: `mcp_genko_check` で、人の「入稿前の点検」と同じ問題の一覧を受け取る（`preflight` の結果の `checks` にも入る）。
- 取り消し: `mcp_genko_undo` で自分の最後の変更を取り消す（人の変更と承認は取り消せない）。
- 書き出し: `mcp_genko_export`（`format`: pdf・tiff・png・psd・pack・epub・strip・webtoon・sns、`pages`、`dpi`、`area`: paper・bleed・trim）。承認は要らない。書き出し先は原稿の `exports/`。

## 承認を頼む

`waiting_for` の内容ごとに `mcp_genko_request_approval` を 1 回出す。

- `name` / `art`: `gate` と `pages`。
- `sheet`: `gate: "sheet"` と `character_id`。候補が複数あれば `note` におすすめを書く。
- `export`: 全ページの仕上げが済んだら、`mcp_genko_preflight` で止めている理由を確かめ、`mcp_genko_export_proof` で校正を出してから依頼する。

依頼を出したら `mcp_genko_review_page` で確認ページ（review.html）を作り、その場所（`path`）と何を見てほしいかを、
メッセージで人間に知らせる。承認するのは人間で、Genko アプリの承認箱（または確認ページにあるコマンド）で行う。人間が「OK」と返事をしても、あなたが承認を付けることはできない。

## してはいけないこと

- 承認を付けようとしない。承認は人間だけが行う（Genko の MCP には承認の道具が無い）。
- 人間が承認したページのコマ割りを変えない。直す必要があれば人間に頼む。（`cut_frame`・`move_gutter`・`set_frame` の `poly` も同じ）
- 人間が描いた線・塗り（`fill`・`fill_area`・`transform_area`・`delete_area`・`paste`・`reshape_stroke` で触れるもの）を、頼まれずに動かしたり消したりしない。
- 人間が固定した欄（pinned）・人間が描いた領域・承認済みの設定画を変えようとしない。
- 台詞や効果音を「絵の中の文字」として描かせない。文字は Genko が描く。
- 画像のバイト列を道具の引数に書かない。画像はファイル（`inbox`）かアップロードで渡す。
- 正式な書き出し（書き出しの承認として記録するもの）はしない。それは人間だけ。`export` で各形式に書き出して見せるのはよい。
