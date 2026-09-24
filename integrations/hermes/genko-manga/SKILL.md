---
name: genko-manga
description: Genko Studio を MCP で操作して、企画から漫画のネーム（コマ割り・台詞の配置）と作画（生成した絵をコマに置く）までを進める手順。漫画のネーム、コマ割り、作画、Genko の話が出たら使う。
---

# Genko でネームを作る

Genko は漫画原稿のシステムで、MCP サーバー `genko` として接続されている（道具は `mcp_genko_<名前>`）。
Genko は文章も絵も作らない。企画書・脚本・ネーム計画は、あなたが書く。Genko はそれを検査し、コマ割りと縦書きの写植を計算して、プレビュー画像を返す。

## 最初に

1. `mcp_genko_projects` でプロジェクトを確かめる。無ければ `mcp_genko_create_project`（例: name `summer.genko`、pages 8）。
2. `mcp_genko_inspect` を `target: "rules"` で呼び、ネームの規則を読む。`target: "schemas"` で入力の形を読む。

## ループ

1. `mcp_genko_next` で次の作業を1つ取る。`tools` に使う道具の目安がある。
2. 作業の種類ごとに:
   - `write_bible`: 企画書を書いて `mcp_genko_set_bible`。
   - `write_script`: 脚本を書いて `mcp_genko_set_script`。beat ごとに `page` を決める。見せ場（reveal）は偶数ページの先頭。
   - `plan_page`: `mcp_genko_inspect`（target `page`）でそのページの beat を読み、ネーム計画を書いて `mcp_genko_submit_name`。
   - `review_name`: `mcp_genko_render` で画像を見て、読み順の迷い・窮屈なコマ・弱いめくりを確かめる。直すなら `submit_name` を `replace: true` で送り直す。よければ `mcp_genko_record_review`。
   - `revise_page`: 人間の指示（`comments`）に従って `submit_name` を `replace: true` で送り直す。
   - `make_sheet`: `mcp_genko_generation_request`（`character_id`）で設定画の依頼パックを受け取り、画像生成で作る。
     画像は返された `inbox` のフォルダに保存し、`mcp_genko_import_images`（`request_id`、`images: [{file, origin}]`）で取り込む。
     候補ができたら `request_approval`（`gate: "sheet"`）で人間に選んでもらう。
   - `gen_panel` / `fix_panel`: `mcp_genko_generation_request`（`page`, `frame_id`。修正は `mode: "edit"` と `parent`、
     一部だけなら `mode: "inpaint"` と `regions`）で依頼パックを受け取る。`prompt` は下書きなので書き直してよいが、
     登場人物の見た目の記述と `avoid`（文字・フキダシ・色を描かない）は守る。`files` の参照画像（設定画・顔）と
     構図のガイドを、画像ツールが受け付けるなら渡す。サイズは `size.suggested_px` か `size.tool_sizes` から選ぶ。
     生成した画像を `inbox` に保存して `import_images`。`origin` には使ったツール・モデル・実際のプロンプトを書く。
   - `import_pending`: 依頼済みで画像がまだのコマ。`inbox` に画像を置いて `import_images`。
   - `review_candidates`: `mcp_genko_candidates` と `mcp_genko_render`（`kind: "compare"`、`candidate_id`）で比べる。
     構図がネームに合うか、人物が設定画に似ているか、手足の破綻、画内の文字、台詞の場所が空いているかを見て
     `review_candidates` で点数を残し、良いものを `mcp_genko_adopt`。どれも駄目なら修正の依頼を作る。
   - `report_regions`: 採用した絵の顔と人物の位置を `mcp_genko_report_regions` で報告する（`box01` は画像の中の 0..1）。
     人物がいない絵なら `apply_ops` の `record_review`（`kind: "regions"`、`frame_id`、`input_hash` に採用中の候補 id）。
   - ページの全コマに絵が入ったら `request_approval`（`gate: "art"`）で人間に作画の確認を頼む。
   - `upscale_panel`: 画像ツールに高解像度化があれば `generation_request`（`mode: "upscale"`）で作り直して取り込む。
     無ければ `record_review`（`kind: "upscale"`）で理由を残す。
   - `finish_page`: `mcp_genko_finish_page` を `commit: false` で見てから `commit: true`。
   - 全ページが仕上がったら `mcp_genko_preflight` で止めている理由を確かめ、`mcp_genko_export_proof` で校正を出し、
     `request_approval`（`gate: "export"`）で人間に本番の書き出しを頼む。
3. 書く道具は、まず `commit: false` で呼ぶ。`issues` に `severity: "error"` があれば、`path` の場所だけを直して送り直す。エラーが無くなったら `commit: true`。
4. 同じ指摘が 3 回直しても消えないときは、無理に続けず、`mcp_genko_ask_human` で人間に相談する（その作業は人間が閉じるまで `next` に出なくなる）。
5. 自己点検が済んだページは `mcp_genko_request_approval`（pages を指定）で人間に承認を頼む。
6. `next` の結果が空で `waiting_for` だけになったら、人間の承認待ち。人間に「ネームの確認をお願いします」と知らせ、確認用の画面は `genko studio review <プロジェクト> --out review.html` で作れることを伝える。

## してはいけないこと

- 承認を付けようとしない。承認は人間だけが行う（Genko の MCP には承認の道具が無い）。
- 人間が承認したページを作り直さない。直す必要があれば人間に頼む（承認済みのネームのコマ割りは変えられない）。
- 人間が固定した欄（pinned）・人間が描いた領域・承認済みの設定画を変えようとしない。
- 台詞や効果音を「絵の中の文字」として扱わない。文字は Genko が描く。
- 画像のバイト列を道具の引数に書かない。画像はファイル（`inbox`）かアップロードで渡す。
- 本番の書き出しをしない。書き出しは人間が行う（`export_proof` の校正だけはよい）。
- 並行して動くときは `next` を `claim: true` で呼ぶ。
