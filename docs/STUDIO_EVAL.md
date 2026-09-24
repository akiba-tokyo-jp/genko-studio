# 実際のエージェントでの確認と評価（M5-1・D5）

CI では走らせない。人間が Hermes Agent（LLM は Claude か ChatGPT、画像は ChatGPT の画像生成）で行い、結果を記録する。
Genko の側で数えられるものは Genko が記録するので、人間が書き留めるのは「道具の外で起きたこと」だけでよい。

## 1. 準備

1. Genko を入れる: `uv tool install "genko-studio[mcp] @ /path/to/genko-studio"`（`genko` コマンドが使えること）。
2. 原稿のフォルダを決める（例 `~/manga`）。
3. Hermes の設定に `integrations/hermes/config.example.yaml` の `mcp_servers.genko` を足す。`--agent ai:hermes` のまま。
4. スキルを入れる: `integrations/hermes/genko-manga/` を Hermes のスキルのフォルダに置く（同じ内容を MCP の resource `genko://guide/skill` でも読める）。
5. 画像ツールを登録する: `genko studio tools set openai:gpt-image-1`。対応サイズや参照・マスクの可否が違えば `--file` で直す（`genko studio tools example` が雛形）。
6. Hermes で `mcp_genko_projects` が呼べることを確かめる。

## 2. M5-1 パイロットページの通し

Hermes に頼む文の例:

> Genko で 8 ページの短編を作って。題材は「〇〇」。ネームは全ページ、絵は 1 ページ目（パイロット）を承認まで。承認が要るところで止まって、確認ページの場所を知らせて。

人間がすること（すべて `--as human:<名前>`）:

| 場面 | コマンド |
|---|---|
| 確認ページを開く | エージェントが知らせた `studio/review.html`（自分で作るなら `genko studio review PROJ --out review.html`） |
| ネームの承認 | `genko studio approve PROJ name --pages 1-8 --as human:名前` |
| 直してほしい | `genko studio comment PROJ --page 3 "指示" --as human:名前`（コマだけなら `--frame コマid`） |
| 設定画の承認 | `genko studio approve PROJ sheet --character hina --candidate 候補id --as human:名前`（顔の範囲を指定するなら `--face x,y,w,h`） |
| 作画の承認 | `genko studio approve PROJ art --pages 1 --as human:名前` |
| エージェントの相談に答える | `genko studio close-ticket PROJ チケットid --reply "指示" --as human:名前` |
| 取り消し | `genko studio revoke PROJ name --pages 2 --reason "理由" --as human:名前` |

通しが終わったら（途中でもよい）記録を取る:

```bash
genko studio stats PROJ > stats.json     # ネームの出し直し、1 コマの取り込み枚数（中央値）、失敗した道具の呼び出しと理由
genko studio audit PROJ                  # 承認を変えたのが人間だけか（ok: true であること）
```

`stats` の見どころ:

- `images_per_adopted_panel.median` ≤ 8（受入基準）。
- `tool_calls.failure_reasons`: 存在しない道具・引数の間違い・同じ指摘の繰り返し。道具の説明と SKILL.md の直しに使う。
- `pages[].name_submits`: ページごとのネームの出し直し。
- `tickets.help`: エージェントが人間に相談した回数。

人間が書き留めること（`docs/eval/` に日付ごとの Markdown で残す）:

- 使った LLM と画像ツールの版、所要時間。
- エージェントが依頼パックを読み違えた例（参照画像を渡さなかった、サイズを守らなかった、文字を描かせた など）と、その時の道具の応答。
- 取り込みに失敗した例（inbox の外に置いた、来歴を書かなかった など）。
- 良かった候補・悪かった候補の例（候補 id）。

## 3. D5 盲検のキャラ同定

目的: 描いたキャラクターが、設定画と同じ人物に見えるか。10 コマ × 主要キャラ（3 人程度）を、設定画だけを見た評価者 5 名が当てる。目標は 90% 以上。

1. 標本を作る（1 人だけが写った採用済みのコマから、キャラごとに最大 10 コマ。台詞は入れない）:

   ```bash
   genko studio eval-sample PROJ --out d5 --per-character 10 --seed 1
   ```

   `d5/form.html`（設定画は A・B・C の文字だけで示す）、`d5/answers.csv`、`d5/key.json`（正解。評価者に見せない）ができる。
2. 評価者に `form.html` と `answers.csv` を渡す。評価者は自分の名前と、各標本の文字を書く（1 人 1 ファイルでも、1 ファイルにまとめてもよい）。
3. 採点する:

   ```bash
   genko studio eval-score d5 answers.csv
   ```

   `accuracy` が 0.9 以上なら合格（`passed: true`）。`by_character` で低いキャラがいれば、設定画の渡し方（顔の切り出し範囲、参照の順）と、
   直しの手順（`focus_character` の inpaint）を見直す。

## 4. 評価セット（D0 の premise を使う）

premise を 5 本用意し、それぞれで 2 の通しを行う。測るもの（§10.8）:

- ネーム: ページごとの出し直しの回数、指摘の種類の内訳、最後まで行けたページの割合（`stats`）。
- 道具の選び間違いの回数（`stats.tool_calls`）。
- 絵: 1 コマあたりの取り込み枚数、採用までの直しの回数（`stats`）。
- 人手評価: 読みやすさ、テンポ、めくり、キャラの同定（D5）。
