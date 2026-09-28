# Genko エージェント駆動の漫画制作 設計

対象: genko-studio @ `7e65e4c`（Python 3.11+、core 依存は Pillow のみ、GUI は PySide6 extra）。
本書は設計である。この文書の時点でリポジトリのコードは変えていない。
読者: Genko の実装者、Genko を操作する AI エージェント（Hermes Agent など）とその設定者、原稿の承認者（人間）。
表記: 行番号は `7e65e4c` のもの。op 名・JSON キー・コードは英語、説明は日本語。
改訂: 第3版（2026-09-24）。**方針を変えた。Genko は Claude・ChatGPT などの LLM も画像生成モデルも内蔵しない。** 外部の AI エージェント（Hermes Agent など）が MCP で Genko を操作し、ネーム出しや画像生成はエージェント側のモデルとツールで行う。第2版の「Claude を組み込む」前提（LLM プロバイダ、画像バックエンド、外部送信の設定、LLM の費用管理）は削除した。既存コードの調査結果（§0.5）、データモデル、op、段組 DSL、写植、スクリーントーン、M1〜M3 の土台工事は引き継いだ。利用者が決めたことと残っていることは §13 にまとめた。

---

## 0. 目的と要約

### 0.1 目的

生成AIが主体となって、企画 → 脚本 → ネーム → 作画（生成・編集）→ 仕上げ → 書き出しまでを進める。
ただし「考える」「描く」のは Genko の外にいる AI エージェントであり、Genko はエージェントが操作する**原稿のシステム**に徹する。

- **エージェント（Hermes Agent など）:** 自分の LLM（Claude、ChatGPT など）で企画・脚本・ネーム計画を書き、自分の画像ツール（ChatGPT の画像生成など）で絵を作る。Genko のプレビューを見て自分で直す。
- **Genko:** エージェントが渡すデータ（企画書、脚本、ネーム計画、画像）を検査し、コマ割り・写植・配置・仕上げ・書き出しを決まった手順で計算し、原稿として保存する。何をどう直せばよいかを具体的に返す。
- **人間:** 要所（キャラ設定画、ネーム、ページごとの絵、書き出し）で承認し、いつでも直せる。

参考は MANGA BRIDGE v2 の2画面である。

- 画面1（`1.jpg`）: 製品モック。プロンプトとモデル中心のコンソール。
- 画面2（`2.jpg`）: 手描きアタリ起点の操作シミュレーション。コマ単位の「指示 → 候補 → 採用／修正指示 → 履歴」。

参考UIはボタンを人が押す前提だが、Genko では同じ操作をエージェントの道具（MCP ツール）として出し、人間向けの画面は確認と承認に絞る。

### 0.2 要約（1画面）

- **Genko は文章も画素も作らない。** 「画像生成は `put_raster` の外側。Genko は画素を発明しない」という既存の原則を、台詞や脚本にも広げる。Genko の中に LLM や画像モデルの呼び出しはなく、**Genko から外部への通信もない**。何をどの AI に送るかはエージェント側で決まる。
- **接続は MCP。** Genko は MCP サーバーとして動き、Hermes Agent は `~/.hermes/config.yaml` の `mcp_servers` に登録して道具を読み込む。同じマシンなら stdio、別のマシンなら HTTP（トークン必須）で、どちらにも対応する（どちらで使うかは決めなくても進められる、§13.2）。CLI と HTTP API からも同じ操作ができる。
- **道具はエージェントのために作る。** 入力は平らな JSON（再帰なし）、エラーは直す場所の JSON ポインタ付き（例 `/panels/2/lines/0: 台詞が 40 字を超えている`）、書き込みは既定で dry_run、プレビュー画像を必ず返す。「次にやること」（worklist）を状態から計算して返すので、エージェントは迷わず進める。
- **作業単位はコマ**（葉 Frame）。`Frame.panel`（PanelSpec）に指示・参照・領域・候補・採用を持つ。
- **単一コマンドバス。** 変更はすべて `genko.ops.apply_ops`。MCP の道具も最後は op になる。AI 専用の書き込み経路は作らない。
- **ネーム:** エージェントがネーム計画（段組 DSL + コマごとの指示 + 台詞）を出し、Genko が検査・コマ割り・縦書き写植・プレビュー描画を行う。エージェントはプレビューを見て直し、人間が `name_ok` を付ける。
- **絵:** Genko がコマごとの**生成依頼パック**（生成サイズ、プロンプトの下書き、キャラ設定画・ネームの切り抜き・マスクなどの参照ファイル、描かせてはいけないもの）を出す。エージェントは自分の画像ツールで生成し、画像を候補として Genko に取り込む。採用するとコマに収まる。
- **画像のバイト列は LLM の文脈を通さない。** 画像はファイル（同じマシン）かアップロード（別マシン）で受け渡し、ツールの引数に base64 を書かせない。LLM が数 MB の base64 を正しく書き写すことはできないからである。
- **ゲート。** 既存の `name_ok` に `sheet`・`script`・`art`（ページ単位 `art_ok`）・`export` を加える。**承認できるのは人間だけで、MCP には承認の道具を出さない。** エージェントは承認を「依頼」し、人間が CLI・レビュー画面・GUI で承認する。
- **来歴。** 取り込んだ画像はすべて、どの生成依頼に対して、どのエージェントが、どのツール・モデル（自己申告）・プロンプトで作ったかを持つ。これは候補の比較や作り直しのための作業記録で、書き出しには含めない。
- **非破壊。** 候補は消さない。採用の取り消しは `unadopt` か別候補の採用。取り込んだアタリは不変資産。
- **構造から導出。** Genko で作ったネームなら、マスク・文字よけ・人物の位置・構図ガイド・生成サイズはデータから計算する。
- **オフラインで試験。** 試験は「筋書きどおりに道具を呼ぶ偽エージェント」と、あらかじめ用意した画像で行う。ネットワークも LLM も要らない。
- **最初の価値は「エージェントがネームを出せる」こと。** 今の保存形式の上に MCP サーバーとネームの道具を薄く載せる（§11 の M0）。並行して既存の欠陥を直し（M1）、その後に保存形式の更新（M2）、コマへの画像配置（M3）、生成依頼パックと画像の取り込み（M4）と積む。

### 0.3 アーキテクチャ図

```text
  Hermes Agent（別プロセス。同じマシンか別マシン）
   ├─ LLM: Claude / ChatGPT / ほか ……… 企画・脚本・ネーム計画を書く、プレビューを見て直す
   ├─ 画像ツール: ChatGPT の画像生成 ほか … 生成依頼パックを使って絵を作る
   ├─ スキル: genko-manga（SKILL.md）…… 手順とルール（Genko が配布）
   └─ MCP クライアント
          │  stdio（同じマシン） または HTTP + Bearer トークン（別マシン）
          │  道具: status / next / inspect / render / set_bible / set_script / submit_name /
          │        generation_request / import_images / candidates / adopt / request_approval …
          ▼
 ┌──────────────── genko mcp（MCP サーバー） ─────────────────┐     人間
 │ actor を起動時に固定（例 ai:hermes）。承認の道具は無い        │     ├─ CLI: genko studio approve …（human:*）
 │                                                             │     ├─ review.html（確認と承認コマンド）
 │  StudioService（CLI・HTTP・GUI と共通の窓口）                │     └─ GUI（M7）
 │   ├ schemas + lint ……… 入力の検査。JSON ポインタ付きの指摘  │            │
 │   ├ layout / letter …… 段組 DSL → コマ、縦書き写植（決定的）│            │
 │   ├ genreq …………… 生成依頼パック（サイズ・下書き・参照）    │            │
 │   ├ importer ………… 画像の検証 → assets/ → 候補（来歴付き）  │            │
 │   ├ worklist ………… 状態から「次にやること」を計算（純関数） │            │
 │   └ render ………… name / proof / コマの切り抜き・比較画像    │            │
 └──────────────┬──────────────────────────────────────────────┘            │
                │  commit.py: lock → load → ops 生成 → dry_run → apply → 保存   │
                ▼                                                             ▼
 genko.ops.apply_ops   単一コマンドバス（全か無か、dry_run、actor、page lock、ゲート規則）
                │
                ▼
 project.json（判断: bible・脚本・PanelSpec・候補メタ・採用・承認）
 assets/（画像。内容アドレスで不変） studio/requests/（生成依頼パック） studio/inbox/（取り込み待ち）
                │
                ▼
 render / export   コマでクリップ、モノクロ仕上げ、縦書き写植 → PNG / TIFF(2値) / PDF / PSD / EPUB / pack
                   （NAME・DRAFT・候補・参照画像は出ない。preflight: 承認・来歴・実効 dpi）
```

### 0.4 第2版からの変更

| 項目 | 第2版（Claude を組み込む） | 第3版（エージェントが操作する） |
|---|---|---|
| 文章を書く主体 | Genko 内の Scriptwriter / Namer（Claude API） | エージェントの LLM（Claude、ChatGPT ほか。Hermes の設定で選ぶ） |
| 絵を作る主体 | Genko 内の ImageBackend（ComfyUI） | エージェントの画像ツール（今は ChatGPT の画像生成） |
| Genko から外部への通信 | Claude API、ComfyUI | **なし** |
| 外部送信の設定（locality） | `studio init` で必須 | 削除（エージェント側の問題） |
| LLM の費用・キャッシュ・モデル設定 | ledger、予算の予約、プロンプトキャッシュ | 削除（エージェント側の問題） |
| 出力の形式を強制する方法 | Claude の structured outputs | MCP ツールの入力スキーマ + Genko の検査と JSON ポインタ付きの指摘 |
| 工程を進める主体 | Genko の runner（worklist を回す） | エージェントのループ。Genko は worklist（`next`）で次の作業を示すだけ |
| 画像の審査（judge） | Claude vision | エージェントが比較画像を見て点数を記録する。最終判断は人間 |
| 内容安全 | ローカル分類器を必須に | Genko は制限をかけない。描く内容はネームと絵を作る AI とその方針で決まり、人間が art ゲートで全ページを見る（M9-2 は実装しない） |
| 部品のライセンス | ComfyUI の部品ごとの manifest | 削除（Genko は生成しない。使ったツールとモデルは来歴として記録するだけ） |
| 接続 | CLI・HTTP が主、MCP は M9 | **MCP が主**（M0 から）。CLI・HTTP は同じ機能 |
| 引き継いだもの | – | 既存コードの調査（§0.5）、データモデル、op、段組 DSL、写植、スクリーントーン、読み順とめくり、M1〜M3 の土台工事、HTTP の安全対策 |

### 0.5 既存コードの確認済み事実（設計の前提と訂正）

以下は `7e65e4c` で読んで確かめた。提案段階の誤りは「訂正」、第2版で見直したものは「訂正（第2版）」「新規（第2版）」と書いた。計測は改訂時に同じコミットで行った。

| # | 事実 | 場所 |
|---|---|---|
| 1 | `apply_ops` は作業用に `copy.deepcopy(episode)` し（994）、commit 時にもう一度 deepcopy を undo_stack に積んでから `_copy_state` する（1012-1015）。`bytes` は共有される（`copy.deepcopy(b) is b`）。**訂正（第2版）:** `undo_stack` は Episode の dataclass フィールド（models.py:393）なので、**どちらの deepcopy も undo 履歴全体を複製する**。ストロークの座標リストも毎回複製される。1 op の費用は「undo の深さ × プロジェクトの大きさ」。計測（B4 16 ページ、各ページ NAME ストローク 30 本）: 深さ 0 で 0.10 秒、10 で 1.0 秒、20 で 2.1 秒、50 で約 5.5 秒。lt_convert の後は深さ 11 で 1 op 25.7 秒。古い `raster_png` を無制限の `undo_stack` が持ち続ける問題もある | ops.py:972-1016, models.py:393 |
| 2 | **訂正:** `_copy_state` は現在 Episode の全フィールド（undo_stack 以外）を列挙して写しており、今は何も失っていない。新フィールドを足すと黙って消える、という将来の罠である | ops.py:97-113 |
| 3 | `ProjectLock.acquire` は「存在確認 → 書き込み」で `O_EXCL` を使わない。15分の失効判定は **lock.py:25**（訂正: 38 は `__enter__`）。**新規（第2版）:** 失効したロックは unlink してから書き直す（26-27）ので、2つのプロセスが同時に失効を見ると、後の方が先の方の新しいロックを消す。`release` は所有者を確かめずに unlink する（33-35）ので、自分のロックが失効したプロセスが次の持ち主のロックを消す | lock.py:17-35 |
| 4 | CLI `apply` はロック前にロードする（131 load、132 lock、133 apply、agent 未指定）。HTTP `/v1/apply` も同じ（**server.py:108 load、110 lock、111 apply**。訂正: server.py は 179 行しかない） | __main__.py:131-133, server.py:104-116 |
| 5 | `put_raster` は name_ok を見ない。relpath はページ番号と role から作る。USER レイヤーは `user-<id>.png` | ops.py:366-389（386） |
| 6 | `save_raster` は USER レイヤーでも `pages/NNN/user.png` を使う。`flood_fill` も role 名 | raster.py:22-26, ops.py:872 |
| 7 | `delete_page` と `reorder` はページ番号を振り直すが relpath・tickets・page_locks・spread_with・onion_from は直さない。`duplicate_page` は relpath を共有し、複製した台詞の `frame_id` は元ページのフレームを指したまま | ops.py:427-460, models.py:404-413 |
| 8 | render はラスタをページ全体に `resize` し（463）、フレームでクリップしない。**訂正:** ストロークを焼いたラスタは焼く時点で葉フレームにクリップ済み（raster.py:29-39、61 で呼ぶ）。put_raster したラスタだけが非クリップ。`opacity` 0 は `or 1.0` で 1 になる（465）。`Frame.bleed` は描画で使われない | render.py:451-467 |
| 9 | `name_ok` はページ省略時に全ページを対象にし、stage を ink にする。ゲート検査は add_stroke（ink）・flood_fill（ink）・lt_convert（→ink）の3か所だけ。`advance to=finish` は無条件 | ops.py:278-283, 336-337, 840-841, 932-933, pipeline.py:10-13 |
| 10 | **訂正:** compact な `snapshot` は、すでに葉フレームの矩形と台詞の `x_mm/y_mm` を含む。含まないのはストローク座標（`--full` 時のみ）と画像。bible・ラスタ有無・page_locks は無い。`inspect_stroke` は headless.py:75-87 | headless.py:27-72 |
| 11 | **訂正:** マネキンは 2D 棒人形。腕と脚は yaw だけで、肘・膝・肩・首の関節がない。`size` と `rot` は描画で無視され、肢の長さは mm 固定（頭は腰の 42 mm 上）。OpenPose の直接の入力元にはならない | ops.py:700-741, render.py:255-289 |
| 12 | **新規指摘:** `Page.is_recto` は綴じ方向を無視して「奇数 = recto」。`render_spread` は recto を右に置く。既定 `Binding.RIGHT`（右綴じ、奇数ページが左）では見開きが左右逆になる。しかも `add_stroke` の見開き座標は相手ページを右側とみなしており（x ≥ 紙幅で相手ページへ）、描画と食い違っている。**新規（第2版）:** `set_spread` は対が向かい合えるかを検査しない（ops.py:495-499）。右綴じの既定ではページ 1 と 2 は同じ紙の表裏で、見開きにならない。GUI のキャンバスは相手ページを常に右に描く（canvas.py:67-74） | models.py:271-272, render.py:555-569, models.py:213, ops.py:303, 495-499, app/canvas.py:67-74 |
| 13 | 話者名は print でも描かれる。x/y なしの台詞は `balloon` 既定 `"speech"` のため原点 (0,0) に描かれる | render.py:353-354, 378-379, 515-516 |
| 14 | フキダシの尾は底辺中央から幅 12 px 固定の三角形。ルビは最初の run だけで親文字に揃わない。`_columns` は明示改行を扱わない | render.py:352, 377, tategaki.py:95-113, 136-146 |
| 15 | `_draw_tone` の網点半径は `spacing*density*0.45`。被覆率が dpi に依存し density と一致しない（調査実測: 150 dpi で濃度に関係なく約 0.92 が黒） | render.py:144-171 |
| 16 | `edit_line` は `_find_line` が story を先に探すため `episode.story` だけを変える。ロード後の story と `page.texts` は別オブジェクトなので、保存される `page.texts` が古くなる | ops.py:116-124, 230-245, migrate.py |
| 17 | `migrate_payload` は `version` を読まない。未知キーは次の保存で消え、`version` は 2 に戻る | migrate.py:81-155, io.py:73 |
| 18 | name_ok なしで exportable な role に `put_raster` する既存テスト: test_p1.py:78、test_p4.py:73、test_p5_lt.py:20、test_p7_factory.py:46, 51, 68, 71, 101, 111、test_p9_factory.py:57（test_p7_factory.py:161 は直前に name_ok がある） | tests/ |
| 19 | OPS_SCHEMA に無い実装済み op が 8 つある: `select_frame`, `edit_stroke`, `simplify_stroke`, `set_ruler`, `add_prim3d`, `lt_convert`, `add_ticket`, `set_ticket`。docs/ops.schema.json は型ヒントの羅列で JSON Schema ではない | ops.py:28-72 |
| 20 | GUI はロックを取らずに保存する（手動保存と 60 秒 autosave）。フレーム選択は `page.selected_frame_id` の直接代入、フキダシのドラッグ中は StoryLine を直接書き換える。キャンバスはラスタを描かず、サブビューだけが `render_page` を使う | app/main.py:145-148, 371, 448-467, canvas.py:150-154, main.py:344-346 |
| 21 | HTTP は CORS `*`、本文の `path` に任意のパスを受け、put_raster の `path` は任意のローカルファイルを読む。`/v1/export` は形式指定を無視して PNG 連番。`JOBS` は同期 apply の結果メモ。**新規（第2版）:** 本文は Content-Type に関係なく JSON として読む（44-48）。全応答に `Access-Control-Allow-Origin: *`（146, 154）で、Host の検査もない。したがって利用者が開いた任意の Web ページが、preflight の要らない `text/plain` の POST で `/v1/apply` と `/v1/new`（`dest` は任意パス）を叩け、`put_raster` の `path` で任意のローカルファイルをプロジェクトに読み込ませられる。`GET /v1/inspect` は未発表の台詞をオリジンをまたいで読ませる。DNS rebinding も通る | server.py:14, 44-48, 104-122, 146, 154, ops.py:371-372 |
| 22 | テストの基準線: 98 passed / 1 failed（`tests/test_tategaki.py::test_small_kana_sits_right_in_em_box`）。**訂正（第2版）:** フォントが無いせいではない。試験は `set_meta{font_path: C:\Windows\Fonts\YuGothM.ttc}` を指定し、Linux では同梱の DelaGothicOne に落ちる（render.py:80-89）。そのフォントで小書き仮名の重心が右に寄らない（cx 284.2 対 286.4）。同梱フォントに固定しても通らない | tests/test_tategaki.py:17-19, 61-68, tategaki.py:84 |
| 23 | **新規（第2版）:** project.json はストロークを2〜3回書く。`layers[].strokes`（io.py:33）に加えて `name_strokes` / `ink_strokes`（io.py:119-120）も書く。後の2つはレイヤーから導く property（models.py:247-259）で、中身は重複である | io.py:33, 119-120, models.py:247-259 |
| 24 | **新規（第2版）:** `lt_convert` は2値画像の横方向の黒の連なり（run）を1本ずつストロークにする（lt.py:20-36、ops.py:945）。計測: 1518×2150 px のページ1枚で 36,402 本、project.json は 40.5 MB | lt.py:20-36, ops.py:926-945 |
| 25 | **新規（第2版）:** `load_episode` は全ページの全ラスタをディスクから読み（io.py:135-148）、`save_episode` は毎回すべてのラスタを書き直す（io.py:127-132） | io.py:127-148 |
| 26 | **新規（第2版）:** `lock_page` と `unlock_page` は `_check_page_lock` の対象外（ops.py:959）。`lock_page` は op の `agent` 引数をそのまま所有者にし（764）、`unlock_page` は誰のロックでも外す（767-770）。AI がロックを外したり奪ったりできる | ops.py:762-770, 959 |
| 27 | **新規（第2版）:** `export_pack` は解像度を `min(dpi, 150)` に切る（pack.py:12-13）。CLI の `export` は `--dpi` の既定が 150（__main__.py:35）で、pack には dpi を渡さない（110）。600 dpi の2値原稿は pack から出せない | pack.py:12-13, __main__.py:35, 110 |
| 28 | **新規（第2版）:** `export_psd` は `pages[0]` だけを平らにした1枚で書く。「テキストレイヤー」は話数全体の全台詞ぶんの空の全面レイヤーで（psd.py:49-73, 82-89）、レイヤー名は ASCII に置換され日本語が `?` になる（psd.py:31-35）。出力ファイル名は `episode.title` をそのまま使うので、Windows で使えない文字が通る（export.py:33, 56, 61） | psd.py:31-89, export.py:33-61 |
| 29 | **新規（第2版）:** 同梱フォントは `Path(__file__).parents[2]/assets/fonts` で探す（render.py:80）。wheel は `src/genko` だけを含む（pyproject の `packages`）ので、インストールしたビルド（Windows 配布など）には同梱フォントが無い。CJK の字形と写植の golden が環境で変わる | render.py:80-89, pyproject.toml |
| 30 | **新規（第2版）:** 既存テストの前提。test_p1.py:84 は `pages/001/bg.png` がディスクにあること、test_p1.py:118-124 は `docs/ops.schema.json` の `data["ops"][].op`、test_p5_psd.py はレイヤー数 = 台詞数 + 合成1、test_p5_pack.py は既定 dpi での pack、test_p5_spread.py:8・test_p7_factory.py:184・test_p8_shell.py:36 は既定の右綴じで `set_spread` 1–2、test_server.py は cwd の外の `tmp_path`、test_p4.py:110 は `handle_request` の3引数での直接呼び出しを前提にする | tests/ |


---

## 1. 参考UI（MANGA BRIDGE v2）の機能分解と Genko 対応表

列の意味: 「担当」は、Genko の設計でその機能を誰が行うか（エージェント / Genko / 人間）。「Genko が用意するもの」は道具（MCP ツール名は §8.1）と内部の仕組み。

| 機能 | 参考UI | 担当 | Genko が用意するもの |
|---|---|---|---|
| 工程バー | 1 企画・テキスト / 2 ネーム / 3 生成・編集 / 4 仕上げ / 5 書き出し | Genko が状態を集計、人間が見る | `status`（工程ごとの件数、承認待ち、次の作業の件数）。GUI の StepBar（M7） |
| 企画・台本 | 1 企画・テキスト | エージェントが書く | `set_bible`、`set_script`（スキーマ検査 + lint）。既存テキストの取り込みはエージェントが構造化して `set_script` に渡す |
| テキストからネーム | 台本からページ割り・コマ割り・台詞配置・ラフ | エージェントが計画、Genko が計算 | `submit_name`（段組 DSL → コマ、縦書き写植、プレビュー、指摘）。人物の位置（blocking）はデータから導出 |
| ネーム・画像読込 | 手描きアタリの取り込み | 人間が用意、Genko が取り込む | `studio import-name`（原本は不変資産、DRAFT に配置）（M8） |
| コマ・領域判定 | 判定開始、番号付きコマ枠、再解析 | Genko（XY-cut）が提案、エージェントが補助、人間が確定 | XY-cut 検出 → 段組の提案。手書き台詞の読み取りはエージェントの画像認識で行い、提案として出す（M8） |
| 読み順 | 「読み順: 右上 → 左上 → 下」、番号バッジ | Genko | `Page.leaf_frames()`（縦分割は右が先）。snapshot の `order`・`label`・`reading_summary`、プレビューの番号オーバーレイ |
| 領域ラベル・領域編集 | 人物 / 背景 / テキスト / 小物 / コマ枠 / その他、+領域、範囲修正、削除、戻す | Genko が導出、エージェントと人間が編集 | `Region`（`panel.regions`）と region op。人間が作った領域（`source:"user"`）はエージェントが上書きしない |
| スコープ | コマ全体 / 人物 / 背景 / 文字 | Genko | 生成依頼パックのマスク（コマ・領域・文字よけ） |
| コマサムネイル・比較 | 16-1、16-2 …、元画像 / 比較 / 候補 / 採用中 | Genko が描画 | `render`（ページ、コマの切り抜き、アタリの重ね比較、ガイド）。画像を返し、ファイルにも置く |
| 解析データ | 検出ラベルと信頼度 | エージェントが報告、Genko が記録 | エージェントが画像を見て報告した箱を `add_region{source:"agent"}` で受け、`studio/analysis/` に全件を残す |
| このコマのメモ・今回の指示 | 自由文 | 人間かエージェントが書く | `panel.memo`、`panel.instruction`（書いた actor 付き）。生成依頼パックに入る |
| タグ・プロンプト辞書 | 補完 | Genko が辞書を持つ | `studio/vocab.json`（shot / angle / 表情の日本語 ↔ 英語 ↔ タグ）。生成依頼パックの下書きに使う |
| 参照素材 | このコマのアタリ → 構図・ポーズ、登録素材から追加、シルエット | Genko が束ね、エージェントが画像ツールに渡す | `panel.refs[{source, role, weight, order}]`。生成依頼パックが役割ごとにファイルを並べる（§4.5） |
| 生成対象・空白の補完、共通制約 | 生成範囲と余白、全コマ共通の制約 | Genko | 文字よけの領域、`pad_mm`、共通制約 → ページ → メモ → 今回の指示の順で依頼パックに入れる |
| 生成前に指示を確認 | 確認ボタン | Genko が出し、エージェントと人間が見る | `generation_request`（生成しない。依頼パックを作って返すだけ） |
| プロンプト表示と編集 | 使用プロンプト、ネガティブ、コピー | エージェントが最終形を決める、人間が上書きできる | 依頼パックの下書き。人間の上書き `panel.gen.prompt_override`（pinned）。取り込み時に「実際に使ったプロンプト」を来歴に記録 |
| 生成設定 | モデル、モード、解像度、シード | エージェント（画像ツール側） | 依頼パックは推奨サイズと縦横比だけを出す。モデル名とパラメータは取り込み時に来歴として記録（自己申告） |
| バリエーション生成・編集（インペイント）・参照して生成 | 4 候補、インペイント、参照生成 | エージェントの画像ツール | 依頼パックの `mode`（new / edit / inpaint）とマスク・元画像。結果は `import_images` で候補になる（`parent` 付き） |
| 採用・修正指示・履歴 | 採用、修正指示、履歴を見る | エージェントが採用、人間が art ゲートで確認 | `adopt`（コマに配置）、`request_fix`（修正の依頼）、候補の系譜と来歴、journal |
| キャラクタースタジオ | キャラ一覧・登録 | エージェントが設定画を作る、人間が承認 | キャラ schema（look、`hair_value`、tokens、refs、locked）、設定画の依頼パック、候補、`approve sheet`（人間）、sheet ゲート |
| 背景・小物ライブラリ | ライブラリ | エージェントか人間が登録 | `studio.locations`、`studio.props`（参照画像と来歴）。依頼パックが参照として使う |
| SD 仕上げ・モノクロのページ | ナビ、ページビュー | Genko（決定的） | `screentone.py`（平網・AM 網点・ベタ）、`lineart.py`、顔よけの再写植 |
| 書き出し | 書き出しボタン | 人間が承認、Genko が出力 | preflight（承認、来歴、実効 dpi）、600 dpi の2値 TIFF・pack |
| エンジン状態・外部送信の表示 | ComfyUI 接続中、VRAM、外部送信なし | – | Genko は外部と通信しないので、この表示は要らない。エージェントの状態は Hermes 側で見る |
| 最近のプロジェクト・設定 | 一覧、設定 | Genko | `projects`（`--root` 配下の一覧）、GUI の起動画面（M7） |

参考UIから**採らない**もの。

- ナビゲーションが3系統（工程バー・左ナビ・クイックバー）重なっている構成。
- モデル名をナビやデータのキーに入れること。モデル名は来歴の値として記録するだけ。
- 1024×1365 の縦長画像を横長のコマに使う（画面1の不整合）。生成サイズはコマの比から決める。
- カラー生成をそのままモノクロページに置く（画面1の不整合）。mono 原稿はグレースケールで生成させ、Genko が決定的に仕上げる。
- 検出結果を正とすること。画面2自身が「目視の仮設定」と書いている。検出は常に提案で、人間が確定する。
- 「16-1」のような位置ラベルをデータのキーにすること。キーは frame id、ラベルは表示時に導出する。

---

## 2. 設計原則

### 2.1 保つもの

1. **単一コマンドバス。** 原稿の変更はすべて `genko.ops.apply_ops` を通る。GUI、CLI、HTTP、MCP の区別はない。dry_run、全か無か、`ops[i] <op>: msg` 形式のエラー、page lock、undo はすべての書き手に効く。既存 GUI の抜け道（main.py:371 のフレーム選択、canvas.py:150-154 のドラッグ）も op に直す。
2. **ヘッドレス優先。** 参考UIのボタンはすべて JSON スキーマ付きの道具にする。GUI は同じ道具を呼ぶ薄いクライアントである。
3. **画素境界。** 「画像生成は `put_raster` の外側。Genko は画素を発明しない」を保つ。
   - 画素はエージェントが取り込んだ画像か、人間の筆と取り込み画像だけ。
   - 取り込んだバイト列は検証してから `assets/` に内容アドレスで置く。
   - op は hash を参照し、配置（`adopt_candidate` / `place_asset`）を記録するだけ。
   - Genko が行う画素処理は、既存画素の決定的変換（クロップ、リサンプル、クリップ、フィルター、スクリーントーン、線抽出）だけ。
4. **ゲート。** `name → (name_ok) → ink → finish → export` を保つ。本番に出る role への採用は name_ok の後だけ。NAME と DRAFT は出荷しない。
5. **単位は mm、ページは 1 始まり、読み順は `Page.leaf_frames()`。** コマは軸平行のギロチン木。

### 2.2 加えるもの

6. **Genko は文章も生成しない。外部とも通信しない。** Genko の中に LLM や画像モデルの呼び出し、ネットワークへの送信は置かない。創作上の判断はすべてエージェントからデータとして届き、Genko は検査・計算・保存・描画だけを行う。これにより、どの LLM・どの画像ツールを使うかは利用者とエージェント側で自由に替えられ、未発表原稿がどこへ送られるかもエージェント側の設定だけで決まる。
7. **道具はエージェントのために作る。**
   - 入力は平らな JSON（再帰なし、`oneOf` なし、数値や文字列長の制約なし。範囲は Genko が検査する）。どの LLM でもツール引数として書ける。
   - エラーと警告は `{code, severity, path, message, hint}` で返す。`path` は入力の JSON ポインタで、エージェントはその場所だけを直して出し直せる。
   - 書き込む道具は既定で dry_run（プレビューと指摘だけ返す）。`commit:true` で初めて保存する。ただし記録系の道具（`record_review`、`report_regions`、`generation_request`、`import_images`、`review_candidates`、`adopt`、`request_fix`、`request_approval`）は即時にコミットする。どれも冪等で、取り消せる（`unadopt`、別の候補）。
   - 応答は小さく保つ（数 KB）。大きなもの（ページ全体の状態、画像）は必要なときに別の道具で取る。
   - 何かを作る道具は呼び出し側の id を受け、同じ呼び出しを繰り返しても重複しない（冪等）。
   - 「次にやること」（worklist）を状態から計算して返す。エージェントの記憶に頼らない。
8. **画像のバイト列は LLM の文脈を通さない。** 画像はファイルパス（同じマシン、許可したフォルダ内）かアップロード（別マシン、HTTP）で受け渡す。道具の応答に入れる画像は、LLM が見るための縮小プレビューだけにする。
9. **生成画素の来歴。** ページ上の生成画素はすべて候補 → 生成依頼 → 来歴（エージェント、ツール、モデル、使ったプロンプト、参照したファイル）に辿れる。ツールとモデルはエージェントの自己申告であり、Genko は検証できないことを明記して記録する。
10. **非破壊の候補。** 候補はページに入らない。採用は1つの op で、取り消せる（`unadopt`、別候補の採用、journal の undo）。却下した候補も履歴として残す。取り込んだアタリは不変資産で、NAME/DRAFT にだけ置く。
11. **承認は人間だけ。** ゲート op は actor が `human:*` のときだけ効く（§5.1）。MCP サーバーは起動時に `ai:*` の actor で固定され、承認の道具を持たない。エージェントは `request_approval` で承認を依頼し、人間が CLI・review.html・GUI で承認する。CLI の actor は自己申告なので、これはセキュリティではなく**ワークフローの整合性の守り**である。HTTP ではトークンに actor を結び付ける。
12. **人間の編集はエージェントの再実行より強い。** `pinned` フィールド、`source:"user"` の領域、`locked` のキャラ、人間のプロンプト上書きは、エージェントが上書きできない。変えたい場合はチケットで人間に頼む。
13. **エージェントの入力は信用しない。** 大きさの上限、パスの閉じ込め（`--root` と取り込みフォルダの外は読まない・書かない）、画像の検証（Pillow で開けること、画素数の上限）、コードの実行なし。脚本や指示の文字列はデータとして扱い、Genko の動作を変える指示としては解釈しない。
14. **構造から導出し、検出は取り込み時だけ。** Genko のネームは、コマ、読み順、台詞の箱、人物の位置（blocking）、prim、ruler をデータとして持つ。マスク、文字よけ、構図ガイド、生成サイズはここから計算する。
15. **文字は画像に描かせない。** 台詞・ナレーション・効果音は Genko のベクター描画（縦書き、ルビ、フキダシ）で行う。生成依頼パックは「文字・フキダシを描かない」を必ず含む。
16. **判断は project.json、証拠はサイドカー、バイトは assets。** project.json は小さく保つ（hash とメタデータだけ）。生成依頼パックと解析結果は `studio/` に、画像とストロークは `assets/` に置く。予算: 16 ページのネーム作業（lt_convert したページを含む）で project.json は 1 MB 未満、journal の 1 行は 64 KB 未満。試験で守る。
17. **性能に予算を置く。** 1 op の apply は undo の深さに依らない（16 ページ・深さ 50 で 100 ms 未満）。道具の応答はプレビュー描画を含めて 2 秒以内（B4、プレビュー 1024 px）。GUI の筆は画面に 16 ms で反映し、ディスクへの commit は裏で 200 ms 以内。数値はベンチマーク試験で守る（§10.3）。
18. **オフラインで試験可能。** core は Pillow のみ。試験は筋書きどおりに道具を呼ぶ偽エージェントと、用意した画像で行う。実際の Hermes Agent を使う試験は手動のチェックリストにする。

---

## 3. パイプライン全体

### 3.1 流れ

```text
 premise / 既存テキスト / 手描きアタリ
    │
    ├─(任意) S0 取り込み ──────────────────────────────┐
    ▼                                                  │
 S1 企画 ──[bible]──▶ S2 キャラ設定画 ──[sheet ①]──▶ S3 脚本 ──[script]──▶
    │                                                  ▼
 S4 ネーム（ページ割り → 段組 → コマ指示 → 写植 → プレビューで自己点検）──[name ②]──▶
 S5 生成・編集（パイロットページ先行: 依頼パック → エージェントが生成 → 取り込み → 比較 → 採用/修正）──[art ③]──▶
 S6 仕上げ（線・トーン・ベタ → 顔よけ再写植 → 効果）── advance finish ──▶
 S7 書き出し（preflight）──[export ④]──▶ 原稿ファイル
```

ゲートは `[...]` で示した。①〜④は人間の承認点（§13 で「要所で人が承認」に決定）。`bible` と `script` はプリセット次第（§3.4）。

### 3.2 ステージ一覧

| ステージ | エージェント（LLM・画像ツール） | Genko（決定的） | 人間 | ゲート |
|---|---|---|---|---|
| S0 取り込み（任意） | 手書き台詞の読み取り、領域の提案 | XY-cut コマ検出、位置合わせ、提案の重ね表示 | 提案を確定 | – |
| S1 企画 | premise から bible（キャラ、場所、制約）を書く | スキーマ検査、見分け lint | 任意の確認 | `bible`（既定 auto） |
| S2 キャラ設定画 | 依頼パックで設定画を生成し、取り込む | 設定画の依頼パック、顔の切り出し、固定 | 1キャラ1枚を承認 | **sheet ①** |
| S3 脚本 | scene → beat を書く、指摘を直す | 脚本 lint（字数、話者、ページ予算、めくり） | 任意で承認 | `script`（既定 human） |
| S4 ネーム | ページごとにネーム計画を書く、プレビューを見て直す | 段組 → コマ、写植、人物の位置、ネーム lint、プレビュー | 直す、承認する | **name ②** |
| S5 生成・編集 | 依頼パックで生成、取り込み、比較画像で評価、採用、修正 | 依頼パック、画像の検証と取り込み、配置とクリップ、比較画像 | 修正指示、ページごとに承認 | **art ③（ページ単位）** |
| S6 仕上げ | 任意: 高解像度化を画像ツールで行い取り込む | スクリーントーン、線抽出、顔よけの再写植、効果 | 校正確認 | `advance to=finish` |
| S7 書き出し | 任意: 校正用 PDF を出す | preflight、既存の exporter | 承認と本番書き出し | **export ④** |

### 3.3 各ステージ

#### S0 取り込み（任意、M8）

- 人間が描いたアタリ（画面2の赤線）を `studio import-name PROJ ./scans/*.png` で取り込む。原本は `assets/` の不変資産になり、DRAFT に置かれる（出力されない）。
- Genko の `XYCutDetector` が二値化と余白の射影でコマ枠を切り、段組の提案（信頼度付き）を出す。
- 手書きの縦台詞はエージェントが画像を見て読み、`add_line` の提案として出す（dry_run の重ね表示）。
- 人間が確定するまで原稿は変わらない。

#### S1 企画

- **入力:** premise（数行〜1ページ）、ページ数、判型（B4 mono など）、禁止事項。
- **エージェント:** bible を書いて `set_bible` に渡す（スキーマ `bible@1`、§4.3）。
  - logline、plot、テーマ
  - characters（見た目: 髪型、**モノクロでの髪の値 `hair_value`: beta | tone | white**、目、体格と身長、場面ごとの服、目印、シルエットの要点、一人称と口調、年齢）
  - locations（時間帯の変種つき）、style の要点、constraints（共通制約。例「流血表現なし」）
- **Genko の検査:**
  - id の一意性、全キャラに `hair_value` がある。
  - 主要キャラ同士で `hair_value` とシルエットの要点が両方一致しない（モノクロでの見分け）。
- **コミット:** `upsert_character`、`upsert_location`、`set_bible{plot, constraints}`、`set_studio{style}`（M0 では既存の `set_bible` にまとめて入れ、詳細はサイドカーに置く）。

#### S2 キャラ設定画（一貫性の錨。コマより先に行う）

- 各キャラについて:
  1. エージェントが `generation_request{purpose:"character_sheet", character_id}` で依頼パックを受け取る（正面・横・背面 + 表情の列、白背景、モノクロ、文字なし）。
  2. エージェントが画像ツールで数枚作り、`import_images` で候補にする。
  3. 人間が1枚を承認する（`studio approve sheet --character hina --candidate cd_02`）か、自分の絵を取り込む（来歴 `self`）。
  4. 承認した画像が `refs[{kind:"sheet"}]`、顔の切り出しが `refs[{kind:"face"}]` になり、`locked:true` になる。以後エージェントは見た目を黙って書き換えられない。
- 繰り返し出る場所（例: 屋上）は背景の参照画像を1枚作って `locations[].refs` に置く。
- **ゲート:** **sheet ①**（人間）。

#### S3 脚本

- エージェントが scene → beat を書いて `set_script` に渡す。beat の kind は `action | dialogue | monologue | narration | sfx`。話者はキャラ id、beat は感情、ページ割り当て（`page`）、めくりの印（`reveal`）を持つ。
- **Genko の lint（エラーは直す場所付きで返す）:**
  - 台詞1つ ≤ 40 字（既定）、1 beat の台詞群は 3 フキダシ以内。
  - 1ページの beat 数が帯域内。話者は既知のキャラ。ページ予算の合計が合う。
  - `reveal` はめくり直後のページ（偶数ページ）の先頭に来る（§9.3）。
- **ゲート:** `script`（studio 既定 human）。

#### S4 ネーム（最重要）

1. **ページごとのネーム計画（エージェント）。** 入力は `inspect{script, page}` で取るそのページの beat、前後ページの要約、ページの側（偶数・奇数）。出力は段組 DSL（§5.5）とコマごとの指示（shot、angle、人物の pose / expression / facing / pos / scale、action、emotion、location、time、fx、beat_ids、台詞の balloon 種別と改行位置）。
2. **検査とコンパイル（Genko、`submit_name`）。**
   - スキーマ検査 → ネーム lint（下記）→ 段組を既存の op（`split_frame` など）にコンパイル → 写植器（`letter.py`）で台詞の箱を計算 → `apply_ops(dry_run=True)` → 描画。
   - 応答: 指摘の一覧（JSON ポインタ付き）、読み順の要約、**name モードのプレビュー画像**（コマ番号と指示の重ね書き付き）、`commit:true` なら保存した revision。
   - エラーがあれば保存しない。エージェントは指摘の場所を直して出し直す。
3. **ネーム lint:**
   - 台詞箱の面積 / コマ面積 ≤ 0.35、コマの短辺 ≥ 25 mm
   - 同じ shot が3コマ以上続かない、scene 冒頭に状況説明のコマ（ELS / LS / FS）がある
   - **180度ルール**（同じ scene の2人の左右は `cross:true` でない限り保つ）
   - フキダシがコマ境界をまたがない、台詞がコマに収まる
   - `reveal` は偶数ページ、引き（`hook`）は奇数ページ
   - そのページの dialogue / monologue / narration の beat はちょうど1回ずつ置く
4. **自己点検（エージェント）。** プレビュー画像を見て、読み順の迷い、窮屈なコマ、弱いめくりを直す。点検の結果は `record_review{target:{page_id}, kind:"name", score, notes}` で残す（worklist が同じ点検を出し直さないため）。
5. **人間の確認。** `studio review` の HTML かプレビュー画像で見て、直すなら指示を書くか自分で op を使い、よければ `studio approve name --page N` を付ける。エージェントは `request_approval{gate:"name", pages}` で依頼し、承認を待つ間は他のページに進める。
- **ラフ（アタリ）の既定は構造的なもの**（人物の箱 + 向き + prim + ruler）。エージェントが画像ツールでラフ画像を作った場合は、`generation_request{purpose:"draft"}` の依頼に `import_images` で取り込み、`adopt{to:"draft"}` で DRAFT に置ける（出力されない。name_ok 前でも可）。
- **ゲート:** **name ②**（人間）。

#### S5 生成・編集（コマ単位、パイロットページ先行）

name の承認が付いたページだけが対象。順序は「パイロットページ（既定: 1ページ目）→ 残りを読み順」。worklist がこの順で `gen_panel` を出す。

1. **依頼パック（Genko、`generation_request`）。** 生成はしない。コマごとに次をまとめ、`studio/requests/<id>/` に置いて返す（§4.5）。
   - 推奨の生成サイズと縦横比（コマの mm 比 + 余白。画像ツールの対応サイズに丸めた候補も出す）
   - プロンプトの下書き（日本語・英語の自然文と、タグ形式）。組み立ては決定的: style → キャラの固定の記述（`tokens`、そのまま）→ shot と angle の語彙 → 場所 → 人物の動きと表情 → 共通制約 → ページ → コマのメモ → 今回の指示
   - 描かせないもの（文字、フキダシ、効果音、色、キャラの `never[]`）
   - 参照ファイル: キャラの顔と設定画、場所の画像、ネームの切り抜き（構図）、人物の位置と向きを描いた図（ポーズ）、文字よけの図、修正ならマスクと元画像
   - 文字よけの位置（「左上の 30% は台詞用に空けておく」のような文も下書きに入る）
2. **生成（エージェント）。** 依頼パックを読み、自分の画像ツール（今は ChatGPT の画像生成）で数枚作る。参照画像を渡せるツールなら渡す。最終的なプロンプトはエージェントが決めてよい。
3. **取り込み（Genko、`import_images`）。** 画像ファイルを検証して `assets/` に置き、依頼に対する候補にする。来歴（ツール、モデル、実際に使ったプロンプト、渡した参照、パラメータ）を必ず付ける。依頼後にコマの指示が変わっていれば、その候補は `stale:true`。
4. **比較と評価（エージェント）。** `render{kind:"compare"}` で候補にアタリを重ねた画像を見て、構図、キャラの一致、手足の破綻、文字よけ、画内の文字を評価し、`review_candidates` で点数とメモを残す。
5. **採用または修正。**
   - エージェントは最も良い候補を `adopt` できる（art ゲートは人間なので、採用はページの確定ではない）。
   - 修正は、依頼パックの `mode:"edit"`（元画像 + 指示）か `mode:"inpaint"`（元画像 + 領域マスク）で作り直し、`parent` 付きの候補として取り込む。
   - 予算: 1コマ最大 8 枚、修正 2 巡（policy）。超えたらチケットで人間に回す。
6. **複数人物のコマ。** まず構図全体を生成し、次に人物ごとにその人物の領域マスクと参照で修正（inpaint）する。
7. **パイロットページとスタイル固定。** パイロットページが art 承認されると `studio.style.locked_from_page` が立ち、依頼パックの style の文言、仕上げパラメータ、画像ツールの指定（来歴で最も多く使われたもの）が固定される。以後のページはこれを継ぐ。
8. **ゲート: art ③（ページ単位の `art_ok`、人間）。** 条件は「指示のある全コマが採用済みか skip」。

#### S6 仕上げ（ほぼ決定的）

1. **解像度。** 採用画像の実効 dpi（§9.7）が足りないコマは、エージェントに高解像度化を依頼する（依頼パック `mode:"upscale"`。画像ツールに高解像度化の機能があれば使い、結果を `parent` 付きの候補として取り込む）。ツールが無ければ Genko の LANCZOS 再標本化 + 線抽出で済ませ、preflight で実効 dpi を示す。
2. **モノクロ仕上げ（render 時、非破壊）。** placed layer の `finish`（null なら `studio.style.finish`）で、線マスク、白・黒点、3〜4 段の平網、lpi と角度を指定する。print では 1bit で出す（§9.5）。
3. **写植の再調整。** 採用画像の顔と人物の箱（エージェントが見て `add_region` で報告、または人間が引いた領域）を避けて写植器を再実行し、`move_line` の提案として出す。
4. **効果。** `panel.fx` を `add_effect`（集中線・流線）にする。効果もコマでクリップする（M6 で直す）。
5. **`advance{to:"finish"}`。** strict_gates では `art_ok` が要る。

#### S7 書き出し

- **preflight（すべて理由付きで拒否する）:**
  - 全ページが `finish`、指示のあるコマに未採用が無い、位置のない台詞が無い。
  - 実効 dpi が閾値以上（gray 350、2値の線 600。未満は警告、`--force` で通す）。
  - 承認: sheet・name・art がすべて人間の承認済み。
  - 採用画像の来歴（どのツール・モデル・プロンプトで作ったか）が記録されている（無くても止めず、警告だけ）。
  - 取り込んだ画像に試験用の画像（`origin.kind:"fixture"`）が無い。
- **出力:** 既存 exporter で出す。NAME/DRAFT は role で除外済み（render.py:445-459）。候補はレイヤーではないので出ようがない。pack と print は `spec.dpi`（B4 商業原稿は 600）で出す（M6 で pack の 150 dpi 上限を外す）。
- **校正用の書き出し（`export_proof`）** はエージェントも使える。全ページに「校正」の透かしが入る。本番の書き出しは人間だけ（**export ④**）。

### 3.4 ゲートとプリセット

利用者の決定（§13）により、既定のプリセットは `studio`（要所で人間が承認）である。

| ゲート | 対象 | `studio`（既定） | `quick` |
|---|---|---|---|
| `bible` | 話数 | auto | auto |
| `script` | 話数 | human | auto |
| `sheet` ① | キャラ | human | human |
| `name` ② | ページ | human | human |
| `art` ③ | ページ | human | human |
| `export` ④ | 話数 | human | human |

- 承認は `{gate, page_id | character_id, by, rev, at}` で記録する（§4.3 `studio.approvals`）。
- `auto` は承認不要（worklist が待たない）。第2版の「AI の仮承認（omakase）」は置かない。承認が要るゲートでは、エージェントは待つ。
- エージェントは `request_approval{gate, pages?, character_id?, note}` で依頼を出す。依頼は kind=gate のチケットになり、`studio status` と review.html と GUI の承認箱に並ぶ。人間が承認するとチケットは閉じ、エージェントの `next` から待ちが消える。
- **承認の通知。** Hermes Agent はメッセージ連携（チャットなど）を持つので、エージェントが人間に「ネームの承認をお願いします」と review.html の場所を知らせることはできる。ただし承認そのものは人間の操作（CLI、GUI、または M7 以降の承認リンク）でしか付かない。エージェントが「人間が OK と言った」と代わりに承認することはできない。
- `name_ok` の既存の意味（フラグを立てて stage を ink に進める）は保つ。
- 別の軸として autonomy がある。`gated`（既定）ではエージェントはゲートの内側で自由にコミットする。`assist` ではエージェントの書き込みがすべて提案（proposal）になり、人間が受理するまでコミットされない。


---

## 4. データモデル

### 4.1 原則

- **project.json に画像バイトを入れない。** ラスタ、候補、シート、参照、取り込んだアタリは `assets/` に内容アドレスで置く。Episode は hash だけを持つ。
- **ラスタの保存を内容アドレスにする。** 作業ラスタ（`raster_png` を持つ既存レイヤー）はメモリ上ではそのまま。`save_episode` が保存時に sha256 を計算して `assets/` に書き、relpath をその hash パスにする。ops と render は変えない。ページ番号や role から作るパスがなくなるので、§0.5 の 5〜7 の衝突と上書きが構造的に消える。
- **ストロークの保存も内容アドレスにする（v3）。** メモリ上の `Layer.strokes` はそのまま。`save_episode` はレイヤーごとにストロークを正準 JSON にして `assets/` に置き（H1 から座標と筆圧は `xy`・`p` に float64 リトルエンディアンの base64 で詰める。数の並びの古い形も読める。新しい線の座標は 0.001 mm に丸める）、project.json には `strokes_blob`（hash）と本数だけを書く。`name_strokes` / `ink_strokes` の重複（§0.5 の 23）は v3 では書かない（読み込みは v2 のために残す）。lt_convert の 3 万本も project.json を太らせない。
- **差分保存。** 保存は、hash が既にストアにある資産を書かない。project.json は一時ファイル + `os.replace`（Windows では `PermissionError` を最大 2 秒まで間隔を広げて再試行）。読み込みはラスタを遅延読み込みにする（render と op が触れたときに読む）。
- **AI の画像は `placed` レイヤー**（新しい `LayerKind`）。バイト列をメモリに持たず、`asset` と `placement_mm` を持つ。render は Episode に注入した AssetStore から開く（LRU）。
- **コマのデータは Frame に置く**（`Frame.panel`）。`duplicate_page` は `_refresh_frame_ids` で id を変えてもオブジェクトは保つので（ops.py:951-954）、指示がコマと一緒に動く。
- **新しい Episode フィールドは `studio` と `revision`（ほかに `start_side`・`extra` と一時の `assets`、§4.6）。** `_copy_state` を汎用化してから足す（§5.1）。


### 4.2 プロジェクトフォルダ

```text
title.genko/
  project.json                  # v3。判断だけ（小さい）。apply_ops 経由でしか変わらない
  project.lock                  # OS のファイルロックで保持（§5.1-10）。中身は {token, agent, pid, host, acquired_at}
  project.v2.bak.json           # v2 → v3 初回保存時のバックアップ
  assets/
    9c/9c1e….png                # 不変。作業ラスタ、候補、設定画、参照、アタリ原本、ガイド画像
    4b/4b07….json               # 不変。生成依頼パックの写し、解析結果、チェックポイント
    3c/3c1f….strokes.json       # 不変。レイヤー1枚分のストローク（v3）
  studio/
    journal.jsonl               # 1 commit 1 行: {rev, base_rev, actor, at, ops(正規化済み), origin}
    checkpoints.jsonl           # {rev, project: "sha256:…"}。50 commit ごとと版移行の前
    requests/rq_3f9a51c0/       # 生成依頼パック（§4.5）。request.json と参照ファイル。エージェントが読む
    inbox/rq_3f9a51c0/          # 取り込み待ちの画像（同じマシンのエージェントがここに置く）
    proposals/prop_….json       # assist モードの未受理バッチ
    claims/<item>.json          # 作業項目の短い予約（§7.4）
    history/                    # 退避した却下候補
    audit.jsonl                 # ロックの引き取り、policy と tools の変更
    analysis/<asset>.json       # エージェントや検出器が報告した領域の全件
    reviews/                    # review.html とプレビュー画像
    drafts/                     # M0（v2 のまま）の bible・脚本・ネーム計画・PanelSpec。M3 で project.json に取り込む
  pages/001/*.png               # v2 の旧ラスタ。読むだけ。`genko gc --legacy` で消す
```

- **journal は op の列だけを持つ。** 全文スナップショットは持たない。1 行は正規化済みの ops（生成された id、写植の座標、`put_raster` の path を取り込んだ資産 hash に置き換えたもの）で、通常は数 KB。64 KB を超える op は本体を blob にして hash で参照する。
- **チェックポイント。** 50 commit ごとと版移行の前に、その時点の project.json を `assets/` に置き、`checkpoints.jsonl` に rev と hash を書く。undo と過去の rev の復元は「直前のチェックポイント + journal の再生」で作る。保持は「100 commit または 20 MB の小さい方」。
- **`requests/` と `inbox/` はエージェントとのやり取りの場所。** 同じマシンのエージェントは `requests/<id>/` のファイルを読み、生成した画像を `inbox/<id>/` に書いてから `import_images` を呼ぶ。別マシンのエージェントは同じものを HTTP で読み書きする（§8.1）。取り込みが終わった `inbox/` のファイルは消す（資産は `assets/` に残る）。
- 秘密情報（API キー、トークン）はどこにも保存しない。Genko 自身は外部サービスの鍵を持たない。HTTP のトークンはユーザー設定ディレクトリの `tokens.json` にだけ置く（§8.4）。
- **パスの長さ（Windows）。** 資産の相対パスは `assets/ab/<64 桁>.strokes.json` で最大約 90 文字。プロジェクトの絶対パスが 150 文字を超えると `genko doctor` が警告し、書き込みは `\\?\` 形式の長いパスで行う（Windows CI で試験する）。

### 4.3 project.json v3（抜粋）

```json
{
  "version": 3,
  "revision": 128,
  "generator": "genko-studio 0.3.0",
  "title": "夏の午後の約束",
  "episode": 1,
  "binding": "right",
  "start_side": null,
  "spec": {"width_mm": 257, "height_mm": 364, "dpi": 600, "bleed_mm": 3, "inner_margin_mm": 10, "expression": "mono", "preset": "commercial-b4"},
  "page_locks": {"pg_7c1e9a0b2d41": "human:leaf"},
  "tickets": [
    {"id": "tk_31a0", "page_id": "pg_7c1e9a0b2d41", "page_index": 4, "kind": "gate", "gate": "name",
     "assignee": "human", "status": "open", "created_by": "ai:hermes",
     "text": "4ページ目のネームを見てください。めくりの位置を変えました", "thread": []}
  ],
  "bible": {
    "plot": "夏の屋上で、幼馴染が5年前の約束を果たしに来る。",
    "constraints": ["台詞・効果音を画像に描かない", "流血表現なし", "背景を描き込みすぎない"],
    "characters": [
      {
        "id": "hina",
        "name": "日向ひな", "reading": "ひなた ひな", "role": "heroine", "age": 16,
        "look": {
          "hair": "肩までのストレート、横に流した前髪",
          "hair_value": "beta",
          "eyes": "大きく、やや垂れ目",
          "build": "細身", "height_cm": 156,
          "marks": ["左側に星形の髪留め"],
          "silhouette": "髪留め + 前髪"
        },
        "outfits": {"default": "半袖のセーラー服", "sc_04": "浴衣"},
        "speech": {"first_person": "わたし", "style": "丁寧語まじり、照れると語尾が小さくなる"},
        "tokens": {
          "en": "a slim 16-year-old girl with shoulder-length straight black hair, side-swept bangs, a small star-shaped hair clip on the left",
          "tags": "1girl, black hair, shoulder-length hair, side-swept bangs, star hairclip, serafuku"
        },
        "never": ["眼鏡"],
        "refs": [
          {"asset": "sha256:9c1e…", "kind": "sheet", "approved_by": "human:leaf"},
          {"asset": "sha256:77a0…", "kind": "face", "approved_by": "human:leaf"}
        ],
        "locked": true,
        "pinned": ["look.hair_value"]
      }
    ]
  },
  "studio": {
    "premise": "夏の屋上で、幼馴染が5年前の約束を果たしに来る",
    "policy": {
      "preset": "studio",
      "autonomy": "gated",
      "strict_gates": true,
      "gates": {"bible": "auto", "script": "human", "sheet": "human", "name": "human", "art": "human", "export": "human"},
      "limits": {"max_images_per_panel": 8, "max_fix_rounds": 2}
    },
    "style": {
      "expression": "mono",
      "text_ja": "モノクロの日本の漫画原稿。グレースケールで、線ははっきり、トーンは貼らない（Genko が仕上げる）",
      "text_en": "black-and-white Japanese manga panel, greyscale, clean confident linework, no screentone",
      "avoid": ["文字", "フキダシ", "効果音の描き文字", "署名", "色"],
      "lettering": {"font_mm": 4.2, "max_col_mm": 42},
      "finish": {"mode": "line_tone", "lpi": 60, "angle": 45, "black": 0.16, "white": 0.88, "levels": [0.1, 0.2, 0.3]},
      "locked_from_page": "pg_3f9a0c1d2e4b"
    },
    "locations": [
      {"id": "loc_rooftop", "name": "校舎の屋上", "description": "フェンスと給水塔のある学校の屋上",
       "times": ["sunset", "night"], "refs": [{"asset": "sha256:5d1e…", "kind": "establishing"}]}
    ],
    "props": [
      {"id": "prop_hairclip", "name": "ひなの髪留め", "description": "小さな星形の髪留め",
       "refs": [{"asset": "sha256:8e2a…", "kind": "reference"}]}
    ],
    "assets": {
      "sha256:77a0…": {"kind": "face", "origin": "generated"},
      "sha256:9c1e…": {"kind": "sheet", "origin": "generated"},
      "sha256:77b0…": {"kind": "style", "origin": "self"}
    },
    "script": {
      "logline": "…",
      "page_budget": 16,
      "scenes": [
        {"id": "sc_03", "title": "屋上の約束", "location_id": "loc_rooftop", "time": "sunset",
         "cast": ["hina", "sora"], "summary": "…",
         "beats": [
           {"id": "b_0012", "kind": "action", "text": "ソラが屋上の扉を開ける", "chars": ["sora"], "page": 3},
           {"id": "b_0013", "kind": "dialogue", "speaker": "hina", "text": "…やっぱり来てくれたんだ",
            "emotion": "安堵", "page": 4, "reveal": true}
         ]}
      ]
    },
    "approvals": [
      {"gate": "sheet", "character_id": "hina", "by": "human:leaf", "rev": 41, "at": "2026-09-24T10:02:00Z"},
      {"gate": "name", "page_id": "pg_7c1e9a0b2d41", "by": "human:leaf", "rev": 90, "at": "2026-09-24T10:40:02Z"}
    ],
    "orphans": []
  },
  "pages": [
    {
      "id": "pg_7c1e9a0b2d41",
      "index": 4,
      "stage": "ink",
      "name_ok": true,
      "art_ok": false,
      "plan": {"beat_ids": ["b_0013", "b_0014"], "turn_role": "reveal",
               "reviews": {"name": {"by": "ai:hermes", "score": 0.8, "rev": 88, "input_hash": "sha256:…"}}},
      "note": "",
      "frames": [
        {"id": "f4_root", "rect": {"x": 13, "y": 13, "width": 231, "height": 338}, "split_axis": "horizontal",
         "children": [
           {"id": "f4_p1", "rect": {"x": 13, "y": 13, "width": 231, "height": 110}, "children": [],
            "clip": true, "bleed": true, "border_mm": 0.8,
            "panel": {"status": "adopted", "…": "§4.4"}}
         ]}
      ],
      "layers": [
        {"id": "L_bg", "role": "bg", "kind": "fill", "exportable": true},
        {"id": "L_name", "role": "name", "kind": "strokes", "exportable": false, "strokes_blob": "sha256:3c1f…", "stroke_count": 30},
        {"id": "L_art_f4_p1", "role": "user", "kind": "placed", "title": "art 4-1", "exportable": true,
         "asset": "sha256:ab12…", "frame_id": "f4_p1",
         "placement_mm": {"x": 10, "y": 10, "width": 237, "height": 116},
         "fit": "cover", "clip_to": "bleed",
         "source": {"candidate": "cd_05", "request": "rq_3f9a51c0"},
         "finish": null},
        {"id": "L_ink", "role": "ink", "kind": "raster", "exportable": true, "asset": "sha256:0f3a…"},
        {"id": "L_fin", "role": "finish", "kind": "strokes", "exportable": true}
      ]
    }
  ],
  "story": [
    {"id": "ln_b0013", "page_index": 4, "page_id": "pg_7c1e9a0b2d41", "text": "…やっぱり\n来てくれたんだ",
     "speaker": "日向ひな", "speaker_id": "hina", "beat_id": "b_0013", "frame_id": "f4_p1",
     "x_mm": 30, "y_mm": 20, "w_mm": 14, "h_mm": 52, "balloon": "speech", "tail": [60, 70],
     "wrap": "vertical", "ruby_runs": [], "path": null}
  ],
  "extra": {}
}
```

注:

- `pages[].texts` は v3 では書かない。ロード時に `story` から組み立て、**同じオブジェクト**を `page.texts` に入れる（§0.5 の 16 を解消）。
- placed layer は `L_ink` より下に入れる（BG → NAME → 絵 → INK → FINISH）。人間の加筆が絵の上に乗る。
- `clip_to` は新しいキーである。既存の `Layer.clip`（下のレイヤーでクリップ）と名前がぶつからないようにした。
- `tokens` はキャラの見た目を画像ツール向けに固定した記述である。エージェントが一度書き、人間が直せ、`locked` で凍結する。依頼パックはこれを言い換えずにそのまま入れる。
- 参照素材は `studio.assets` の索引（種類と出どころのメモ）で持つ。索引は整理のためのもので、使用を制限しない。
- ストロークは `strokes_blob` の hash で持ち、project.json には座標を書かない。

### 4.4 PanelSpec（`Frame.panel`、葉フレームだけ）

```json
{
  "status": "adopted",
  "beat_ids": ["b_0013"],
  "shot": "MS",
  "angle": "low",
  "location_id": "loc_rooftop",
  "time": "sunset",
  "characters": [
    {"id": "hina", "outfit": "default", "pose": "振り返る", "expression": "やわらかい笑顔",
     "facing": "left", "pos": "right", "scale": 0.8, "box_mm": [150, 25, 70, 95], "head_mm": [175, 30, 22, 26]}
  ],
  "props": ["prop_hairclip"],
  "action": "屋上で振り返るヒロイン",
  "emotion": "安堵と期待",
  "fx": [],
  "emphasis": 0.6,
  "cross": false,
  "memo": "顔と台詞を優先。背景を埋めすぎない。",
  "instruction": {"text": "顔の大きさと向きをアタリに合わせる。上半身を描き、左右の台詞用余白を保つ。", "by": "human:leaf"},
  "pinned": ["shot", "characters"],
  "refs": [
    {"id": "rf_1", "source": {"kind": "name_crop"}, "role": "composition", "order": 0},
    {"id": "rf_2", "source": {"kind": "blocking"}, "role": "pose", "order": 1},
    {"id": "rf_3", "source": {"kind": "character", "id": "hina", "ref": "face"}, "role": "character", "order": 2},
    {"id": "rf_4", "source": {"kind": "location", "id": "loc_rooftop"}, "role": "background", "order": 3}
  ],
  "regions": [
    {"id": "rg_01", "kind": "person", "rect_mm": [148, 22, 74, 100], "char": "hina", "source": "user", "confidence": null},
    {"id": "rg_02", "kind": "text", "line_id": "ln_b0013", "source": "derived", "confidence": 1.0}
  ],
  "gen": {"pad_mm": 3.0, "prompt_override": null, "avoid_override": null, "size_override": null},
  "attempts": {"requests": 2, "images": 5, "fix_rounds": 1},
  "brief_hash": "sha256:e19c…",
  "candidates": [
    {"id": "cd_01", "asset": "sha256:cd34…", "request": "rq_3f9a51c0", "parent": null, "mode": "new",
     "brief_hash": "sha256:e19c…",
     "origin": {"kind": "agent", "actor": "ai:hermes", "tool_id": "openai:gpt-image-1", "tool": "image_generate", "model": "gpt-image-1",
                "reported": true, "prompt": "A black-and-white Japanese manga panel …",
                "params": {"size": "1536x1024", "quality": "high"},
                "refs_used": ["refs/hina_face.png", "guides/composition.png"], "note": ""},
     "mapping": {"frame_rect_mm": [13, 13, 231, 110], "pad_mm": 3.0, "px": [1536, 1024]},
     "review": {"by": "ai:hermes", "score": 0.55, "note": "顔が小さく台詞位置に重なる", "fix": "顔を大きく、右寄せに"},
     "status": "rejected", "stale": false, "stale_geometry": false},
    {"id": "cd_05", "asset": "sha256:ab12…", "request": "rq_77c0e1a2", "parent": "cd_01", "mode": "edit",
     "brief_hash": "sha256:e19c…",
     "origin": {"kind": "agent", "actor": "ai:hermes", "tool_id": "openai:gpt-image-1", "tool": "image_edit", "model": "gpt-image-1",
                "reported": true, "prompt": "Make her face larger and move her to the right …", "params": {},
                "refs_used": ["source.png"], "note": ""},
     "mapping": {"frame_rect_mm": [13, 13, 231, 110], "pad_mm": 3.0, "px": [1536, 1024]},
     "review": {"by": "ai:hermes", "score": 0.86, "note": "構図とキャラが合う"},
     "status": "adopted", "stale": false, "stale_geometry": false}
  ],
  "adopted": {"art": "cd_05"},
  "reviews": {"art": {"by": "ai:hermes", "score": 0.86, "input_hash": "sha256:…"}},
  "archived": []
}
```

- `status`: `empty | briefed | requested | candidates | fix_requested | adopted | skip`。`skip`（絵を置かないコマ）は human の `set_panel{set:{status:"skip"}}` だけが設定できる。
- `brief_hash` は、生成に効くフィールド（shot、characters、props、refs、regions、instruction、location、gen の上書き、style の版）の正準 JSON の sha256。候補の `brief_hash` と違えば `stale:true`。
- `origin` は候補の来歴である。`reported:true` は「エージェントの自己申告」を意味し、Genko は `tool`・`model`・`prompt` を検証しない。`kind` は `agent | human | self | fixture | genko`（`genko` は Genko 内の決定的変換。`fixture` は試験用で、書き出しの preflight が拒否する）。
- `mapping` はコマと画像の対応（§9.2）。フレームの矩形が変わると `stale_geometry:true`。
- `pinned` に入ったフィールドは `ai:*` の `set_panel` で変えられない。`gen.*_override` は human だけが設定でき、設定すると自動で pinned になる。
- `attempts` は依頼と取り込みの回数の持続記録で、worklist が上限（`policy.limits`）を超えたコマを人間に回すために使う（§7.2）。
- 候補レコードは約 500 バイト。40 コマ × 8 枚でも 160 KB ほど。2000 件を超えたら却下分を `studio/history/*.json` に退避する（`genko gc --archive-candidates`）。

### 4.5 生成依頼パック（`studio/requests/<id>/request.json`）

依頼パックは、エージェントが画像ツールに渡すものをすべて1か所にまとめたものである。Genko は生成しない。作るだけで、同じ内容の依頼は同じ id になる（内容の hash から id を作る）。写しは `assets/` にも置き、候補の来歴から辿れる。

```json
{
  "type": "genko.genreq@1",
  "id": "rq_3f9a51c0",
  "purpose": "panel_art",
  "mode": "new",
  "target": {"page_id": "pg_7c1e9a0b2d41", "page": 4, "frame_id": "f4_p1", "label": "4-1"},
  "brief_hash": "sha256:e19c…",
  "size": {
    "frame_mm": [231, 110], "pad_mm": 3.0, "aspect": "2.04:1",
    "suggested_px": [1536, 744],
    "tool_sizes": [{"px": [1536, 1024], "crop": "縦を中央で切る"}, {"px": [1792, 1024], "crop": "縦を中央で切る"}],
    "print_px_at_600dpi": [5598, 2740]
  },
  "prompt": {
    "ja": "モノクロの日本の漫画のコマ。夕方の学校の屋上。細身の16歳の少女（肩までの黒いストレート髪、横に流した前髪、左に星形の髪留め、半袖のセーラー服）が、画面右寄りでこちらへ振り返り、やわらかく笑う。ローアングルのミディアムショット（腰から上）。左上の3割は台詞用に空けておく。",
    "en": "Black-and-white Japanese manga panel, greyscale, clean linework. School rooftop at sunset. A slim 16-year-old girl with shoulder-length straight black hair, side-swept bangs and a small star-shaped hair clip on the left, short-sleeved sailor uniform, turning back toward the viewer on the right side of the frame, gentle smile. Low-angle medium shot, waist up. Keep the upper-left 30% of the image calm and empty for speech balloons.",
    "tags": "monochrome, greyscale, manga, 1girl, black hair, shoulder-length hair, side-swept bangs, star hairclip, serafuku, looking back, smile, upper body, from below, rooftop, sunset"
  },
  "avoid": ["文字・フキダシ・効果音の描き文字", "色", "署名・透かし", "眼鏡（hina）"],
  "keepout": [{"box01": [0.0, 0.0, 0.32, 0.55], "why": "台詞 2 つ（読み順 1・2）"}],
  "characters": [{"id": "hina", "tokens_en": "a slim 16-year-old girl …", "files": ["refs/hina_face.png", "refs/hina_sheet.png"]}],
  "files": {
    "composition": "guides/composition.png",
    "pose": "guides/pose.png",
    "keepout": "guides/keepout.png",
    "references": ["refs/hina_face.png", "refs/hina_sheet.png", "refs/loc_rooftop.png"],
    "source": null,
    "mask": null
  },
  "order_of_instructions": ["共通制約", "ページ", "コマのメモ", "今回の指示"],
  "notes_for_agent": [
    "composition と pose は構図の参考。線をなぞらせる必要はない",
    "参照画像を渡せないツールでは prompt だけで作り、取り込み時に refs_used を空にする",
    "画像は inbox/rq_3f9a51c0/ に置いてから import_images を呼ぶ（別マシンなら HTTP でアップロード）"
  ],
  "import": {"tool": "import_images", "request_id": "rq_3f9a51c0", "inbox": "studio/inbox/rq_3f9a51c0/"}
}
```

- **`purpose`:** `panel_art`（コマの絵）、`character_sheet`（設定画）、`location`（背景の参照）、`draft`（ネーム用のラフ、DRAFT に置く）。
- **`mode`:** `new`（新規）、`edit`（元画像 `source` + 指示で直す）、`inpaint`（元画像 + `mask` の白い所だけ描き直す）、`upscale`（高解像度化）。候補の `mode` にはほかに `derive`（Genko の決定的変換、`origin.kind:"genko"`）がある。修正の依頼は `parent` の候補を `source` に入れる。
- **サイズ。** `suggested_px` はコマの比から計算した理想（約 1 MP、64 の倍数）。画像ツールが決まったサイズしか出せない場合に備え、`tool_sizes` に「どのサイズで作ってどう切るか」を並べる。候補を取り込むときに縦横比が違っても、配置は cover で合わせ、切れる範囲を `mapping` に残す。対応サイズの一覧はユーザー設定の `tools.json`（§6.5）から引く。
- **プロンプトは下書きである。** 組み立ては決定的（style → キャラの `tokens` → shot と angle の語彙 → 場所 → 動きと表情 → 制約の順）で、エージェントは自由に書き直してよい。実際に使ったプロンプトは取り込み時に来歴へ書く。人間が `gen.prompt_override` を書いたコマでは、下書きの代わりにそれが入り、`notes_for_agent` に「人間の指定なので変えない」と書かれる。
- **ガイド画像**（`guides/`）は Genko が描く。`composition` はネーム（NAME のストロークとコマ）の切り抜き、`pose` は人物の箱・頭の位置・向きの矢印、`keepout` は文字よけの範囲。どれも生成サイズで描き、白地に黒の単純な線にする（どの画像ツールにも参照として渡しやすい）。
- **参照画像**（`refs/`）は承認済みのキャラの顔と設定画、場所の画像、コマに登録された参照素材の写しである。
- **同じマシンではファイルパス、別マシンでは URL** で渡す。`generation_request` の応答は `request.json` の中身と、各ファイルのパス（同じマシン）か `GET /v1/requests/{id}/files/{name}` の URL（別マシン、トークン付き）を返す。応答にはガイドの縮小プレビュー（長辺 512 px）を画像として1枚だけ付ける。

### 4.6 モデルへの追加（Python）

```python
# src/genko/models.py（追加分だけ）
class LayerKind(str, Enum):
    ...
    PLACED = "placed"                  # 資産 hash + placement_mm。ページ大の raster_png を持たない

@dataclass(frozen=True)
class Stroke:                          # M1: 不変にする。編集は置き換え（edit_stroke / simplify_stroke は新しい Stroke を作る）
    id: str
    points: tuple[tuple[float, float], ...] = ()
    pressure: tuple[float, ...] = ()
    width_mm: float = 0.35
    kind: str = "gpen"
    handles: tuple | None = None

    def __deepcopy__(self, memo: dict) -> "Stroke":
        return self                    # 不変なので共有する。deepcopy の費用がストロークの本数に比例しなくなる

@dataclass
class Layer:
    ...                                # 既存フィールドはそのまま
    asset: str | None = None           # "sha256:<hex>"。raster は保存時に算出、placed は常に持つ
    frame_id: str | None = None        # placed: 対象コマ
    placement_mm: Rect | None = None   # placed: 画像全体のページ上の範囲（mm）
    fit: str = "cover"                 # cover | contain | stretch
    clip_to: str = "frame"             # frame | bleed | none
    source: dict | None = None         # {"candidate": "cd_…", "request": "rq_…"}
    finish: dict | None = None         # None → studio.style.finish
    strokes_blob: str | None = None    # v3: 保存時に算出。メモリ上は strokes を持つ

@dataclass
class Frame:
    ...
    panel: PanelSpec | None = None     # 型は genko/studio/state.py。models は注釈だけで import しない

@dataclass
class StoryLine:
    ...
    beat_id: str | None = None
    speaker_id: str | None = None

@dataclass
class Page:
    ...                                # 既存の必須フィールドの後ろに既定値付きで足す
    id: str = field(default_factory=lambda: "pg_" + new_id())
    art_ok: bool = False
    plan: dict | None = None
    extra: dict = field(default_factory=dict)

    def side(self, binding: Binding, start_side: str | None = None) -> str: ...   # "left" | "right"（§9.3）

@dataclass
class Episode:
    ...
    revision: int = 0
    start_side: str | None = None
    studio: StudioState = field(default_factory=StudioState)
    extra: dict = field(default_factory=dict)
    assets: AssetStore | None = field(default=None, repr=False, compare=False)   # 一時。保存しない

    def __deepcopy__(self, memo: dict) -> "Episode":            # M1: undo 履歴と資産ストアを写さない
        new = object.__new__(type(self))
        memo[id(self)] = new
        for f in dataclasses.fields(self):
            if f.name in _TRANSIENT:                             # {"undo_stack", "assets"}
                continue
            object.__setattr__(new, f.name, copy.deepcopy(getattr(self, f.name), memo))
        new.undo_stack = []
        new.assets = self.assets                                 # 不変ストアは共有
        return new
```

`apply_ops` の commit 側も変える（M1）。今は commit 時に `copy.deepcopy(episode)` をもう一度作って undo に積む（ops.py:1012-1015）。`_copy_state(episode, work)` の後、元のフィールドの値はどこからも参照されなくなるので、**複製せずにそのまま undo の項目へ移す**（`Episode` の浅い入れ物に旧値を入れる）。これで 1 op の deepcopy は作業用の1回だけになり、その費用は undo の深さに依らない。ストロークは不変なので共有され、1回の費用もリストの張り替え程度になる。

`StudioState`、`PanelSpec`、`Script`、`Scene`、`Beat`、`Region`、`Candidate`、`Approval` は `src/genko/studio/state.py` の dataclass で、`to_dict` / `from_dict` と検証を持つ。core のまま（Pillow も不要）。

`AssetStore` は `__deepcopy__` で自分自身を返す。`apply_ops` の deepcopy でストアが複製されない。

### 4.7 識別子と寿命のルール

| 事象 | ルール |
|---|---|
| ページ | `Page.id` が不変の識別子。`index` は表示と既存 op のため残す。tickets、page_locks、承認、生成依頼は `page_id` で持つ |
| `delete_page` / `reorder` | `_remap_page_refs(episode, mapping)` が `spread_with`、`onion_from`、チケットの `page_index`、StoryLine の `page_index` を直す。削除ページを指す参照は None にし、チケットは `status:"orphaned"` にする |
| `duplicate_page` | 新しい `Page.id` を付ける。フレーム id は既存どおり刷新し、**旧 id → 新 id の対応表**を返させて、複製した台詞の `frame_id`、placed layer の `frame_id`、panel.regions の `line_id` を張り替える。資産は不変なので共有してよい（コピー不要） |
| `split_frame` | 親は id を保って内部節点になる（models.py:309-342）。親の `panel` は読み順で先に来る子へ移す（縦分割なら右の子）。もう一方の子は空。親に placed art があれば `force:true` が要り、art は `studio.orphans` へ移す |
| `merge_frame` | 親は読み順で先の子の panel を継ぐ。他の子の panel と placed art は `studio.orphans` へ移し、理由を残す。子に採用済みの art があれば `force:true` が要る |
| `resize_frame` | placed layer の `placement_mm` はそのまま（mm で持つ）で、クリップは新しい矩形に従う。候補の `mapping.frame_rect_mm` と違えば `stale_geometry:true` |
| `apply_layout` / `reset_frames` | `name_ok` 後は拒否（`revoke{gate:"name"}` が先）。placed art があるときは `force:true` が要り、art を orphans へ移す |
| 候補の受け取り先が消えた | `import_candidates` はエラーにせず orphans に入れる（エージェントが生成している間にコマが消えても、生成した画像を失わない） |
| name_ok 後のレイアウト | studio プロジェクトでは `lock_layout_after_name_ok`（既定 on）。台詞の編集と移動は可 |

### 4.8 版の更新と migrate.py の計画

**段階 R0（M1-5。書式は変えないリリース）:**

- `migrate_payload` が `payload["version"]` を読む。`SUPPORTED_VERSION = 2`。
- `version > SUPPORTED_VERSION` なら `UnsupportedProjectVersion` を上げる。CLI は `{ok:false}` で exit 2、HTTP は 409、GUI はダイアログを出して読み取り専用で開く。
- 未知のトップレベルキーとページ単位のキーは `Episode.extra` / `Page.extra` に入れ、保存時にそのまま書き戻す。
- これを v3 writer より**前のリリース**で出す。1つ古いビルドが v3 ファイルを開いても黙って壊さない。

**段階 R1（v3 writer）:**

```python
# src/genko/migrate.py
SUPPORTED_VERSION = 3
MIGRATIONS: dict[int, Callable[[dict], dict]] = {
    1: v1_to_v2,     # 既存の形による判定（layers がない → default_layers + name/ink strokes）を関数化
    2: v2_to_v3,
}

def migrate_payload(payload: dict) -> Episode:
    version = _detect_version(payload)              # "version" キー、なければ形で判定
    if version > SUPPORTED_VERSION:
        raise UnsupportedProjectVersion(version, SUPPORTED_VERSION)
    while version < SUPPORTED_VERSION:
        payload = MIGRATIONS[version](payload)
        version += 1
    return _episode_from_v3(payload)                # 未知キーは extra へ
```

`v2_to_v3` は純関数で、次を行う。

1. 各ページに `id` を振る（`pg_` + new_id）。
2. `page_locks` のキーを `str(index)` から page id に変える。
3. tickets に `page_id` を足す。
4. `revision: 0`、`studio: {}`、`Frame.panel: null`、Layer の新フィールドを既定値で入れる。
5. `story` を正本にし、`pages[].texts` は読み捨てる（同一オブジェクトで組み直す）。
6. ラスタの `raster_relpath`（`pages/NNN/…`）は読み込みに使い、**最初の v3 保存時**に `assets/` へ書き直す。
7. `layers[].strokes` を読み、`name_strokes` / `ink_strokes` は `layers` が無い v1 のときだけ使う。**最初の v3 保存時**にストロークを blob にし、project.json には `strokes_blob` と本数だけを書く。
8. M0 のサイドカー（`studio/drafts/`）は読むだけで、取り込みは M3 の `studio adopt-drafts`（op 経由）で行う。

最初の v3 保存では `project.v2.bak.json` を書く。`pages/` の旧ファイルは `genko gc --legacy` まで消さない。

- `save_episode` は `version: 3`、`revision`（+1）を書き、journal に追記する（§5.1）。書き込みは一時ファイル + `os.replace`（Windows でも原子的）。
- 試験:
  - `tests/test_migrate.py` の v1 読み込み、`tests/test_io.py` の往復はそのまま通す。
  - v2 フィクスチャ（ラスタ、USER レイヤー、lt_convert したページ付き）→ v3 → 保存 → 読み込みで原稿の状態が一致すること。
  - v3 の project.json に `name_strokes` / `ink_strokes` とストローク座標が無く、16 ページのネーム作業フィクスチャで 1 MB 未満であること。
  - version 4 を拒否すること。
  - 未知キーが残ること。
- **変える既存テスト:** `tests/test_p1.py:84` は `pages/001/bg.png` の存在を見ている。v3 では `dest / bg.raster_relpath`（`assets/…`）の存在を見るように直す（M2-3 の PR に含める）。


### 4.9 inspect / snapshot の追加

compact な snapshot（`headless.snapshot`）は小さいまま保つ。ストローク座標と画像バイトは入れない。

- トップレベル: `revision`、`studio: {step, gates_pending, approvals_requested, worklist_counts, preset}`、`bible: {characters: [{id, name, locked}]}`。
- ページ: `id`、`side`（left / right）、`art_ok`、`locked_by`、`reading_summary`（例「右上 → 左上 → 下」）。
- 葉: `order`、`label`（例 "4-2"）、`panel_status`、`has_spec`、`candidates: {n, adopted, stale}`、`open_request`。
- レイヤー: `kind`、`has_raster`、`frame_id`、`title`。

詳細は `inspect_stroke`（headless.py:75-87）と同じ形の対象指定クエリで取る。MCP の `inspect` も同じ。

- `genko inspect PROJ --panel 4:f4_p1`: PanelSpec 全体、ガイド一覧、候補一覧。
- `--candidate cd_05`: 候補、来歴、系譜、評価。
- `--script [--scene sc_03] [--page 4]` / `--bible` / `--studio` / `--request rq_3f9a51c0`。

---

## 5. 新しい ops

### 5.1 バスそのものの変更（M1・M2）

M1 は性能・安全・ロック・actor（1〜3、7、10）を、M2 は v3 に要るもの（4〜6、8、9、11、12）を入れる。

1. **actor。**
   - `apply_ops(..., agent=...)` は引数としてはある（ops.py:976）が、CLI も HTTP も渡していない（__main__.py:133、server.py:111）。
   - `genko apply --agent human:leaf` と HTTP を足し、`apply_ops` と `ProjectLock` に渡す。HTTP では本文の値ではなくトークンに結び付いた actor を使う（§8.4）。
   - 文法は `human:<name>`、`ai:<agent>`（`ai:hermes`、`ai:hermes-namer` など。MCP サーバーは起動時に決めた actor を使う、§6.4）、`system:genko`（Genko 自身の自動処理）、`legacy:unknown`。
   - **actor の省略。** studio プロジェクト（`studio.policy` がある）では、省略した呼び出しは `legacy:unknown` になる。編集はできるが、ゲート op・policy の変更・ロックの解除はできない。studio でない既存プロジェクトでは、旧既定の `genko` を今までどおり扱う（既存テストと既存の使い方はそのまま通る）。
   - `docs/AGENT.md` を直し、エージェントは常に `--agent ai:<name>` を付けると書く（M1-4）。
2. **ゲート op の actor 規則。**
   - `approve`、`revoke`、`name_ok`、`set_studio` の policy 系フィールド（gates、autonomy、strict_gates）、`unlock` 系フラグは `human:*` だけが本承認できる。
   - `ai:*` はゲート op を一切使えない（第3版では AI の仮承認を置かない）。承認は `request_approval` で人間に依頼する（§5.3）。
   - `system:genko` と `legacy:unknown` はゲートに触れない。
   - `export` の承認は常に人間。
3. **page lock の抜け道を塞ぐ。**
   - `edit_line`、`move_line`、`delete_line`、`set_balloon_path` は、行から page を引いて `_check_page_lock` にかける（今は `page` を持たないので素通り、ops.py:957-969）。
   - `set_ticket` はチケットの `page_id` で検査する。
   - **`lock_page` / `unlock_page`**（今は検査の対象外で、所有者は op の `agent` 引数、ops.py:762-770, 959）:
     - 所有者は常に actor。`agent` 引数は受けるが、actor と違えば拒否する。
     - 他人のロックがあるページへの `lock_page` は拒否する。例外は `human:*` が `ai:*` のロックを引き取る場合だけで、`audit.jsonl` に残す。
     - `unlock_page` は所有者か `human:*` だけ。`ai:*` と `legacy:unknown` は人間のロックを外せない。
   - page_locks のキーは page id（M2）。
4. **呼び出し側 id。** 何かを作る op（`add_page{ids}`、`split_frame{child_ids}`、`apply_layout{ids}`、`add_line{id}`、`add_region{region.id}`、`add_mannequin{id}`、`add_ticket{id}`、`add_layer{id}`、`bind_ref{ref.id}`）は、任意で呼び出し側の id を受ける。
   - 形式は `^[A-Za-z0-9_-]{4,40}$` で、重複は `ApplyError`。
   - これで1つのバッチの中で、作ってから参照できる。再試行したコミットも重複せず冪等になる。
5. **`page_id` の受理。** `page`（番号）を取る op はすべて `page_id` も受ける。両方あれば一致を検査する。
6. **`_validate(work)`（触れた範囲だけを厳格に）。**
   - op ループの後、dry_run の返却と commit の前（ops.py:1008 の位置）で実行する。失敗すれば全体が中止され、何も残らない。
   - **厳格に見るのは、そのバッチの ops が触れた実体だけ**（ページ、フレーム、台詞、キャラ、候補）。プロジェクト全体の走査はしないので、1 op の費用がプロジェクトの大きさに比例しない。
   - 検査すること:
     - `StoryLine.frame_id` がそのページの葉である（今は未検査）。
     - panel は葉にだけある。
     - `adopted` が指す候補がある。
     - placed layer の `frame_id` がそのページにある。
     - beat、キャラ、location、prop の参照が解決する。
     - ゲート不変条件（strict_gates のとき）: `stage ∈ {ink, finish}` なら `name_ok`、`art_ok` なら `name_ok`、`stage = finish` なら `art_ok`。
   - **資産の欠落はエラーにしない。** `assets/` が無い・同期が遅れている（project.json だけ複写した、クラウド同期の遅れ）場合でも、人間の修正を含む op は通す。欠落は結果の `warnings[]` に出し、書き出しの preflight でエラーにする。`genko doctor PROJ [--relink DIR]` が欠落を一覧にし、別の場所の資産フォルダから hash で拾い直す。
   - エラー形式は `validate: page pg_7c1e frame f3_p1 panel.characters[0].id: unknown character chr_x`。
7. **汎用 `_copy_state` と、undo 履歴を複製しない apply（M1）。**

   ```python
   _TRANSIENT = frozenset({"undo_stack", "assets"})

   def _copy_state(dst: Episode, src: Episode) -> None:
       for f in dataclasses.fields(Episode):
           if f.name not in _TRANSIENT:
               setattr(dst, f.name, getattr(src, f.name))
   ```

   - `Episode.__deepcopy__` は `undo_stack` と `assets` を写さない（§4.6）。作業用の deepcopy は1回で、費用はプロジェクトの大きさに比例し、undo の深さに依らない。
   - commit 時の2回目の deepcopy（ops.py:1012）をやめ、置き換えられた旧フィールドの値をそのまま undo の項目に移す。
   - `Stroke` を不変にし、`__deepcopy__` で自分を返す（§4.6）。ストロークを書き換える op（`edit_stroke`、`simplify_stroke`、`erase_raster` のストローク側）は新しい Stroke に置き換える。
   - **予算と試験:** 16 ページ（各ページ NAME ストローク 30 本）で、undo の深さ 50 の apply が 100 ms 未満、かつ深さ 0 の 1.5 倍以内（`test_apply_cost_independent_of_undo_depth`）。lt_convert したページを含むフィクスチャでも深さに依らないこと。
   - さらに速くする「触れたページだけ複写する copy-on-write」は、GUI がモデルを直接書き換える抜け道（§0.5 の 20）を塞いだ後（M7）に入れる。それまで入れると、直接の書き換えが undo の項目に漏れる。
8. **revision と journal（M2）。**
   - `save_episode` は `revision` を +1 し、`studio/journal.jsonl` に `{rev, base_rev, actor, at, ops, origin}` を追記する。全文スナップショットは書かない（§4.2）。
   - `ops` は**正規化済み**で記録する。apply_ops が生成した id、写植の座標（`add_line` は常に明示座標）、`put_raster` の `path` を取り込んだ資産 hash に置き換えた値を op の写しに書き込むので、再生（replay）が決定的で、フォントやローカルファイルに依らない。
   - 50 commit ごとと版移行の前にチェックポイントを置く（§4.2）。
   - `apply --expect-revision N` と HTTP の `expect_revision` で楽観的並行制御を行う。不一致は `ApplyError("revision conflict: expected N, found M")`、HTTP 409。revision は `project.json` の先頭にあり、v3 の project.json は小さいので、確認は安い。
9. **CLI/HTTP 越しの undo（M2）。**
   - `genko undo PROJ [--as human:leaf]` は、最新の commit が同じ actor のものなら、直前のチェックポイントから `rev - 1` まで journal を再生した状態に戻す。他の actor の commit が後にあれば拒否する（`--force` は上書き）。
   - `genko redo` も用意する。
   - M7 で「rebase undo」を足す。対象 commit の直前へ戻してから、後続の commit の正規化済み ops を再適用し、再適用に失敗したら拒否する。エージェントが作業している間にも、人間が自分の変更を取り消せるようにするためである。
   - GUI 内の in-memory `undo_stack` は上限 50 のまま。7 の修正で、深さが費用に効かなくなる。
10. **ロック（M1）。**
    - **OS のファイルロックを主にする。** `project.lock` を開き、POSIX は `fcntl.flock(LOCK_EX | LOCK_NB)`、Windows は `msvcrt.locking(LK_NBLCK)` で取る。プロセスが死ねば OS が外すので、「失効」を推測しない。中身（token、agent、pid、host、acquired_at）は表示と診断のためだけにある。
    - **OS ロックが使えないファイルシステム（一部のネットワーク共有）での予備:** `os.open(path, O_CREAT | O_EXCL | O_WRONLY)` で作り、ランダムな token を書く。
      - `release` は、ファイルの token が自分のものと一致するときだけ消す（今は無条件に unlink、lock.py:33-35）。
      - 失効したロック（15 分）の奪取は unlink ではなく、観測した token を名前に含む墓石（`project.lock.stale-<token>`）への `os.rename` で行う。rename の後に墓石の中身を読み、token が観測値と一致したときだけ `O_EXCL` で作り直す。一致しなければ（他人の新しいロックを動かしてしまった）、墓石を `os.link` で元の名前に戻して待ち直す。
      - この経路は最善努力であり、`genko doctor` がこのファイルシステムでは同時実行を1プロセスに絞るよう警告する。
    - CLI と HTTP は**ロックの中でロード**する（今はロックの前、__main__.py:131-133、server.py:108-111）。
    - 試験: 子プロセス 8 つが同時に取る（1つだけ成功）、失効したロックを同時に奪う（1つだけ成功し、新しい持ち主のロックは消えない）、自分のロックが失効したプロセスの `release` が次の持ち主のロックを消さない。
11. **スキーマ登録簿（`src/genko/schema.py`、M2）。**
    - 各 op を `OpSpec(name, params_schema, scope, actor_rule, creates, strict_compatible)` で登録する。
    - `params_schema` は、エージェント側のどの LLM（Claude、GPT、オープンモデル）でもツール引数として扱える保守的な JSON Schema で書く。使えるのは `enum`、`const`、`anyOf`、`allOf`、`$ref`。`additionalProperties:false`、任意項目は nullable。**`oneOf`、再帰、数値の範囲（`minimum` / `maximum`）、文字列長（`minLength` / `maxLength`）は使わない。** 範囲は handler 側で検査する。
    - ここから次を生成する。
      - (a) 従来形の `OPS_SCHEMA`（`genko schema` の互換）
      - (b) `docs/ops.schema.json`。既存の `ops` 目録の配列（`{op, …}` の並び。test_p1.py:118-124 が読む）を**そのまま残し**、同じファイルに機械用の JSON Schema（`$defs` と、`op` の `const` で分けた `anyOf`）を足す
      - (c) MCP ツールの入力スキーマ（§8.1）。80 を超える op を1つの巨大な union にしない。道具ごとに、その道具が受ける op だけを並べる
    - テスト: `_apply_one` と `apply_studio_op` が扱う op 名がすべて登録されていること、docs ファイルが生成物と一致すること、**生成した tool スキーマの lint**（`oneOf`、数値・文字列の制約、`false` 以外の `additionalProperties`、再帰参照を含まない）。
12. **`strict_gates`（既存 op の挙動変更は opt-in、M2）。**
    - `studio.policy.strict_gates` が true のプロジェクト（`studio init` の既定）だけ、次を強制する。
      - `put_raster`、`erase_raster`、`filter_raster`、`flood_fill` を exportable な role（ink / bg / finish / user）に使うときは name_ok が要る。
      - `advance{to:"finish"}` には `art_ok` が要る。
      - `set_spread` は向かい合う対だけを受ける（§9.3）。
      - frame_id があって座標の無い `add_line` は拒否する（写植は Genko の写植器が計算し、op は明示座標を持つ）。
    - 既存プロジェクトと既存テスト（§0.5 の 18、30）の挙動は変えない（`set_spread` は警告だけ）。
    - 新しい op（`adopt_candidate` など）は最初から常に厳格にする。

### 5.2 既存 op の拡張

| op | 追加・変更 | 効果 | undo / ロック |
|---|---|---|---|
| `add_page` | `ids?: [str]` | 新ページに呼び出し側の page id を付ける | 通常 |
| `delete_page` | – | `_remap_page_refs` で tickets / spread_with / onion_from を直す。page_locks は id キーなので不変 | page lock |
| `reorder` | – | 同上 | 全ページのうち1つでも他人がロックしていれば拒否 |
| `duplicate_page` | `id?`（新 page id） | 新 page id を付け、frame id の対応表で台詞・placed layer・領域を張り替える。panel と候補メタは複製し、資産は共有 | page lock（元ページ） |
| `split_frame` | `child_ids?: [a, b]`、`force?` | panel を読み順で先の子へ移す。placed art は force 時に orphans へ | page lock。studio では name_ok 後は拒否 |
| `merge_frame` | `force?` | 子の panel は archived / orphans へ | 同上 |
| `resize_frame` | – | 候補を `stale_geometry` にする | 同上 |
| `add_line` | `id?`、`beat_id?`、`speaker_id?`、`page_id?` | **op の中では写植しない。** 配置は `letter.py` がネーム計画の取り込み時に計算し、x/y/w/h を明示した op を出す。strict_gates では frame_id があって座標の無い add_line を拒否する（旧プロジェクトの挙動は従来どおり）。journal の再生がフォントやマシンに依らない | page lock |
| `edit_line` / `move_line` / `delete_line` / `set_balloon_path` | – | v3 では story と page.texts が同一オブジェクトになり、片方だけ変わる問題が消える | **行のページで lock 検査**（新） |
| `add_mannequin` | `id?`、`frame_id?` | 人物配置との対応付け | page lock |
| `put_raster` | 画像の検証（Pillow で開けること、寸法の上限） | 壊れたバイト列を受け付けない。strict_gates では exportable な role に name_ok が要る。HTTP と MCP の下では `path` を `--root` 配下に限る。journal には `path` ではなく取り込んだ資産 hash を書く | page lock |
| `erase_raster` / `filter_raster` / `flood_fill` | – | strict_gates では exportable な role に name_ok が要る | page lock |
| `name_ok` | actor 規則 | `approve{gate:"name"}` の別名。page を省略すると全ページが対象（既存どおり）。承認記録を残す | human のみ |
| `advance` | – | strict_gates では `to:"finish"` に art_ok が要る | page lock |
| `add_ticket` / `set_ticket` | `id?`、`page_id`、`kind`（generic / fix / review / gate）、`gate?`、`character_id?`、`text?`、`candidate_id?`、`comment?`（thread に追記） | `created_by` は actor から入れる。今の `add_ticket` は page_index・frame_id・role・assignee・rate・status しか保存しない（ops.py:621-633）ので拡張する。修正指示と審査の往復に使う | page lock（チケットのページ） |
| `lock_page` / `unlock_page` | `agent` は actor と一致するときだけ受ける | 所有者は actor。他人のロックは奪えない（`human:*` が `ai:*` のロックを引き取る場合を除く）。unlock は所有者か `human:*` だけ。キーは page id | actor 規則（§5.1-3）。`_check_page_lock` の除外を外し、専用の規則で検査する |
| `set_meta` | `start_side?: left \| right \| null` | 見開きの開始側を上書きする（§9.3） | – |
| `set_spread` | – | `Page.side` から、対が隣り合い、左右が逆で、綴じ方向で先に読む側に若い番号が来ることを検査する（§9.3）。strict_gates では拒否、旧プロジェクトは `warnings[]` | page lock |
| `add_stroke` | `space?: page \| spread` | 既定の `page` は今の意味のまま（原点は `op.page`、x ≥ 紙幅なら相手ページへ）。`spread` では x を物理的な見開きの左端から測る（左ページ [0, W)、右ページ [W, 2W)）。GUI のキャンバスは `spread` を使う（§9.3） | page lock（両ページ） |



### 5.3 新しい op

実装は `src/genko/studio/ops.py` の `apply_studio_op`。`ops._apply_one` の末尾、`raise ApplyError(f"unknown op: {name}")`（ops.py:830）の直前で、名前が `STUDIO_OPS` にあれば遅延 import して渡す（既存の遅延 import と同じ流儀、ops.py:316, 667, 800）。
すべて純粋なメモリ上の変更で、ネットワークは使わない。I/O は資産ストアの**読み取り**（存在と画像ヘッダの確認）だけで、`put_raster` がパスを読むのと同じ種類である。MCP の道具（§8.1）はこれらの op を組み立てて `commit.py` で適用する。

**設定とゲート**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_studio` | `premise?`, `policy?`, `style?`, `unlock_style?` | merge-patch。スタイル固定後の `style` 変更は `unlock_style:true`（人間）が要る | policy 系は human。episode 範囲 |
| `approve` | `gate: bible\|script\|sheet\|name\|art\|export`, `page?`/`page_id?`, `character_id?`, `candidate_id?`, `face_box_px?` | 承認を記録する。name → `name_ok=true`, stage=ink。sheet → 候補を採用し、`face_box_px`（エージェントが `report_regions` で提案した顔の箱か、人間が指定した箱）で顔を切り出して `refs` に足し、`locked:true`。art（パイロットページ）→ `studio.style.locked_from_page` も立てる。art → 前提（指示のある全コマが adopted か skip）を検査して `art_ok=true`。export → preflight の結果を添えて記録する。対応する承認依頼のチケットを閉じる | **human のみ**。`ai:*`・`system:*`・`legacy:unknown` は拒否 |
| `revoke` | `gate`, `page?`/`page_id?`, `reason` | name → `name_ok=false`, `stage=name`, `art_ok=false`。art → `art_ok=false`。採用済みの候補は残す | human のみ |
| `request_approval` | `gate`, `pages?`, `character_id?`, `note` | kind=gate のチケットを作る（同じ対象の未処理の依頼があれば note を追記するだけ） | 誰でも。page lock |

**企画・キャラ・資産**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `upsert_character` | `character{id, …}`, `unlock?` | キャラを1人分だけ差し替える（`set_bible` はリスト全体の置換）。`locked` のキャラは `unlock:true`（human）が要る。`pinned` のパスは ai:* が変えられない | episode |
| `delete_character` | `id`, `force?` | 参照されていれば `force` が要る | episode |
| `upsert_location` / `delete_location` | `location{id, …}` / `id` | 場所の登録 | episode |
| `upsert_prop` / `delete_prop` | `prop{id, name, description, refs?}` / `id`, `force?` | 小物の登録。参照されていれば削除に `force` が要る | episode |
| `register_assets` | `assets: {"sha256:…": {kind, mime, w, h, label?, origin?, note?}}` | ストアにあることと画像ヘッダを確かめ、`studio.assets` の索引に入れる。`origin`（generated / self / other など）は整理用のメモで任意。取り込んだ候補は `generated` として自動で付く | episode。読み取りのみの I/O |
| `attach_reference` / `detach_reference` | `target{character_id\|location_id\|prop_id}`, `asset`, `kind: sheet\|face\|turnaround\|outfit\|establishing\|style\|reference` | 参照画像の付け外し | sheet と face の付け替えは sheet ゲートの規則に従う |

**脚本**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_script` | `script` | 脚本全体を置き換える。dry_run は差分要約を返す | episode。承認済みの脚本を ai:* が変えると script の承認が外れ、再承認が要る |
| `upsert_scene` / `delete_scene` | `scene{id, after?, …}` / `id` | scene 単位の編集 | 同上 |
| `upsert_beat` / `delete_beat` / `move_beat` | `scene_id`, `beat{id, …}`, `after?` / `id` / `id, scene_id, after` | beat 単位の編集。既に置いた台詞（`beat_id`）の本文は `edit_line` で別に直す | 同上 |

**ネーム**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_page_plan` | `page`, `beat_ids`, `turn_role: normal\|hook\|reveal` | ページへの beat 割り当て | page lock |
| `apply_layout` | `page`, `tiers[]`, `gutter_mm?`, `ids{slot: frame_id}`, `root_id?`, `force?` | 段組 DSL（§5.5）からギロチン木を丸ごと作る。葉の id は `ids` のとおり | page lock。name_ok 後や placed art ありでは拒否（force で orphans へ） |
| `reset_frames` | `page`, `root_id?`, `force?` | 根1つに戻す | 同上 |
| `set_panel` | `page`, `frame_id`, `set{…}`, `unset?[]`, `pin?[]`, `unpin?[]` | PanelSpec に merge し、`brief_hash` を再計算する。`gen.*_override` は human だけが設定でき、設定すると自動で pinned になる | page lock。`pinned` は ai:* から守られる |
| `record_review` | `target{page_id, frame_id?}`, `kind: name\|art`, `score?`, `notes`, `input_hash` | エージェントの自己点検の結果を残す（`page.plan.reviews[kind]` か `panel.reviews[kind]`）。worklist は同じ `input_hash` の点検を出し直さない（§7.2） | page lock |

**領域と参照**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `add_region` | `page`, `frame_id`, `region{id, kind: person\|face\|background\|text\|prop\|frame\|other, rect_mm\|poly_mm, char?, source?, confidence?}` | 領域を足す。human が作ると `source:"user"`、ai:* が作ると `source:"agent"` を強制 | page lock |
| `edit_region` / `delete_region` | `page`, `frame_id`, `id`, `set?` | `source:"user"` の領域は human だけが変えられる | page lock |
| `replace_regions` | `page`, `frame_id?`, `source: agent\|detected\|derived`, `regions[]` | その source の領域だけを置き換え、user の領域は残す（再解析が人間の「範囲修正」を消さない） | page lock |
| `bind_ref` / `unbind_ref` / `reorder_refs` | `page`, `frame_id`, `ref{id, source, role: composition\|pose\|character\|background\|style\|mask\|reference, order}` / `id` / `order[]` | 参照素材の役割付き登録。依頼パックがこの順でファイルを並べる | page lock |

**生成依頼と候補**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `open_request` | `request{id, purpose, mode, target, brief_hash, parent?}` | 依頼を記録する（パックのファイル自体は `studio/requests/` に Genko が書く）。`panel.status` を `requested` にし、`attempts.requests` を増やす。同じ id の依頼があれば何もしない | page lock |
| `close_request` | `id`, `reason: done\|cancelled\|superseded` | 依頼を閉じる | page lock |
| `import_candidates` | `request_id`（受け先は依頼の `target`: コマ、キャラ、場所のどれか）, `candidates[{id, asset, mode, parent?, origin{kind, actor, tool_id, tool?, model?, prompt?, params?, refs_used?, note?}, px}]` | 候補を足すだけ。資産は検証・登録済みであること。**`origin` が無ければ拒否。** `origin.actor` は呼び出しの actor と一致すること。依頼の `brief_hash` が今と違えば `stale:true`。受け先がなければ orphans へ。`attempts.images` を更新する | page lock |
| `review_candidates` | `page`, `frame_id`, `reviews[{candidate_id, score, note?, fix?}]` | 評価を記録する（誰の評価かは actor） | page lock |
| `set_candidate` | `page`, `frame_id`, `candidate_id`, `status: candidate\|shortlisted\|rejected`, `reason?` | 候補の状態を変える | page lock |
| `adopt_candidate` | `page`, `frame_id`, `candidate_id`, `to?: art\|bg\|draft`（既定 art）, `fit?: cover\|contain\|stretch`, `offset_mm?: [dx, dy]`, `scale?`, `clip_to?: frame\|bleed\|none` | `(frame_id, to)` ごとの placed layer を作るか使い回し、`asset`・`placement_mm`（mapping から計算）・`source` を設定する。`panel.adopted[to]` を更新し、status を adopted にする。art と bg は INK の下に入れる。**art_ok が付いたページで ai:* が採用を替えると `art_ok` は外れる**（人間の再確認が要る） | **art / bg は name_ok が常に必要**。draft は name_ok 前でも可で非出力。page lock |
| `unadopt` | `page`, `frame_id`, `to?` | 採用を外す。直前の採用があればそれに戻す | page lock |
| `set_placement` | `page`, `frame_id`, `to?`, `fit?`, `offset_mm?`, `scale?`, `clip_to?` | 配置の微調整 | page lock |
| `place_asset` | `page`, `asset`（`"sha256:…"` だけ。ローカルパスは受けない）, `to?: art\|bg\|draft\|name` または `layer_id`, `frame_id?`, `placement_mm?`, `fit?`, `clip_to?` | 人間の持ち込み画像、背景ライブラリ、取り込んだアタリを置く。資産は先に取り込み、`register_assets` で来歴を付けておく。name / draft は非出力を強制。`put_raster` は「全ページに stretch で place_asset」と同じ意味になる | art / bg は strict_gates で name_ok が要る。page lock |
| `request_fix` | `page`, `frame_id`, `candidate_id?`, `instruction`, `scope: frame\|person\|background\|text\|regions`, `region_ids?` | kind=fix のチケットを作り、panel.status を `fix_requested` にする。worklist が `fix_panel` としてエージェントに出す | page lock |

**仕上げ**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_finish` | `page`, `frame_id?`, `finish{mode, black, white, levels, lpi, angle, line_threshold}` \| null | コマ単位でスタイルの仕上げを上書きする（render 時に効く） | page lock |

**画像ツールの情報は op ではない。** 画像ツール（例 `openai:gpt-image-1`）の対応サイズや機能は、原稿ではなく利用者の環境の事実なので、ユーザー設定ディレクトリの `tools.json` に置く（§6.5）。

placed 資産への決定的な画素変換（線抽出で INK ラスタを作る、など）は **op の中では行わない。** `studio derive` がロックの外で新しい資産を作り、`import_candidates{mode:"derive", origin.kind:"genko"}` で候補にしてから採用する（actor は `studio derive` を呼んだ人間かエージェント）。op が資産ファイルを書くことはない。メモリ上の作業ラスタ（`raster_png`）に対する既存の `filter_raster` は今のまま op で行う。

### 5.4 保証がどう保たれるか

- **全か無か・dry_run。** 新しい op は Episode のメモリ上の変更だけなので、既存のループ（ops.py:997-1007）と `ops[i] <op>: msg` 形式がそのまま効く。`_validate` も同じ中止経路を使う。MCP の道具はまず dry_run し、その結果（指摘とプレビュー）を返す。
- **コピーの費用。** 今の apply は undo 履歴ごと deepcopy するので、深さ 50 で 1 op 約 5.5 秒かかる（§0.5 の 1）。エージェントは短い間隔で何度も書き込むので、M1 で undo 履歴を写さず、commit 時の2回目の複製もやめ、ストロークを不変にして共有する（§5.1-7）。予算は 16 ページ・深さ 50 で 1 op 100 ms 未満で、性能試験で守る。
- **undo。**
  - プロセス内の undo（GUI）は今のまま。プロセスをまたぐ場合は journal を使う（§5.1 の 9）。
  - エージェントの作業での「戻す」は、たいてい前向きの op（`unadopt`、履歴から別の候補を採用）で足りる。候補は消えない。
- **ロック。**
  - 新しいページ単位・コマ単位の op はすべて `page` か `page_id` を必須にするので、`_check_page_lock` が効く。
  - エージェントの生成は Genko の外で起き、Genko のロックを持たない。取り込み（`import_images`）とネームの保存だけが短いロック区間を取る（§7.3）。
- **画素境界。** 生成画素は、検証と登録を経た資産として `import_candidates` → `adopt_candidate` の道でしかページに入らない。試験用の画像（`origin.kind:"fixture"`）は書き出しの preflight が拒否する。
- **承認。** ゲートを進める op はすべて human の actor を要る。MCP サーバーの actor は `ai:*` に固定されるので、エージェントが MCP 経由で承認することは構造的にできない。

### 5.5 ネームの段組 DSL（再帰しない）

エージェント側の LLM は様々で、ツール引数に再帰スキーマや数値の範囲（`minimum` / `maximum`）、文字列長の制約を扱えないものがある（Claude の structured outputs もそうである）。そこで入力は平らにし、範囲の検査は Genko が行う。
Genko の Frame 木は再帰的だが、商業漫画のページはほぼ「段（tier）× 段内のコマ」なので、平らな DSL で書ける。

```json
{
  "page": 4,
  "turn_role": "reveal",
  "tiers": [
    {"h": 0.34, "cols": [{"slot": "p1", "w": 0.62}, {"slot": "p2", "w": 0.38}]},
    {"h": 0.66, "cols": [{"slot": "p3", "w": 1.0, "rows": [{"slot": "p3a", "h": 0.5}, {"slot": "p3b", "h": 0.5}]}]}
  ],
  "panels": [
    {"slot": "p1", "shot": "LS", "angle": "high", "location_id": "loc_rooftop", "time": "sunset",
     "characters": [{"id": "sora", "pose": "opening_door", "expression": "nervous", "facing": "left", "pos": "right", "scale": 0.5}],
     "action": "屋上の扉を開ける", "emotion": "緊張", "fx": [], "emphasis": 0.3, "beat_ids": ["b_0012"],
     "lines": []},
    {"slot": "p2", "shot": "MS", "angle": "low", "characters": [{"id": "hina", "pose": "turning_back", "expression": "gentle_smile", "facing": "right", "pos": "center", "scale": 0.8}],
     "action": "振り返る", "emotion": "安堵", "fx": [], "emphasis": 0.6, "beat_ids": ["b_0013"],
     "lines": [{"beat_id": "b_0013", "balloon": "speech", "breaks": ["…やっぱり", "来てくれたんだ"]}]}
  ]
}
```

- 段は上から、段内の `cols` は**右から左**に並べる。コンパイラは段を上から水平分割し、各段を縦分割する。`split_frame` の子は左が a、右が b で、`Page._walk_leaves` は縦分割を `-x` 順（右が先）に読む（models.py:283-284）ので、比は右の列が b になるように計算する。こうすると**列の並び = 読み順**になる。
- `rows` は1つの列をさらに縦に割る3段目（任意）。これより深い入れ子は不可。
- 範囲検査（`0 < h < 1`、各段の `h` の和 ≈ 1、`w` の和 ≈ 1、最小コマ辺）はコンパイラが行う。
- 定型（`3tier_2-1-2`、`splash`、`4koma`、`climax_wide` など）は `src/genko/studio/layouts.json` に置き、`"template": "3tier_2-1-2"` で呼べる。
- 斜めコマ・重なりコマ・枠外への食み出しは今は表せない（§12）。
- **ソースマップ。** コンパイラは `[(op_index, "/tiers/1/cols/0"), (op_index, "/panels/2/characters/0/id"), …]` を返す。dry_run が `ops[7] set_panel: unknown character chr_x` を返したら、Genko がそれを `@ /panels/2/characters/0/id` に直してエージェントに返す。


---

## 6. モジュール構成

### 6.1 ファイル配置

特記のないものは core（stdlib + Pillow）。*(mcp)*、*(app)* は extra が要る。LLM や画像モデルの SDK はどこにも入らない。

```text
src/genko/
  assets.py              # AssetStore: 内容アドレスの put/open/has/verify/gc、ストロークの blob、LRU、tmp + os.replace（Windows 再試行）
  journal.py             # revision、journal.jsonl（正規化 ops）、チェックポイント、undo/redo
  schema.py              # op 登録簿 → OPS_SCHEMA / docs/ops.schema.json / MCP ツールの入力スキーマ
  commit.py              # lock(OS ロック + token) → revision 確認 → load → build ops → dry_run → apply → 差分保存
  validate.py            # _validate(work)（触れた範囲）
  doctor.py              # `genko doctor`: 資産の欠落、relink、パス長、フォント
  fonts/DelaGothicOne-Regular.ttf   # package data（importlib.resources で読む。wheel に入る）
  placement.py           # コマと画像の対応（mapping）、fit、frame/bleed マスク（render と raster._clip が共用）
  guide.py               # コマ単位のガイド画像: composition（ネームの切り抜き）/ pose（人物の箱と向き）/ keepout / mask / compare
  screentone.py          # mono 仕上げ: levels、線マスク、平網の量子化、AM 網点（被覆率が正しい）、ベタ
  lineart.py             # ラスタの線抽出（取り込み画像用。runs_to_strokes の置き換え）
  studio/
    __init__.py
    state.py             # StudioState / PanelSpec / Script / Scene / Beat / Region / Candidate / Request / Approval
    ops.py               # apply_studio_op と STUDIO_OPS（schema.py に登録）
    service.py           # StudioService: MCP・CLI・HTTP・GUI が呼ぶ唯一の窓口（§6.3）
    issues.py            # Issue{code, severity, path, message, hint}
    schemas/*.json       # エージェントの入力スキーマ（bible@1, script@1, name_plan@1, panel_patch@1, import@1, review@1）
    jsonschema_lite.py   # 上のスキーマに必要な範囲だけの検査器（stdlib。外部の jsonschema に依存しない）
    lint.py              # bible lint、脚本 lint、ネーム lint（計画と幾何）
    layout.py            # 段組 DSL → op（apply_layout / M0 では split_frame の列）、定型、ソースマップ
    layouts.json         # 段組の定型
    letter.py            # フキダシの寸法（tategaki）、スロット探索、尾、顔よけ
    blocking.py          # PanelSpec の pos/scale/facing/shot → 人物の箱と頭の箱（mm）
    vocab.json           # shot / angle / 表情 / 感情の語彙（日本語 ↔ 英語 ↔ タグ）= タグ・プロンプト辞書
    genreq.py            # 生成依頼パックの組み立て（サイズ、プロンプトの下書き、ガイドと参照の書き出し）
    importer.py          # 画像の取り込み: inbox のパス / アップロード済み資産 → 検証 → assets → import_candidates
    tools_registry.py    # 画像ツールの登録（ユーザー設定の tools.json）: 対応サイズ、参照・編集・マスクの可否
    worklist.py          # next_actions(episode) → [WorkItem]（純関数。reviews・attempts・チケットを読む）
    claims.py            # 作業項目の短い予約（複数のエージェント・サブエージェント用、§7.4）
    review.py            # review.html（ページのプレビュー、コマの指示、候補、承認コマンド）
    xycut.py             # アタリのコマ検出（M8）
    guide/manga_rules.md # 漫画のルール（段組 DSL、読み順、めくり、字数、shot の語彙）。MCP の resource とスキルが使う
    cli.py               # `genko studio …`
    http.py              # /v1/studio/*、/v1/requests/*、/v1/assets/*（純関数ルーター）
  mcp/
    server.py            # (mcp) MCP サーバー。stdio と HTTP。道具は StudioService の薄い包み
    tools.py             # 道具の名前・説明・入力スキーマ（schema.py の登録簿から作る）
    resources.py         # resource: ルール文書、入力スキーマ、プレビュー画像、review.html
integrations/
  hermes/
    genko-manga/SKILL.md # Hermes のスキル（agentskills.io 形式）。手順、ルール、承認で止まること
    config.example.yaml  # ~/.hermes/config.yaml の mcp_servers の例（stdio と HTTP）
tests/
  agents/                # 筋書きどおりに道具を呼ぶ偽エージェント（MCP クライアントとして動く）
  fixtures/studio/       # bible・脚本・ネーム計画の JSON、取り込み用の画像（origin.kind = fixture）
  golden/                # 段組の矩形、写植の箱、ガイドの統計、72 dpi のページ
  perf/                  # apply・保存・道具の応答時間のベンチマーク（既定で走る。閾値は §10.3）
```

`.gitignore` は `*.png` を `docs/**` 以外で無視しているので、`!tests/golden/**` と `!tests/fixtures/**` を足す。できるだけ PNG ではなく JSON の統計値で golden を持つ。

### 6.2 AssetStore

```python
# src/genko/assets.py
class AssetStore:
    def __init__(self, root: Path, *, lru_items: int = 64) -> None: ...
    def put_bytes(self, data: bytes, *, ext: str) -> str: ...          # "sha256:<hex>"。tmp + os.replace、既存なら何もしない
    def put_image(self, image: Image.Image, *, fmt: str = "PNG") -> str: ...
    def put_json(self, obj: dict) -> str: ...                          # 正準 JSON（sort_keys, ensure_ascii=False）
    def has(self, ref: str) -> bool: ...
    def path(self, ref: str) -> Path: ...                              # assets/ab/abcdef….png
    def open_bytes(self, ref: str) -> bytes: ...
    def open_image(self, ref: str) -> Image.Image: ...                 # LRU キャッシュ
    def verify_image(self, data: bytes, *, max_pixels: int = 64_000_000) -> tuple[int, int]: ...
    def gc(self, referenced: set[str], *, keep_days: int = 30, dry_run: bool = True) -> list[str]: ...
    def __deepcopy__(self, memo: dict) -> "AssetStore":
        return self
```

- `load_episode` / `save_episode` が `episode.assets = AssetStore(root / "assets")` を設定する。
- `render_page(page, dpi, mode, episode)` は既存の `episode` 引数から `episode.assets` を使う。プロセス全体のグローバル状態は持たない。
- `episode` なしで placed layer を描こうとしたら、proof では灰色の仮表示、print では `RenderError` にする。
- `put_strokes(layer) -> str` / `open_strokes(ref) -> list[Stroke]`: ストロークの正準 JSON を blob にする（v3）。
- **GC（`genko gc`）は `project.lock` を取って走る。** 「参照されている」とみなすもの: project.json、保持期間内のチェックポイントとそこから journal で辿れる資産、候補、発行中の生成依頼パック（`studio/requests/`）と取り込み待ち（`studio/inbox/`）の画像、`studio/drafts/` の成果物。ロックの外で書かれ、まだどこからも参照されていない新しい資産は `keep_days` の間は消さない。


### 6.3 StudioService（道具の実装）

MCP・CLI・HTTP・GUI は同じ `StudioService` を呼ぶ。道具の中身はここにだけ書き、MCP サーバーは薄い包みにする。

```python
# src/genko/studio/service.py（core）
@dataclass(frozen=True)
class ToolResult:
    ok: bool
    data: dict                                   # 小さな JSON（数 KB）
    issues: list[Issue] = ()                     # {code, severity, path, message, hint}
    images: tuple[ImageOut, ...] = ()            # LLM に見せるプレビュー（長辺 1024 px まで、最大 2 枚）
    files: tuple[FileRef, ...] = ()              # 同じマシン: 絶対パス / 別マシン: ダウンロード URL
    revision: int | None = None

class StudioService:
    def __init__(self, root: Path, actor: str, *, transport: Literal["local", "remote"]) -> None: ...

    # 状態
    def projects(self) -> ToolResult: ...
    def create_project(self, name: str, *, title: str, pages: int, spec_preset: str, binding: str = "right") -> ToolResult: ...
    # policy は `studio` の既定（要所で人間が承認）が入る
    def status(self, project: str) -> ToolResult: ...
    def next(self, project: str, *, role: str | None = None, limit: int = 5, claim: bool = False) -> ToolResult: ...
    def inspect(self, project: str, *, target: dict) -> ToolResult: ...
    def render(self, project: str, *, page: int, frame_id: str | None = None,
               kind: str = "name", max_px: int = 1024) -> ToolResult: ...

    # 企画・脚本・ネーム（既定は dry_run。commit=True で保存）
    def set_bible(self, project: str, bible: dict, *, commit: bool = False) -> ToolResult: ...
    def set_script(self, project: str, script: dict, *, commit: bool = False) -> ToolResult: ...
    def submit_name(self, project: str, page: int, plan: dict, *, commit: bool = False) -> ToolResult: ...
    def edit_panel(self, project: str, page: int, frame_id: str, patch: dict, *, commit: bool = False) -> ToolResult: ...
    def apply_ops(self, project: str, ops: list[dict], *, commit: bool = False) -> ToolResult: ...
    def record_review(self, project: str, target: dict, kind: str, score: float | None, notes: str) -> ToolResult: ...

    # 絵
    def generation_request(self, project: str, *, page: int | None = None, frame_id: str | None = None,
                           character_id: str | None = None, location_id: str | None = None, purpose: str = "panel_art",
                           mode: str = "new", parent: str | None = None, tool: str | None = None,
                           instruction: str | None = None) -> ToolResult: ...
    def import_images(self, project: str, request_id: str, images: list[dict]) -> ToolResult: ...
    def candidates(self, project: str, page: int, frame_id: str) -> ToolResult: ...
    def review_candidates(self, project: str, page: int, frame_id: str, reviews: list[dict]) -> ToolResult: ...
    def adopt(self, project: str, page: int, frame_id: str, candidate_id: str, **placement) -> ToolResult: ...
    def report_regions(self, project: str, page: int, frame_id: str, regions: list[dict]) -> ToolResult: ...
    def request_fix(self, project: str, page: int, frame_id: str, instruction: str, *,
                    scope: str = "frame", candidate_id: str | None = None) -> ToolResult: ...

    # 仕上げ・書き出し・人間とのやり取り
    def finish_page(self, project: str, page: int, *, commit: bool = False) -> ToolResult: ...
    def preflight(self, project: str) -> ToolResult: ...
    def export_proof(self, project: str, *, format: str = "pdf") -> ToolResult: ...
    def request_approval(self, project: str, gate: str, *, pages: list[int] | None = None,
                         character_id: str | None = None, note: str = "") -> ToolResult: ...
    def tickets(self, project: str, *, status: str = "open") -> ToolResult: ...
```

- **`project` は `--root` からの相対名**（例 `summer.genko`）。絶対パスや `..` は受けない。
- **書き込む道具は `commit=False` が既定。** dry_run の結果（指摘、差分の要約、プレビュー）を返す。エージェントは指摘が無くなってから `commit=True` で呼ぶ。`commit=True` でもエラーがあれば保存しない。
- **`issues` の `path`** は、その道具の入力の JSON ポインタである（`submit_name` なら計画の中の位置）。op のエラー（`ops[7] set_panel: …`）もソースマップで入力の位置に直してから返す。`hint` は直し方の短い説明（例「この段の cols の w の合計が 1.08。1.0 になるように直す」）。
- **`images` は LLM に見せる用**で、縮小した PNG を最大2枚。**`files` は画像ツールに渡す用**で、原寸のファイル。同じマシンでは絶対パス、別マシンではトークン付きのダウンロード URL（§8.1）。
- 人間用の操作（`approve`、`revoke`、本番の `export`、policy の変更）は StudioService の別の面（`HumanService`）に置き、actor が `human:*` のときだけ作れる。MCP サーバーは `HumanService` を持たない。

### 6.4 MCP サーバー（`genko mcp`）

- **実装。** extra `mcp` の公式 Python SDK を使う（現行は 2.x。実装時に API を確認する）。道具の名前・説明・入力スキーマは `schema.py` の登録簿から作り、手で二重に書かない。依存が重荷になったら stdlib の JSON-RPC に替えられるよう、StudioService とは薄い包みでつなぐ。
- **起動と actor。**
  - 同じマシン（stdio）: `genko mcp --root D:/manga --agent ai:hermes`。Hermes がサブプロセスとして起動する。
  - 別マシン（HTTP）: `genko mcp --root /srv/manga --http --host 0.0.0.0 --port 8766 --allow-host genko.local`。トークン（§8.4）が必須で、actor はトークンから決まる（例 `ai:hermes`）。TLS は前段のリバースプロキシか SSH トンネルで付ける。
  - どちらでも actor は起動時に固定され、道具の引数で変えられない。`human:*` のトークンで MCP を起動することは拒否する（エージェントに人間の権限を渡さない）。
- **道具の名前は短くする。** Hermes は MCP の道具を `mcp_<サーバー名>_<道具名>` という名前で登録する。サーバー名を `genko` にすれば、道具は `mcp_genko_status`、`mcp_genko_submit_name` のようになる。
- **応答。** MCP の結果は、テキスト（`data` と `issues` の JSON）、画像コンテンツ（`images`）、ファイルの場所（`files`）の順で返す。Hermes の文書では、MCP の道具が返した画像をモデルがそのまま見られるかが確認できなかったので、画像は必ずファイル（同じマシン）か URL（別マシン）としても返し、エージェントが自分の画像認識の道具で開けるようにする。
- **resource と prompt。** ルール文書（`genko://guide/manga-rules`）、入力スキーマ（`genko://schema/name_plan` など）、プロジェクトのプレビュー（`genko://project/{name}/page/{n}.png`）を resource として出す。Hermes は、サーバーが resource と prompt に対応していればそれを読む道具を自動で足す。
- **時間の上限。** 道具の応答は 2 秒以内を目標にする（§2.2-17）。重い書き出し（本番の 600 dpi）は MCP に出さない（人間の CLI で行う）。`export_proof` は 150 dpi の校正用だけで、数ページなら数秒で終わる。Hermes 側の設定で `timeout` を 60 秒程度にしておく（§8.2 の設定例）。
- **エージェントの入力の上限。** 1 回の入力は 256 KB まで、ops は 1 回 500 個まで、取り込む画像は 1 枚 30 MB・64 MP まで。超えたら理由付きで拒否する。

### 6.5 画像ツールの登録（`tools.json`）

Genko は画像ツールを呼ばないが、依頼パックのサイズの候補を出すために、エージェントが使う画像ツールの性質を知っておくとよい。ユーザー設定ディレクトリの `tools.json` に置く（原稿ではなく利用者の環境の事実なので、project.json には入れない）。

```json
{
  "openai:gpt-image-1": {
    "label": "ChatGPT / OpenAI の画像生成",
    "sizes_px": [[1024, 1024], [1536, 1024], [1024, 1536]],
    "supports": {"references": true, "edit": true, "mask": true, "seed": false},
    "notes": "サイズと機能は版で変わる。確かめて直す"
  }
}
```

- 値は例である。実際のサイズや機能は提供元と版で変わるので、利用者かエージェントが確かめて直す（`genko studio tools set`）。
- 登録の無いツールの画像も取り込める。その場合、サイズの候補は一般的な正方形・横長・縦長を出す。
- エージェントは `generation_request{tool: "openai:gpt-image-1"}` のように使うツールを伝えられる。依頼パックはそのツールの対応サイズで `tool_sizes` を作り、参照やマスクを渡せないツールなら、その旨を `notes_for_agent` に書く。

### 6.6 pyproject の extras

```toml
[project]
dependencies = ["pillow>=10.0"]                  # 変えない

[project.optional-dependencies]
app = ["pyside6>=6.6"]
dev = ["pytest>=8.0"]
mcp = ["mcp>=2,<3"]                              # 公式 MCP Python SDK（実装時に版を確認）

[tool.hatch.build.targets.wheel]
packages = ["src/genko"]                         # 同梱フォントは src/genko/fonts/ に移し、package data として wheel に入れる
```

`genko mcp` は `mcp` を遅延 import する（`app` の PySide6 と同じ流儀、__main__.py:161-164）。入っていなければ `{ok:false, error:"install genko-studio[mcp]"}` を返す。LLM や画像モデルの extra は置かない。


---

## 7. オーケストレーション（誰が工程を進めるか）

### 7.1 方式: エージェントのループ + Genko の worklist

| 役割 | 担当 | 理由 |
|---|---|---|
| 工程を進める（次に何をするか選び、道具を呼ぶ） | エージェント（Hermes Agent のループ） | 利用者の決定。LLM とツールの選び方、並列化（サブエージェント）、人間への連絡はエージェント側の機能を使う |
| 次にやるべきことの計算 | Genko の worklist（状態の純関数） | エージェントの記憶や会話履歴に頼らない。エージェントを入れ替えても、途中で止まっても、同じ答えが出る |
| 検査・計算・保存・描画 | Genko | 決定的で試験できる。どの LLM が書いた計画でも同じ基準で検査する |
| 手順とルールの説明 | スキル（`SKILL.md`）と MCP resource（`manga-rules`） | エージェントが毎回手順を推測しない |
| 承認 | 人間 | §3.4 |

エージェントの基本のループ（スキルに書く）:

```text
1. status でプロジェクトの様子を見る
2. next で次の作業を1つ取る（claim で予約）
3. 作業の種類に応じて道具を呼ぶ
     - 書く作業: inspect で入力を取る → LLM で書く → 道具を dry_run（commit=false）で呼ぶ
       → issues があれば path の場所を直して再送（最大 3 回）→ issues が無ければ commit=true
     - 見る作業: render でプレビューを見る → 直す or record_review で結果を残す
     - 絵の作業: generation_request → 画像ツールで生成 → inbox に置く → import_images
       → render{kind:"compare"} で比べる → review_candidates → adopt か修正の依頼
4. 承認が要るところまで来たら request_approval を出し、人間に知らせる（Hermes のメッセージ連携など）
5. next が await_human だけになったら止まる。人間の承認や修正指示が入ったら 1 に戻る
```

- 3 回直しても指摘が消えない作業は、エージェントが `request_fix` か kind=review のチケットで人間に回す（スキルに書く）。worklist は、未解決のチケットがある対象の項目を `blocked_by` 付きで出すので、同じ失敗を繰り返さない。
- ループの回し方（1 回に何ページ進めるか、サブエージェントに分けるか）はエージェントの自由である。Genko は順序を強制しない。強制するのはゲートと検査だけである。

### 7.2 worklist（`next`）

`worklist.next_actions(episode) -> list[WorkItem]` は状態の**純関数**である。保存する「完了印」はない。状態が規則を満たせば項目は消える。

```json
{
  "id": "wi_3f9a51c0",
  "kind": "gen_panel",
  "target": {"page_id": "pg_7c1e9a0b2d41", "page": 4, "frame_id": "f4_p1", "label": "4-1"},
  "why": "ネーム承認済み、指示あり、採用が無い、未処理の依頼が無い",
  "tools": ["generation_request", "import_images", "render", "review_candidates", "adopt"],
  "blocked_by": [],
  "priority": 30
}
```

- `id = hash(kind, target, 関係する入力の hash)` なので再起動しても同じになる。
- `tools` は、この作業で呼ぶ道具の順の目安。エージェントはこれを見て道具を選べる。
- **項目の種類（上から順に評価する）:**

  ```text
  write_bible → [gate bible] → write_character_look(char) → make_sheet(char) → [gate sheet(char)]
  → write_script → [gate script] → plan_page(page) → fix_page(page) → review_name(page) → [gate name(page)]
  → gen_panel(frame) → import_pending(request) → review_candidates(frame) → fix_panel(frame)
  → report_regions(frame) → [gate art(page)] → upscale_panel(frame) → finish_page(page) → [gate export]
  ```

  - `fix_page`: ネーム lint のエラーが残っているページ。
  - `review_name`: 今の内容（`input_hash`）で `record_review{kind:"name"}` がまだ無いページ。
  - `import_pending`: `studio/inbox/<id>/` にファイルが置かれているのに、まだ取り込んでいない依頼。
  - `report_regions`: 採用画像の顔と人物の箱が無いコマ（写植の顔よけに使う、§9.4）。
  - `upscale_panel`: 実効 dpi が閾値に届かない採用画像。
- **持続記録で純関数を保つ。**
  - エージェントの自己点検は `record_review`（`input_hash` 付き）で残る。同じ入力の点検は出し直さない。入力（ページの内容、PanelSpec）が変われば hash が変わり、項目が戻る。
  - 絵の作業は `panel.attempts {requests, images, fix_rounds}` で残る。`policy.limits` を超えたコマは `gen_panel` を出さず、`ticket:limit` で人間に回す。
- 次の場合、項目は `blocked_by` 付きで出る（エージェントは飛ばして次へ進む）。
  - 人間がロックしたページ: `locked:human:leaf`
  - ゲート待ち: `await_human:name`（承認依頼が出ていれば `requested:tk_31a0`）
  - 人間向けの未解決チケット: `ticket:tk_31a0`
  - 他のエージェントが予約中: `claimed:ai:hermes-2`
- `next` の応答が `await_human:*` だけになったら、エージェントは作業を止めてよい。応答には `{"waiting_for": [{"gate": "name", "pages": [3, 4]}, …]}` が付く。

### 7.3 コミット手順（`commit.py`、全経路共通）

```python
def commit(project: Path, build_ops: Callable[[Episode], tuple[list[dict], list]], *,
           agent: str, expect_revision: int | None = None, retries: int = 3) -> dict:
    for attempt in range(retries):
        with ProjectLock(project, agent=agent):                 # OS ロック + token、短時間
            if expect_revision is not None:
                found = read_revision(project)                  # project.json の先頭だけを読む（v3 は小さい）
                if found != expect_revision:
                    raise RevisionConflict(expect_revision, found)
            episode = load_episode(project, lazy_rasters=True)  # ロックの中でロード。ラスタは触れたときに読む
            ops, srcmap = build_ops(episode)                    # 純関数。呼び出し側 id で冪等
            apply_ops(episode, ops, dry_run=True, agent=agent)  # 失敗 → ApplyError（srcmap で翻訳）
            result = apply_ops(episode, ops, agent=agent)
            save_episode(episode, project, only_changed=True)   # revision += 1、journal 追記、変わった資産だけ書く
            return result
    raise RevisionConflict(...)
```

- **MCP の取り込み系の道具（ネーム計画・候補）:** `expect_revision` を付けない。ロックの中で最新の状態から ops を作り直す。ops は冪等で、すでに入った候補 id は飛ばす。前提（frame があるか、`brief_hash`）もその場で確かめる。
- **CLI / HTTP の apply:** `--expect-revision` / `expect_revision` は任意。
- ロックの保持は数十〜数百ミリ秒で、画像の検証、資産の書き込み、プレビューの描画はロックの外で行う。エージェント側の生成は、そもそも Genko の外で起きる。
- **GUI は1 op ごとに commit しない（M7）。**
  - GUI はメモリ上に正本のセッション（`Episode`）を持ち、すべての変更を `apply_ops` で**メモリ上に**適用する（筆の1本も op）。画面への反映はディスクを待たない。
  - ディスクへの commit は裏のスレッドでまとめて行う。筆を離して 1 秒操作が無いとき、ページを切り替えたとき、承認の前に、溜まった ops を1回の commit にする。
  - commit は、セッションが読んだ revision を `expect_revision` にして `read_revision` で安く確かめる。一致すれば、変わった資産だけを書く差分保存で済む（全ラスタを書き直す今の保存、§0.5 の 25 はやめる）。
  - 衝突したら（AI が裏で commit した）、最新を読み直し、溜まっていたローカルの ops を再生（rebase）する。再生できない op は人間に見せて選ばせる。
  - **遅延の予算:** 筆の画面反映 16 ms（ディスクなし）、裏の commit 200 ms（16 ページ、差分保存）、衝突時の再読込と再生 1 秒。`tests/perf/` に GUI 相当の op 列（筆 200 本 + 台詞の移動）のベンチマークを置く。
- Windows では project.json の `os.replace` が、GUI の監視、ウイルス対策、OneDrive などのせいで `PermissionError` になることがある。間隔を広げて最大 2 秒まで再試行し、なお失敗すればロックを離して `{ok:false, error:"project.json busy"}` を返す（Windows CI で試験する）。


### 7.4 複数のエージェントと人間の並行作業

- **予約（claim）。** Hermes はサブエージェントを立てて並列に作業できる。同じコマやページを2つが取り合わないように、`next{claim:true}` は返した項目に短い予約（lease、既定 10 分、道具を呼ぶたびに延長）を付ける。予約は `studio/claims/<item>.json` を `O_EXCL` で作る。期限切れの予約は無効で、別のエージェントが取れる。予約は書き込みを止めない（弱い予約）。本当に止めたいときは page lock を使う。
- **page lock。** 人間が作業中のページは `lock_page`（人間）で守る。エージェントはそのページに書けず、`next` もそのページの項目を `blocked_by` 付きで出す。エージェントは人間のロックを外せない（§5.1-3）。
- **revision。** 取り込みとネームの保存はロックの中で最新の状態を読み直してから行う（§7.3）。GUI の保存は `expect_revision` で衝突を検出し、読み直して自分の op を再生する（M7）。

### 7.5 再開（resume）

- **状態はファイルにしかない。** worklist は再計算する。依頼パック、取り込み待ちの画像、予約はファイル。資産は内容アドレスで不変。
- エージェントが途中で止まっても、次に `next` を呼べば続きの作業が出る。
- **依頼パックは消えない。** エージェントが生成の途中で止まっても、`studio/requests/<id>/` は残り、`inbox/<id>/` に置いた画像は `import_pending` として出る。同じ依頼を作り直しても id は同じ（内容の hash）なので重複しない。
- **取り込みは冪等。** 同じ画像（同じ sha256）を同じ依頼に2回取り込んでも、候補は1つ。

### 7.6 自律度（`studio.policy.autonomy`）

| 値 | 意味 |
|---|---|
| `gated`（既定） | エージェントはゲートの内側では自由にコミットする（ネームの保存、候補の取り込み、採用）。ゲートは人間 |
| `assist` | エージェントの書き込みはすべて提案（`studio/proposals/prop_….json` = ops + プレビュー + `base_rev`）になる。人間の `studio accept PROP` が今の revision で dry_run し直し、`human:*` としてコミットする |

プリセット（`studio` / `quick`、§3.4）は gates と autonomy の組をまとめて設定する。

---

## 8. インターフェース

### 8.1 MCP の道具

MCP サーバー名は `genko`。Hermes からは `mcp_genko_<道具名>` として見える。すべての道具は `project`（`--root` からの相対名）を取る（`projects` と `create_project` を除く）。

| 道具 | 入力（主なもの） | 返すもの | 書き込み |
|---|---|---|---|
| `projects` | – | `--root` 配下のプロジェクト（名前、題名、ページ数、工程） | なし |
| `create_project` | `name`, `title`, `pages`, `spec_preset`（判型: `commercial-b4`（B4 商業誌・投稿、仕上がり 220×310・基本枠 180×270）、`doujin-b5`、`doujin-a5`、`a4-mono`、`webtoon`）, `binding`。policy は `studio` の既定 | 作ったプロジェクト | あり |
| `status` | – | 工程ごとの件数、承認待ち、承認依頼の状態、次の作業の件数 | なし |
| `next` | `role?`, `limit?`, `claim?` | worklist の項目（`tools` の目安付き）、`waiting_for` | 予約だけ |
| `inspect` | `target`（`bible` / `script` / `page` / `panel` / `candidate` / `request`） | 対象の JSON（小さく） | なし |
| `render` | `page`, `frame_id?`, `kind`（`name` / `proof` / `crop` / `compare` / `guide:pose` / `guide:keepout`）, `max_px?` | プレビュー画像 + ファイル | なし |
| `set_bible` | `bible`（bible@1）, `commit` | 指摘、差分の要約 | `commit:true` のとき |
| `set_script` | `script`（script@1）, `commit` | 指摘、ページごとの beat 数 | 同上 |
| `submit_name` | `page`, `plan`（name_plan@1）, `commit` | 指摘（JSON ポインタ）、読み順の要約、name のプレビュー画像 | 同上 |
| `edit_panel` | `page`, `frame_id`, `patch`（panel_patch@1）, `commit` | 指摘、新しい `brief_hash` | 同上 |
| `apply_ops` | `ops`, `commit` | 既存の apply の結果（細かい修正用。台詞の移動、コマの分割など） | 同上 |
| `record_review` | `target`, `kind`, `score?`, `notes` | 記録した点検 | あり |
| `report_regions` | `page`, `frame_id`, `regions[]`（顔・人物の箱。画像の 0..1 座標か mm） | 記録した領域、写植の移動の提案 | あり（`source:"agent"`） |
| `generation_request` | `page`+`frame_id` か `character_id` か `location_id`、`purpose`, `mode`, `parent?`, `tool?`, `instruction?` | 依頼パック（§4.5）、ファイルの場所、ガイドの縮小プレビュー | 依頼の記録 |
| `import_images` | `request_id`, `images[{file? \| asset?, origin{tool_id, tool?, model?, prompt?, params?, refs_used?, note?}}]` | 候補の id、比較のプレビュー | あり |
| `candidates` | `page`+`frame_id` か `character_id` か `location_id` | 候補の一覧（点数、状態、来歴の要約）と縮小の並べ画像 | なし |
| `review_candidates` | `page`, `frame_id`, `reviews[]` | 記録した評価 | あり |
| `adopt` | `page`, `frame_id`, `candidate_id`, `to?`（`art` / `bg` / `draft`）, `fit?`, `offset_mm?`, `scale?` | 配置後のコマのプレビュー | あり |
| `request_fix` | `page`, `frame_id`, `candidate_id?`, `instruction`, `scope` | 修正のチケット | あり |
| `finish_page` | `page`, `commit` | 仕上げの提案（写植の移動、効果）とプレビュー | 同上 |
| `preflight` | – | 書き出しを止めている理由の一覧 | なし |
| `export_proof` | `format`（`pdf` / `png`） | 校正用ファイルの場所（透かし入り、150 dpi） | ファイルだけ |
| `request_approval` | `gate`, `pages?`, `character_id?`, `note` | 承認依頼のチケットと review.html の場所 | あり |
| `propose_lines`（M8） | `page`, `lines[]`（手書き台詞の読み取り結果と位置） | dry_run の重ね表示 | 提案だけ |
| `tickets` | `status?` | チケットの一覧（人間からの修正指示と返事を含む） | なし |

**出さない道具:** `approve`、`revoke`、本番の `export`、policy の変更、`unlock_page`（人間のロックを外す）。エージェントに人間の権限を持たせないためである。

**画像の受け渡し。**

- **同じマシン（stdio）。** `generation_request` はファイルの絶対パスを返す。エージェントは生成した画像を `studio/inbox/<request_id>/` に保存し、`import_images{images:[{file:"studio/inbox/rq_…/a.png", origin:{…}}]}` を呼ぶ。`file` は `inbox/` の中だけを受ける（他の場所は読まない）。
- **別マシン（HTTP）。** `generation_request` はダウンロード URL（`GET /v1/requests/{id}/files/{name}`、Bearer トークン）を返す。エージェントは端末の道具（`curl` など）で参照ファイルを取り、生成した画像を `POST /v1/assets`（本文は画像そのもの、`Content-Type: image/png`）でアップロードして `sha256` を受け取り、`import_images{images:[{asset:"sha256:…", origin:{…}}]}` を呼ぶ。
- どちらの場合も、画像のバイト列をツールの引数に書かせない（§2.2-8）。
- エージェントの画像ツールが URL を返す場合でも、Genko はその URL を取りに行かない（Genko は外部と通信しない）。エージェントが自分で保存してから渡す。

### 8.2 Hermes Agent との接続

**設定例（`~/.hermes/config.yaml`）。** Genko は `integrations/hermes/config.example.yaml` に同じものを置く。

```yaml
mcp_servers:
  # 同じマシン: Hermes が genko をサブプロセスとして起動する
  genko:
    command: "genko"
    args: ["mcp", "--root", "/home/leaf/manga", "--agent", "ai:hermes"]
    timeout: 60
    connect_timeout: 20
    tools:
      exclude: []            # 役ごとに絞るなら include / exclude を使う

  # 別マシン: genko を HTTP で起動しておき、トークンで接続する
  # genko:
  #   url: "https://genko.local/mcp"
  #   headers:
  #     Authorization: "Bearer ${GENKO_TOKEN}"
  #   timeout: 60
```

- Hermes は stdio のサーバーには、明示した `env` と最小限の環境変数だけを渡す。Genko は外部サービスの鍵を必要としないので、`env` は空でよい。
- 道具が多すぎると LLM が選び間違えるので、サブエージェントごとに `tools.include` で絞れる。例: ネーム担当は `status, next, inspect, render, set_script, submit_name, edit_panel, record_review, request_approval`、作画担当は `next, inspect, render, generation_request, import_images, candidates, review_candidates, adopt, request_fix, report_regions`。
- 設定の書き方（キー名、タイムアウトの意味）は Hermes の版で変わりうる。実装時に Hermes の MCP の文書で確認し、例を直す。

**スキル（`integrations/hermes/genko-manga/SKILL.md`）。** Hermes のスキルは agentskills.io 形式と互換なので、その形で書く。中身:

- 目的と前提（Genko は検査と計算をする道具で、文章と絵はエージェントが作る）
- §7.1 のループと、作業の種類ごとの手順（道具の呼び順、dry_run → 直す → commit）
- ネームのルール（段組 DSL、列は右から左、読み順、めくり、40 字、shot の語彙）。`genko://guide/manga-rules` と同じ内容
- 画像生成のルール（依頼パックを使う、文字を描かせない、グレースケール、参照画像を渡す、実際に使ったプロンプトを来歴に書く、画像を inbox に置いてから取り込む）
- 止まるところ（承認が要るゲート、3 回直しても消えない指摘、上限を超えたコマ）と、人間への知らせ方
- してはいけないこと（人間のロックに触れない、承認を代わりに付けようとしない、画像を base64 で引数に書かない）

スキルはエージェントが自分で書き換えることがある（Hermes は学んだ手順をスキルとして保存する）。Genko 側の正本はリポジトリの `SKILL.md` で、MCP の resource としても同じ文書を出すので、エージェントはいつでも最新の規則を読み直せる。

### 8.3 CLI

出力の規約は今のまま: stdout に JSON を1つ（`ok` 付き）、人向けのログは stderr。stdout は常に UTF-8 で書く（`sys.stdout.reconfigure(encoding="utf-8")`）。Windows の cp932 コンソールで表示したい場合は `--ascii`（`ensure_ascii=True`）を使う。

エージェント向けの道具は、MCP と同じものを `genko studio <道具名>`（`_` を `-` にした名前。例 `submit-name`、`generation-request`、`import-images`）でも呼べる（MCP の無い環境や、試験、デバッグのため）。人間向けのコマンドは CLI と GUI にしかない。

```bash
# MCP サーバー
genko mcp --root DIR --agent ai:hermes                                  # stdio
genko mcp --root DIR --http --host 0.0.0.0 --port 8766 --allow-host NAME # 別マシン用（トークン必須）

# エージェント向けの道具（MCP と同じ。入力は JSON ファイルか標準入力）
genko studio status  demo.genko
genko studio next    demo.genko [--limit 5]
genko studio submit-name demo.genko --page 4 plan.json [--commit]
genko studio generation-request demo.genko --page 4 --frame f4_p1 [--mode edit --parent cd_01]
genko studio import-images demo.genko --request rq_3f9a51c0 a.png b.png --origin origin.json
genko studio derive  demo.genko --page 4 --frame f4_p1 --kind lineart

# 人間向け（actor は human:*）
genko studio init    demo.genko --title 夏の午後の約束 --pages 16 --b4 --preset studio
genko studio review  demo.genko --out review.html                        # 確認画面（プレビュー、指示、候補、承認コマンド）
genko studio approve demo.genko name --page 4 --as human:leaf
genko studio approve demo.genko sheet --character hina --candidate cd_02 --as human:leaf
genko studio revoke  demo.genko name --page 4 --reason "3コマ目を割り直す" --as human:leaf
genko studio fix     demo.genko --page 4 --frame f4_p1 --instruction "顔をもう少し右向きに" --as human:leaf
genko studio accept  demo.genko prop_1a2b --as human:leaf                # assist の提案を受理
genko studio tools set openai:gpt-image-1 --sizes 1024x1024,1536x1024,1024x1536
genko studio import-name demo.genko ./scans/*.png                        # アタリ取り込み（M8）
genko studio adopt-drafts demo.genko                                     # M0 のサイドカーを op で取り込む（M3）

# 既存コマンドの拡張
genko apply   demo.genko ops.json --agent human:leaf [--expect-revision 128] [--dry-run]
genko undo    demo.genko [--as human:leaf] [--force] ;  genko redo demo.genko
genko render  demo.genko --page 4 --frame f4_p1 --kind crop|compare|guide:pose --out x.png
genko inspect demo.genko [--panel 4:f4_p1 | --candidate cd_05 | --script | --bible | --request rq_…]
genko export  demo.genko ./out --format png|tiff|pdf|psd|epub|pack [--dpi N] [--force] [--allow-fixture]   # 本番。studio は preflight と export 承認を通す
genko doctor  demo.genko [--relink DIR]
genko gc      demo.genko [--legacy] [--archive-candidates] [--dry-run]
genko serve   --root DIR [--port 8765] [--allow-origin URL]              # HTTP API（トークン必須）
genko token add --actor ai:hermes ;  genko token revoke TOKEN_ID          # トークンの発行と失効（人間が行う）
```

`studio review` は GUI（M7）ができるまでの確認画面である。静的な HTML に、ページのプレビュー、コマの指示、候補と来歴、指摘、承認依頼、コピーできる承認コマンドを並べる。

### 8.4 HTTP

`handle_request(method, path, body)`（server.py:39）に `ctx=None` を足し、純関数のまま保つ。`ctx` は `{root, actor, headers}` で、サーバーの `_Handler` が認証と検査の後に作って渡す。`/v1/studio/` で始まる経路は1つの分岐から `genko.studio.http.handle(method, route, query, data, ctx)` に渡す。MCP の HTTP 接続（`genko mcp --http`）も、認証と検査に同じ関数を使う。

| メソッド / 経路 | 内容 |
|---|---|
| `GET /health` | 認証なしでは `{ok:true}` だけ |
| `POST /v1/studio/{道具名}` | MCP の道具と同じ（`{project, …}`）。エージェントのトークンで呼べる |
| `POST /v1/studio/approve` ほか人間向け | human のトークンだけ |
| `GET /v1/requests/{id}/files/{name}?project=` | 依頼パックのファイル（参照画像、ガイド） |
| `POST /v1/assets?project=` | 画像のアップロード（本文は画像そのもの。形式と大きさに上限）→ `{asset:"sha256:…"}` |
| `GET /v1/assets/{sha256}.png?project=` / `GET /v1/pages/{n}.png?project=&mode=` / `GET /v1/pages/{n}/frames/{id}.png?project=&kind=` | 画像の取得 |
| `POST /v1/apply` | 既存の形 `{ok, applied, snapshot, job_id}` を保つ。`expect_revision` を足し、agent はトークンから決める |
| `POST /v1/export` | 修正: `format` と `dpi`（既定 `spec.dpi`）を尊重する（今は PNG 連番しか出さない）。本番は human のトークンだけ |


**安全（M1。既存の経路も含めて全部に、最初から入れる）:**

今のサーバーは、利用者が開いた任意の Web ページから書き込みと読み出しができる（§0.5 の 21）。AI 機能で扱うもの（未発表の脚本、キャラ、生成画像）が増えるので、互換のための猶予は置かない。

- **トークン。** `/health` の最小応答と `/openapi.json` 以外のすべての経路で `Authorization: Bearer <token>` を要求する（無ければ 401）。
  - トークンは `genko serve` の初回起動時に生成し、ユーザー設定ディレクトリの `tokens.json`（`token → actor`）に置く。project.json には置かない。
  - 人間用とエージェント用を分けて発行し、actor はトークンから決まる。
- **Origin の検査。** `Origin` ヘッダがあり、`--allow-origin` で許した値でなければ 403。ブラウザはオリジンをまたぐ POST に `Origin` を付けるので、これで CSRF を止める。
- **Host の検査。** `Host` が `127.0.0.1:<port>`、`localhost:<port>`、`[::1]:<port>` のどれでもなければ 421。DNS rebinding を止める。 `--host` で公開したときは `--allow-host` で許した名前も受ける。
- **本文は JSON だけ。** 本文のある要求は `Content-Type: application/json` でなければ 415。preflight の要らない `text/plain` の POST を受けない。
- **CORS。** `Access-Control-Allow-Origin: *` をやめる（server.py:146, 154）。既定では CORS ヘッダを出さず、`OPTIONS` は 403。`--allow-origin` を指定したときだけ、その origin を返す。
- **`--root`（必須）。** `HeadlessServer(root=…, tokens=…)` の明示の引数にする（cwd を既定にしない）。本文と query の `path`、`dest`、`out`、`put_raster` の `path` は、実パス（シンボリックリンクを解決した後）が root の下になければ 403。
- 既定で `127.0.0.1` に bind する（今と同じ）。別マシンのエージェントから使うときだけ `--host` で明示し（§6.4）、TLS は前段のリバースプロキシか SSH トンネルで付ける。
- **変える既存テスト:** `tests/test_server.py` は `HeadlessServer(root=tmp_path, tokens={"t": "human:test"})` で起動し、要求に `Authorization` を付ける。`tests/test_p4.py:110` の直接呼び出しは `ctx` なしのまま通る。
- **足す試験:** `text/plain` の POST が 415、`Origin: https://evil.example` が 403、`Host: evil.example` が 421、応答に `Access-Control-Allow-Origin` が無い、トークン無しが 401、root の外のパスが 403、シンボリックリンクで root の外へ出るパスが 403。


### 8.5 GUI（人間の確認と承認）

GUI は人間が見て、直して、承認するための画面である。エージェントの作業を GUI から起動することはしない（エージェントは Hermes 側で動く）。

- **メモリ上の正本のセッション。** 変更はすべて `MainWindow._apply`（main.py:180-187）→ `apply_ops`（`human:<user>`）でメモリ上のセッションに適用する。ディスクへの commit は `commit.py` で裏のスレッドがまとめて行う（§7.3）。エージェントの書き込みは `QFileSystemWatcher` で検知し、再読込して未 commit のローカル op を再生する。
- 既存の抜け道（main.py:371 のフレーム選択、canvas.py:150-154 のドラッグ中の直接変更）は `select_frame` / `move_line` の op に直す。
- 見開きは `Page.side` に従って物理的な左右で描き、筆は `add_stroke{space:"spread"}` で送る（§9.3）。

| 参考UIの要素 | Genko の GUI 要素（`app/studio/`） | 裏側 |
|---|---|---|
| 工程バー 1〜5 | 工程ごとの件数バッジ（例「ネーム承認待ち 3」） | `status` |
| （新）承認箱 | エージェントの承認依頼の一覧。ページのプレビューを見て1件ずつ承認・差し戻し（一括ボタンは無い） | `approve`、`revoke`、チケット |
| ページビュー + 読み順 | 読み順バッジと指示の重ね書き（name / proof だけ） | render のオーバーレイ、`reading_summary` |
| 元画像 / 比較 / 候補 / 採用中 | コマ表示の切替、候補グリッド（点数、来歴の要約） | `render --kind`、候補 |
| 今回の指示 / このコマのメモ | 指示欄。書くとエージェントの `next` に `fix_panel` が出る | `set_panel`、`request_fix` |
| 領域の編集 | 重ね書きの領域エディタ（人が作ると `source:"user"`） | region op |
| プロンプト / ネガティブ | 依頼パックの下書きと、候補ごとの「実際に使ったプロンプト」。人間の上書き欄（pinned） | 依頼パック、`origin.prompt`、`gen.prompt_override` |
| キャラクタースタジオ | 設定画の候補、承認、固定 | `approve sheet` |
| 背景・小物ライブラリ | 場所・小物・資産（来歴のバッジ） | `upsert_location`、`upsert_prop` |
| 履歴 | 候補の系譜と来歴、journal | 候補、journal |

---

## 9. 品質戦略

漫画として読めるかどうかは、主に6つで決まる。同じキャラに見えること、絵がネームに従うこと、読み順とめくりが崩れないこと、フキダシが顔を隠さないこと、モノクロの仕上げが揃っていること、そして点検の仕組みである。文章と絵の出来はエージェントのモデルに依るが、Genko はそのどれにも「確かめる道具」と「崩れにくい入力」を用意する。

### 9.1 キャラクターの一貫性（効きの強い順）

1. **承認済みの設定画を毎回渡す（ゲート①）。** 依頼パックは、そのコマに出るキャラの顔と設定画を必ず `refs/` に入れ、`notes_for_agent` に「参照画像として渡す」と書く。参照画像を渡せる画像ツールなら、これが最も効く（ChatGPT の画像生成でも、画像を添えた生成や編集ができる。使える機能は版と使い方で違うので、`tools.json` に記録する）。
2. **見た目の固定の記述。** キャラの `tokens`（英語の文とタグ）を一度だけ書き、人間が直し、`locked` で凍結する。依頼パックのプロンプト下書きはこれを言い換えずにそのまま入れる。`never[]` は「描かせないもの」に入る。
3. **モノクロでは値が個性になる。** `hair_value`（beta / tone / white）とシルエットの要点をプロンプトに入れる。仕上げは全ページ同じ平網の段で行うので、「トーン髪」はどのページでも同じに見える。
4. **服のタイムライン。** コマの beat が属する scene から `outfits[scene_id]`（なければ `default`）を決めて下書きに入れる。
5. **修正は編集で。** キャラが似ていない候補は、作り直すより `mode:"edit"`（元画像 + 設定画 + 「顔を設定画に合わせる」）か `mode:"inpaint"`（顔の領域マスク）で直すほうが、構図を保てる。
6. **パイロットページとスタイル固定。** 1ページ目の art 承認で、style の文言、仕上げパラメータ、使う画像ツールを固定する。全体の一貫性を最も安く得られる。
7. **見開きの連続性の点検。** proof の見開き（§9.3 の修正後の `render_spread`）をエージェントが見て、服、髪、小物、目線、左右の立ち位置、時間帯を確かめ、ずれはチケットにする。最終的には人間の art ゲートで見る。

画像ツールに seed の指定が無い場合（ChatGPT の画像生成など）、同じ依頼から同じ画像は再現できない。Genko は取り込んだ画像そのものを正とし、来歴には「どの依頼から、どのプロンプトで作ったか」を残す。作り直しは再現ではなく、新しい候補である。

### 9.2 構図がネームに従う

**人物の位置（`blocking.py`）。**

- 入力は PanelSpec の `pos`（left / left_third / center / right_third / right）、`scale`（0–1、コマの高さに対する比）、`shot`（ELS / LS / FS / MS / MCU / CU / ECU）、`facing`（left / right / front / back）。
- 出力は人物の箱と頭の箱（mm）、つまり `characters[].box_mm` と `head_mm`。shot ごとに見える範囲を決める（CU は頭と肩、MS は腰から上）。
- 写植の顔よけ（§9.4）と、依頼パックのガイドの両方に使う。

**ガイド画像（`guide.py`）。** ChatGPT のような画像ツールには ControlNet のような「線に従わせる」仕組みが無いので、ガイドは**参照画像と文章の両方**で伝える。

| ガイド | 画像 | 文章（プロンプト下書きに入る） |
|---|---|---|
| composition | ネーム（NAME のストロークとコマ）の切り抜き。白地に黒の線 | 「人物は画面右寄り、腰から上」など、blocking から作った位置の説明 |
| pose | 人物の箱、頭の位置、向きの矢印を描いた図 | 「こちらへ振り返る」「左を向く」 |
| keepout | 文字よけの範囲を灰色で塗った図 | 「左上の 3 割は静かに空けておく（台詞用）」 |

- ガイドはフキダシ、枠線、話者名を入れない（`mode=name` の描画はこれらが混ざるので使わない）。NAME ストロークは mm 基準の線幅で描く。
- **コマと画像の対応（`placement.py`）。** ガイドと配置で同じ変換を使う。候補の縦横比がコマと違っても、配置は cover で合わせ、どこが切れるかを `mapping` に残す。フレームを後で動かした候補は `stale_geometry` になり、置き直すか作り直す。
- **比較画像（compare）。** アタリ（NAME の切り抜き）を赤 50% で候補に重ねたもの。エージェントの評価と人間の「比較」表示に使う。
- **ガイド追従度（決定的な目安）。** 候補のエッジ図（縮小 + Pillow `FIND_EDGES`）と、ガイドの人物の箱との重なり。取り込み時に計算し、候補の一覧に出す。構図が大きく外れた候補を、エージェントが見る前に下位に回せる。

### 9.3 読み順とページめくり

- コマの順は常に `Page.leaf_frames()`（縦分割は右が先）。段組 DSL の列は右から左に書くので、コンパイルした木の順がそのまま読み順になる。
- 表示ラベル（3-1、3-2）は導出するだけで、キーにしない。
- **綴じ方向の修正（M2）。** `Page.side(binding, start_side)` を足し、`is_recto()` を置き換える。呼び出し元は `render_spread` だけ。
  - 右綴じ（`Binding.RIGHT`、既定）: 奇数ページが左、偶数ページが右。見開きは (2, 3)、(4, 5)、… で、右の偶数ページを先に読む。ページ 1 と 2 は同じ紙の表裏で、向かい合わない。
  - 左綴じ: 奇数ページが右。見開きは (2, 3)、(4, 5)、… で、左の偶数ページを先に読む。
  - 雑誌の扉などで開始側が違う場合は `set_meta{start_side}` で上書きする（例: 右綴じで `start_side:"right"` にすると 1 が右に来て、(1, 2) が見開きになる）。
  - `render_spread` は `side == "right"` のページを右に置く。
- **向かい合う対の検査（M2）。** `set_spread{page, with}` は、2ページが隣り合い、`side` が逆で、綴じ方向で先に読む側（右綴じなら右）に若い番号が来るときだけ受ける。strict_gates では違反を拒否し、旧プロジェクトは `warnings[]` を返して従来どおり記録する（既存の test_p5_spread.py:8、test_p7_factory.py:184、test_p8_shell.py:36 は右綴じの 1–2 を使っているが、警告だけなので通る）。
- **見開きの座標（M2）。** `add_stroke` の `page` 引数の意味は変えない。既定（`space:"page"`）は今のまま、原点は `op.page` で、x ≥ 紙幅なら相手ページへ移す（ops.py:303）。物理的な見開きで描くための `space:"spread"` を足し、x を見開きの左端から測る（左ページ [0, W)、右ページ [W, 2W)。どちらのページかは `side` で決まる）。GUI のキャンバスは同じ PR で直し（canvas.py:67-74 は相手を常に右に描く）、見開きを物理的な左右で描いて `space:"spread"` で送る。
- **試験（M2）:** 右綴じの (2, 3) を受け、`render_spread` で 2 が右・3 が左に来る。右綴じの (1, 2) は strict で拒否・旧プロジェクトで警告。左綴じの (2, 3) を受け、2 が左に来る。`start_side:"right"` の右綴じで (1, 2) を受ける。`space:"spread"` の筆が物理的に正しいページに入る。既存の test_p8_shell.py:34（`space` なし）はそのまま通る。
- **めくり。**
  - 右綴じでは見開きを右→左に読むので、めくった直後に最初に読むのは右（既定では偶数）ページである。
  - 引き = 左ページの最後のコマ。`reveal` = 次の右ページの最初のコマ。
  - 脚本 lint とネーム lint はこれで `reveal` と引きの位置を検査する。左綴じでは左右が逆。
- **ネーム lint の読み順検査:**
  - 右側の縦長のコマが段をまたぎ、目の流れが二通りに読める配置を警告する（典型的なネームの誤り）。
  - フキダシがコマ境界をまたぐものを禁止する。

### 9.4 フキダシと写植（顔を避ける）

- **寸法。**
  - `tategaki.compose` の実寸で測る。字の大きさは `style.lettering.font_mm`。
  - 1行の長さは `min(0.6 × コマの高さ, max_col_mm)`。
  - 計画の `breaks` で改行を指定する。そのために `tategaki._columns` が明示改行（`\n`）で行を切れるよう拡張する（M0。ネーム縦串で必要）。
- **スロット探索（`letter.py`）。** ネーム計画の取り込み時に走り、結果は `add_line` / `move_line` の明示座標になる（op の中で配置しない。journal の再生がフォントに依らない）。
  - コマの内側（2 mm 内寄せ）に格子状の候補位置を並べる。
  - 費用 = 1000 × 顔との重なり + 10 × 体との重なり + 1000 × 他のフキダシとの重なり + 読み始め（右上）からの距離 + 順序違反の罰。
  - 順序の制約: i+1 番目のフキダシは i 番目の左か下に置く。
  - 貪欲法 + 幅 4 のビームサーチ。
- **尾（tail）。**
  - 話者の頭の箱に向ける。話者がコマの外にいるときはコマの縁に向ける。
  - ナレーションは尾なし。モノローグと心の声は楕円（尾なし）。
  - 描画の修正（M6）: 尾の根元幅は固定 12 px（render.py:352, 377）をやめ、フキダシの短辺の 1/5 以上を mm で取る。縦長のフキダシには横からの尾も出せるようにする。
- **楕円の余白（M6）。** 縦書きの楕円は文字の箱に外接させる（半径 = 箱の半分 × √2 + 余白）。今は `em/4` の内側余白しかなく、行の角が輪郭を越えることがある（render.py:318-340）。
- **ルビ（M6）。** 親文字に揃えて置く。すべての run を使う（今は最初の run だけ、tategaki.py:136-146）。
- **話者名（M2）。** name と proof だけに描く（今は print にも出る、render.py:353-354, 378-379）。
- **絵の前後。**
  - 絵ができる前は、人物配置の顔箱を避けて置く。そのうえで文字よけマスクとプロンプトの一文（例「empty space on the upper left」）で、**文字の周りに絵を描かせる**。
  - 採用後は、エージェントが画像を見て報告した顔と人物の箱（`add_region{kind:"person", source:"agent"}`）か人間が引いた領域を使い、写植器を再実行する。移動は `move_line` の提案として出し、人間は戻せる。

### 9.5 モノクロ仕上げとスクリーントーン（`screentone.py`、render 時、Pillow のみ）

placed layer ごとに、print と proof で次を行う。

1. 元資産を輝度にし、`placement_mm` を出力 dpi に合わせて LANCZOS で再標本化する。黒点と白点で levels をかける。
2. **線のマスク。** 線の閾値より暗く、局所コントラストが高い画素を純黒にし、網をかけない。
3. **ベタと白。** `L < black` は黒、`L > white` は白。
4. **中間調。**
   - K 段の平網（既定 10 / 20 / 30%）に量子化する。連続的なディザではなく、実際のトーンの見え方に合わせる。
   - 各段を `lpi` と `angle` の AM 網点で描く。セルの大きさは `s = 出力 dpi / lpi`（px）。
   - 被覆率 `cov ≤ 0.5` は黒点で、半径 `r = s·√(cov/π)`。
   - `cov > 0.5` は黒地に白抜きの点で、半径 `r = s·√((1−cov)/π)`。
   - こうすれば被覆率は dpi に関係なく density と一致する（今の `_draw_tone` は 150 dpi でどの濃度も約 0.92 が黒）。
5. print は 1bit で出す。TIFF G4（`to_bitonal`）で何も失わない。
6. proof は網点を描かずに量子化したグレーで見せる（画面上のモアレを避ける）。「網点表示」の切り替えを付ける。
7. 既存の `_draw_tone`（render.py:144-171）も同じ関数で描き直す。region の多角形を守り、素材の kind が `noise` なら FM（`Image.convert("1")` の誤差拡散）にする。
8. カラーの profile や webtoon（`expression:"color"`）では 2〜6 を飛ばす。

**線抽出（`lineart.py`）。**

- 生成画像用のラスタ線抽出で、手順は次のとおり。
  1. 輝度 L を、`MaxFilter` で求めた局所の最大値で割る（割り算による背景除去）。
  2. 正規化して閾値で切る。
  3. N px 未満の点を取り除く。
  4. RGBA の INK ラスタにする。
- `studio derive --kind lineart`（Genko 内の決定的な処理。ロックの外で資産を作り、候補として取り込む）で作り、INK 候補として採用する。人間の既存の LT（`lt_convert`）はそのまま残す。
- 生成の目標は、mono 原稿ではグレースケールの漫画調とする（プロンプトに `monochrome, greyscale, manga`、ネガティブに `color`）。仕上げが決定的で全コマ同一なので、それ自体がスタイル一貫性の強いてこになる。


### 9.6 点検の層

| 層 | 何を見るか | 担当 | 結果 |
|---|---|---|---|
| 1 入力の検査 | スキーマ、id、参照の解決、範囲 | Genko（`jsonschema_lite`、`_validate`） | エラー（JSON ポインタ付き）。保存しない |
| 2 決定的 lint | 脚本（字数、話者、予算、めくり）とネーム（密度、最小コマ、shot の連続、180度、状況説明、境界またぎ、めくり、beat を1回ずつ） | Genko（`lint.py`） | エラーはエージェントが直す。警告は残して人間に見せる |
| 3 自己点検 | name のプレビュー（読み順の迷い、窮屈さ、弱いめくり） | エージェント（画像を見る） | 直すか `record_review` |
| 4 候補の目安 | 縦横比、文字よけの範囲のエッジ密度、輝度帯、ガイド追従度 | Genko（取り込み時に計算） | 候補の一覧の順位 |
| 5 候補の評価 | 比較画像、キャラの設定画、指示 | エージェント | `review_candidates`、採用か修正 |
| 6 連続性 | ページと見開きの proof | エージェント | チケット |
| 7 印刷の点検 | 顔にかかるフキダシ（報告済みの領域）、実効 dpi。画内文字はエージェントの評価と人間の art ゲート | Genko（preflight）+ エージェント | preflight の警告と拒否 |
| 8 人間 | ゲート①〜④、いつでも修正 | 人間 | 承認、差し戻し、修正指示 |

- エージェントの評価は助言である。採用はエージェントもできるが、ページの確定（art ③）は人間が行う。
- **表現の制限は Genko では掛けない。** 何を描くかはネームと画像を作る AI（とそのサービスの方針）で決まる。Genko は内容を検査せず、人間が art ゲートで全ページを見る。

### 9.7 解像度

- 配置した層ごとに実効 dpi = 元画像の横 px ÷（`placement_mm.width` / 25.4）を記録する。
- 例: 180×80 mm のコマを 600 dpi で刷るには約 4252×1890 px（約 8 MP）が要る。1〜2 MP 級の生成画像では足りない。
- 閾値は、グレーの絵が 350、2値の線が 600。下回るコマは worklist に `upscale_panel` として出す。
  - エージェントの画像ツールに高解像度化があれば、依頼パック `mode:"upscale"` で頼み、結果を `parent` 付きの候補として取り込む。
  - 無ければ Genko の LANCZOS 再標本化 + 線抽出（§9.5）で仕上げる。線が甘くなることがあるので、preflight はコマごとに**達成した実効 dpi** を一覧で出し、閾値未満を警告する（`--force` で通せる）。
- 書き出しの dpi は `spec.dpi`（B4 商業原稿は 600）を既定にする。今の pack は 150 dpi に切り（pack.py:12-13）、CLI の既定も 150（__main__.py:35）なので、M6 で直す。

### 9.8 文字と効果音

- 台詞・ナレーション・心の声は Genko のベクター描画。
- 依頼パックの「描かせないもの」に文字・フキダシ・描き文字・署名を必ず入れる。画内に文字が出た候補は、エージェントが評価で見つけて `mode:"inpaint"`（文字の領域）で消す。
- 効果音（SFX）は v1 では新しい balloon 種別 `sfx` とし、Genko の装飾文字（縁取り、回転、大きさ）で描く（M6）。描き文字を画像ツールで作るかは未決（§12）。
- 効果（集中線・流線）はコマでクリップする（今は `_draw_effects` がコマをはみ出す、render.py:174-205）。

---

## 10. テスト戦略

### 10.1 原則

- すべてオフライン、Qt なし、リポジトリのルートから実行する（`tests/test_p1.py` は `docs/ops.schema.json` を相対パスで読む）。
- ユーザー設定ディレクトリ（`tools.json`、`tokens.json`）は `GENKO_CONFIG_DIR` で上書きでき、試験は一時ディレクトリを使う。
- 既存のパターンを使う。メモリ上の episode + `apply_ops`、`main([...])` + `capsys`、`handle_request` の直接呼び出し、`HeadlessServer(port=0)`、Pillow で作るメモリ上の PNG。
- 基準線は 98 passed / 1 failed。落ちている `test_small_kana_sits_right_in_em_box` は、フォントが無いのではなく、同梱の DelaGothicOne で小書き仮名の重心が右に寄らないことが原因（§0.5 の 22）。M1 で `tategaki.glyph` の小書き仮名の配置（tategaki.py:84 付近）を調べて直す。字形そのものが em 箱の中央寄りのフォントなら、試験を「小書き仮名は全角より小さく、右上寄りの箱に収まる」に緩める。あわせてフォントを `src/genko/fonts/` の package data にし、`importlib.resources` で読む（wheel に入らない問題、§0.5 の 29）。
- **既存テストを変えるときは、PR ごとに一覧を書く**（§11 の各マイルストーンの「変える既存テスト」）。黙って直さない。
- **Windows CI。** `windows-latest` のジョブを足し、全試験を走らせる。Windows 固有の試験（下記 §10.7）はここでだけ走る。


### 10.2 偽エージェントと試験用の画像

- **偽エージェント（`tests/agents/`）。** 筋書き（JSON）どおりに MCP の道具を呼ぶ。MCP の試験では stdio のサーバーを子プロセスで起動し、本物の MCP クライアントとしてつなぐ。単体の試験では `StudioService` を直接呼ぶ。筋書きには「わざと壊した計画 → 指摘を読んで直した計画」の組を入れ、エージェントの直し方の経路を通す。
- **試験用の画像（`tests/fixtures/studio/images/`）。** Pillow で作る単純な画像（グラデーションに人物の楕円、顔の位置を PNG の tEXt に埋める）。取り込み時は `origin.kind:"fixture"` にし、書き出しの preflight が本番では拒否すること、`--allow-fixture` で通ることを試す。
- **Hermes Agent は CI で使わない。** 実際の Hermes（Claude や ChatGPT を使う）での確認は、手動のチェックリストと評価（§10.8）で行う。

### 10.3 M1・M2 の回帰試験（先に失敗する試験を書く）

| 試験 | 確かめること | 段 |
|---|---|---|
| `test_apply_cost_independent_of_undo_depth` | 16 ページ（各 NAME 30 本）で、深さ 50 の apply が 100 ms 未満かつ深さ 0 の 1.5 倍以内（3 回の最良値） | M1 |
| `test_apply_cost_after_lt_convert` | lt_convert したページを含むフィクスチャでも深さに依らない | M1 |
| `test_deepcopy_skips_undo_stack` / `test_strokes_are_shared_not_copied` | `copy.deepcopy(ep).undo_stack == []`、Stroke は同一オブジェクト | M1 |
| `test_no_op_mutates_stroke_in_place` | 全 op の筋書きを流し、既存の Stroke が書き換わらない | M1 |
| `test_http_rejects_text_plain_post` / `test_http_rejects_foreign_origin` / `test_http_rejects_foreign_host` / `test_http_no_wildcard_cors` / `test_http_requires_token` / `test_http_root_confinement` | CSRF（415）、Origin（403）、DNS rebinding（421）、CORS、401、root の外とシンボリックリンク（403） | M1 |
| `test_lock_acquire_is_exclusive` | 子プロセス 8 つが同時に取っても1つだけ成功する | M1 |
| `test_stale_lock_takeover_race` | 失効したロックを子プロセス 2 つが同時に奪っても1つだけ成功し、新しい持ち主のロックは消えない（O_EXCL の予備経路） | M1 |
| `test_release_checks_token` | 自分のロックが失効した後の `release` が次の持ち主のロックを消さない | M1 |
| `test_cli_loads_inside_lock` | ロード → 他者の書き込み → apply の順で更新が失われない | M1 |
| `test_ai_cannot_unlock_human_lock` / `test_ai_cannot_take_over_lock` / `test_lock_owner_is_actor` | `ai:*` は人間のロックを外せず奪えない。`agent` 引数で他人を名乗れない | M1 |
| `test_legacy_actor_cannot_approve_in_studio` / `test_legacy_project_default_actor_unchanged` | studio では actor 省略が `legacy:unknown` で承認不可、旧プロジェクトは従来どおり | M1 |
| `test_ai_actor_cannot_approve` | `ai:*` の name_ok と approve は拒否される | M1 |
| `test_edit_line_respects_page_lock` | 他人のロックしたページの台詞は変えられない | M1 |
| `test_newer_version_refused` / `test_unknown_keys_roundtrip` | 版ゲートと extra の保持 | M1 |
| `test_font_loaded_from_package_data` | インストールした wheel 相当（`importlib.resources`）から同梱フォントを読める | M1 |
| `test_cli_json_utf8_on_cp932` | `PYTHONIOENCODING=cp932` の子プロセスで CLI の JSON が壊れない（UTF-8 か `--ascii`） | M1（Windows CI でも） |
| `test_delete_page_then_put_raster_keeps_pixels` | ページ削除後の put_raster が別ページの画素を上書きしない（保存 → 読込で確認） | M2 |
| `test_duplicate_page_does_not_share_raster` | 複製元を後で編集しても、再読込後に複製の絵と混ざらない | M2 |
| `test_two_user_layers_filtered_do_not_collide` | USER レイヤー2枚に filter_raster をかけても別々のバイト列のまま | M2 |
| `test_duplicate_page_lines_point_to_new_frames` | 複製した台詞の frame_id が新しいフレームを指す | M2 |
| `test_ticket_and_lock_follow_page_after_delete` | tickets と page_locks がページ id で追従する | M2 |
| `test_edit_line_persists_after_reload` | edit_line が保存と再読込を越えて残る（story と texts が同一） | M2 |
| `test_project_json_size_budget` | 16 ページのネーム作業（lt_convert 1 ページを含む）で project.json が 1 MB 未満、`name_strokes` / `ink_strokes` を書かない | M2 |
| `test_journal_line_and_total_cap` | journal の1行が 64 KB 未満、保持が 100 commit / 20 MB を超えない | M2 |
| `test_undo_across_processes` | CLI の apply → 別プロセスの `genko undo` が効く（チェックポイント + 再生） | M2 |
| `test_replay_is_font_independent` | 写植の座標が journal に入り、別のフォント設定で再生しても台詞の位置が同じ | M2 |
| `test_validate_only_touched_entities` / `test_missing_asset_is_warning_not_error` | 触れていないページの不整合で op が落ちない。資産の欠落は warnings に出て、書き出しの preflight で止まる | M2 |
| `test_gc_holds_lock_and_keeps_pending_assets` | GC がロックを取り、発行中の生成依頼・取り込み待ちの資産と新しい未参照資産を消さない | M2 |
| `test_advance_finish_requires_art_ok_when_strict` / `test_legacy_project_unchanged` | strict_gates の有無で挙動が分かれる | M2 |
| `test_ops_registry_complete` / `test_ops_schema_doc_is_generated` / `test_tool_schemas_lint` | 扱う op がすべて登録され、docs が生成物と一致し、`ops` 目録が残り、tool スキーマに `oneOf`・数値や文字列の制約・`false` 以外の `additionalProperties`・再帰が無い | M2 |
| `test_copy_state_covers_all_fields` | Episode の全フィールド（一時のものを除く）が commit と undo を通る | M2 |
| `test_render_spread_right_binding` / `test_set_spread_facing_pairs` / `test_spread_space_stroke` | §9.3 の試験 | M2 |
| `test_speaker_label_absent_in_print` | print に話者名の画素がない | M2 |
| `test_put_raster_rejects_non_image` | 画像でないバイト列は ApplyError | M2 |
| `test_os_replace_retries_on_permission_error`（Windows） | project.json を別プロセスが開いていても、再試行で保存できるか、`busy` で失敗を返す | M2（Windows CI） |
| `test_long_japanese_path`（Windows） | 深い日本語のパスの下で `assets/ab/<64 桁>` を読み書きできる | M2（Windows CI） |


### 10.4 単体試験と golden

- 段組 DSL → フレーム矩形（JSON の golden）。ソースマップの位置。列は右から左で、葉の順 = 計画の順。
- `letter.py` のフキダシの箱（package data の同梱フォントで固定、JSON の golden）。明示改行で列が切れる。
- `lint.py`: 規則ごとに「通る例」と「落ちる例」を持ち、指摘の `path` が入力の正しい位置を指す。
- 入力スキーマ: 全スキーマが保守的な形（再帰なし、`oneOf` なし、数値・文字列長の制約なし、`additionalProperties:false`、任意項目は nullable）であることを検査する lint 試験。`jsonschema_lite` が使う範囲の JSON Schema を正しく判定する。
- `genreq.py`: 依頼パックの JSON の golden（サイズの候補、プロンプトの下書き、描かせないもの、文字よけの箱）。同じ入力から同じ id になる。キャラの `tokens` が言い換えられずに入る。人間の上書きがあればそれが入る。
- `guide.py`: 種類ごとの画素統計（黒の比率、重心）を JSON で持つ。
- `importer.py`: 画像でないバイト列、画素数の上限超え、`inbox/` の外のパス、`--root` の外へ出るシンボリックリンクを拒否する。同じ画像の2回目の取り込みで候補が増えない。`origin` の無い取り込みを拒否する。
- `screentone.py`: 目標被覆率 ±3%（150 / 300 / 600 dpi、10〜90%）。
- `placement.py`: mm → px → mm の往復、cover の切り取り範囲。
- worklist: 状態を作って `next` の項目と順序を確かめる。`record_review` の後に同じ点検が出ない。上限を超えたコマがチケットになる。承認待ちだけになると `waiting_for` が出る。

### 10.5 性質試験

- 乱数の種を固定し、split / merge / resize / duplicate / delete / reorder / adopt / unadopt / set_panel の列をランダムに作る。
- 満たすべきこと: `_validate` が常に通る、保存 → 読込で一致する、placed art の画素が print で溝（コマ外）に出ない（bleed 指定を除く）、候補と採用の参照が解決する。
- 写植の性質: フキダシはコマの中にある、顔の箱と重ならない、順序の制約を守る。
- 段組の性質: 葉は内枠の中にある、重ならない、溝の幅を守る、葉の順 = 計画の順。

### 10.6 E2E（オフライン、偽エージェント）

偽エージェントが MCP（stdio）で `genko mcp --root tmp --agent ai:test` につなぎ、筋書きの人間（`human:test`、CLI で承認）と組んで、8 ページの話を最後まで作る。

- (a) 企画 → 脚本 → 8 ページのネーム。全ページが指摘なしで保存され、2回走らせると同じ project.json になる（決定的）。
- (b) `ai:test` はどのゲートも承認できない（MCP に道具が無く、`apply_ops` 経由の `approve` も拒否される）。人間の承認が無ければ各ゲートで `waiting_for` を出して止まる。
- (c) 絵: 各コマで依頼パック → 試験用の画像を `inbox/` に置く → `import_images` → `adopt`。人間の art 承認の後、仕上げ、preflight、書き出しで TIFF と PDF ができる。print に NAME / DRAFT / 候補の画素がない（番兵色で確かめる）。
- (d) 本番書き出しは `origin.kind:"fixture"` の画像があるので拒否される。`--allow-fixture` で通る。
- (f) 別マシンの形: HTTP の MCP（`--http`）とトークンで同じ筋書きが通る。画像はアップロード（`POST /v1/assets`）で渡し、参照ファイルは URL で取る。
- (g) 途中で偽エージェントを止め、新しい偽エージェントで `next` から続けて完了する（再開）。同じ依頼を作り直しても重複しない。
- (h) わざと壊した計画を出し続ける筋書きで、偽エージェントが3回で諦めてチケットを出し、`next` が同じ項目を出し続けない。

### 10.7 HTTP・並行性・GUI

- HTTP は `handle_request` の直接呼び出し（`ctx` 付き）と `HeadlessServer(port=0, root=tmp_path, tokens=…)` で試す。全経路でトークンが無ければ 401、§8.4 の CSRF・Origin・Host・Content-Type・CORS・root の試験、`expect_revision` の衝突は 409、`/v1/export` が形式と dpi を守ること、エージェントのトークンで人間向けの経路が 403 になること。
- 並行性: 偽エージェント2つが `next{claim:true}` で同じ項目を取らないこと。GUI 相当のセッションの commit と MCP の commit を交互に入れ、更新が失われないこと。
- GUI は offscreen の手動チェックリスト（承認箱: 依頼 → プレビュー → 承認 / 差し戻し）。PySide6 は CI に入れない。
- **Windows CI（`windows-latest`）:** 全試験に加え、`os.replace` の再試行、長い日本語パス、cp932 のコンソール出力、OS ロック（`msvcrt.locking`）の排他と、プロセスが死んだときの解放、stdio の MCP サーバーの起動（パスに日本語を含む `--root`）。

### 10.8 実際のエージェントでの確認と評価

- CI では走らせない。手順書（`docs/STUDIO_EVAL.md`）に沿って、人間が Hermes Agent（LLM は Claude か ChatGPT、画像は ChatGPT の画像生成）で行う。
- 評価セットは5つの premise。測るもの:
  - ネーム: ページごとの出し直しの回数、指摘の種類の内訳、最後まで行けたページの割合
  - 道具の選び間違い（存在しない道具、引数の間違い）の回数。道具の説明とスキルの直しに使う
  - 絵: 1コマあたりの取り込み枚数、採用までの修正回数
  - 人手評価（読みやすさ、テンポ、めくり、キャラの同定）
- **評価に要るデータは作業として計画する**（§11 の各マイルストーンに入れる）。
  - D0（M0）: premise 5 本、読みやすさ・テンポ・めくりのルーブリック、評価者 2 名の手順書。
  - D5（M5）: 盲検のキャラ同定。10 コマ × 主要キャラ 3 人の組を、キャラの設定画だけを見た評価者 5 名が当てる。
  - D8（M8）: スキャンしたアタリ 20 ページ以上と、その正解（コマの矩形 mm と台詞）。

---

## 11. ロードマップ

見積りは「このコードベースを知る開発者1人の人週」。レビューと手戻りを含む目安で、データ作り（D…）は別に数える。

依存関係:

```text
M0（エージェントがネームを出せる。v2 のまま、MCP stdio）──┐
M1（土台の修理: 性能・安全・ロック・actor。MCP の HTTP 接続）──┴─▶ M2（v3 形式と保存）─▶ M3（コマ配置と studio 状態）
   ─▶ M4（生成依頼パックと画像の取り込み。オフライン E2E）─┬─▶ M5（最初の1ページを Hermes + ChatGPT で）─▶ M6（印刷できるモノクロ）
                                                             ├─▶ M7（GUI。M5 と並行可）
                                                             └─▶ M8（アタリ取り込み。M4 の後いつでも）
M5 + M7 ─▶ M9（拡張）
```

- M0 と M1 は並行できる。M0 を最初に利用者へ届ける。
- M3 は M0 のサイドカー（bible・脚本・ネーム計画）を op で project.json に取り込む。
- 別マシンでの運用（§13 の未決事項）が決まったら、M1-7（MCP の HTTP 接続）を先に出す。

合計の目安は約 35 人週 + データ作り（D0、D5、D8）。第2版より、LLM と画像バックエンドの組み込みが無くなった分だけ減り、MCP・依頼パック・取り込みの分だけ増えた。

### M0 エージェントがネームを出せる（v2 のまま、MCP stdio）— 約 3 人週 + D0

範囲: Hermes Agent が MCP で、企画書・脚本・ネーム（段組、コマ指示、台詞）を Genko に渡し、Genko が検査・コマ割り・写植・プレビューを行って原稿にする。**新しい保存形式、資産ストア、新しい op は作らない。** 既存の op（`split_frame`、`resize_frame`、座標を明示した `add_line`、`set_bible`、`set_note`、`add_ticket`）にコンパイルし、bible・脚本・ネーム計画・PanelSpec はサイドカー（`studio/drafts/`）に置く。

| PR | 内容 |
|---|---|
| M0-1 | `genko.studio` の骨組み（core）: `issues.py`、`jsonschema_lite.py`、入力スキーマ `bible@1` / `script@1` / `name_plan@1`、スキーマの形の lint 試験、サイドカーの読み書き（一時ファイル + `os.replace`） |
| M0-2 | 段組 DSL コンパイラ → 既存 op。既存の `split_frame` は子の id を指定できない（id は `new_id()`、models.py:324-337）ので、1ページを1つのロックの中で「段の分割を apply → 新しい葉を読み順で読み戻して slot に対応付け → 列の分割を apply → … → 台詞を apply」と段ごとに `apply_ops` を呼び、最後に1回だけ保存する。途中で失敗したら保存しないので、ファイルから見れば全か無か。ソースマップ。定型（`layouts.json`） |
| M0-3 | `letter.py` v1（`tategaki.compose` の寸法、`_columns` の明示改行、コマ内の読み順の配置、`blocking.py` の頭の箱を避ける）→ 座標を明示した `add_line`。コマ指示の要約をページの `set_note` に |
| M0-4 | `lint.py`（bible・脚本・ネーム計画・幾何）。指摘は入力の JSON ポインタと直し方の `hint` 付き |
| M0-5 | `StudioService` と MCP サーバー（extra `mcp`、stdio）。**`apply_ops` 道具は StudioService の層で許可リストの op だけを通す**（M1-4 を待たない）。ゲート op（`name_ok`・`approve`・`revoke`）、`lock_page` / `unlock_page`、`set_studio` の policy、`put_raster` の `path`、`set_meta` の `font_path` は拒否する。M0 の `record_review` と `request_approval` はサイドカー（`studio/drafts/reviews.json`、`approvals.json`）に書き、`next` はそれを読む（新しい op を作らないため）。道具: `projects`、`create_project`、`status`、`next`（ネームまでの worklist）、`inspect`、`render`（name のプレビュー）、`set_bible`、`set_script`、`submit_name`、`edit_panel`、`apply_ops`（dry_run が既定）、`record_review`、`request_approval`、`tickets`。同じ道具の CLI（`genko studio …`） |
| M0-6 | 人間向け: `genko studio init`、`studio review`（HTML）、`studio approve name`（M0 では既存の `name_ok` を人間の actor で呼ぶ）。Hermes 用の `SKILL.md` と `config.example.yaml`、MCP resource `manga-rules` |
| M0-7 | 偽エージェントによる E2E（§10.6 の a、b、g、h のネームまでの部分）、性能の目安（`submit_name` の応答 2 秒以内） |
| M0 の実装メモ | 実装時の割り切り: 人間の承認は M0 では name だけ（`studio approve name`）。脚本は承認不要（auto）。承認の取り消しと `edit_panel` は M3 で入れる（それまでは `submit_name` の `replace:true` で作り直す）。人間の修正指示は `genko studio comment` で出し、エージェントの `next` に `revise_page` として出る。`genko mcp` は stdio だけ（HTTP は M1-7） |
| D0 | premise 5 本、ルーブリック、評価者 2 名の手順書、Hermes での手動チェックリスト（MCP の画像コンテンツが見えるか、Hermes から ChatGPT の画像生成を参照画像つきで呼び、ファイルに保存できるか） |

受入基準:

- オフライン（偽エージェント）: 固定した筋書きから 8 ページのネームが決定的に出る（段組と写植の golden）。全ページが dry_run と lint を通る。
- 出力の project.json は `7e65e4c` のビルドでそのまま開ける（書式を変えていない）。
- MCP の道具の入力スキーマが保守的な形の lint を通る。道具の応答に画像のバイト列の引数が無い（画像はプレビューとファイルだけ）。
- MCP の道具では承認できない（`ai:*` の `name_ok` は拒否）。人間の CLI でだけ承認できる。
- 手動（Hermes Agent + Claude か ChatGPT、5 premise）: 各ページが出し直し 3 回以内で指摘なしになる。評価者 2 名で 5 話中 4 話以上が読みやすさ 4/5 以上。
- **単独でも出せる成果:** エージェントが書き、人間が承認したネーム。人間の漫画家がそのまま清書に使える。

変える既存テスト: なし。

### M1 土台の修理（性能・安全・ロック・actor、MCP の HTTP 接続）— 約 3 人週

**実施状況（2026-09-24）:** M1-1〜M1-6 を実装した。M1-1 は undo 履歴を複製しない形にした（Stroke の不変化は見送り。deepcopy は作業用の1回だけで、費用は undo の深さに依らない）。M1-3 のロックは OS のファイルロック（flock / msvcrt）で、古いビルドが書いたロックも尊重する。M1-6 の小書き仮名は、同梱フォントが小書きを全角に近い大きさで描くのが原因だったので、縮小して右上に置くよう直した。Windows を含む CI（`.github/workflows/ci.yml`）を足した。M1-7（MCP の HTTP 接続）は別マシンでの運用が決まるまで後回し。

範囲: エージェントが頻繁に書き込むようになる前に、今のコードの欠陥を直す。書式は変えない。

| PR | 内容 |
|---|---|
| M1-1 | 汎用 `_copy_state`、undo 履歴を deepcopy しない（`Episode.__deepcopy__`、commit 時の旧状態の移し替え）、Stroke の不変化、性能試験 |
| M1-2 | HTTP の安全（全経路のトークン、Origin / Host の検査、JSON 限定、CORS `*` の削除、`--root` 必須と閉じ込め、`handle_request` の `ctx`） |
| M1-3 | `ProjectLock`（OS ロック + token、予備経路の墓石 rename、所有者を確かめる release）、CLI と HTTP はロックの中でロード |
| M1-4 | actor の受け渡し（`--agent`、HTTP はトークンから）、studio での `legacy:unknown`、ゲート op の actor 規則、`lock_page` / `unlock_page` の所有者規則、台詞 op と `set_ticket` の page lock 検査、AGENT.md の更新 |
| M1-5 | 版ゲート R0（`version` を読む、新しすぎるファイルを拒否、未知キーを `extra` で保持）。**書式は v2 のまま単独でリリース** |
| M1-6 | フォントを package data に、小書き仮名の配置の調査と修正（または試験を緩める）、CLI の UTF-8 出力と `--ascii`、Windows CI |
| M1-7 | MCP の HTTP 接続（`genko mcp --http`）: M1-2 のトークン・Origin / Host の検査・`--allow-host` を共用、actor はトークンから、`POST /v1/assets` のアップロードと依頼ファイルの取得（`genko mcp --http` は `/mcp` に加えて `/v1/assets`・`/v1/requests/*` を同じ handler で出す）、`genko token add --actor ai:hermes` / `genko token revoke`。**別マシンでの運用が決まるまで後回しにしてよい** |

受入基準:

- 既存の 98 試験 + 直した小書き仮名の試験が緑（Linux と Windows）。
- §10.3 の M1 の試験が通る（深さ 50 で 1 op 100 ms 未満、CSRF / DNS rebinding / root、ロックの競合、AI のロック奪取の拒否）。
- M1-7 を入れた場合: §10.6 の (f) が通る（HTTP の MCP とトークンで同じ筋書き）。

変える既存テスト: `tests/test_server.py`（`root=tmp_path` とトークンを渡す）。`tests/test_tategaki.py::test_small_kana_sits_right_in_em_box`（調査の結果、緩める場合だけ）。

### M2 v3 形式と保存 — 約 4.5 人週

**実施状況（2026-09-24）:** M2-1〜M2-6 を実装した。設計からの変更点: journal は正規化した ops の再生ではなく、保存ごとの project.json の前後（内容アドレスの資産。v3 では小さい）を記録し、undo / redo はそれを戻す（`split_frame` などが新しい id を作るため、ops の再生では同じ状態にならない）。journal は `studio/journal.jsonl`、ops は監査用に併記する。ラスタは読み込み時にまとめて読む（遅延読み込みは見送り）。Stroke の不変化は見送り。`strict_gates` は studio のプロジェクトで有効（`advance to=finish` に art_ok を要る規則は art_ok ができる M3 で入れる）。見開きは綴じ方向から左右を決め、GUI のキャンバスも相手ページを物理的な側に描く。

| PR | 内容 |
|---|---|
| M2-1 | 呼び出し側 id（§5.1-4）、`_validate`（触れた範囲、資産の欠落は警告）、`put_raster` の画像検証、`genko doctor` |
| M2-2 | `Page.id`、v3 移行（`MIGRATIONS`、`v2_to_v3`、バックアップ）、page_locks と tickets の id 化、`_remap_page_refs`、`duplicate_page` の frame id 対応表、story と texts の同一化 |
| M2-3 | `AssetStore`、ラスタとストロークの内容アドレス保存（`name_strokes` / `ink_strokes` の重複をやめる）、差分保存と遅延読み込み、Windows の `os.replace` 再試行、`genko gc --legacy`（ロックを取る） |
| M2-4 | journal（正規化 ops + チェックポイント、上限）、`genko undo / redo`（プロセスをまたぐ） |
| M2-5 | スキーマ登録簿、生成した `OPS_SCHEMA` と `docs/ops.schema.json`（`ops` 目録を保持 + `anyOf` の機械用スキーマ）、MCP ツールの入力スキーマ、tool スキーマの lint（8 op の登録漏れを解消） |
| M2-6 | `strict_gates`、`Page.side`、`set_spread` の対の検査、`add_stroke{space:"spread"}`、`render_spread` と canvas.py の見開き、print の話者名を消す、opacity 0 の修正 |

受入基準:

- §10.3 の M2 の試験が通る。
- v1 と v2 のフィクスチャが読めて、v3 で保存し直しても失うものがない。
- 16 ページのネーム作業（lt_convert 1 ページを含む）で project.json が 1 MB 未満、journal の1行が 64 KB 未満。
- 別プロセスの `genko undo` が効く。`docs/ops.schema.json` が生成物で、`ops` 目録が残る。

変える既存テスト: `tests/test_p1.py:84`（`pages/001/bg.png` ではなく `bg.raster_relpath` の資産パスを見る）。`tests/test_p1.py:118-124` は `ops` 目録を残すので変えない。見開きの既存テスト 3 本は旧プロジェクトでは警告だけなので変えない。

### M3 コマ配置と studio 状態（画素境界）— 約 3.5 人週

範囲: 取り込んだ画像を「コマに置く」ことを正しくし、studio の状態を project.json に持つ。まだ依頼パックは出さない。

**実施状況（2026-09-24）:** M3-1〜M3-6 を実装した（GUI の描画を除く）。
- 画像は `import_image`（MCP・CLI）で `assets/` に入れ、`import_candidates` でコマの候補にし、`adopt_candidate` で採用する。
- 採用した絵は `LayerKind.PLACED` の層として元の資産を指す。print のたびに見える部分だけを元画像から LANCZOS で再標本化し、コマで切り取る（bleed のコマは外側の辺を紙の端まで伸ばす）。
- studio の状態（bible_doc、脚本、ページの計画と自己点検、コマのブリーフ・候補・採用、承認の記録、チケット）は project.json に持つ。すべて `apply_ops` の op で変わるので、undo と journal がそのまま効く。
- M0 のサイドカーは `genko studio adopt-drafts` で取り込み、`studio/drafts.adopted` に改名する。

設計からの変更点:
- `studio/state.py` は PanelSpec のクラスではなく、project.json を読むだけの関数にした。コマのブリーフは `Frame.panel`（dict）に持つ。
- `Episode.assets` の注入の代わりに、読み込み時に `Episode.asset_dir` を付ける（保存しない一時フィールド）。
- 絵の工程の作業（`make_sheet`・`make_art`・`import_art`・`choose_art`・`fix_art`・art 承認待ち）も `next` に出す。登場人物の設定画が承認されるまで、そのコマの作画は `make_sheet` を先に出す。
- 資産の参照は project.json 全体から探す（候補・孤児・設定画も gc で消えない）。

後回しにしたもの:
- GUI のキャンバスで placed layer を描く（M7）。
- `guide.py` の crop 以外のガイド（M4）。
- raster 層の `_clip_mask` の bleed 対応（M4）。

| PR | 内容 |
|---|---|
| M3-1 | `LayerKind.PLACED` と Layer の新フィールド（io / migrate / snapshot）、`Episode.assets` の注入 |
| M3-2 | `placement.py` と placed layer の描画（元画像から LANCZOS、frame / bleed クリップ）。`Frame.bleed` を `_clip_mask` と `raster._clip` に実装。bleed 辺の枠線を描かない |
| M3-3 | `Frame.panel`、`studio/state.py` の `PanelSpec`、`set_panel`（human の上書き欄を含む）、寿命のルール（§4.7）、`studio adopt-drafts`（M0 のサイドカーを op で取り込む） |
| M3-4 | studio の state と ops（キャラ、場所、小物、脚本、page plan、承認、取消、領域、参照、`record_review`、`request_approval`、`request_fix`、`set_finish`）。来歴必須の `register_assets` |
| M3-5 | `import_candidates`（来歴必須）、`set_candidate`、`adopt_candidate`、`unadopt`、`set_placement`、`place_asset`（hash だけ） |
| M3-6 | `guide.py` の crop、`render --frame --kind crop`、`GET /v1/pages/{n}/frames/{id}.png`。GUI のキャンバスがラスタと placed layer を描く |

受入基準:

- 縦横比の違う PNG 3枚を3つのコマに採用し、600 dpi の print で溝の画素が白いこと。
- 元画像から再標本化されている（ページ引き伸ばしよりエッジが鋭い）。
- undo で採用が戻る。ページを複製しても絵が新しいフレーム id に付いている。
- project.json に base64 もストローク座標もない。print に NAME / DRAFT / 候補が出ない。
- M0 で作ったネームを取り込むと、PanelSpec が葉に付き、脚本が `studio.script` に入る。

変える既存テスト: なし（見込み）。


### M4 生成依頼パックと画像の取り込み（オフライン E2E）— 約 4 人週

範囲: 絵の工程の道具をそろえ、偽エージェントと試験用の画像で企画から書き出しまでを通す。

**実施状況（2026-09-24）:** M4-1〜M4-6 を実装した。
- 依頼パック（`studio/genreq.py`）: サイズ（約 1 MP・64 の倍数、`tools.json` のサイズと切り方、印刷に要る画素数）、プロンプトの下書き（ja・en・tags。キャラの `tokens_en` はそのまま）、描かせないもの、文字よけの範囲、人物の位置、ガイド画像と参照の写し、修正用の元画像とマスク。同じ内容なら同じ id。
- 取り込み（`studio/importer.py`）: inbox の中のファイルとアップロード済みの資産だけを読む。30 MB・64 MP まで、同じ画像は 1 回だけ、来歴は必須。取り込み時の目安（縦横比のずれ、文字よけの範囲の混み具合、明るさ、ガイド追従度）で候補を並べる。
- MCP の道具: `generation_request`、`import_images`、`candidates`、`review_candidates`、`adopt`、`request_fix`、`report_regions`、`finish_page`、`preflight`、`export_proof`、`ask_human`。`render` に `kind`（compare・ガイド）、`next` に `claim` を足した。
- 作業リストは書き出しまで出る。上限（1 コマ 8 枚・修正 2 巡）を超えたコマと、`ask_human` で相談中の作業は止まる。
- 設定画: 依頼 → 取り込み → 人間の `approve sheet` で顔を切り出して `face` の参照にし、キャラを `locked` にする。場所の参照画像は `adopt location_id`。
- 書き出し: `preflight`（承認、未採用のコマ、位置のない台詞、資産の欠落、実効 dpi 350 未満、試験用の画像。来歴の欠落は警告）、校正の透かし入り書き出し、人間の `genko studio export`。
- HTTP: `POST /v1/assets`（画像のアップロード）、`GET /v1/requests/{id}/files/{name}`。
- 試験: 偽エージェント（`tests/agents/`）が MCP だけで 8 ページを企画から書き出しまで作る E2E（§10.6 の a〜d、g、h）、依頼パックの golden、取り込みの拒否と冪等性、予約、応答 2 秒以内。

設計からの変更点:
- ガイドはネームの NAME ストロークに加えて blocking の人物の形を描く（今のネームはストロークを持たないため）。
- `finish_page` の写植の再調整は、報告された顔の領域に重なる台詞だけを動かす。
- 顔の切り出しは、承認時に人間が `--face x,y,w,h` で範囲を指定できる。省略すると画像上部の中央を切り出す。
- 予約は弱い予約で、書き込みは止めない。

後回しにしたもの:
- MCP の HTTP 接続そのもの（M1-7）。画像のアップロードと依頼ファイルの取得は、今の HTTP サーバーで先に使える。
- 本番の 600 dpi 書き出しを MCP に出すこと（人間の CLI だけ）。
- `assist` の自律度（提案モード）。

| PR | 内容 |
|---|---|
| M4-1 | `blocking.py`、`guide.py` の composition / pose / keepout / mask / compare、`vocab.json` |
| M4-2 | `genreq.py`（依頼パック: サイズの候補、プロンプトの下書き、描かせないもの、参照の写し、同じ内容なら同じ id）、`open_request` / `close_request`、`tools_registry.py`（`tools.json`） |
| M4-3 | `importer.py`（`inbox/` のパス、アップロード済み資産、検証、上限、冪等）、`import_candidates`、取り込み時の目安（縦横比、文字よけ、輝度、ガイド追従度） |
| M4-4 | MCP の道具: `generation_request`、`import_images`、`candidates`、`review_candidates`、`adopt`、`request_fix`、`report_regions`、`finish_page`、`preflight`、`export_proof`。worklist を書き出しまで広げる。予約（claim） |
| M4-5 | キャラ設定画の流れ（依頼 → 取り込み → 人間の `approve sheet` → 顔の切り出し → `locked`）、場所の参照画像 |
| M4-6 | export の preflight（承認、fixture の拒否、実効 dpi）、proof の透かし。SKILL.md に絵の手順を足す |

受入基準:

- §10.6 の (a)〜(e)、(g)、(h) が CI で、ネットワークなしで通る（8 ページの PDF と TIFF）。
- 依頼パックの golden が安定している。道具の応答が 2 秒以内。
- 本番書き出しは fixture の画像と未確認のツールで拒否される。

### M5 最初の1ページ（Hermes Agent + ChatGPT）— 約 3 人週 + D5

範囲: 実際のエージェントと画像ツールで、パイロットページを art 承認まで持っていき、依頼パックとスキルを直す。

**実施状況（2026-09-24）:** コードで用意できる部分（M5-2〜M5-4 と、M5-1・D5 の記録と集計の道具）を実装した。M5-1（実際の Hermes Agent + ChatGPT での通し）と D5（評価者による盲検）は人間が行う作業で、利用者の指示どおり全マイルストーンの実装後に行う。手順は `docs/STUDIO_EVAL.md`。
- パイロットページ: 作業リストはパイロットページ（既定は 1 ページ目）の作画から出し、その作画が承認されるまで他のページの作画を止める。承認時に、最も使われた画像ツールと参照画像（最も大きいコマの採用画像）を `studio.style.locked` に固定し、以後の依頼パックは既定でそのツールを使い `refs/style_pilot.png` を添える。パイロットの作画承認を取り消すと固定も外れる。
- 複数人物のコマ: 依頼パックに手順（`steps`: 構図全体 → 領域の報告 → 人物ごとの inpaint）を入れる。`focus_character` でその人物の領域だけをマスクにし、その人物の参照だけを添え、プロンプトの先頭に「直すのはこの人物だけ」を入れる。`regions: ["face:<人物>"]` で顔だけを直せる。
- 人間の確認: review.html に、承認依頼・相談の一覧と操作のコマンド、設定画の候補と承認コマンド、コマごとの候補の比較（採用・評価・来歴・試験用の印）、preflight の状況を載せた。エージェントは `review_page` で作って場所を人間に知らせる。人間は `close-ticket --reply` で相談に答える。
- 記録: MCP の道具の呼び出しを `studio/logs/tools.jsonl` に残し、`genko studio stats` で集計する（ネームの出し直し、1 コマの取り込み枚数の中央値、失敗した呼び出しと理由、応答時間）。
- 監査: 承認の変化を `studio/audit.jsonl` に actor 付きで残し（間引かない）、`genko studio audit` で人間以外の変化を見つける。受入基準の「エージェントが一度も承認していない」は journal ではなくこの記録で確かめる（journal は最新 100 件に間引くため）。
- D5: `genko studio eval-sample`（1 人だけが写った採用済みのコマをキャラごとに最大 10、台詞なし、設定画は A・B・C の文字だけ）と `eval-score`（評価者ごと・キャラごとの正答率、90% で合格）。
- スキル: SKILL.md を作画・直し・承認の頼み方まで書き直し、パッケージ内の正本を MCP の resource `genko://guide/skill` で出す。`config.example.yaml` に役ごとの道具の絞り方を足した。

設計からの変更点:
- 見つけて直した穴: 人間が undo した承認を、エージェントが redo で戻せた。undo/redo で承認が変わる場合は、人間以外を拒否する。

実際の通しの後に行うこと（M5-1・M5-2 の残り）:
- 記録（`stats`、`docs/eval/`）をもとに、依頼パックの文言、参照画像の並べ方、`tools.json` の既定値、SKILL.md を直す。

| PR | 内容 |
|---|---|
| M5-1 | 手動の通し: Hermes Agent（LLM は Claude か ChatGPT）+ ChatGPT の画像生成で、パイロットページを art 承認まで。道具の選び間違い、依頼パックの読み違い、取り込みの失敗を記録する |
| M5-2 | 依頼パックの文言の調整（プロンプト下書き、文字よけの伝え方、参照画像の並べ方）、`tools.json` の既定値の見直し、SKILL.md の改訂 |
| M5-3 | 複数人物のコマ（構図 → 人物ごとの編集）、顔の修正（`mode:"inpaint"` の顔マスク）、パイロットページとスタイル固定 |
| M5-4 | 人間の確認の道具: review.html の候補の比較、承認依頼の一覧、Hermes のメッセージ連携で review.html の場所を知らせる手順（承認そのものは人間の CLI） |
| D5 | 盲検のキャラ同定の標本（§10.8） |

受入基準:

- パイロットページの全コマが採用まで行き、1コマの取り込み枚数の中央値 ≤ 8。
- D5 で盲検の評価者が同じキャラを 90% 以上同定する。届かなければ、設定画の渡し方と編集による修正の手順を見直す。
- エージェントが一度も承認を付けていない（journal で確かめる）。

### M6 印刷できるモノクロ — 約 4 人週

**実施状況（2026-09-24）:** M6-1〜M6-6 を実装した。PSD を CLIP STUDIO PAINT と Photoshop で開く確認は人間の作業で、手順は `docs/PSD_CHECKLIST.md`（未実施）。
- 網点（`screentone.py`）: 整数格子の網（画面ベクトル (m, n)、1 辺 m²+n² 画素のタイル）を、網点の中心からの距離の順位で作ったしきい値の配列で描く。被覆率は 150 / 300 / 600 dpi、10〜90% で ±1% 以内（試験は ±3%）。50% を超えると黒地に白抜きになる。ノイズの素材は誤差拡散。
- 仕上げ: 配置した絵を、描画のときに輝度 → levels → 線のマスク → ベタと白 → 3 段の平網に量子化する。print は 1bit 相当の白黒、proof は網点なしの量子化したグレー。設定は `studio.style.finish` と `set_finish`。カラーのページは飛ばす。既存のトーン層も同じ関数で描き直した。
- 線抽出（`lineart.py`）と `derive`（kind lineart）: 局所の最大値で割って背景を消し、しきい値で切り、小さな点を落とす。Genko の決定的な処理として候補にし（来歴 `genko`、枚数の上限に数えない）、`to: "ink"` で絵の上に置ける。`upscale_panel` の作業に `derive` を足した。
- 写植の顔よけ: `finish_page` が報告された顔の箱に重なる台詞を動かす（M4）。試験用の顔の箱で、4 ページの全台詞と顔の重なりが 0 であることを試験で確かめた。
- フキダシ: 楕円は文字の箱を外接して囲む（名前の写植の寸法も同じ規則で測る）。尾の根元は短辺の 1/3（弦の長さで測る）で、話者の方の辺から出る。叫び・ささやき・心の声の形、SFX（縁取りの大きな文字、フキダシなし）、ルビはすべての組を親文字の横に揃える。効果線はコマでクリップし、集中線は中心を空ける。
- 書き出し: print・pack・PSD の既定を `spec.dpi` にした（pack の 150 dpi の上限を外した）。ファイル名を Windows で使える形にする。EPUB は EPUB 3 の固定レイアウトで、右綴じは rtl、見開きの左右を指定する（作業用の一時フォルダも残さない）。
- PSD: ページごとに 1 ファイル、実レイヤー（紙、配置した絵（仕上げ前のグレー）、ラスタ、ペン入れ、トーン、効果、コマ枠、台詞ごと、ノンブル）。レイヤー名は Unicode（`luni`）、レイヤーは中身の範囲に切り詰め、解像度を記録する。psd-tools（開発用の依存）で読み戻す試験を足した。

設計からの変更点:
- 尾の根元は設計の「短辺の 1/5 以上」より太い 1/3 にした（1/5 では長い尾が線にしか見えなかった）。
- PSD の絵は印刷用の網点ではなく仕上げ前のグレーで入れる（ペイントソフトでトーンを貼り直せるように）。合成画像は印刷と同じ。

| PR | 内容 |
|---|---|
| M6-1 | `screentone.py` と render の仕上げ、`_draw_tone` の置き換え |
| M6-2 | `lineart.py` と `studio derive`、高解像度化の依頼（`mode:"upscale"`）と LANCZOS の予備経路、`upscale_panel` の worklist |
| M6-3 | 顔を避けた写植の再配置（`report_regions` の箱を使い、`move_line` の提案） |
| M6-4 | フキダシ描画の修正（尾、楕円の余白、ルビの揃え）、SFX 種別、効果のクリップ |
| M6-5 | 書き出しの修正: pack と print の既定を `spec.dpi` に（pack.py の 150 上限を外す、CLI `--dpi` の既定を `spec.dpi` に）、preflight のコマごとの実効 dpi、ファイル名の無害化（Windows で使えない文字）、EPUB の rtl と固定レイアウトのメタデータ |
| M6-6 | PSD をページごとの実レイヤーに書き直す（配置した絵・INK・写植、レイヤー名は Unicode）。CLIP STUDIO と Photoshop で開けることを確かめ、確かめられない場合は PSD を書き出し形式から外す |

受入基準:

- 被覆率が ±3%。パイロットページの2値 TIFF に灰色の画素がない。pack が 600 dpi で出る。
- 試験用の顔の箱でフキダシと顔の重なりが 0。
- PSD を CLIP STUDIO と Photoshop で開き、レイヤー名とレイヤー数が期待どおり（手動チェックリスト）。

変える既存テスト: `tests/test_p5_psd.py`（レイヤー数の期待を「ページの実レイヤー数」に変える）、`tests/test_p5_pack.py`（600 dpi の B4 は CI で重いので `dpi=72` を明示する）。実際に変えたもの: この 2 本に加え、`tests/test_p7_factory.py`（空のユーザーレイヤーは PSD に出ないので、Unicode のレイヤー名を確かめる）、`tests/test_studio_units.py`（楕円のフキダシの寸法が √2 倍になった）、`tests/test_m3_art.py` と `tests/test_m4_pipeline.py`（色で配置を確かめる箇所だけ白黒仕上げを外す）。

### M7 GUI（人間の確認と承認）— 約 4 人週

**実施状況（2026-09-24）:** M7-1〜M7-4 を実装した。M7-5（承認リンク）は任意のため入れていない。画面2の流れと承認箱の手動チェックリストは `docs/GUI_CHECKLIST.md`（人間が行う、未実施）。GUI の自動試験は画面なし（offscreen）で CI でも走る。
- セッション（`app/session.py`、Qt を使わない）: メモリ上の正本に `human:<名前>` の op を適用して即座に表示し、1 秒操作が無いとき・ページ切替・承認の直後・終了時にまとめて保存する。保存前に revision を確かめ、エージェントが保存していれば読み直して自分の未保存の op を再生する（rebase）。再生できない op は上書きせず、一覧で知らせる。`QFileSystemWatcher` で project.json の変化を見て取り込む。元に戻すは、未保存の op ならメモリで、保存済みなら自分の最後の保存を journal で戻す。
- 工程バー、承認箱（1 件ずつプレビューを見て承認か差し戻し。一括ボタンは無い）、起動画面（最近のプロジェクト）。承認箱の中身と、承認・差し戻しの op は `app/review_model.py`（Qt を使わない）に置き、試験した。差し戻しはエージェントの `next` に出る指示になる（ネームはページの修正、作画はコマの修正、設定画は `reject_sheet`、相談は返事）。
- コマ表示（印刷 / 校正 / 比較 / 元画像）、候補の一覧と来歴（ツール、モデル、実際のプロンプト、依頼、親、評価）、採用、指示欄（コマに pinned で残し、修正のチケットにする）、領域エディタ（`source:"user"`）、キャラと背景のライブラリ。編集画面の下に印刷どおりのページを表示する。
- 抜け道の修正: コマの選択は `select_frame` の op にした。フキダシのドラッグ中は表示だけを動かし、離したときに `move_line` を送る。GUI の actor は `human:<名前>`（`$GENKO_USER`、無ければログイン名）。
- 並行する保存の試験: GUI の未保存の変更とエージェントの保存が両方残ること、消えた台詞への移動が衝突として報告されること、承認箱からの承認が人間の actor で監査記録に残ることを、サービス層と offscreen の GUI の両方で確かめた。

設計からの変更点:
- まとめ保存は裏のスレッドではなく、画面のスレッドでタイマーから行う（保存は数十ミリ秒で終わり、スレッドをまたぐとセッションの排他が要るため）。
- 採用後に報告された顔や人物の領域は、コマの指示の hash（`brief_hash`）に含めない（採用した絵が「指示が変わった後の候補」になってしまうため）。人が描いた領域だけが指示として数える。
- 設定画の差し戻しのために人間専用の op `reject_sheet` を足した。

| PR | 内容 |
|---|---|
| M7-1 | メモリ上の正本のセッション、裏のまとめ commit、差分保存、`QFileSystemWatcher` の再読込と op の再生 |
| M7-2 | 工程バー、承認箱（依頼の一覧、1件ずつの承認と差し戻し）、起動画面（最近のプロジェクト） |
| M7-3 | コマ表示（元画像 / 比較 / 候補 / 採用中）、候補グリッドと来歴、指示欄、領域エディタ、キャラと背景のライブラリ |
| M7-4 | GUI の抜け道（main.py:371、canvas.py:150-154）を op に直す。rebase undo |
| M7-5 | （任意）承認リンク: `genko serve` が人間用の短命なリンクを発行し、ブラウザで開くとプレビューと承認ボタンが出る。エージェントはリンクを人間に届けられるが、押せるのは人間のトークンを持つブラウザだけ |

受入基準:

- 画面2の流れ（指示 → 候補 → 採用 / 修正 → 履歴）と承認箱の手動チェックリストが通る。
- 並行するエージェントの commit を GUI の保存が消さない（サービス層の試験）。

### M8 アタリ取り込み（画面2の入口）— 約 3 人週 + D8

**実施状況（2026-09-24）:** M8-1〜M8-4 を実装した。D8（スキャンした実際のアタリ 20 ページ以上と正解）は人間が用意するデータで未実施。評価の道具（`genko studio eval-atari`）と手順（`docs/STUDIO_EVAL.md`）は用意した。
- 取り込み（`studio import-name`、MCP の `import_name`）: スキャンは不変の資産（来歴 `self`）になり、DRAFT に置かれる（印刷されない）。位置合わせは `page`（紙全体）・`live`（描いた範囲を版面に合わせる）・`auto`（形が版面に近ければ live）。
- コマの検出（`studio/xycut.py`、Pillow だけ）: 1 mm あたり 4 画素に縮め、二値化し、3 mm 未満の塊（ゴミ、消しくず、台詞の走り書きの点）を落とし、溝で再帰的に切る。溝は「ほぼ空の行・列」で、両側にコマの枠線（溝と平行な長い線）があるときだけ切る。角の線のはみ出しは溝を塞ぐので、溝を探すときは領域の両端 4 mm を見ない。葉はインクの範囲に縮め、枠線の囲み具合と溝の綺麗さから信頼度を出す。結果は段組 DSL（段 → 列 → 行）にもなる。
- 精度（合成データ）: 手描き風のアタリ（揺れる線、途切れ、角のはみ出し、人物のアタリ、台詞の走り書き、ゴミ）6 種類の段組で、150 dpi・300 dpi とも全コマが 5 mm 以内で復元された（試験は 90% 以上を要求）。余白付き・縮尺違いのスキャンも `auto` で合った。
- 提案: 解析の全出力を `studio/analysis/<ページ id>/` に JSON と重ね画像で置き、コマ割りの提案にする。手書き台詞はエージェントが読んで `propose_lines`（位置は mm かアタリ画像の中の 0..1）で提案する。どちらも人間が確定するまで原稿を変えない。確定は人間専用（`accept` / `reject`、承認箱）。確定したコマ割りは `set_layout` で検出した矩形のまま作り、台詞は中心の入るコマに付く。既にコマ割りがあるページへの確定は force が要る。再解析は提案を足すだけで、人が描いた領域（`source:"user"`）には触れない。
- 重ね表示: `render kind=atari`、review.html、GUI の承認箱（確定 / 却下）。
- 作業リスト: アタリのページは `plan_page` の代わりに `read_atari` → `brief_panels` → ネーム承認待ち。提案は `waiting_for` の `proposal` に出る。アタリのページだけなら脚本は要らない。

設計からの変更点:
- 段組の提案は DSL の比率ではなく、検出した矩形の木をそのまま `set_layout`（新しい op）で作る（DSL の既定の溝幅に丸めると 5 mm の基準を外れうるため）。DSL の形も解析結果に残す。
- 傾いたスキャンの補正（deskew）は入れていない。D8 の実データで必要なら足す。

| PR | 内容 |
|---|---|
| M8-1 | `studio import-name`（一括、原本は不変資産、来歴 `self`、DRAFT に配置、ページ mm への位置合わせ） |
| M8-2 | `XYCutDetector` → 段組の提案（信頼度付き、`studio/analysis/` に全出力） |
| M8-3 | 手書き台詞の読み取りはエージェント: MCP の道具 `propose_lines`（台詞と位置の提案。dry_run の重ね表示）、`report_regions` |
| M8-4 | 提案の重ね表示と人間の確定（review.html と GUI） |
| D8 | スキャンしたアタリ 20 ページ以上と正解（§10.8） |

受入基準:

- D8 で、コマの 90% 以上が完全一致か 5 mm 以内で復元される。
- すべて提案として出て、人間の確定まで原稿を変えない。`source:"user"` の領域は再解析で消えない。

### M9 拡張 — 約 3 人週

**実施状況（2026-09-24）:** M9-1・M9-3・M9-4 を実装した。M9-2（ローカルの内容安全の分類器）は、内容の制限はかけない（描く内容はネームと絵を作る AI で決まる）という決定に従い、実装しない。
Claude Code などでの通し確認は人間が行う（手順は `docs/OTHER_AGENTS.md`）。

- M9-1: `genko export --format webtoon|sns` と `genko studio export --format webtoon|sns`（`src/genko/profiles.py`）。裁ち落としを切り、網点にせず、sRGB を付ける。webtoon は縦長を上限の高さで切って出す（1 本の画像を丸ごとメモリに持たない）。カラーのページは印刷でも網点にせず、依頼パックは `color: true`、カラーの絵柄の下書き、`avoid` に「色」を入れない。
- M9-3: `src/genko/mannequin.py`（首・肘・膝を足した関節、身長 = 8 頭身、`rot` の傾き・回転、横向きのプリセット）。ネーム・校正の描画とポーズのガイド画像が同じ骨組みを使う。コマの中のマネキンは依頼パックの `guides/pose.png` に描かれる。
- M9-4: `docs/OTHER_AGENTS.md`、`integrations/claude-code/`（`.mcp.json` の例、道具の名前を `mcp__genko__` にしたスキル、人間用コマンドを止める permissions の例）、`integrations/generic/mcp_client_example.py`（公式 Python SDK の最小の通し。テストで実行する）。

| PR | 内容 |
|---|---|
| M9-1 | カラーの出力 profile（webtoon・SNS 向け。screentone を飛ばす） |
| M9-2 | ローカルの内容安全の分類器（任意。エージェントがローカルの画像モデルを使う場合向け） |
| M9-3 | マネキンの拡張（肘・膝・首、size と rot の描画）とポーズのガイド画像 |
| M9-4 | 他のエージェント（Claude Code、ほかの MCP クライアント）向けの手順書と、Hermes 以外での通し確認 |

---

## 12. リスクと未決事項

### 12.1 リスク

| リスク | 対策 |
|---|---|
| **エージェントの出来が LLM と設定で大きく変わる** | Genko 側の検査を厚くする（スキーマ、lint、JSON ポインタ付きの指摘と直し方）。スキルと MCP resource に規則と例を置く。評価（§10.8）で道具の選び間違いを数え、道具の説明を直す |
| **道具が多すぎて LLM が選び間違える** | 道具は約 25 に絞り、細かい操作は `apply_ops` にまとめる。worklist の項目に `tools` の目安を付ける。Hermes の `tools.include` で役ごとに絞る |
| **画像のバイト列が LLM の文脈に入る** | 画像はファイルかアップロードで受け渡し、引数に base64 を受けない（§2.2-8）。道具の応答の画像は縮小プレビューだけ |
| **Hermes の画像ツールの前提が未確認**（ChatGPT の画像生成を呼べるか、参照画像を渡せるか、指定のフォルダに保存できるか） | M0 の手動チェックリスト（D0）で確かめる。できない場合は、エージェント側に小さな道具（画像生成 API を呼んで inbox に保存するスクリプト）を足すことを M5 の前提にする |
| **Hermes が MCP の画像コンテンツをモデルに見せるか不明** | 画像は必ずファイル（同じマシン）か URL（別マシン）でも返す。M0 の手動確認で実際の挙動を確かめ、文書に書く |
| **画像ツールに seed も ControlNet も無い**（ChatGPT の画像生成など） | 構図は参照画像（ネームの切り抜き、人物の位置の図）と文章の両方で伝える。キャラは設定画を参照に渡し、編集で直す。再現は求めず、取り込んだ画像を正とする。ガイド追従度の目安で外れた候補を下位に回す |
| キャラの一貫性は依然として最難関 | 設定画の承認（ゲート①）、固定の記述、編集による修正、パイロットページでのスタイル固定。M5 で数値で測る（D5） |
| mono の線画の品質が商業水準に届かない | グレースケールの漫画調を直接作らせる。決定的な仕上げを全コマ同一にする。ゲート③は人間 |
| 解像度の差（1〜2 MP の生成と 600 dpi の印刷） | 実効 dpi をコマごとに記録する。高解像度化の依頼と LANCZOS の予備経路。preflight で達成 dpi を示す |
| **エージェントが人間の承認を装う** | MCP サーバーの actor を `ai:*` に固定し、承認の道具を出さない。`apply_ops` 経由の承認も actor で拒否する。CLI の actor は自己申告なので、エージェントに端末の道具を持たせる場合は、Genko の CLI を人間の名前で呼ばないようスキルに書き、HTTP では人間用のトークンをエージェントに渡さない。本当の分離が要るなら承認リンク（M7-5）か、承認を人間の鍵で署名する（§12.2 の 7） |
| **エージェントの入力による破壊**（巨大な入力、`--root` の外のパス、壊れた画像） | 入力の上限、パスの閉じ込め、画像の検証、dry_run が既定（§2.2-13、§6.4） |
| 別マシン運用での盗み見と改ざん | トークン必須、TLS は前段で付ける、`--allow-host`、既定は `127.0.0.1` に bind |
| エージェントとGUIの並行書き込み | `revision`、ロック内ロード、`expect_revision`、GUI のメモリ上のセッションと裏のまとめ commit、人間のロックはエージェントが外せない |
| **apply と保存の性能**（undo 履歴ごとの deepcopy、ストロークの肥大） | M1 で undo 履歴を写さない、Stroke を不変にして共有、性能試験。M2 でストロークを blob に、差分保存、project.json の大きさの試験 |
| **HTTP 経由の CSRF・DNS rebinding・任意ファイルの読み書き** | M1 で全経路にトークン、Origin / Host の検査、JSON 限定、CORS `*` の削除、`--root` の閉じ込め |
| 古いビルドが v3 ファイルを壊す | 版ゲートを先のリリースで出す（M1-5） |
| 生成物の記録 | 候補ごとに来歴（ツール、モデル、プロンプト）を残す。公開先が求める場合に使える |
| 資産の肥大化 | `genko gc`（ロックを取る、参照されていない、30日より古い）。却下した候補は履歴として残し、2000 件で退避する |
| フォント依存の決定性 | フォントを package data として同梱し、写植の結果は op の明示座標として保存する |
| **Windows 固有の問題** | 再試行つきの置換、長いパス、UTF-8 の stdout と `--ascii`、Windows CI、日本語パスでの MCP の起動試験 |
| 参考UIの全機能を追う範囲の膨張 | 道具が先、GUI は後（M7）。GUI は確認と承認に絞る |

### 12.2 実装上の未決事項（推奨既定つき）

| # | 決めること | 推奨既定 |
|---|---|---|
| 1 | PanelSpec の置き場 | `Frame.panel`（型は `studio/state.py`）。M0 の間はサイドカー |
| 2 | 候補メタの置き場 | project.json（小さなレコード、undo が効く）。2000 件を超えたら却下分を `studio/history/` に退避 |
| 3 | MCP の実装 | extra `mcp` の公式 Python SDK。依存が重荷になったら stdlib の JSON-RPC に替える（StudioService との間は薄い包みにしておく） |
| 4 | 道具の粒度 | 約 25 の道具 + `apply_ops`。道具を増やすより、worklist の `tools` の目安とスキルで導く |
| 5 | 書き込む道具の既定 | dry_run（`commit:false`）。エージェントは指摘が無くなってから `commit:true` |
| 6 | エージェントに採用（adopt）を許すか | 許す（`gated`）。ページの確定（art ③）は人間。より厳しくしたいプロジェクトは `assist` |
| 7 | 承認の本人性 | 当面はワークフローの守り（MCP に承認の道具が無い、actor の固定、HTTP のトークン）。必要になれば承認リンク（M7-5）か、承認を人間の鍵で署名する |
| 8 | 複数 actor のもとでの永続 undo | M2 は「自分の最新 commit だけ戻せる」線形 undo。M7 で rebase undo |
| 9 | 斜めコマ・多角形コマ | 当面は長方形で近似。多角形フレームは M9 以降、需要を見てから |
| 10 | ネームの既定のラフ | 構造的（人物の箱 + 向き + prim）。画像ツールのラフはページ単位の任意 |
| 11 | mono の戦略 | グレースケールの漫画調を直接作らせ、Genko が決定的に仕上げる |
| 12 | 描き文字（SFX）を画像ツールで作るか | v1 は Genko の装飾文字。画像ツールの描き文字は M9 以降に検討 |
| 13 | 1コマの枚数と修正巡回の既定 | 1 回の依頼で 2〜4 枚、上限 8 枚、修正 2 巡。超えたらチケット |
| 14 | ロックの方式 | OS のファイルロックを主、`O_EXCL` + token を予備 |
| 15 | PSD の扱い | M6 でページごとの実レイヤーに直す。確かめられなければ形式の一覧から外す |
| 16 | 見開きの開始側 | `start_side:null`（綴じ方向の既定）。雑誌の扉の都合だけ `set_meta` で上書き |

---

## 13. ユーザー確認事項

### 13.1 決まったこと（2026-09-24）

| # | 決めたこと | 内容 | 設計への反映 |
|---|---|---|---|
| 1 | 主な入口 | テキスト起点（企画 → 脚本 → ネーム） | M0 がネームの道具。アタリ取り込みは M8 |
| 2 | 出力の形 | モノクロの紙の原稿（B4、600 dpi の2値） | スクリーントーンと2値の書き出しが本線。カラーは M9 |
| 3 | AI の組み込み | **Genko に Claude や ChatGPT を組み込まない。** Hermes Agent などの外部エージェントが操作し、ネーム出しや画像生成はエージェント側の Claude・ChatGPT で行う | 第3版の全体（§0.4）。Genko は外部と通信しない |
| 4 | 接続方法 | MCP | M0 から MCP サーバー（stdio）。CLI・HTTP も同じ機能 |
| 5 | 人の承認 | 要所で人が承認（キャラ設定画、ネーム、ページごとの絵、書き出し） | 既定プリセット `studio`。MCP に承認の道具を出さない |
| 6 | 画像生成 | 今は ChatGPT（エージェント経由）。変える可能性あり | 依頼パックはツールに依らない形。`tools.json` でツールごとの対応サイズと機能を持つ |

### 13.2 決めなくてよいこと

次の点は、Genko の設計では決めずに済むようにした。

- **Hermes と Genko を同じマシンで動かすか。** 回答が無くても進められる。同じマシン（stdio）でも別マシン（HTTP）でもつながるように作り、M0 は同じマシンで作る。別マシンで使うことになったら M1-7 を先に出す。
- **販売するか、AI 利用を開示するか。** Genko は関与しない。公開は人が行い、必要なら人が「生成AI使用」と記載する。
- **表現の方針。** Genko は制限を掛けない。何を描くかはネームや画像を作る AI 側で決まる。
- **参照画像の出どころ。** Genko は参照画像の使用を制限しない。出どころのメモは整理用の任意項目。
