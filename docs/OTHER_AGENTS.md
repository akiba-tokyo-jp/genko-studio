# Hermes Agent 以外から使う（M9-4）

Genko の MCP サーバーは `genko mcp --root <原稿のフォルダ> --agent ai:<名前>`（stdio）で、どの MCP クライアントからも同じ道具が使える。
道具の名前はクライアントが前に付ける名前で変わるだけで、中身は同じ:

| クライアント | 道具の名前 | スキル（手順書）の置き場所 |
|---|---|---|
| Hermes Agent | `mcp_genko_<名前>` | `integrations/hermes/genko-manga/` |
| Claude Code | `mcp__genko__<名前>` | `integrations/claude-code/skills/genko-manga/` |
| そのほか | クライアントによる | MCP の resource `genko://guide/skill`（`mcp_genko_` の名前で書いてある） |

`--agent` は必ず `ai:` で始める。MCP には承認の道具が無く、`ai:` の名前では承認も取り消しもできない。

## Claude Code

1. Genko を入れる: `uv tool install "genko-studio[mcp] @ /path/to/genko-studio"`。
2. サーバーを登録する（どちらか）:

   ```bash
   claude mcp add genko -- genko mcp --root ~/manga --agent ai:claude-code
   ```

   またはプロジェクトの `.mcp.json` に `integrations/claude-code/mcp.example.json` の中身を置き、`--root` を直す。
3. スキルを置く: `integrations/claude-code/skills/genko-manga/` を `.claude/skills/`（そのプロジェクトだけ）か `~/.claude/skills/`（全部）にコピーする。
   中身は Hermes のものと同じで、道具の名前だけ `mcp__genko__` になっている。
4. Claude Code はシェルを使えるので、人間用のコマンド（`genko studio approve` など）も叩けてしまう。
   `integrations/claude-code/settings.example.json` の `permissions.deny` を `.claude/settings.json` に足し、承認・取り消し・本番の書き出し・提案の確定と、
   原稿のフォルダの直接の書き換えを止める。これは手違いを防ぐためのもので、同じ利用者の権限で動く以上、完全な壁ではない。
   人間の操作を確実に分けたいときは、Genko を別の利用者か別のマシンで動かす（`genko serve` とトークン）。
   承認を変えたのが誰かは `genko studio audit PROJ` で確かめられる。
5. 確かめる: Claude Code で `/mcp` を開き `genko` が connected であること。「Genko のプロジェクトを一覧して」と頼んで `mcp__genko__projects` が呼ばれること。

## Claude Desktop

`claude_desktop_config.json`（設定 → 開発者 → 設定を編集）に足す:

```json
{
  "mcpServers": {
    "genko": {"command": "genko", "args": ["mcp", "--root", "/Users/you/manga", "--agent", "ai:claude-desktop"]}
  }
}
```

スキルは、プロジェクトの指示に `genko://guide/skill` の中身を貼るか、最初に「resource genko://guide/skill を読んで」と頼む。
Claude Desktop は画像を inbox のフォルダに保存できないことが多い。そのときは HTTP の `POST /v1/assets` で渡すか、
人間が画像を inbox に置いて `import_images` だけを頼む。

## そのほかの MCP クライアント（Python SDK）

`integrations/generic/mcp_client_example.py` が最小の例（公式の Python SDK、stdio）。サーバーを子プロセスで起こし、
プロジェクトを作って企画書・脚本・1 ページのネームを送り、`next` が次に何を求めるかと、スキルの長さを JSON で出す:

```bash
uv run --extra mcp python integrations/generic/mcp_client_example.py --root /tmp/manga
# {"tools": 31, "create": true, "bible": true, "script": true, "name": true, "next": ["review_name"], "skill_chars": …}
```

自分のエージェントに組み込むときは、固定の JSON をエージェントの出力に置き換え、`next` のループ（SKILL.md の「ループ」）を回す。
別のマシンから使うときは、MCP の代わりに `genko serve`（HTTP、トークン必須）を使う。

## 出力の形（M9-1）

人間が選ぶ本番の書き出しに、画面向けの 2 つを足した。どちらも裁ち落としを切り、網点にせず（トーンは平らなグレー、カラーの絵は色のまま）、sRGB のプロファイルを付ける。

```bash
genko studio export PROJ --format webtoon --out ./web --as human:名前   # 幅 800 px の縦長を 1280 px ごとに切る: 001.png, 002.png …
genko studio export PROJ --format sns --out ./sns --as human:名前       # 1 ページ 1 枚、長辺 2048 px の JPEG
genko export PROJ ./web --format webtoon --width 720 --max-height 2000 --jpeg
genko export PROJ ./sns --format sns --long-edge 1600 --spreads          # 見開きも 1 枚ずつ
```

カラーのページ（`spec.expression` が `color`）は、印刷でも網点にしない。依頼パックの `color` が true になり、プロンプトの下書きはカラーの絵柄で、
`avoid` から「色」が外れる。

## マネキン（M9-3）

ポーズを指定したいときは、コマの中にマネキン（棒人形）を置く。人間は `genko apply PROJ ops.json`、エージェントは `apply_ops` で置ける
（ネームの段階で、構図を固めるためにエージェントが置いてもよい）。

```json
{"op": "add_mannequin", "page": 1, "pos": [120, 200, 0], "height_mm": 90, "preset": "walk"}
{"op": "pose_mannequin", "page": 1, "id": "…", "joints": {"r_arm": {"yaw": -1.2}}, "rot": [0, 0.5, 0]}
```

- 関節は首・肩・肘・手首・股・膝・足首。`yaw` は紙の面での曲げ、`pitch` は手前・奥への曲げ（線が短くなる）。
- `rot` は `[前後の傾き, 体の回転, 左右の傾き]`。体の回転が 90° を越えると後ろ向きになる。
- プリセット: `stand`、`walk`、`run`、`sit`（横向き）、`point`、`look_back`、`arms_up`。
- マネキンはネームと校正にだけ描かれ、印刷には出ない。コマの中にあると、そのコマの依頼パックの `guides/pose.png` に描かれ、
  `notes_for_agent` に「マネキンがポーズの指定」と入る。

## 内容の制限（M9-2）

実装しない。何を描くかはネームと絵を作る AI（とその提供元の方針）で決まり、Genko は制限をかけない。人間は作画の承認で全ページを見る。

## 通しの確認（人間が行う）

Hermes 以外で `docs/STUDIO_EVAL.md` の「パイロットページの通し」を 1 回行い、次を確かめる:

- [ ] 道具の一覧が出る（31 個）。`inspect`（`target: "rules"`）が読める。
- [ ] スキルが読み込まれ、エージェントが `commit: false` → `commit: true` の順で書く。
- [ ] ネームの承認待ちで止まり、`review_page` の場所を知らせてくる。
- [ ] 依頼パックの参照画像・ガイドを画像ツールに渡し、inbox に置いて `import_images` できる。
- [ ] エージェントが承認・本番の書き出しをしようとしない（Claude Code では deny が効いている）。
- [ ] `genko studio audit PROJ` が `ok: true`。
