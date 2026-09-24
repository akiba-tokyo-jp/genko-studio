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
   - `make_sheet`: その登場人物の設定画を画像生成で作り、`mcp_genko_import_image` で取り込み、`mcp_genko_apply_ops` の
     `import_candidates`（`character_id` と `candidates:[{id, asset, px, origin:{kind:"agent"}}]`）で候補にして、
     `mcp_genko_request_approval`（`gate: "sheet"`、`character_id`）で人間に選んでもらう。
   - `make_art` / `import_art` / `fix_art`: `mcp_genko_inspect`（target `panel`）でコマのブリーフ・寸法（mm）・承認済みの設定画を読み、
     その縦横比で絵を生成する。台詞と効果音は描かせない。`import_image` → `import_candidates`（`page`, `frame_id`）。
     `fix_art` は `comments` の指示に従う。
   - `choose_art`: `mcp_genko_render`（`frame_id`）で候補を見比べ、`review_candidates` で点数を残し、`adopt_candidate` で採用する。
     はみ出しは Genko が切り取る。位置は `set_placement`（`offset_mm`, `scale`）で直せる。
   - ページの全コマに絵が入ったら `request_approval`（`gate: "art"`）で人間に作画の確認を頼む。
3. 書く道具は、まず `commit: false` で呼ぶ。`issues` に `severity: "error"` があれば、`path` の場所だけを直して送り直す。エラーが無くなったら `commit: true`。
4. 同じ指摘が 3 回直しても消えないときは、無理に続けず、`request_approval` の `note` に状況を書いて人間に相談する。
5. 自己点検が済んだページは `mcp_genko_request_approval`（pages を指定）で人間に承認を頼む。
6. `next` の結果が空で `waiting_for` だけになったら、人間の承認待ち。人間に「ネームの確認をお願いします」と知らせ、確認用の画面は `genko studio review <プロジェクト> --out review.html` で作れることを伝える。

## してはいけないこと

- 承認を付けようとしない。承認は人間だけが行う（Genko の MCP には承認の道具が無い）。
- 人間が承認したページを作り直さない。直す必要があれば人間に頼む（承認済みのネームのコマ割りは変えられない）。
- 人間が固定した欄（pinned）・人間が描いた領域・承認済みの設定画を変えようとしない。
- 台詞や効果音を「絵の中の文字」として扱わない。文字は Genko が描く。
