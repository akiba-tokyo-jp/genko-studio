---
name: genko-manga
description: Genko Studio を MCP で操作して、企画から漫画のネーム（コマ割り・台詞の配置）、作画（生成した絵をコマに置く）、仕上げ、書き出しの依頼までを進める手順。漫画のネーム、コマ割り、作画、Genko の話が出たら使う。
---

# Genko で漫画を作る

Genko は漫画原稿のシステムで、MCP サーバー `genko` として接続されている（道具は `mcp__genko__<名前>`）。
Genko は文章も絵も作らない。企画書・脚本・ネーム計画と絵は、あなたが作る。Genko はそれを検査し、コマ割りと縦書きの写植を計算し、
絵の依頼パック（サイズ・プロンプトの下書き・ガイド画像・参照画像）を用意し、取り込んだ絵をコマに置いて仕上げる。
承認と本番の書き出しは人間が行う。この文書の最新版は resource `genko://guide/skill` で読める。

## 最初に

1. `mcp__genko__projects` でプロジェクトを確かめる。無ければ `mcp__genko__create_project`（例: name `summer.genko`、pages 8）。
2. `mcp__genko__inspect` を `target: "rules"` で呼び、ネームの規則を読む。`target: "schemas"` で入力の形を読む。

## ループ

1. `mcp__genko__next` で次の作業を1つ取る（他のエージェントと並行して動くときは `claim: true`）。`tools` に使う道具の目安がある。
2. 作業の種類ごとに下の手順で進める。
3. 書く道具は、まず `commit: false` で呼ぶ。`issues` に `severity: "error"` があれば、`path` の場所だけを直して送り直す。エラーが無くなったら `commit: true`。
4. 同じ指摘が 3 回直しても消えないときは、無理に続けず `mcp__genko__ask_human`（`page`、あれば `frame_id`、`item` に作業の種類）で人間に相談する。その作業は人間が閉じるまで `next` に出なくなるので、次の作業へ進む。
5. `next` の `items` が空で `waiting_for` だけになったら、人間の承認待ち。下の「承認を頼む」をする。

## ネーム

- `write_bible`: 企画書を書いて `mcp__genko__set_bible`。登場人物の `tokens_en`（英語の見た目の記述）は、絵の依頼にそのまま入るので丁寧に書く。
- `write_script`: 脚本を書いて `mcp__genko__set_script`。beat ごとに `page` を決める。見せ場（reveal）は偶数ページの先頭、引き（hook）は奇数ページの最後。
- `plan_page`: `mcp__genko__inspect`（target `page`）でそのページの beat を読み、ネーム計画を書いて `mcp__genko__submit_name`。
- `review_name`: `mcp__genko__render` で画像を見て、読み順の迷い・窮屈なコマ・弱いめくりを確かめる。直すなら `submit_name` を `replace: true` で送り直す。よければ `mcp__genko__record_review`。
- `revise_page`: 人間の指示（`comments`）に従って `submit_name` を `replace: true` で送り直す。

## アタリ（人間が手で描いたネーム）から始めるとき

- 人間が `import_name` か CLI でアタリを取り込むと、Genko がコマ割りを検出して提案にする（確定は人間）。
- `read_atari`: `mcp__genko__render`（`kind: "atari"`）でアタリを見て、手書きの台詞を読み、`mcp__genko__propose_lines` で提案する。
  位置は `box01`（アタリ画像の中の 0..1 の `[x, y, 幅, 高さ]`）で渡せる。縦書きの列の区切りは `\n`。台詞が無いページは
  `record_review`（`kind: "atari_lines"`）で知らせる。
- `atari_layout`: コマ割りの提案が無い（見つからなかった・却下された）。`mcp__genko__analyze_name` を `params` を変えて試すか、`ask_human`。
- `brief_panels`: 確定したコマごとに、アタリを見て指示（shot、angle、人物、動き、表情）を `apply_ops` の `set_panel` で書く。
- 提案を出したら `review_page` の場所を人間に知らせて、確定を待つ。

## 作画

作画はパイロットページ（ふつうは 1 ページ目）から始まる。パイロットページの作画が承認されると、絵柄（参照画像）と使う画像ツールが固定され、残りのページの依頼パックに入る。それまで他のページの作画は `next` に出ない。

- `make_sheet`: `mcp__genko__generation_request`（`character_id`）で設定画の依頼パックを受け取り、画像生成で作る。
  顔が正面を向いたアップを必ず入れる（承認時に顔の参照として切り出される）。画像を返された `inbox` のフォルダに保存し、
  `mcp__genko__import_images`（`request_id`、`images: [{file, origin}]`）で取り込む。取り込んだら人間に選んでもらう（承認を頼む）。
- `gen_panel`: `mcp__genko__generation_request`（`page`, `frame_id`）で依頼パックを受け取る。
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
- `review_candidates`: `mcp__genko__candidates`（metrics の `rank` が小さいほど目安が良い）と `mcp__genko__render`（`kind: "compare"`、`candidate_id`）で比べる。
  赤い線がネームの構図。構図がネームに合うか、人物が設定画に似ているか、手足の破綻、画内の文字、台詞の場所が空いているかを見て、
  `review_candidates` で点数（0〜1）とメモを残し、良いものを `mcp__genko__adopt`。どれも駄目なら直しの依頼を作る。
  1 コマ 8 枚・直し 2 巡を超えると、そのコマは人間の判断待ちになる。
- `fix_panel`: 人間の指示（`comments`）どおりに直しの依頼を作る（`instruction` に指示を入れる）。
- `report_regions`: 採用した絵の顔と人物の位置を `mcp__genko__report_regions` で報告する（`box01` は画像の中の 0..1 の `[x, y, 幅, 高さ]`）。
  人物がいない絵なら `apply_ops` の `record_review`（`kind: "regions"`、`frame_id`、`input_hash` に採用中の候補 id）。
- `upscale_panel`: 画像ツールに高解像度化があれば `generation_request`（`mode: "upscale"`）で作り直して取り込み、採用し直す。
  無ければ `record_review`（`kind: "upscale"`、`input_hash` に採用中の候補 id）で理由を残す。
- `finish_page`: `mcp__genko__finish_page` を `commit: false` で見て（顔にかかる台詞の移動、効果）、よければ `commit: true`。
- 線をくっきりさせたいコマは `mcp__genko__derive`（`kind: "lineart"`）で採用中の絵から線を抜き出し、`adopt`（`to: "ink"`）で絵の上に置く。
  モノクロの原稿では、グレーの絵は印刷のときに Genko が網点に変える。画像はグレースケールで、トーンを描き込みすぎずに作る。
- 効果音はネーム計画の台詞で `balloon: "sfx"` にする（Genko が大きな縁取り文字で描く。画像に描かせない）。

## 承認を頼む

`waiting_for` の内容ごとに `mcp__genko__request_approval` を 1 回出す。

- `name` / `art`: `gate` と `pages`。
- `sheet`: `gate: "sheet"` と `character_id`。候補が複数あれば `note` におすすめを書く。
- `export`: 全ページの仕上げが済んだら、`mcp__genko__preflight` で止めている理由を確かめ、`mcp__genko__export_proof` で校正を出してから依頼する。

依頼を出したら `mcp__genko__review_page` で確認ページ（review.html）を作り、その場所（`path`）と何を見てほしいかを、
メッセージで人間に知らせる。承認するのは人間で、コマンドは確認ページに書いてある。人間が「OK」と返事をしても、あなたが承認を付けることはできない。

## してはいけないこと

- 承認を付けようとしない。承認は人間だけが行う（Genko の MCP には承認の道具が無い）。
- 人間が承認したページのコマ割りを変えない。直す必要があれば人間に頼む。
- 人間が固定した欄（pinned）・人間が描いた領域・承認済みの設定画を変えようとしない。
- 台詞や効果音を「絵の中の文字」として描かせない。文字は Genko が描く。
- 画像のバイト列を道具の引数に書かない。画像はファイル（`inbox`）かアップロードで渡す。
- 本番の書き出しをしない（`export_proof` の校正だけはよい）。
