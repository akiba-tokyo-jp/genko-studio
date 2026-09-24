# Genko AI 制作パイプライン 設計

対象: genko-studio @ `7e65e4c`（Python 3.11+、core 依存は Pillow のみ、GUI は PySide6 extra）。
本書は設計である。この文書の時点でリポジトリのコードは変えていない。
読者: Genko の実装者、Genko を操作する AI エージェント、原稿の承認者（人間）。
表記: 行番号は `7e65e4c` のもの。op 名・JSON キー・コードは英語、説明は日本語。
改訂: 第2版。批評の指摘 27 件（undo 履歴ごとの deepcopy、ストロークによる project.json の肥大、HTTP の CSRF と DNS rebinding、部品ごとのライセンス、画像の内容安全、著作権と来歴、外部送信の既定、ロックの競合、ロードマップの順序ほか）を反映した。数値は改訂時に `7e65e4c` で再計測した。利用者が決めることは §13 にまとめた。

---

## 0. 目的と要約

### 0.1 目的

生成AIが主体となって、企画 → 脚本 → ネーム → 作画（生成・編集）→ 仕上げ → 書き出しまでを進める。
人間は要所のゲートで承認し、どの段階でも同じコマンドバスで直せる。

参考は MANGA BRIDGE v2 の2画面である。

- 画面1（`1.jpg`）: 製品モック。プロンプトとモデル中心のコンソール。
- 画面2（`2.jpg`）: 手描きアタリ起点の操作シミュレーション。コマ単位の「指示 → 候補 → 採用／修正指示 → 履歴」。

Genko は画面2の流れを採る。ヘッドレス（AI とエージェント向け）が主、GUI は確認・比較・採用・修正のための従である。

最初に届ける価値は「Claude が書くネーム」で、今の保存形式の上に薄く載せる（§11 の M0）。画像生成はその後に積む。外部送信、商用利用と画像モデル、自律度と内容の方針は利用者が決める（§13）。

### 0.2 要約（1画面）

- **分業。**
  - Claude: 構造化された判断。bible、脚本、ネーム計画（段組 DSL）、コマ指示（PanelSpec）、プロンプトの方言化、批評。
  - 画像モデル: 1コマずつの**絵だけ**。文字・フキダシ・効果音は描かせない。
  - Genko の決定的コード: コマ幾何、読み順、縦書き写植、フキダシ配置、ガイド画像、配置とクリップ、スクリーントーン、書き出し。
  - 人間: ゲートの承認と、いつでも可能な修正。
- **作業単位はコマ**（葉 Frame）。`Frame.panel`（PanelSpec）に指示・参照・領域・候補・採用を持つ。
- **単一コマンドバス。** 変更はすべて `genko.ops.apply_ops`。AI 専用の書き込み経路は作らない。
- **画素境界。** 生成は apply_ops の外、`project.lock` の外で走るジョブである。結果は `assets/` に内容アドレスで置き、op は hash を参照するだけ。ページに画素が入るのは `adopt_candidate` と `place_asset`（`put_raster` の一般化）だけ。Genko は画素を発明しない。
- **ゲート。** 既存の `name_ok` に `sheet`・`script`・`art`（ページ単位 `art_ok`）・`export` を加える。ゲート op は `ai:*` を拒み、studio プロジェクトでは actor を名乗らない呼び出し（`legacy:unknown`）も拒む。おまかせ運用でも AI が付けられるのは仮承認（provisional）だけで、本番書き出しの前に人間が仮承認をページごとにプレビューで確かめる（一括確定はない）。
- **来歴とライセンス。** 生成画素はすべて recipe（backend・workflow hash・モデル・seed・プロンプト・制御画像）と job を持つ。recipe は使った部品（チェックポイント、LoRA、ControlNet、IP-Adapter と CLIP-vision、アップスケーラ、検出器、分類器、カスタムノード）ごとに id・hash・ライセンス・商用可否を持つ。商用モードでは、未確認か非商用の部品を使った採用画像が1枚でもあれば書き出しを拒む。`ai_manifest.json` は既定で内部用。
- **外部送信は利用者が明示的に選ぶ。** `studio init` で必ず選ばせ、黙った既定値は置かない。推奨は「テキストだけ Claude に送り、画像は送らない」。`GENKO_OFFLINE=1` では、ローカルでない provider はすべて例外を上げる。locality は設定の自己申告ではなく、解決した接続先ホストから導く（loopback だけが local）。
- **内容安全。** 画像側は既定ネガティブ、ローカルの出力分類器、judge の `safety` 軸を常に使う。bible の年齢で 18 歳未満のキャラが写るコマで性的と判定された画像は、候補として記録せず採用もできない（監査記録だけ残す）。参照素材と LoRA は来歴（origin・権利者・ライセンス）が必須。
- **非破壊。** 候補は消さない。採用の取り消しは `unadopt` か別候補の採用。取り込んだアタリは不変資産。
- **構造から導出。** Genko で作ったネームなら、マスク・文字よけ・ポーズ・構図ガイド・生成サイズはデータから計算する。画素検出は取り込んだアタリにだけ使う。
- **オフラインで試験。** FakeLLM・MockImageBackend・MockVision・MockSafety・スタブ ComfyUI を使う。`anthropic` と WebSocket クライアントは extra に置き、ComfyUI 用 HTTP は stdlib `urllib` で書く。
- **価値が先、土台はすぐ後。** M0 で「Claude が書くネーム」を今の v2 形式の上に薄く出す（新しい書式・資産・op なし。既存 op へコンパイルする）。M1 で既存の欠陥（undo 履歴ごとの deepcopy、HTTP の CSRF と DNS rebinding、ロックの競合、actor の既定、フォントの同梱漏れ）を直す。M2 で v3 形式（page id、内容アドレスのラスタとストローク、op の journal、スキーマ登録簿）へ移る。
- **最初の画像。** M4 でモックだけの 8 ページ PDF を作って生成側の契約を凍結し、M5 で ComfyUI による最初の 1 ページを完成させる。M6 で印刷品質（600 dpi の 2 値、pack、PSD）に届かせる。

### 0.3 アーキテクチャ図

```text
        人間（GUI / CLI）                        外部エージェント（Claude Code / MCP / HTTP）
             │ ops・承認（human:*）                     │ tools（ai:*）。HTTP は token + Origin/Host 検査
             ▼                                          ▼
 ┌──────────────────── StudioService  (genko/studio/service.py) ─────────────────────┐
 │  PolicyGate: locality（GENKO_OFFLINE）・license・content safety・budget（予約制）   │
 │  worklist.next_actions(ep, jobs)  ──▶  stages/*  (決定的コード + LLM 1呼び出し + 検証)   │
 │        │                                   │                                      │
 │        │                    providers ─────┼──────────────────────────────┐       │
 │        │                      LLMProvider   ImageBackend        VisionAnalyzer    │
 │        │                     (Fake/Anthropic) (Mock/ComfyUI)   (Mock/XY-cut/Claude) │
 │        │                                   │                 SafetyClassifier(local)│
 │        ▼                                   ▼                                       │
 │  JobStore  studio/jobs/*.json        AssetStore  assets/ab/<sha256>.png|json (不変)  │
 │  Ledger    studio/ledger.jsonl       GPU scheduler (profile 単位でまとめる)             │
 └────────┬───────────────────────────────────────────────────────────────────────────┘
          │  commit.py:  lock(OS ロック + token) → revision 確認 → load → ops 生成 → dry_run → apply → 差分保存
          ▼
 genko.ops.apply_ops   単一コマンドバス（全か無か, dry_run, _validate(触れた範囲), actor, page lock）
          │             undo 履歴は deepcopy しない
          ▼
 project.json v3  判断だけ: bible・script・PanelSpec・候補メタ・採用・承認・revision（ストロークとラスタは hash 参照）
 studio/journal.jsonl（正規化 ops）+ 定期チェックポイント（CLI/HTTP 越しの undo）
          │
          ▼
 render_page   placed layer を frame/bleed でクリップ、mono 仕上げ、縦書き写植
          │
          ▼
 export        PNG / TIFF(2値) / PDF / Webtoon / PSD / EPUB / pack  + ai_manifest（既定は内部用）
              （NAME・DRAFT・候補・参照画像は出ない。preflight: license・safety・dpi・仮承認）
```

### 0.4 ベースと接ぎ木

3案（agent-native / staged-pipeline / creator-mvp）の審査結果に従い、**creator-mvp をベース**にする。
データモデルの追加が最小で、既存プロジェクトとテストへの互換性が最も高く、M4 のモック縦串で契約を早く凍結できるからである。
他の2案と審査員の指摘から、次を接ぎ木する。

| 決定 | 採用元 | 理由 |
|---|---|---|
| `strict_gates` は `studio init` で作ったプロジェクトだけ既定で有効 | creator-mvp | 既存テストが name_ok なしで `put_raster` を呼ぶ（§0.5 の 18）。無条件にすると壊れる |
| 1つの `commit.py`（CLI・HTTP・GUI・runner 共通）、GUI の revision 確認と再読込 | creator-mvp | GUI がロックなしで保存し、AI の書き込みを上書きする問題を塞ぐ |
| 候補の `brief_hash` による stale 判定（ビルドグラフは作らない） | creator-mvp | 軽い。ジョブ実行中の指示変更を検出できる |
| MockBackend が真の顔箱を PNG tEXt に埋め、MockVision が読む | creator-mvp | フキダシの顔よけを決定的に試験できる |
| render 時のスクリーントーン（`r = s·√(cov/π)`）、パイロットページとスタイル固定、品質の人手評価基準 | creator-mvp | 漫画としての品質に直結する |
| `_copy_state` を `dataclasses.fields(Episode)` の走査に変える | agent-native | 新フィールドが黙って消える罠をなくす |
| `_validate(work)` を ops.py:1008 相当の位置に置く | agent-native | 参照整合とゲート不変条件を、全か無かのまま検査する |
| actor 文法 `human:` / `ai:` / `system:` / `legacy:unknown`。studio プロジェクトで actor を省略した呼び出しは `legacy:unknown`（編集はできるが承認できない）。studio でない既存プロジェクトは従来どおり | agent-native（第2版で修正） | 名乗らないエージェントが人間の権限でゲートを通すのを防ぐ。既存テストは studio プロジェクトではないので通る |
| 1つの op スキーマ登録簿から OPS_SCHEMA・docs/ops.schema.json・strict tool 定義・MCP を生成 | agent-native | 8 op の登録漏れを構造的になくす |
| 生成系 op の呼び出し側 id、段組 DSL のソースマップ | agent-native | 1バッチで作成と参照ができる。dry_run のエラーを DSL の位置に戻せる |
| `PanelFrameMapping` をガイドと配置で共有、`stale_geometry` | agent-native | 制御画像・候補・コマが画素単位で揃う |
| ラスタと**ストローク**を保存時に内容アドレス化（メモリ上の `raster_png` と strokes はそのまま）。v3 は `name_strokes` / `ink_strokes` の重複を書かない | agent-native（第2版で拡張） | relpath 衝突・上書きの3バグを構造的に消す。lt_convert 1回で 40 MB になる project.json を小さく保つ。ops と render は変えずに済む |
| `preview_ops` = deepcopy（undo 履歴は写さない）への apply + `render_page(mode="name")` | agent-native | ネームの自己点検に API 変更が要らない |
| worklist は状態の純関数（`mark_done` なし） | agent-native | 再起動は「再計算して続行」だけ |
| `pinned` フィールド、name モードのコマ指示オーバーレイ、`assist` モード | agent-native | 人間の意図を AI の再実行から守る |
| 永続 journal（正規化 ops + 定期チェックポイント。全文スナップショットは持たない）と CLI/HTTP 越しの undo | agent-native（第2版で修正） | 人間が GUI と CLI を行き来しても undo できる。journal が GB 単位に膨らまない |
| 版ゲート（新しすぎるファイルを拒否）を v3 writer より前のリリースで出す | staged-pipeline | 1つ古いビルドが v3 データを黙って消すのを防ぐ |
| materialise 手順（lock → load → compile → dry_run → expect_revision → commit、衝突時は再試行） | staged-pipeline | 冪等なコミット |
| 再開可能な ImageBackend（submit / poll / fetch / cancel、prompt_id 保存） | staged-pipeline（第2版で修正） | クラッシュ後に GPU ジョブへ再接続する。backend の再起動で handle が消えていたら lost とし、冪等キーで再投入する |
| モック画素の書き出し拒否（`allow_mock_export`） | staged-pipeline | 仮絵の誤出荷を防ぐ |
| Anthropic アダプタを純関数の request builder とレスポンス解析で試験。スタブ ComfyUI の `/object_info` 検査、hash 名アップロード | staged-pipeline | オフラインで決定的 |
| 「再実行で provider 呼び出し 0」「kill -9 後の再開で二重投入なし」の E2E | staged-pipeline | キャッシュと再開の回帰を捕まえる |
| XY-cut コマ検出（出力がそのままギロチン木） | staged-pipeline | Genko の Frame 木に正確に合う |
| 下書き解像度で生成し、採用分だけ upscale（大きいコマはタイル再生成）。最初の画像スタックは1つ（どれにするかは利用者が決める、§13 の決定4） | staged-pipeline / 実現性審査 | 費用と VRAM |
| 複数人物のコマ: 構図を先に生成し、人物ごとにインペイント | staged-pipeline | キャラの取り違えを減らす |
| モデル互換表（`claude-haiku-4-5` に effort と adaptive thinking を送らない） | staged-pipeline | 送ると 400 になる |
| GPU スケジューラ（profile ごとにまとめ、モデル再読込を減らす） | 実現性審査（新規） | 12–24 GB カードでの往復ロードを避ける |
| `Page.is_recto` と `render_spread` の綴じ方向修正を、めくり判定より前に行う。`set_spread` は向かい合う対だけを受ける | 品質審査（新規、第2版で修正） | 既定の右綴じで見開きが左右逆になっている |
| undo 履歴を deepcopy しない（`Episode.__deepcopy__`、commit 時の旧状態の移し替え）と apply の性能試験 | 第2版の批評 | 深さ 50 で 1 op 約 5.5 秒かかる現状を直す |
| 全 HTTP 経路にトークン・Origin/Host 検査・JSON 限定・`--root` 閉じ込めを最初（M1）から入れる | 第2版の批評 | 任意の Web ページから書き込みと読み出しができる現状を塞ぐ |
| 部品ごとのライセンス manifest と商用書き出しの preflight、カスタムノードの pin | 第2版の批評 | 非商用部品と未検証ノードの混入を構造的に止める |
| 画像の内容安全（ローカル分類器、safety 軸、未成年の強制遮断、監査記録） | 第2版の批評 | アニメ系モデルの既定の挙動に備える |
| 外部送信の明示選択、接続先からの locality 導出、`GENKO_OFFLINE` | 第2版の批評 | 未発表原稿を黙って送らない |
| 薄い「Claude ネーム」縦串を v2 形式の上で最初に出す | 第2版の批評 | 最初の価値を土台工事の後ろに置かない |

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

列の意味: 「参考UI」は画面上の要素、「Genko既存」は今あるもの、「追加するもの」は本設計で足すもの（op 名・モジュール名は §5・§6）。

| 機能 | 参考UI | Genko既存 | 追加するもの |
|---|---|---|---|
| 工程バー | 1 企画・テキスト / 2 ネーム / 3 生成・編集 / 4 仕上げ / 5 書き出し | `Page.stage` name/ink/finish、`name_ok`、`pipeline.advance` | `studio status` の話数ロールアップ（工程・未承認ゲート数・worklist 件数）。GUI の StepBar。ゲートで工程が開く |
| プロジェクト管理 | 最近のプロジェクト（表紙・更新日）、保存・書き出し・設定 | `.genko` フォルダ、`new` / `inspect` | 最近使った一覧（ユーザー設定ディレクトリの `recent.json`: パス、表紙 = 1 ページ目の proof を 72 dpi で縮小したキャッシュ、更新日時 = journal の最後の commit）。GUI の起動画面に並べる（M7） |
| 設定・ヘルプ・初回起動 | 設定、ヘルプ | なし | `genko studio doctor`: ComfyUI の URL と到達性、profile ごとのモデルの有無と hash、カスタムノードの pin、Claude の認証状態（SDK の認証解決。`ant auth status` と同じ情報）、locality の選択、フォント。GUI の初回起動ウィザードは同じ結果を表示し、locality を選ばせる（M7） |
| エンジン状態 | 「ComfyUI 接続中」、VRAM ゲージ | `/health` は自分自身だけ | `studio backends --health`。`/health`（認証後）に provider 到達性・キュー長・`/system_stats`・不足ノード |
| 外部送信の表示 | 「外部送信なし」「原本は保持」 | NAME/DRAFT は出力されない、`dry_run` | `studio.policy.locality`（テキスト／画像を別々に、`studio init` で明示選択、推奨は画像を送らない）。`GENKO_OFFLINE=1` で外部送信を全停止。画像バックエンドの locality は接続先ホストから導く。ステータスバーに常時表示し、クリックで切り替え（human のみ）。取り込み原本は不変資産。`--plan-only` |
| 企画・台本 | 1 企画・テキスト | `set_bible(plot, characters, constraints)`、`add_line` | Script（scene / beat）、`set_script` ほか、Scriptwriter、脚本 lint。既存テキストの取り込み `studio import-script` |
| テキストからネーム | 台本からページ割り・コマ割り・台詞配置・ラフ | `split_frame` / `merge_frame` / `resize_frame`、`add_line`（frame_id, wrap, ruby_runs）、`add_mannequin` | 段組 DSL と `apply_layout`、`set_panel`、`letter.py`（写植）、`blocking.py`（人物配置）、Namer、ネーム lint |
| ネーム・画像読込 | 16 ページの赤線アタリ（手書き縦台詞） | `put_raster(layer=name/draft)`（非出力を強制） | `studio import-name`（一括、原本は不変資産、ページ mm へ位置合わせ）、`place_asset(to=draft)` |
| コマ・領域判定 | 判定開始、番号付きコマ枠、再解析、「目視の仮設定」 | なし（コマは構造として作る） | XY-cut 検出（Pillow、出力はギロチン木）→ `apply_layout` の提案。Claude vision は補助。dry_run の重ね表示を人間が確定 |
| 読み順 | 「読み順: 右上 → 左上 → 下」、番号バッジ | `Page.leaf_frames()`（縦分割は右が先） | name/proof だけの番号オーバーレイ。snapshot に `order` と `label`、ページごとの文章要約 `reading_summary`（コマの重心から「右上 → 左上 → 下」を導出） |
| 領域ラベル | 人物 / 背景 / テキスト / 小物 / コマ枠 / その他 | 台詞の箱、マネキン、フレーム矩形 | `Region`（`panel.regions`）。`add_region` / `edit_region` / `delete_region` / `replace_regions`。`source:"user"` は AI が上書きしない |
| 領域編集 | +領域、範囲修正、領域削除、戻す、「ユーザー編集」 | GUI 内だけの undo | region op（人間が作ると `source:"user"`）、journal による CLI/HTTP 越しの undo |
| スコープ | コマ全体 / 人物 / 背景 / 文字 | `add_tone` などの frame_id 指定 | `guide.py` のマスク（コマ・領域・文字よけ）を生成・修正の範囲に使う |
| コマサムネイル | 16-1、16-2、16-3、ソート | サブビュー（ページ全体のみ） | `render --frame --kind crop`、`GET /v1/pages/{n}/frames/{id}.png`。ラベルは表示専用、キーは frame id。並べ替え（読み順 / 状態 / 点数 / 未採用だけ）は表示の設定 |
| 解析データ | コマごとの検出ラベルと信頼度（画面1のタブ） | なし | 検出器の出力は全件を `studio/analysis/<asset>.json`（検出器 id、ラベル、箱、信頼度）に残し、PanelSpec の `regions` には確定分だけ入れる。「解析データ」タブと `inspect --panel` が両方を並べ、`precheck` と `review.axes` も出す |
| このコマのメモ | 「顔と台詞を優先。背景を埋めすぎない。」 | `set_note`（ページ単位） | `panel.memo`（`set_panel`） |
| 今回の指示 | 自由文 | なし | `panel.instruction`（書いた actor 付き）。recipe のコンパイルに入る |
| タグ・プロンプト辞書 | 補完（smile） | なし | `studio/vocab.json`（shot / angle / 表情の日本語 ↔ 方言）。GUI の補完。LLM のコンパイラが大半を肩代わりする。作家名・作品名は辞書に入れず、指示と辞書の入力をブロックリストで検査する（§12） |
| 参照素材 | 「このコマのアタリ → 構図・ポーズ」、並べ替え、登録素材から追加、シルエットマスク（画面1の参考画像） | NAME ストローク／ラスタ、マネキン、prim3d、ruler | `panel.refs[{source, role, weight, order}]`、`bind_ref`。`guide.py` が制御画像に変換。role ごとの運び手は §4.4 の対応表（シルエットは `role:"mask"` → `ImageRequest.mask` か `segmentation` 制御）。登録素材は来歴が必須 |
| 生成対象・空白の補完 | 生成範囲と余白 | フレーム矩形、StoryLine の箱 | 文字よけマスク（台詞箱 + 余白）、`pad_mm`、outpaint 用マスク |
| 共通制約 | 全コマ共通 | `bible.constraints` | 共通制約 → ページ → コマのメモ → 今回の指示、の順でコンパイル。snapshot に表示 |
| 生成前に指示を確認 | 確認ボタン | `apply --dry-run` | `plan_generation`（GPU を使わない）: 最終プロンプト、ネガティブ、サイズ、制御画像とマスクの縮小版、費用見積り |
| プロンプト表示と編集 | 使用プロンプト、ネガティブ、コピー（編集可） | なし | recipe に保存（日本語仕様と方言プロンプトの両方）。コマ単位の上書き `panel.gen.prompt_override` / `negative_override`（`set_panel`、human だけ、自動で `pinned`）。上書きがあればコンパイル結果の代わりに使い、安全用の既定ネガティブは常に足す。GUI と review.html にコピーボタン |
| 生成設定 | モデル Qwen-Image-2.1、モード（Text to Image ほか）、解像度、シード | なし | profile（ワークフロー + チェックポイント + 既定値）。モード（txt2img / img2img / edit / inpaint）、解像度の上書き `gen.size_override`（既定はコマの mm 比から）、seed の固定 `gen.seed_lock` と振り直し。CLI `studio gen --mode --profile --size --seed --reroll` |
| バリエーション生成 | 4 候補 | なし | ImageBackend ジョブ（n=4、下書き解像度）→ `add_candidates` |
| 編集（インペイント）／参照して生成 | ボタン | `erase_raster`, `filter_raster`（手作業） | fix ジョブ（edit / inpaint、領域マスク）、`parent` 付きの子候補、`role:"reference"` の参照 |
| 元画像 / 比較 / 候補 / 採用中 | 表示切替、原寸 / フィット / 100% / 全画面、拡大 | `render` name/proof/print、オニオンスキン | ガイドの切り出し、比較描画（アタリを赤 50% で重ねる）、候補グリッド、採用中。表示倍率は GUI だけの設定（原稿は変えない） |
| 採用 | 採用ボタン（候補 0 枚なら無効） | `put_raster`（ページ全体・非クリップ） | `adopt_candidate`（コマ対応の placed layer、クリップ、name_ok 必須） |
| 修正指示 | ボタン | `add_ticket` / `set_ticket` | `request_fix`（ticket kind=fix、指示、スコープ）。critic の修正提案 |
| 履歴／プロンプト履歴 | 履歴を見る、生成・プロンプト履歴 | メモリ上の undo_stack（保存されない） | 候補の系譜（parent）+ recipe + journal、`inspect --candidate`、GUI の履歴ドロワー |
| キャラクタースタジオ／キャラ登録 | キャラ一覧・登録 | `bible.characters`（自由な dict） | キャラ schema（look、`hair_value`、outfits、tokens、refs、lora、seed_base、locked）。シート生成 → `adopt_sheet` |
| 背景・小物ライブラリ | ライブラリ | `materials/catalog.json`（トーン・効果の 4 件） | `studio.locations` と `studio.props`（id、名前、説明、方言トークン、参照資産、来歴）。`upsert_prop` / `delete_prop`、PanelSpec の `props[]`、establishing 画像の使い回し |
| Qwen 生成・編集 | モデル名入りのナビ | なし | 汎用の「生成」パネルと profile 選択。ナビにモデル名を出さない |
| SD 仕上げ（Illustrious） | ナビ | `filter_raster`, `add_tone`, `add_effect`, `lt_convert` | 任意の finish ジョブ（低 denoise、新しい候補として採用）+ 決定的なモノクロ仕上げ |
| 画像解析・プロンプト抽出 | ナビ（画面2では削除） | なし | `VisionAnalyzer.describe`（任意）。入力画像は来歴の登録が必須で、出力から作家名・作品名を落とす。第三者作品の模倣には使わない（§12） |
| 仕上がったモノクロページと縦書きフキダシ | 画面1のページビュー | tategaki、フキダシ、フレーム線 | `screentone.py`（render 時）、`lineart.py`、顔よけの再配置 |
| 書き出し | 書き出しボタン | png / tiff / pdf / strip / psd / epub / pack | preflight（ライセンス、内容安全、実効 dpi、仮承認の確認）、`approve{gate:"export"}`、`ai_manifest.json`（既定は内部用）、pack と print は `spec.dpi`（600）が既定、ページごとの PSD、EPUB の rtl |
| パネルの開閉・クイックバー | 右パネル / ページパネルの表示切替、キャラ登録 / 背景・小物 / 履歴 | なし | GUI の表示設定だけ（M7）。呼ぶ道具は同じ op とジョブ |
| カラーの生成プレビュー | 画面1はカラーの絵 | `expression:"color"`（webtoon） | 第2の出力 profile `color`（M9 以降。§13 の決定2 でカラーを主にするなら前倒し）。screentone を飛ばし、PanelSpec と recipe は mono と共通 |

参考UIから**採らない**もの。

- ナビゲーションが3系統（工程バー・左ナビ・クイックバー）重なっている構成。Genko は工程バーを主にする。
- モデル名をナビやデータのキーに入れること。profile で抽象化する。
- seed と VRAM を最前面に出すこと。「生成設定」タブに畳む。
- 1024×1365 の縦長画像を横長のコマに使う（画面1の不整合）。生成サイズはコマの比から決める。
- カラー生成をそのままモノクロページに置く（画面1の不整合）。mono 原稿はグレースケールで生成し、決定的に仕上げる。カラー出力そのものは捨てず、第2の出力 profile として後から足す（§13 の決定2）。
- 「外部送信なし」を表示だけで約束すること。Genko は locality を接続先から導き、`GENKO_OFFLINE` で強制する。
- 検出結果を正とすること。画面2自身が「目視の仮設定」と書いている。検出は常に提案で、人間が確定する。
- 「16-1」のような位置ラベルをデータのキーにすること。キーは frame id、ラベルは表示時に導出する。

---

## 2. 設計原則

既存の原則（DESIGN.md / AGENT.md）を保ち、AI 制作に必要な原則を加える。

### 2.1 保つもの

1. **単一コマンドバス。** 原稿の変更はすべて `genko.ops.apply_ops` を通る。GUI、CLI、HTTP、内部の AI 役、外部エージェントの区別はない。dry_run、全か無か、`ops[i] <op>: msg` 形式のエラー、page lock、undo はすべての書き手に効く。既存 GUI の抜け道（main.py:371 のフレーム選択、canvas.py:150-154 のドラッグ）も op に直す。
2. **ヘッドレス優先。** 参考UIのボタンはすべて JSON スキーマ付きの道具（op またはジョブ）にする。GUI は同じ道具を呼ぶ薄いクライアントである。
3. **画素境界。** 「画像生成は `put_raster` の外側。Genko は画素を発明しない」を次の形で保つ。
   - 生成は `apply_ops` の外、`project.lock` の外のジョブで行う。
   - バックエンドが返したバイト列は検証してから `assets/` に内容アドレスで置く。
   - op は hash を参照し、配置（`adopt_candidate` / `place_asset`）を記録するだけ。
   - Genko が行う画素処理は、既存画素の決定的変換（クロップ、リサンプル、クリップ、フィルター、スクリーントーン、線抽出）だけ。
4. **ゲート。** `name → (name_ok) → ink → finish → export` を保つ。本番に出る role への採用は name_ok の後だけ。NAME と DRAFT は出荷しない。
5. **単位は mm、ページは 1 始まり、読み順は `Page.leaf_frames()`。** コマは軸平行のギロチン木。

### 2.2 加えるもの

6. **生成画素の来歴。** ページ上の生成画素はすべて候補 → recipe → job に辿れる。recipe は backend、profile、workflow の hash、チェックポイントと LoRA、seed、プロンプト、ネガティブ、制御画像と参照の hash、PanelFrameMapping、コンパイルした LLM のモデル名とプロンプト版を持つ。書き出しには `ai_manifest.json` を付ける。
7. **非破壊の候補。** 候補はページに入らない。採用は1つの op で、取り消せる（`unadopt`、別候補の採用、journal の undo）。却下した候補も履歴として残す。取り込んだアタリは不変資産で、ラスタは NAME/DRAFT にだけ置く。
8. **人間承認ゲート。** ゲート op は actor が `human:*` のときだけ効く（§5.1）。studio プロジェクトで actor を名乗らない呼び出しは `legacy:unknown` で、編集はできるが承認はできない。AI は仮承認（`provisional:true`）しか付けられず、本番書き出しの前に人間が仮承認をページごとに確定する。CLI の actor は自己申告なので、これはセキュリティではなく**ワークフローの整合性の守り**である。HTTP ではトークンに actor を結び付ける。
9. **オフラインで試験可能。** core は Pillow のみ。LLM、画像バックエンド、vision はすべて Protocol の後ろに置く。Fake と Mock で全工程を CI で通す。ネットワークを使うテストは `@pytest.mark.live` で既定では飛ばす。
10. **判断は project.json、証拠はサイドカー、バイトは assets。** project.json は小さく保つ（hash とメタデータだけ）。recipe と生成記録は `assets/` と `studio/` に置く。**ストロークもバイトとして扱う**（v3 ではページ・レイヤーごとの内容アドレス blob、§4.1）。予算: 16 ページのネーム作業（lt_convert したページを含む）で project.json は 1 MB 未満、journal の 1 行は 64 KB 未満。試験で守る。
11. **仕様が正、プロンプトはコンパイル結果。** 日本語の PanelSpec と bible が正本。モデル方言のプロンプトは決定的テンプレート + LLM のスロット埋めで作り、recipe として記録する。キャラの記述は bible の固定トークンをそのまま入れ、LLM に言い換えさせない。
12. **構造から導出し、検出は取り込み時だけ。** Genko のネームは、コマ、読み順、台詞の箱、人物配置（blocking）、prim、ruler をデータとして持つ。マスク、文字よけ、ポーズ、構図ガイド、生成サイズはここから計算する。
13. **文字は画像に描かせない。** 台詞・ナレーション・効果音は Genko のベクター描画（縦書き、ルビ、フキダシ）で行う。ネガティブに `text, speech bubble` を入れ、judge が `text_in_image` を検出する。
14. **人間の編集は AI の再実行より強い。** `pinned` フィールド、`source:"user"` の領域、`locked` のキャラ、人間のプロンプト上書きは、AI の再実行で上書きしない。再実行が変えたい場合は提案（proposal）になる。
15. **外部送信は明示で、迷ったら送らない。** テキストと画像を Claude に送るかは `studio init` で利用者が選ぶ（黙った既定はない）。locality は接続先ホストから導き（loopback だけが local）、`GENKO_OFFLINE=1` ではローカルでない provider がすべて例外を上げる。送らない設定でもすべての工程に手作業の道が残る（§7.8 の能力表）。
16. **安全と権利は書き出しの前提条件。** 画像の内容安全（分類器・safety 軸・未成年の強制遮断）は常に有効。商用モードでは、ライセンスが未確認か非商用の部品、来歴不明の参照素材や LoRA を使った採用画像があれば書き出しを拒む。判断はすべて監査記録（`studio/audit.jsonl`）に残す。
17. **性能に予算を置く。** 1 op の apply は undo の深さに依らない（16 ページ・深さ 50 で 100 ms 未満）。GUI の筆は画面に 16 ms で反映し、ディスクへの commit は裏で 200 ms 以内。数値はベンチマーク試験で守る（§10.3）。

---

## 3. パイプライン全体

### 3.1 流れ

```text
 premise / 既存テキスト / 手描きアタリ
    │
    ├─(任意) S0 取り込み ─────────────────────────────┐
    ▼                                                 │
 S1 企画 ──[bible]──▶ S2 キャラクタースタジオ ──[sheet ①]──▶ S3 脚本 ──[script]──▶
    │                                                 ▼
 S4 ネーム（ページ割り → 段組 → コマ指示 → 写植 → 人物配置 → lint/批評）──[name_ok ②]──▶
 S5 生成・編集（パイロットページ先行: recipe → plan → 生成 → 事前検査 → 審査 → 採用/修正）──[art ③]──▶
 S6 仕上げ（upscale → 線/トーン/ベタ → 顔よけ再写植 → 効果）── advance finish ──▶
 S7 書き出し（preflight）──[export ④]──▶ 原稿ファイル + ai_manifest.json
```

ゲートは `[...]` で示した。①〜④が既定で人間の承認点。`bible` と `script` はプリセット次第（§3.4）。

### 3.2 ステージ一覧

| ステージ | 入力 | 出力（データ） | LLM (Claude) | 画像モデル | 決定的 Genko | 人間 | ゲート |
|---|---|---|---|---|---|---|---|
| S0 取り込み（任意） | アタリ画像、台本テキスト | NAME/DRAFT 上の原本、コマ・領域・台詞の提案 | 台詞の読み取り（vision）、領域の補助 | – | XY-cut コマ検出、位置合わせ | 提案を確定 | – |
| S1 企画 | premise、ページ数、判型、禁止事項 | `bible`（plot、characters、constraints）、`studio.locations`、`studio.style` | Scriptwriter（effort high、構造化出力） | – | schema 検証、見分け lint | 任意の確認 | `bible`（既定 auto） |
| S2 キャラクタースタジオ | キャラの look | `characters[].tokens`、`refs`（sheet、face）、`locked` | look → 方言トークン（1キャラ1回） | 三面図と表情シート（4 候補） | シートのプロンプト組立、顔の切り出し | 1キャラ1枚を採用 | **sheet ①** |
| S3 脚本 | bible、ページ予算 | `studio.script`（scene → beat） | Scriptwriter + Critic | – | 脚本 lint + 修復ループ | 任意で承認 | `script`（既定 human） |
| S4 ネーム | script、判型、綴じ | Frame 木、`Frame.panel`、StoryLine（台詞の箱）、人物配置、ラフ | Namer（ページ単位、並列）、vision 自己点検 | 任意: DRAFT 用ラフ（既定オフ） | 段組コンパイラ、写植、人物配置、ネーム lint、dry_run | 何でも直せる | **name_ok ②** |
| S5 生成・編集 | PanelSpec、キャラ参照、ガイド | 候補（資産 + recipe）、採点、採用 | recipe の文（ページ単位）、judge（vision）、修正指示のコンパイル | txt2img / edit / inpaint（制御・参照付き） | ガイド・マスク、recipe 組立、事前検査、配置 | 採用・修正指示、パイロットページの承認 | **art ③（ページ単位）** |
| S6 仕上げ | 採用画像 | upscale 済み候補、mono 仕上げパラメータ、最終フキダシ位置、効果 | 顔・人物の検出（vision）、効果の提案 | upscale、任意の SD 仕上げ | screentone、lineart、写植の再配置、集中線 | 校正確認、レベル調整 | `advance to=finish` |
| S7 書き出し | 全ページ | PNG / TIFF / PDF / strip / PSD / EPUB / pack + `ai_manifest.json` | – | – | preflight、既存 exporter | 承認 | **export ④** |

### 3.3 各ステージ

#### S0 取り込み（任意、M8）

- **入力:** 手描きアタリ（画面2の赤線 16 ページ）、または既存の台本テキスト。
- **手順（アタリ）:**
  1. `studio import-name PROJ ./scans/*.png` が各画像を `assets/` に置き、ページを足して `place_asset{to:"draft"}` で DRAFT に置く（非出力を強制）。原本は変えない。
  2. `XYCutDetector.detect_panels` が二値化と余白の射影でコマ枠を再帰的に切る。結果はそのままギロチン木になり、`apply_layout` の提案として出る（信頼度付き）。
  3. 斜めコマや不規則なコマは近似し、`confidence` を下げ、チケットで知らせる。Claude vision は補助（番号付けの確認、崩れた枠）。
  4. `VisionAnalyzer.read_name` が手書きの縦台詞を読み、下書きの StoryLine（`add_line`、`balloon` 推定）を提案する。`images_to_cloud` が false なら Claude vision は使わず、台詞の入力は人間の作業（チケット）になる。
  5. `replace_regions{source:"detected"}` で人物・背景・文字の領域を提案する。
  6. 人間が GUI で dry_run の重ね表示を見て確定する。確定までは原稿を変えない。
- **手順（テキスト）:** `studio import-script PROJ script.md` で Claude が既存テキストを Script に構造化する（S3 の出力と同じ形）。
- **担当:** 決定的（XY-cut、位置合わせ）+ LLM（読み取り）+ 人間（確定）。

#### S1 企画

- **入力:** premise（数行〜1ページ）、ページ数、`PageSpec`（A4 / B4 mono、webtoon）、ジャンル・読者・禁止事項（例「流血表現なし」）、内容方針（`policy.content`、§13 の決定5）。
- **処理:** Scriptwriter が1回の構造化出力（schema `bible@1`）で次を返す。
  - logline、plot、テーマ
  - characters（見た目の DNA: 髪型、**モノクロでの髪の値 `hair_value`: beta | tone | white**、目、体格と身長、場面ごとの服、目印、シルエットの要点、一人称と口調）
  - locations（時間帯の変種つき）
  - style の要点
  - constraints（共通制約。例「台詞・効果音を画像に描かない」）
- **決定的検証:**
  - id の一意性。
  - 全キャラに `hair_value` がある。
  - 全キャラに `age`（数値）か `age_band: adult | minor` がある。不明なら `minor` として扱う（内容安全は安全側に倒す）。
  - 主要キャラ同士で `hair_value` とシルエットの要点が両方一致しない（モノクロでの見分け）。
  - premise、style、constraints に実在の作家名・作品名が入っていない（ブロックリスト。人間が `set_bible` で書いた場合も警告を出す）。
- **コミット:** `upsert_character`、`upsert_location`、`set_bible{plot, constraints}`、`set_studio{style}`。
- **ゲート:** `bible`（既定 auto）。

#### S2 キャラクタースタジオ（一貫性の錨。コマより先に行う）

- 各キャラについて:
  1. Claude が look を方言トークンに一度だけ変換する（`tokens.sdxl_tags`、`tokens.qwen`）。人間が編集できる。
  2. 決定的テンプレートがシートの recipe（正面・横・背面 + 表情の列）を組み、backend が 4 候補を返す。
  3. 人間が1枚を採用する（`adopt_sheet`）か、自分の絵を上げる（`studio asset add … --character hina --origin self --rights-holder "…"`。来歴の無い素材は参照にできない）。
  4. 採用画像が `refs[{kind:"sheet"}]`、顔の切り出しが `refs[{kind:"face"}]` になり、`locked:true` になる。以後 AI の段は見た目を黙って書き換えられない。
- 繰り返し出る場所（例: 屋上）は establishing 画像を1枚作り、`locations[].refs` に置く。
- **ゲート:** **sheet ①**（既定 human）。

#### S3 脚本

- Scriptwriter が scene → beat を出す。beat の kind は `action | dialogue | monologue | narration | sfx`。話者はキャラ id、beat は感情、ページ割り当て（`page`）、めくりの印（`reveal`）を持つ。
- 長い作品は scene ごとに呼ぶ。bible と前後 scene の要約はキャッシュされる接頭辞に置く。
- **決定的 lint（エラーは Claude に返し、修復は最大2回）:**
  - 台詞1つ ≤ 40 字（既定）、1 beat の台詞群は 3 フキダシ以内。
  - 1ページの beat 数が帯域内。
  - 話者は既知のキャラ。
  - ページ予算の合計が合う。
  - `reveal` はめくり直後のページの先頭に来る（`Page.side()` で判定、§9.3）。
- Critic がテンポ、ページ予算、口調を見て `issues[]` を返す。`severity` が major 以上のときだけ改稿し、残りはチケットにする。
- **ゲート:** `script`（studio 既定 human、quick では auto）。

#### S4 ネーム（最重要）

1. **ページ割り（Namer）。** scene と beat をページに割り当てる。綴じ方向と見開きを入力し、`reveal` をめくり直後のページに、引きをめくり前のページの最後のコマに置く。結果は `set_page_plan{page, beat_ids, turn_role}`。
2. **ページごとのネーム計画（Namer、ページ単位で並列）。** 入力はそのページの beat、前後ページの要約、ページの側（`Page.side()`）、段組 DSL の説明。出力は再帰しない段組 DSL（§5.5）と、コマごとの PanelSpec（shot、angle、人物の pose / expression / facing / pos / scale、action、emotion、location、time、fx、beat_ids、台詞の balloon 種別と改行）。
3. **コンパイル（決定的）。** 計画を1つの op バッチにする。
   - `apply_layout`（呼び出し側 id 付き）
   - `set_panel`（葉ごと）
   - `add_line`（id、beat_id、speaker_id、frame_id、wrap:"vertical"、**x_mm / y_mm / w_mm / h_mm は写植器がこの段で計算した明示値**。op の中では配置を計算しない）
   - 人物配置（`panel.characters[].box_mm` と、任意で `add_mannequin`）

   ソースマップ（op 番号 → DSL 上のパス）を一緒に作る。
   M0（v2 のまま）では `apply_layout` と `set_panel` がまだ無いので、既存の `split_frame` を段ごとに適用し、PanelSpec と計画は `studio/drafts/` のサイドカーに置く（§11 の M0）。
4. **写植（決定的、`letter.py`）。** `tategaki.compose` の寸法でフキダシを測り、コマ内で読み順（右上から）に置く。人物の顔箱を避け、尾を話者の頭に向ける（§9.4）。結果は op の明示座標になるので、journal の再生や別マシンでの undo がフォントの違いで位置を変えない。
5. **検証と修復。** `apply_ops(dry_run=True)` とネーム lint を実行する。
   - エラーはソースマップで DSL のパスに直して Claude に返す（例: `ops[7] set_panel: unknown character chr_x @ /panels/2/characters/0/id`）。修復は最大2回。
   - **ネーム lint:**
     - 台詞箱の面積 / コマ面積 ≤ 0.35
     - コマの短辺 ≥ 25 mm
     - 同じ shot が3コマ以上続かない
     - scene 冒頭に状況説明のコマがある
     - **180度ルール**（同じ scene の2人の左右は `cross:true` でない限り保つ）
     - フキダシがコマ境界をまたがない
     - めくりの `reveal` 位置
     - dialogue beat はちょうど1回ずつ置く
6. **自己点検（Claude vision、最大3回で打ち切り）。** `preview_ops`（deepcopy への apply + `render_page(mode="name")`）の画像と計画を見て、読み順の迷い、窮屈なコマ、弱いめくりを指摘し、修正案（op）を出す。修正案は dry_run を通ったものだけ採る。`images_to_cloud` が false なら画像は送らず、計画の JSON とネーム lint の結果だけで批評する（テキストのみ）。結果は `record_stage{stage:"critique_name", score, rev}` で残す（§7.2）。
7. **人間の確認。** name モードの描画には、印刷されないコマ指示オーバーレイ（例「3-2 MS/low ひな 振り返り」）と読み順バッジが出る。人間は分割・結合・フキダシ移動・メモと指示の編集を op で行い、`name_ok` を付ける。
- **ラフ（アタリ）の既定は構造的なもの**（人物の箱 + ポーズプリセット + prim + ruler）。画像モデルによるラフは任意で、`adopt_candidate{to:"draft"}` で DRAFT に置く。name_ok 前でも可で、出力されない。
- **ゲート:** **name_ok ②**（人間）。

#### S5 生成・編集（コマ単位、パイロットページ先行）

name_ok の付いたページだけが対象。順序は「パイロットページ（既定: 1ページ目）→ 残りを読み順」。

1. **recipe のコンパイル。**
   - 決定的な組立: style profile → キャラの固定トークン（`tokens[dialect]`、そのまま）→ LoRA のトリガー → shot の語彙表 → location の参照 → ネガティブの基本形 + キャラの `never[]`。
   - Claude が書くのはシーン文だけ。キャッシュの効く呼び出しで**ページ単位に1回**まとめて書く。ただし `emphasis` が高いコマ（見開きや決めゴマ）は単独で呼ぶ。
   - 制約の順序: 共通制約 → ページ → コマのメモ → 今回の指示。
2. **生成計画（`plan_generation`、生成前に指示を確認）。** GPU を使わずに次を返す。人間と AI が同じものを見る。
   - 生成サイズ: コマの mm 比 + pad を、profile の `size_multiple` に丸め、約 1 MP の下書き解像度にする。
   - 制御画像: ポーズ（人物配置から）、scribble / lineart（NAME のストロークまたはラスタの切り出し）、パース（prim と ruler）。
   - マスク: 文字よけ、領域、コマ。
   - 参照: キャラの顔とシート、location、style。
   - seed、ワークフロー、使う部品とライセンスの状態（`commercial` なら未確認の部品を警告）、送信先（local / cloud）、費用見積り。
   - 内容安全の分類器が無い profile では計画の段階で拒否する。
3. **生成（ジョブ）。** n=4 は「batch 1 の prompt を 4 つ、seed を seed, seed+1, … に分けて投げる」ことで得る（1候補ずつ再生成でき、候補の seed が本当にその画像の seed になる）。`submit` → `poll` → `fetch`。バイト列を検証して `assets/` に置き、短いロック区間で `add_candidates`（`brief_hash` と mapping 付き）をコミットする。
4. **内容安全（決定的、必須、コミットの前）。** ローカルの分類器（`SafetyClassifier`、§6.5）が各画像を採点する。
   - bible で 18 歳未満（または年齢不明）のキャラがそのコマに居て、sexual の判定が閾値を超えた画像は**遮断**する。候補レコードを作らず、資産は `studio/quarantine/` に移して参照から外し、`studio/audit.jsonl` に理由・分類器 id・点数だけを残す。採用も再表示もできない。
   - それ以外で policy（`policy.content`）を超えた画像は候補にするが `safety.flagged:true` を付ける。flagged の候補は自動採用・仮承認・judge の1位事前選択から外れ、人間が理由付きで明示的に採用したときだけページに入る。
   - プロンプトには既定の安全ネガティブ（`nsfw, nude, sexual` など、profile の方言で）を常に足す。人間のプロンプト上書きでも外せない。
5. **事前検査（決定的、安い）。** 視覚審査の前に明らかな失敗を落とす。
   - サイズと比
   - 文字よけ箱の中のエッジ密度
   - mono の平均輝度帯
   - ガイド追従度（候補のエッジ図と、ポーズ・scribble のマスクの IoU）
6. **審査（Claude vision。`images_to_cloud` が true のときだけ）。** 候補、比較画像（アタリを赤 50% で重ねたもの）、キャラの顔参照、PanelSpec を見て、候補ごとに点数を返す。軸は構図・キャラ一致・人体・文字余白・画内文字なし・`safety`（分類器の見落とし）。修正指示も返す。結果は `score_candidates`。画像を送らない設定では、事前検査の順位だけを付け、審査は人間の作業になる。
7. **採用または修正。**
   - 既定は人間が「採用」を押す。judge の1位（flagged でないもの）を事前選択しておく。
   - 修正指示（日本語の自由文 + スコープのチップ コマ全体 / 人物 / 背景 / 文字）は Claude が edit recipe（指示編集）または inpaint recipe（領域マスク）にコンパイルし、`parent` 付きの子候補を作る。
   - キャラ一致が閾値未満の候補は、人間に見せる前に「人物」スコープの顔インペイントを自動で1回試す。
   - 予算: 1コマ最大 8 枚、修正 2 巡（policy）。超えたらチケットで人間に回す。
   - 各段の結果（生成の巡回数、枚数、最後の結果）は `panel.gen_attempts` に残る（§7.2）。
8. **複数人物のコマ。** まず構図（ポーズ + scribble）で全体を生成し、次に人物ごとに、その人物の領域マスクと参照でインペイントする。
9. **パイロットページとスタイル固定。** パイロットページが art 承認されると `studio.style.locked_from_page` が立ち、profile、LoRA の重み、仕上げパラメータ、キャラごとの seed family が固定される。以後のページはこれを継ぐ。
10. **ゲート: art ③（ページ単位の `art_ok`）。** 条件は「briefed な全コマが adopted か skip」。

#### S6 仕上げ（ほぼ決定的）

1. **upscale（採用分だけ）。** 実効 dpi（§9.7）が閾値未満の placed layer に upscale ジョブを出す。結果は `parent` = 採用候補の子候補として採用し直す。系譜は残る。
2. **モノクロ仕上げ（render 時、非破壊）。** placed layer の `finish`（null なら `studio.style.finish`）で、線マスク、白・黒点、3〜4 段の平網、lpi と角度を指定する。print では 1bit で出す（§9.5）。
3. **写植の再調整。** 採用画像から顔・人物の箱を検出し（`VisionAnalyzer.detect`）、写植器を再実行する。変更は `move_line` の提案として出し、人間は戻せる。
4. **効果。** `panel.fx` を `add_effect`（集中線・流線）にする。効果もコマでクリップする（M6 で直す）。
5. **任意の SD 仕上げ。** 採用画像を init、そこから作った lineart を制御にして、低 denoise の img2img を回す。結果は新しい候補で、採用を通す。
6. **`advance{to:"finish"}`。** strict_gates では `art_ok` が要る。

#### S7 書き出し

- **preflight（すべて理由付きで拒否する）:**
  - 全ページが `finish`。
  - briefed / candidates 状態のコマがない。
  - 位置のない台詞がない。
  - 実効 dpi が閾値以上（gray 350、2値の線 600。書き出しの dpi は `spec.dpi`。コマごとの実効 dpi を一覧で示す。未満は警告、`--force` で通す）。
  - **mock backend の画素がない**（`allow_mock_export` がなければ拒否）。
  - 実行中のジョブがない。
  - **ライセンス（`policy.commercial` が true のとき）:** 採用画像の recipe が使った部品がすべて `commercial_ok:true` で、参照素材・LoRA の来歴がすべて既知で商用可。未確認や非商用が1つでもあれば、コマと部品を列挙して拒否する（`--force` では通らない）。
  - **内容安全:** flagged の採用画像は、人間が理由付きで採用したものだけ。遮断対象の資産がどこからも参照されていない。
  - 仮承認が残っていない。仮承認の確定は人間がページごとに行う（§3.4）。残っていれば、ページ・ゲート・プレビュー資産の一覧を返して拒否する。
- **出力:** 既存 exporter で出す。NAME/DRAFT は role で除外済み（render.py:445-459）。候補はレイヤーではないので出ようがない。pack と print は `spec.dpi`（B4 商業原稿は 600）で出す（M6 で pack の 150 dpi 上限を外す）。
- **来歴の記録:** `ai_manifest.json`（採用コマごとの model / workflow / seed / recipe hash / 部品とライセンス / 承認者 / 人間の関与）は既定で `studio/manifests/` に内部用として書き、出力フォルダには入れない。メモ、指示、プロンプトを落とした公開版は `--public-manifest` のときだけ同梱する（開示の方針は §13 の決定6）。
- **校正用の書き出し（`--proof`）** は仮承認のままでも可。全ページに「AI仮承認」の透かしが入る。
- **ゲート:** **export ④**（常に人間）。

### 3.4 ゲートとプリセット

| ゲート | 対象 | `studio`（既定） | `quick` | `omakase` |
|---|---|---|---|---|
| `bible` | 話数 | auto | auto | auto |
| `script` | 話数 | human | auto | AI（仮） |
| `sheet` ① | キャラ | human | human | AI（仮） |
| `name` ② | ページ | human | human | AI（仮、critic 点 ≥ `policy.name_min_score`） |
| `art` ③ | ページ | human（AI が事前選択） | パイロットページは human、以後 AI（仮） | AI（仮） |
| `export` ④ | 話数 | human | human | human |

- 承認は `{gate, target, by, rev, at, provisional}` で記録する（§4.3 `studio.approvals`）。
- `auto` は承認不要（worklist が待たない）。`AI（仮）` は `by:"ai:critic"`、`provisional:true`。flagged の候補を含むページには AI の仮承認は付かない。
- 仮承認は worklist を先に進めるが、本番書き出しは、人間が仮承認を**ページごとに**確定するまで拒否される。確定は人間の `approve{gate, page}`（仮承認を上書きする）で行う。`genko studio approve --pending` と review.html が、残っている仮承認をプレビュー付きで1件ずつ並べる。一括確定の op やボタンは置かない。export 承認は、確定したページ数を `reviewed_pages` として記録する。
- `name_ok` の既存の意味（フラグを立てて stage を ink に進める）は保つ。仮承認でも `name_ok` は立つ。
- 別の軸として autonomy がある（§7.8）。`assist` では AI の書き込みがすべて提案になり、人間が受理するまでコミットされない。

---

## 4. データモデル

### 4.1 原則

- **project.json に画像バイトを入れない。** ラスタ、候補、シート、参照、取り込んだアタリは `assets/` に内容アドレスで置く。Episode は hash だけを持つ。
- **ラスタの保存を内容アドレスにする。** 作業ラスタ（`raster_png` を持つ既存レイヤー）はメモリ上ではそのまま。`save_episode` が保存時に sha256 を計算して `assets/` に書き、relpath をその hash パスにする。ops と render は変えない。ページ番号や role から作るパスがなくなるので、§0.5 の 5〜7 の衝突と上書きが構造的に消える。
- **ストロークの保存も内容アドレスにする（v3）。** メモリ上の `Layer.strokes` はそのまま。`save_episode` はレイヤーごとにストロークを正準 JSON（座標は 0.01 mm に丸め）にして `assets/` に置き、project.json には `strokes_blob`（hash）と本数だけを書く。`name_strokes` / `ink_strokes` の重複（§0.5 の 23）は v3 では書かない（読み込みは v2 のために残す）。lt_convert の 3 万本も project.json を太らせない。
- **差分保存。** 保存は、hash が既にストアにある資産を書かない。project.json は一時ファイル + `os.replace`（Windows では `PermissionError` を最大 2 秒まで間隔を広げて再試行）。読み込みはラスタを遅延読み込みにする（render と op が触れたときに読む）。
- **AI の画像は `placed` レイヤー**（新しい `LayerKind`）。バイト列をメモリに持たず、`asset` と `placement_mm` を持つ。render は Episode に注入した AssetStore から開く（LRU）。
- **コマのデータは Frame に置く**（`Frame.panel`）。`duplicate_page` は `_refresh_frame_ids` で id を変えてもオブジェクトは保つので（ops.py:951-954）、指示がコマと一緒に動く。
- **新しい Episode フィールドは1つ（`studio`）+ `revision`。** `_copy_state` を汎用化してから足す（§5.1）。

### 4.2 プロジェクトフォルダ

```text
title.genko/
  project.json                  # v3。判断だけ（小さい）。apply_ops 経由でしか変わらない
  project.lock                  # OS のファイルロックで保持（§5.1-10）。中身は {token, agent, pid, host, acquired_at}
  project.v2.bak.json           # v2 → v3 初回保存時のバックアップ
  assets/
    9c/9c1e….png                # 不変。作業ラスタ、候補、シート、参照、アタリ原本、制御画像
    4b/4b07….json               # 不変。recipe、審査レポート、成果物（script 下書きなど）、チェックポイント
    3c/3c1f….strokes.json       # 不変。レイヤー1枚分のストローク（v3）
  studio/
    journal.jsonl               # 1 commit 1 行: {rev, base_rev, actor, at, ops(正規化済み), origin}
    checkpoints.jsonl           # {rev, project: "sha256:…"}。50 commit ごとと版移行の前
    jobs/job_7f….json           # ジョブ状態（原子的置換で更新）
    jobs/job_7f….claim          # ワーカーの取得印（O_EXCL、lease 期限付き）
    generations/job_7f….json    # backend 指紋、prompt_id、時間、GPU 秒
    proposals/prop_….json       # assist モードの未受理バッチ
    cache/llm/<key>.json        # LLM 応答キャッシュ（= テストの録画フィクスチャと同形式）
    ledger.jsonl                # 費用と provider 呼び出しの記録（鍵は書かない）。予約と実績
    audit.jsonl                 # 内容安全の遮断、ライセンス判定、locality の変更、承認の確定
    analysis/<asset>.json       # 検出器の全出力（解析データタブ）
    quarantine/                 # 内容安全で遮断した資産（参照されない。GC の対象外で、監査の後に人間が消す）
    manifests/                  # ai_manifest（内部用）
    drafts/                     # M0（v2 のまま）の脚本・ネーム計画・PanelSpec。M3 で project.json に取り込む
  pages/001/*.png               # v2 の旧ラスタ。読むだけ。`genko gc --legacy` で消す
```

- **journal は op の列だけを持つ。** 全文スナップショットは持たない。1 行は正規化済みの ops（生成された id、写植の座標、`put_raster` の path を取り込んだ資産 hash に置き換えたもの）で、通常は数 KB。64 KB を超える op は本体を blob にして hash で参照する。
- **チェックポイント。** 50 commit ごとと版移行の前に、その時点の project.json（ラスタとストロークは hash 参照なので小さい）を `assets/` に置き、`checkpoints.jsonl` に rev と hash を書く。undo と過去の rev の復元は「直前のチェックポイント + journal の再生」で作る。
- **保持と上限。** journal とチェックポイントは「100 commit または 20 MB の小さい方」を保ち、古いものは `genko gc` で落とす。
- API キーはどこにも保存しない（環境変数、または SDK の認証解決）。`studio/` の設定ファイルにも書かない。
- **パスの長さ（Windows）。** 資産の相対パスは `assets/ab/<64 桁>.strokes.json` で最大約 90 文字。プロジェクトの絶対パスが 150 文字を超えると `studio doctor` が警告し、書き込みは `\\?\` 形式の長いパスで行う（Windows CI で試験する）。

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
    {"id": "tk_31a0", "page_id": "pg_7c1e9a0b2d41", "page_index": 3, "frame_id": "f3_p2", "kind": "fix",
     "role": "art", "assignee": "human", "status": "open", "candidate_id": "cd_02",
     "text": "修正2巡で顔が合わない。手で見てください", "thread": []}
  ],
  "bible": {
    "plot": "夏の屋上で、幼馴染が5年前の約束を果たしに来る。",
    "constraints": ["台詞・効果音を画像に描かない", "流血表現なし", "背景を描き込みすぎない"],
    "characters": [
      {
        "id": "hina",
        "name": "日向ひな", "reading": "ひなた ひな", "role": "heroine", "age": 16, "age_band": "minor",
        "look": {
          "hair": "shoulder-length straight hair, side-swept bangs",
          "hair_value": "beta",
          "eyes": "large, gentle, slightly downturned",
          "build": "slim", "height_cm": 156,
          "marks": ["hair clip on the left side"],
          "silhouette": "clip + bangs"
        },
        "outfits": {"default": "summer sailor uniform, short sleeves", "sc_04": "yukata"},
        "speech": {"first_person": "わたし", "style": "丁寧語まじり、照れると語尾が小さくなる"},
        "tokens": {
          "sdxl_tags": "1girl, black hair, shoulder-length hair, side-swept bangs, hairclip, serafuku",
          "qwen": "a slim 16-year-old girl with shoulder-length straight black hair and a hair clip on the left"
        },
        "never": ["glasses"],
        "refs": [
          {"asset": "sha256:9c1e…", "kind": "sheet", "approved_by": "human:leaf"},
          {"asset": "sha256:77a0…", "kind": "face", "approved_by": "human:leaf"}
        ],
        "lora": null,
        "seed_base": 81237,
        "locked": true,
        "pinned": ["look.hair_value"],
        "src": "sha256:5e0d…"
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
      "name_min_score": 0.8,
      "auto_adopt_min_score": null,
      "locality": {"text_to_cloud": true, "images_to_cloud": false, "chosen_by": "human:leaf", "chosen_at": "2026-09-24T09:58:00Z"},
      "commercial": true,
      "content": {"sexual": "none", "violence": "mild", "romance": "allowed", "block_minor_sexual": true},
      "disclosure": "internal",
      "budget": {"llm_usd": 15.0, "gpu_minutes": 120, "max_images_per_panel": 8, "max_fix_rounds": 2},
      "allow_mock_export": false
    },
    "models": {
      "default": {"model": "claude-opus-5", "effort": "high"},
      "stages": {
        "script": {"effort": "high"}, "name": {"effort": "high"},
        "critic": {"effort": "medium"}, "recipe": {"effort": "low"}, "judge": {"effort": "low"}
      }
    },
    "style": {
      "profile": "illustrious-mono@1",
      "negative": "text, speech bubble, signature, watermark, color",
      "refs": [{"asset": "sha256:77b0…", "role": "style"}],
      "lettering": {"font_mm": 4.2, "max_col_mm": 42},
      "finish": {"mode": "line_tone", "lpi": 60, "angle": 45, "black": 0.16, "white": 0.88, "levels": [0.1, 0.2, 0.3]},
      "locked_from_page": "pg_3f9a0c1d2e4b"
    },
    "locations": [
      {"id": "loc_rooftop", "name": "校舎の屋上", "description": "fenced school rooftop, water tank",
       "times": ["sunset", "night"], "refs": [{"asset": "sha256:5d1e…", "kind": "establishing"}]}
    ],
    "props": [
      {"id": "prop_hairclip", "name": "ひなの髪留め", "description": "small star-shaped hair clip",
       "tokens": {"sdxl_tags": "star hairclip", "qwen": "a small star-shaped hair clip"},
       "refs": [{"asset": "sha256:8e2a…", "kind": "reference"}]}
    ],
    "assets": {
      "sha256:9c1e…": {"kind": "sheet", "origin": "generated", "rights_holder": "human:leaf", "license": "own", "commercial_ok": true},
      "sha256:77b0…": {"kind": "style", "origin": "self", "rights_holder": "日向 葉", "license": "own", "commercial_ok": true}
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
            "emotion": "relief", "page": 3, "reveal": true, "ruby": []}
         ]}
      ]
    },
    "approvals": [
      {"gate": "sheet", "character_id": "hina", "by": "human:leaf", "rev": 41, "at": "2026-09-24T10:02:00Z", "provisional": false},
      {"gate": "name", "page_id": "pg_7c1e9a0b2d41", "by": "human:leaf", "rev": 90, "at": "2026-09-24T10:40:02Z", "provisional": false}
    ],
    "orphans": [
      {"kind": "panel", "from_frame": "f3_p4", "page_id": "pg_7c1e9a0b2d41", "reason": "merge_frame", "rev": 97, "panel": {"…": "…"}}
    ]
  },
  "pages": [
    {
      "id": "pg_7c1e9a0b2d41",
      "index": 3,
      "stage": "ink",
      "name_ok": true,
      "art_ok": false,
      "plan": {"beat_ids": ["b_0012", "b_0013", "b_0014"], "turn_role": "reveal"},
      "note": "",
      "frames": [
        {"id": "f3_root", "rect": {"x": 13, "y": 13, "width": 231, "height": 338}, "split_axis": "horizontal",
         "children": [
           {"id": "f3_p1", "rect": {"x": 13, "y": 13, "width": 231, "height": 110}, "children": [],
            "clip": true, "bleed": true, "border_mm": 0.8,
            "panel": {"status": "adopted", "…": "§4.4"}}
         ]}
      ],
      "layers": [
        {"id": "L_bg", "role": "bg", "kind": "fill", "exportable": true},
        {"id": "L_name", "role": "name", "kind": "strokes", "exportable": false, "strokes_blob": "sha256:3c1f…", "stroke_count": 30},
        {"id": "L_art_f3_p1", "role": "user", "kind": "placed", "title": "art 3-1", "exportable": true,
         "asset": "sha256:ab12…", "frame_id": "f3_p1",
         "placement_mm": {"x": 10, "y": 10, "width": 237, "height": 116},
         "fit": "cover", "clip_to": "bleed",
         "source": {"candidate": "cd_05", "recipe": "sha256:4b07…"},
         "finish": null},
        {"id": "L_ink", "role": "ink", "kind": "raster", "exportable": true,
         "asset": "sha256:0f3a…", "raster_relpath": "assets/0f/0f3a….png"},
        {"id": "L_fin", "role": "finish", "kind": "strokes", "exportable": true}
      ]
    }
  ],
  "story": [
    {"id": "ln_b0013", "page_index": 3, "page_id": "pg_7c1e9a0b2d41", "text": "…やっぱり来てくれたんだ",
     "speaker": "日向ひな", "speaker_id": "hina", "beat_id": "b_0013", "frame_id": "f3_p1",
     "x_mm": 30, "y_mm": 20, "w_mm": 14, "h_mm": 52, "balloon": "speech", "tail": [60, 70],
     "wrap": "vertical", "ruby_runs": [], "path": null}
  ],
  "extra": {}
}
```

注:

- `pages[].texts` は v3 では書かない。ロード時に `story` から組み立て、**同じオブジェクト**を `page.texts` に入れる（§0.5 の 16 を解消）。現行ローダーも `texts` が空なら story から組み立てる（migrate.py）。
- placed layer は `L_ink` より下に入れる（BG → NAME → 絵 → INK → FINISH）。人間の加筆が絵の上に乗る。
- `clip_to` は新しいキーである。既存の `Layer.clip`（下のレイヤーでクリップ）と名前がぶつからないようにした。
- `start_side` は任意（null は綴じ方向の既定、§9.3）。
- 参照素材の来歴は `studio.assets` の索引（origin / rights_holder / license / commercial_ok）で持つ。索引に無い資産は参照にも配置にも使えない。
- `policy.locality` に既定値はない。`studio init` で人間が選んだ値と、選んだ人・時刻が入る。画像バックエンドの locality は保存せず、実行時に接続先ホストから導く（§6.4）。
- `policy.commercial`、`policy.content`、`policy.disclosure` も `studio init` で選ぶ（§13）。変更は human だけで、`audit.jsonl` に残る。
- `age_band` は `age` から導く（18 未満は `minor`）。年齢が書かれていないキャラは `minor` とみなす。
- ストロークは `strokes_blob` の hash で持ち、project.json には座標を書かない。

### 4.4 PanelSpec（`Frame.panel`、葉フレームだけ）

```json
{
  "status": "adopted",
  "beat_ids": ["b_0012", "b_0013"],
  "shot": "MS",
  "angle": "low",
  "tilt": 0,
  "location_id": "loc_rooftop",
  "time": "sunset",
  "characters": [
    {"id": "hina", "outfit": "default", "pose": "turning_back", "expression": "gentle_smile",
     "facing": "left", "pos": "right", "scale": 0.8, "box_mm": [150, 25, 70, 95], "head_mm": [175, 30, 22, 26]}
  ],
  "props": ["prop_hairclip"],
  "action": "屋上で振り返るヒロイン",
  "emotion": "安堵と期待",
  "fx": [],
  "emphasis": 0.4,
  "cross": false,
  "memo": "顔と台詞を優先。背景を埋めすぎない。",
  "instruction": {"text": "顔の大きさと向きをアタリに合わせる。上半身を描き、左右の台詞用余白を保つ。", "by": "human:leaf"},
  "pinned": ["shot", "characters"],
  "refs": [
    {"id": "rf_1", "source": {"kind": "name_crop"}, "role": "composition", "weight": 0.5, "order": 0},
    {"id": "rf_2", "source": {"kind": "blocking"}, "role": "pose", "weight": 0.8, "order": 1},
    {"id": "rf_3", "source": {"kind": "character", "id": "hina", "ref": "face"}, "role": "character", "weight": 0.7, "order": 2},
    {"id": "rf_4", "source": {"kind": "location", "id": "loc_rooftop"}, "role": "background", "weight": 0.4, "order": 3},
    {"id": "rf_5", "source": {"kind": "asset", "asset": "sha256:5a5a…"}, "role": "mask", "weight": 1.0, "order": 4}
  ],
  "regions": [
    {"id": "rg_01", "kind": "person", "rect_mm": [148, 22, 74, 100], "char": "hina", "source": "user", "confidence": null},
    {"id": "rg_02", "kind": "text", "line_id": "ln_b0013", "source": "derived", "confidence": 1.0}
  ],
  "gen": {"profile": "illustrious-mono@1", "mode": "txt2img", "seed": 81240, "seed_lock": false, "n": 4, "pad_mm": 3.0,
          "size_override": null, "prompt_override": null, "negative_override": null},
  "gen_attempts": {"rounds": 1, "images": 5, "last_outcome": "adopted", "rev": 97},
  "brief_hash": "sha256:e19c…",
  "candidates": [
    {"id": "cd_01", "asset": "sha256:cd34…", "recipe": "sha256:4b07…", "job_id": "job_7f21", "seed": 81240, "batch_index": 0,
     "parent": null, "mode": "txt2img", "backend": "comfyui", "profile": "illustrious-mono@1", "mock": false,
     "brief_hash": "sha256:e19c…",
     "mapping": {"frame_rect_mm": [13, 13, 231, 110], "pad_mm": 3.0, "gen_px": [1472, 704]},
     "safety": {"classifier": "safety.local@1", "scores": {"sexual": 0.01, "violence": 0.0}, "flagged": false},
     "precheck": {"keepout_edge": 0.09, "adherence": 0.61, "luma_ok": true},
     "review": {"by": "ai:critic", "verdict": "fix", "score": 0.55, "axes": {"composition": 0.7, "character": 0.4, "safety": 1.0},
                "note": "顔が小さく台詞位置に重なる", "fix": "顔を大きく、右寄せに"},
     "status": "rejected", "stale": false, "stale_geometry": false},
    {"id": "cd_05", "asset": "sha256:ab12…", "recipe": "sha256:51c2…", "job_id": "job_7f40", "seed": 81244, "batch_index": 0,
     "parent": "cd_01", "mode": "inpaint", "backend": "comfyui", "profile": "illustrious-mono@1", "mock": false,
     "brief_hash": "sha256:e19c…",
     "mapping": {"frame_rect_mm": [13, 13, 231, 110], "pad_mm": 3.0, "gen_px": [1472, 704]},
     "safety": {"classifier": "safety.local@1", "scores": {"sexual": 0.0, "violence": 0.0}, "flagged": false},
     "precheck": {"keepout_edge": 0.02, "adherence": 0.74, "luma_ok": true},
     "review": {"by": "ai:critic", "verdict": "accept", "score": 0.86, "axes": {"composition": 0.9, "character": 0.88, "safety": 1.0}},
     "status": "adopted", "stale": false, "stale_geometry": false}
  ],
  "adopted": {"art": "cd_05"},
  "archived": []
}
```

- `status`: `empty | briefed | generating | candidates | fix_requested | adopted | skip`。
- `brief_hash` は、生成に効くフィールド（shot、characters、props、refs、regions、instruction、location、gen の上書き、style の版）の正準 JSON の sha256。候補の `brief_hash` と違えば `stale:true`。
- `mapping` は PanelFrameMapping（§9.2）。フレームの矩形が変わると `stale_geometry:true`。例の `gen_px` は `PanelFrameMapping.plan`（237×116 mm、64 の倍数、約 1 MP）の結果 1472×704 である。
- `pinned` に入ったフィールドは `ai:*` の `set_panel` で変えられない。`gen.prompt_override` / `negative_override` / `size_override` / `seed_lock` は human だけが設定でき、設定すると自動で pinned になる。
- `seed` は候補ごとに別の prompt（batch 1）で投げた実際の seed。同じ seed と recipe で1枚だけ作り直せる。
- `safety` が無い候補は存在しない（`add_candidates` が拒否する）。遮断した画像は候補にならない（§3.3 S5-4）。
- `gen_attempts` は生成・修正の巡回の持続記録で、worklist が同じ項目を出し続けないために使う（§7.2）。

**参照の role と、画像要求での運び手。** PanelSpec の role はすべて、`ImageRequest` のどこかに対応する（§6.4）。対応の無い role は `plan_generation` がエラーにする。

| role | `ImageRequest` での運び手 | 元になるもの |
|---|---|---|
| `composition` | `Control(kind="scribble" \| "lineart")` | NAME の切り出し、取り込んだアタリ |
| `pose` | `Control(kind="openpose")` | blocking + ポーズプリセット、またはマネキン（§9.2） |
| `character` | `Ref(role="character")`（IP-Adapter、または指示編集系の複数参照） | キャラの face / sheet |
| `background` | `Ref(role="background")` | location の establishing 画像 |
| `style` | `Ref(role="style")` | `studio.style.refs` |
| `reference` | `Ref(role="reference")`（参照して生成） | 来歴のある任意の登録資産 |
| `mask` | inpaint / edit では `ImageRequest.mask`、txt2img では `Control(kind="segmentation")` でシルエットに沿わせる | シルエット画像、領域 |
| `prop`（`props[]` から） | props の方言トークン + `Ref(role="reference")` | props の参照資産 |
- 候補レコードは約 400 バイト。40 コマ × 8 枚でも 130 KB ほど。2000 件を超えたら却下分を `studio/history/*.json` に退避する（`genko gc --archive-candidates`）。

### 4.5 recipe（`assets/` の JSON、内容アドレス）

```json
{
  "type": "genko.recipe@1",
  "backend": "comfyui",
  "profile": "illustrious-mono@1",
  "workflow": {"id": "sdxl_illustrious_cn", "version": 2, "hash": "sha256:1a9e…"},
  "mode": "inpaint",
  "prompt": "hina_trigger, 1girl, black hair, shoulder-length hair, side-swept bangs, hairclip, serafuku, upper body, turning back, gentle smile, rooftop, sunset, monochrome, greyscale, manga",
  "negative": "text, speech bubble, signature, watermark, color, glasses, nsfw, nude",
  "width": 1472, "height": 704, "seed": 81244, "batch_size": 1,
  "steps": 28, "cfg": 5.5, "sampler": "euler_a", "denoise": 0.55,
  "loras": [["mono_manga_style.safetensors", 0.6]],
  "init": "sha256:cd34…",
  "mask": "sha256:e0e0…",
  "controls": [{"kind": "openpose", "asset": "sha256:1111…", "strength": 0.8, "start": 0.0, "end": 1.0},
               {"kind": "scribble", "asset": "sha256:2222…", "strength": 0.5, "start": 0.0, "end": 0.6}],
  "refs": [{"role": "character", "asset": "sha256:77a0…", "weight": 0.7}],
  "mapping": {"frame_rect_mm": [13, 13, 231, 110], "pad_mm": 3.0, "gen_px": [1472, 704]},
  "brief_hash": "sha256:e19c…",
  "spec_ja": {"shot": "MS", "instruction": "顔を大きく、右寄せに"},
  "components": [
    {"id": "ckpt.illustrious", "kind": "checkpoint", "sha256": "…", "license": "利用者が確認して記入", "commercial_ok": true, "checked_by": "human:leaf"},
    {"id": "lora.mono_manga_style", "kind": "lora", "sha256": "…", "origin": "self", "commercial_ok": true, "checked_by": "human:leaf"},
    {"id": "cn.openpose", "kind": "controlnet", "sha256": "…", "commercial_ok": true, "checked_by": "human:leaf"},
    {"id": "safety.local", "kind": "classifier", "sha256": "…", "commercial_ok": true, "checked_by": "human:leaf"}
  ],
  "custom_nodes": [{"repo": "…/comfyui_controlnet_aux", "commit": "…"}],
  "fingerprint": {"comfyui": "…", "attention": "sdpa", "sampler_impl": "comfy.k_diffusion", "deterministic": true},
  "compiled_by": {"requested_model": "claude-opus-5", "served_model": "claude-opus-5", "prompt_version": "recipe.md@sha256:7c0d…"}
}
```

同じ recipe は同じ hash になり、重複しない。**プロンプト履歴とは、候補と recipe の一覧のことである。**
backend の指紋（チェックポイントと LoRA の hash、ノード版、GPU、ドライバ）と所要時間は `studio/generations/<job_id>.json` に置く。

- `components` は、その画像を作るのに使った部品の全部である（§6.4 の manifest から写す）。`commercial_ok` は `true` / `false` / `null`（未確認）。Genko が出荷する manifest はすべて `null` で、利用者が配布元の条件を確かめて `studio license set` で書く。
- `fingerprint` は再現性の範囲を決める。指紋（部品の hash、ComfyUI とノードの版、attention の実装、sampler の実装、GPU とドライバ）が一致すれば同じ recipe から同じ画素が出ることを期待し、M5 で試験する。一致しなければ再生成の結果には `fingerprint_mismatch` の印を付け、元の資産を正とする。
- `compiled_by.served_model` は実際に応答したモデル（`response.model`）。refusal fallback で要求と変わりうる（§6.3）。

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
    source: dict | None = None         # {"candidate": "cd_…", "recipe": "sha256:…"}
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
            if f.name in _NO_DEEPCOPY:                           # {"undo_stack", "assets"}
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
| ページ | `Page.id` が不変の識別子。`index` は表示と既存 op のため残す。tickets、page_locks、承認、ジョブは `page_id` で持つ |
| `delete_page` / `reorder` | `_remap_page_refs(episode, mapping)` が `spread_with`、`onion_from`、チケットの `page_index`、StoryLine の `page_index` を直す。削除ページを指す参照は None にし、チケットは `status:"orphaned"` にする |
| `duplicate_page` | 新しい `Page.id` を付ける。フレーム id は既存どおり刷新し、**旧 id → 新 id の対応表**を返させて、複製した台詞の `frame_id`、placed layer の `frame_id`、panel.regions の `line_id` を張り替える。資産は不変なので共有してよい（コピー不要） |
| `split_frame` | 親は id を保って内部節点になる（models.py:309-342）。親の `panel` は読み順で先に来る子へ移す（縦分割なら右の子）。もう一方の子は空。親に placed art があれば `force:true` が要り、art は `studio.orphans` へ移す |
| `merge_frame` | 親は読み順で先の子の panel を継ぐ。他の子の panel と placed art は `studio.orphans` へ移し、理由を残す。子に採用済みの art があれば `force:true` が要る |
| `resize_frame` | placed layer の `placement_mm` はそのまま（mm で持つ）で、クリップは新しい矩形に従う。候補の `mapping.frame_rect_mm` と違えば `stale_geometry:true` |
| `apply_layout` / `reset_frames` | `name_ok` 後は拒否（`revoke{gate:"name"}` が先）。placed art があるときは `force:true` が要り、art を orphans へ移す |
| 候補の受け取り先が消えた | `add_candidates` はエラーにせず orphans に入れる（生成中にコマが消えても GPU の成果を失わない） |
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

- トップレベル: `revision`、`studio: {step, gates_pending, provisional_pending, worklist_counts, budget: {llm_usd_spent, llm_usd_reserved, gpu_min_spent}, locality: {text_to_cloud, images_to_cloud, offline, image_backend_host_local}, commercial, preset}`、`bible: {characters: [{id, name, locked, age_band}]}`。
- ページ: `id`、`side`、`art_ok`、`locked_by`、`reading_summary`（例「右上 → 左上 → 下」）。
- 葉: `order`、`label`（例 "3-2"）、`panel_status`、`has_spec`、`candidates: {n, shortlisted, adopted, flagged}`、`stale`。
- レイヤー: `kind`、`has_raster`、`frame_id`、`title`。

詳細は `inspect_stroke`（headless.py:75-87）と同じ形の対象指定クエリで取る。

- `genko inspect PROJ --panel 3:f3_p1`: PanelSpec 全体、ガイド一覧、候補一覧。
- `--candidate cd_05`: 候補、recipe、系譜、審査。
- `--script [--scene sc_03]` / `--bible` / `--studio` / `--job job_7f21`。

---

## 5. 新しい ops

### 5.1 バスそのものの変更（M1・M2）

M1 は性能・安全・ロック・actor（1〜3、7、10）を、M2 は v3 に要るもの（4〜6、8、9、11、12）を入れる。

1. **actor。**
   - `apply_ops(..., agent=...)` は引数としてはある（ops.py:976）が、CLI も HTTP も渡していない（__main__.py:133、server.py:111）。
   - `genko apply --agent human:leaf` と HTTP を足し、`apply_ops` と `ProjectLock` に渡す。HTTP では本文の値ではなくトークンに結び付いた actor を使う（§8.2）。
   - 文法は `human:<name>`、`ai:<role>`（`ai:namer`、`ai:critic`、`ai:claude-code` など）、`system:runner`、`legacy:unknown`。
   - **actor の省略。** studio プロジェクト（`studio.policy` がある）では、省略した呼び出しは `legacy:unknown` になる。編集はできるが、ゲート op・policy の変更・ロックの解除はできない。studio でない既存プロジェクトでは、旧既定の `genko` を今までどおり扱う（既存テストと既存の使い方はそのまま通る）。
   - `docs/AGENT.md` を直し、エージェントは常に `--agent ai:<name>` を付けると書く（M1-4）。
2. **ゲート op の actor 規則。**
   - `approve`、`revoke`、`name_ok`、`set_studio` の policy 系フィールド（gates、autonomy、strict_gates、locality、commercial、content、disclosure、budget、allow_mock_export）、`studio license set`、`unlock` 系フラグは `human:*` だけが本承認できる。
   - `ai:*` は `approve{provisional:true}` だけ許し、しかも `policy.gates[gate]` がそれを許す場合に限る。
   - `system:runner` と `legacy:unknown` はゲートに触れない。
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
   - M7 で「rebase undo」を足す。対象 commit の直前へ戻してから、後続の commit の正規化済み ops を再適用し、再適用に失敗したら拒否する。生成中にも人間が自分の変更を取り消せるようにするためである。
   - GUI 内の in-memory `undo_stack` は上限 50 のまま。7 の修正で、深さが費用に効かなくなる。
10. **ロック（M1）。**
    - **OS のファイルロックを主にする。** `project.lock` を開き、POSIX は `fcntl.flock(LOCK_EX | LOCK_NB)`、Windows は `msvcrt.locking(LK_NBLCK)` で取る。プロセスが死ねば OS が外すので、「失効」を推測しない。中身（token、agent、pid、host、acquired_at）は表示と診断のためだけにある。
    - **OS ロックが使えないファイルシステム（一部のネットワーク共有）での予備:** `os.open(path, O_CREAT | O_EXCL | O_WRONLY)` で作り、ランダムな token を書く。
      - `release` は、ファイルの token が自分のものと一致するときだけ消す（今は無条件に unlink、lock.py:33-35）。
      - 失効したロック（15 分）の奪取は unlink ではなく、観測した token を名前に含む墓石（`project.lock.stale-<token>`）への `os.rename` で行う。rename の後に墓石の中身を読み、token が観測値と一致したときだけ `O_EXCL` で作り直す。一致しなければ（他人の新しいロックを動かしてしまった）、墓石を `os.link` で元の名前に戻して待ち直す。
      - この経路は最善努力であり、`studio doctor` がこのファイルシステムでは同時実行を1プロセスに絞るよう警告する。
    - CLI と HTTP は**ロックの中でロード**する（今はロックの前、__main__.py:131-133、server.py:108-111）。
    - 試験: 子プロセス 8 つが同時に取る（1つだけ成功）、失効したロックを同時に奪う（1つだけ成功し、新しい持ち主のロックは消えない）、自分のロックが失効したプロセスの `release` が次の持ち主のロックを消さない。
11. **スキーマ登録簿（`src/genko/schema.py`、M2）。**
    - 各 op を `OpSpec(name, params_schema, scope, actor_rule, creates, strict_compatible)` で登録する。
    - `params_schema` は structured outputs / strict tools の制約内で書く。使えるのは `enum`、`const`、`anyOf`、`allOf`、`$ref`。`additionalProperties:false`、任意項目は nullable。**`oneOf`、再帰、数値の範囲（`minimum` / `maximum`）、文字列長（`minLength` / `maxLength`）は使わない。** 範囲は handler 側で検査する。
    - ここから次を生成する。
      - (a) 従来形の `OPS_SCHEMA`（`genko schema` の互換）
      - (b) `docs/ops.schema.json`。既存の `ops` 目録の配列（`{op, …}` の並び。test_p1.py:118-124 が読む）を**そのまま残し**、同じファイルに機械用の JSON Schema（`$defs` と、`op` の `const` で分けた `anyOf`）を足す
      - (c) Claude の strict tool 定義。**役ごと・op ごと**に1ツール（例 `set_panel`、`add_region`）。80 を超える op を1つの巨大な union にしない。役（Namer、Critic、art_director、chat）ごとに渡す部分集合を決める
      - (d) MCP ツール
    - テスト: `_apply_one` と `apply_studio_op` が扱う op 名がすべて登録されていること、docs ファイルが生成物と一致すること、**生成した tool スキーマの lint**（`oneOf`、数値・文字列の制約、`false` 以外の `additionalProperties`、再帰参照を含まない）。
12. **`strict_gates`（既存 op の挙動変更は opt-in、M2）。**
    - `studio.policy.strict_gates` が true のプロジェクト（`studio init` の既定）だけ、次を強制する。
      - `put_raster`、`erase_raster`、`filter_raster`、`flood_fill` を exportable な role（ink / bg / finish / user）に使うときは name_ok が要る。
      - `advance{to:"finish"}` には `art_ok` が要る。
      - `set_spread` は向かい合う対だけを受ける（§9.3）。
      - frame_id があって座標の無い `add_line` は拒否する（写植は段の中で行い、op は明示座標を持つ）。
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
| `add_line` | `id?`、`beat_id?`、`speaker_id?`、`page_id?` | **op の中では写植しない。** 配置は `letter.py` がステージの中で計算し、x/y/w/h を明示した op を出す。strict_gates では frame_id があって座標の無い add_line を拒否する（旧プロジェクトの挙動は従来どおり）。journal の再生がフォントやマシンに依らない | page lock |
| `edit_line` / `move_line` / `delete_line` / `set_balloon_path` | – | v3 では story と page.texts が同一オブジェクトになり、片方だけ変わる問題が消える | **行のページで lock 検査**（新） |
| `add_mannequin` | `id?`、`frame_id?` | 人物配置との対応付け | page lock |
| `put_raster` | 画像の検証（Pillow で開けること、寸法の上限） | 壊れたバイト列を受け付けない。strict_gates では exportable な role に name_ok が要る。HTTP サーバー下では `path` を `--root` 配下に限る。journal には `path` ではなく取り込んだ資産 hash を書く | page lock |
| `erase_raster` / `filter_raster` / `flood_fill` | – | strict_gates では exportable な role に name_ok が要る | page lock |
| `name_ok` | actor 規則 | `approve{gate:"name"}` の別名。page を省略すると全ページが対象（既存どおり）。承認記録を残す | human のみ（provisional 規則は §5.1） |
| `advance` | – | strict_gates では `to:"finish"` に art_ok が要る | page lock |
| `add_ticket` / `set_ticket` | `id?`、`page_id`、`kind`（generic / fix / review / gate）、`text?`、`candidate_id?`、`comment?`（thread に追記） | 修正指示と審査の往復に使う | page lock（チケットのページ） |
| `lock_page` / `unlock_page` | `agent` は actor と一致するときだけ受ける | 所有者は actor。他人のロックは奪えない（`human:*` が `ai:*` のロックを引き取る場合を除く）。unlock は所有者か `human:*` だけ。キーは page id | actor 規則（§5.1-3）。`_check_page_lock` の除外を外し、専用の規則で検査する |
| `set_meta` | `start_side?: left \| right \| null` | 見開きの開始側を上書きする（§9.3） | – |
| `set_spread` | – | `Page.side` から、対が隣り合い、左右が逆で、綴じ方向で先に読む側に若い番号が来ることを検査する（§9.3）。strict_gates では拒否、旧プロジェクトは `warnings[]` | page lock |
| `add_stroke` | `space?: page \| spread` | 既定の `page` は今の意味のまま（原点は `op.page`、x ≥ 紙幅なら相手ページへ）。`spread` では x を物理的な見開きの左端から測る（左ページ [0, W)、右ページ [W, 2W)）。GUI のキャンバスは `spread` を使う（§9.3） | page lock（両ページ） |

### 5.3 新しい op

実装は `src/genko/studio/ops.py` の `apply_studio_op`。`ops._apply_one` の末尾、`raise ApplyError(f"unknown op: {name}")`（ops.py:830）の直前で、名前が `STUDIO_OPS` にあれば遅延 import して渡す（既存の遅延 import と同じ流儀、ops.py:316, 667, 801）。
すべて純粋なメモリ上の変更で、ネットワークも生成も行わない。I/O は資産ストアの**読み取り**（存在と画像ヘッダの確認）だけで、`put_raster` がパスを読むのと同じ種類である。

**設定とゲート**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_studio` | `premise?`, `policy?`, `models?`, `style?`, `unlock_style?` | merge-patch。スタイル固定後の `style` 変更は `unlock_style:true`（人間）が要る。`policy.locality` / `commercial` / `content` / `disclosure` の変更は `audit.jsonl` に残る | policy 系は human。episode 範囲 |
| `approve` | `gate: bible\|script\|sheet\|name\|art\|export`, `page?`/`page_id?`, `character_id?`, `provisional?` | 承認を記録する。name → `name_ok=true`, stage=ink。art → 前提（adopted か skip、flagged は人間の明示採用だけ）を検査して `art_ok=true`。人間がページ単位で approve すると、そのページの仮承認は確定に置き換わる。export → preflight の結果を添えて記録し、仮承認が1つでも残っていれば拒否して一覧を返す。記録に `reviewed_pages` を残す | human。`ai:*` は policy が許す provisional だけ。`legacy:unknown` は不可 |
| `revoke` | `gate`, `page?`/`page_id?`, `reason` | name → `name_ok=false`, `stage=name`, `art_ok=false`。art → `art_ok=false`。採用済みの候補は残す | human（AI はチケットで要請する） |

**企画・キャラ・資産**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `upsert_character` | `character{id, …}`, `unlock?` | キャラを1人分だけ差し替える（`set_bible` はリスト全体の置換）。`locked` のキャラは `unlock:true`（human）が要る。`pinned` のパスは ai:* が変えられない | episode |
| `delete_character` | `id`, `force?` | 参照されていれば `force` が要る | episode |
| `upsert_location` / `delete_location` | `location{id, …}` / `id` | 場所の登録 | episode |
| `upsert_prop` / `delete_prop` | `prop{id, name, description, tokens?, refs?}` / `id`, `force?` | 小物の登録（背景・小物ライブラリ）。参照されていれば削除に `force` が要る | episode |
| `register_assets` | `assets: {"sha256:…": {kind, mime, w, h, label?, origin: generated\|self\|licensed\|public_domain\|third_party, rights_holder, license, commercial_ok?, source_url?}}` | ストアにあることと画像ヘッダを確かめ、`studio.assets` の索引に入れる。**来歴（origin・rights_holder・license）は必須。** `third_party` は参照にできるが、`policy.commercial` では書き出しの preflight で拒否される。`generated` は Genko の候補から自動で付く | episode。読み取りのみの I/O |
| `attach_reference` / `detach_reference` | `target{character_id\|location_id\|prop_id}`, `asset`, `kind: sheet\|face\|turnaround\|outfit\|establishing\|style\|reference`, `approved?` | 参照画像の付け外し。索引に無い（来歴の無い）資産は拒否 | `approved:true` は human |
| `adopt_sheet` | `character_id`, `candidate_id`, `face_box_px?` | シート候補を採用し、`refs` に sheet と face を足して `locked:true` にする | sheet ゲートの規則に従う |

**脚本**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_script` | `script`, `src` | 脚本全体を置き換える。dry_run は差分要約を返す | episode。script 承認後の ai:* は proposal になる |
| `upsert_scene` / `delete_scene` | `scene{id, after?, …}` / `id` | scene 単位の編集 | 同上 |
| `upsert_beat` / `delete_beat` / `move_beat` | `scene_id`, `beat{id, …}`, `after?` / `id` / `id, scene_id, after` | beat 単位の編集。既に置いた台詞（`beat_id`）の本文は `edit_line` で別に直す | 同上 |

**ネーム**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_page_plan` | `page`, `beat_ids`, `turn_role: none\|hook\|reveal` | ページへの beat 割り当て | page lock |
| `apply_layout` | `page`, `tiers[]`, `gutter_mm?`, `ids{slot: frame_id}`, `root_id?`, `force?` | 段組 DSL（§5.5）からギロチン木を丸ごと作る。葉の id は `ids` のとおり | page lock。name_ok 後や placed art ありでは拒否（force で orphans へ） |
| `reset_frames` | `page`, `root_id?`, `force?` | 根1つに戻す | 同上 |
| `set_panel` | `page`, `frame_id`, `set{…}`, `unset?[]`, `pin?[]`, `unpin?[]` | PanelSpec に merge し、`brief_hash` を再計算する。`gen.prompt_override` / `negative_override` / `size_override` / `seed_lock` は human だけが設定でき、設定すると自動で pinned になる。指示とプロンプト上書きに実在の作家名・作品名があれば警告（commercial では拒否） | page lock。`pinned` は ai:* から守られる |
| `record_stage` | `target{page_id, frame_id?}`, `stage`, `outcome: ok\|no_change\|failed\|refused\|skipped`, `input_hash`, `score?`, `detail?` | ステージの結果を持続的に残す（`page.plan.stages[stage]` か `panel.stages[stage]`）。critique_name の点数、LLM が何も変えなかった事実、修復2回で失敗した事実がここに入る。worklist はこれを読んで同じ項目を出し直さない（§7.2） | 主に system:runner。page lock |

**領域と参照**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `add_region` | `page`, `frame_id`, `region{id, kind: person\|background\|text\|prop\|frame\|other, rect_mm\|poly_mm, char?, source?, confidence?}` | 領域を足す。human が作ると `source:"user"` を強制 | page lock |
| `edit_region` / `delete_region` | `page`, `frame_id`, `id`, `set?` | `source:"user"` の領域は human だけが変えられる | page lock |
| `replace_regions` | `page`, `frame_id?`, `source: detected\|derived`, `regions[]` | その source の領域だけを置き換え、user の領域は残す（再解析が 範囲修正 を消さない） | page lock |
| `bind_ref` / `unbind_ref` / `reorder_refs` | `page`, `frame_id`, `ref{id, source, role: composition\|pose\|character\|background\|style\|mask\|reference, weight, order}` / `id` / `order[]` | 参照素材の役割付き登録 | page lock |

**候補と採用**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `add_candidates` | `page`+`frame_id` または `character_id`, `job_id`, `brief_hash`, `mapping`, `candidates[{id, asset, recipe, seed, batch_index, parent?, mode, backend, profile, mock, safety}]` | 候補を足すだけ。資産は登録済みであること。`safety`（分類器 id と点数）が無ければ拒否。未成年の性的判定の閾値を超える `safety` を持つ候補も拒否する（runner は遮断した画像をここに送らない。二重の守り）。`brief_hash` が今と違えば `stale:true`。受け先がなければ orphans へ。`gen_attempts` を更新する | 主に system:runner。page lock |
| `score_candidates` | `page`, `frame_id`, `scores[{candidate_id, score, axes, verdict, note?, fix?, precheck?}]`, `by` | 審査を記録する | page lock |
| `set_candidate` | `page`, `frame_id`, `candidate_id`, `status: candidate\|shortlisted\|rejected`, `reason?` | 候補の状態を変える | page lock |
| `adopt_candidate` | `page`, `frame_id`, `candidate_id`, `to?: art\|bg\|draft`（既定 art）, `fit?: cover\|contain\|stretch`, `offset_mm?: [dx, dy]`, `scale?`, `clip_to?: frame\|bleed\|none`, `safety_reason?` | `(frame_id, to)` ごとの placed layer を作るか使い回し、`asset`・`placement_mm`（mapping から計算）・`source` を設定する。`panel.adopted[to]` を更新し、status を adopted にする。art と bg は INK の下に入れる。`safety.flagged` の候補は human が `safety_reason` を付けたときだけ採用でき、`audit.jsonl` に残る | **art / bg は name_ok が常に必要**（新しい op なので互換性の負担がない）。draft は name_ok 前でも可で非出力。flagged は human のみ。page lock |
| `unadopt` | `page`, `frame_id`, `to?` | 採用を外す。直前の採用があればそれに戻す | page lock |
| `set_placement` | `page`, `frame_id`, `to?`, `fit?`, `offset_mm?`, `scale?`, `clip_to?` | 配置の微調整 | page lock |
| `place_asset` | `page`, `asset`（`"sha256:…"` だけ。ローカルパスは受けない）, `to?: art\|bg\|draft\|name` または `layer_id`, `frame_id?`, `placement_mm?`, `fit?`, `clip_to?` | 人間の持ち込み画像、背景ライブラリ、取り込んだアタリを置く。資産は先に `studio asset add`（CLI）か `POST /v1/assets`（HTTP）で入れ、`register_assets` で来歴を付けておく。name / draft は非出力を強制。`put_raster` は「全ページに stretch で place_asset」と同じ意味になる | art / bg は strict_gates で name_ok が要る。page lock |
| `request_fix` | `page`, `frame_id`, `candidate_id?`, `instruction`, `scope: frame\|person\|background\|text\|regions`, `region_ids?` | kind=fix のチケットを作り、panel.status を `fix_requested` にする。runner が拾う | page lock |

**仕上げ**

| op | params | 効果 | undo / ロック・actor |
|---|---|---|---|
| `set_finish` | `page`, `frame_id?`, `finish{mode, black, white, levels, lpi, angle, line_threshold}` \| null | コマ単位でスタイルの仕上げを上書きする（render 時に効く） | page lock |

**部品のライセンスは op ではない。** 部品（チェックポイント、LoRA、ControlNet など）の `commercial_ok` は原稿ではなく環境の事実なので、ユーザー設定ディレクトリの `components.json`（manifest の上書き）に `genko studio license set <id> --commercial-ok yes|no --license "…" --checked-by human:leaf` で書く。書き出しの preflight は、recipe に記録した部品の hash を今の `components.json` で引き直して判定する（後から確認した部品も効く）。recipe の写しは監査のために残す。

placed 資産への決定的な画素変換（線抽出で INK ラスタを作る、など）は **op の中ではなくジョブで行う**。結果の新しい資産を `add_candidates{mode:"derive"}` で候補にし、採用する。op が資産ファイルを書くことはない。
メモリ上の作業ラスタ（`raster_png`）に対する既存の `filter_raster` は今のまま op で行う。

### 5.4 保証がどう保たれるか

- **全か無か・dry_run。** 新しい op は Episode のメモリ上の変更だけなので、既存のループ（ops.py:997-1007）と `ops[i] <op>: msg` 形式がそのまま効く。`_validate` も同じ中止経路を使う。
- **コピーの費用。** 今の apply は undo 履歴ごと deepcopy するので、深さ 50 で 1 op 約 5.5 秒かかる（§0.5 の 1）。M1 で undo 履歴を写さず、commit 時の2回目の複製もやめ、ストロークを不変にして共有する（§5.1-7）。以後の費用はプロジェクトの大きさにだけ比例し、placed layer と候補は hash しか持たないので増えない。予算は 16 ページ・深さ 50 で 1 op 100 ms 未満で、性能試験で守る。
- **undo。**
  - プロセス内の undo（GUI）は今のまま。
  - プロセスをまたぐ場合は journal を使う（§5.1 の 9）。
  - AI 作業での「戻す」は、たいてい前向きの op（`unadopt`、履歴から別の候補を採用）で足りる。候補は消えない。
- **ロック。**
  - 新しいページ単位・コマ単位の op はすべて `page` か `page_id` を必須にするので、`_check_page_lock` が効く。
  - 生成は `project.lock` の外で行う。コミットだけが短いロック区間を取る（§7.4）。ロックの失効時間 15 分より十分短い。
- **画素境界。** 生成画素は、登録済み資産として `add_candidates` → `adopt_candidate` の道でしかページに入らない。モック由来の資産は `mock:true` で、書き出し preflight が拒否する。

### 5.5 ネームの段組 DSL（再帰しない）

structured outputs と strict tools は再帰スキーマを扱えない。数値の範囲（`minimum` / `maximum`）や文字列長の制約も使えない。
Genko の Frame 木は再帰的だが、商業漫画のページはほぼ「段（tier）× 段内のコマ」なので、平らな DSL で書ける。

```json
{
  "page": 3,
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
- **ソースマップ。** コンパイラは `[(op_index, "/tiers/1/cols/0"), (op_index, "/panels/2/characters/0/id"), …]` を返す。dry_run が `ops[7] set_panel: unknown character chr_x` を返したら、runner がそれを `@ /panels/2/characters/0/id` に直して Claude に返す。

---

## 6. モジュール構成とアダプタ

### 6.1 ファイル配置

特記のないものは core（stdlib + Pillow）。*(ai)*、*(comfy)*、*(mcp)*、*(app)* は extra が要る。

```text
src/genko/
  assets.py              # AssetStore: 内容アドレスの put/open/has/verify/gc、ストロークの blob、LRU、tmp + os.replace（Windows 再試行）
  journal.py             # revision、journal.jsonl（正規化 ops）、チェックポイント、undo/redo
  schema.py              # op 登録簿 → OPS_SCHEMA / docs/ops.schema.json / 役ごと op ごとの strict tools / MCP
  commit.py              # lock(OS ロック + token) → revision 確認 → load → build ops → dry_run → apply → 差分保存
  validate.py            # _validate(work)（触れた範囲）
  doctor.py              # `genko doctor`: 資産の欠落、relink、パス長、フォント
  netguard.py            # GENKO_OFFLINE、接続先ホストからの locality 導出（loopback だけ local）
  fonts/DelaGothicOne-Regular.ttf   # package data（importlib.resources で読む。wheel に入る）
  placement.py           # PanelFrameMapping、fit、frame/bleed マスク（render と raster._clip が共用）
  guide.py               # コマ単位の制御画像: crop / lineart / scribble / openpose / perspective / mask / keepout / compare
  screentone.py          # mono 仕上げ: levels、線マスク、平網の量子化、AM 網点（被覆率が正しい）、ベタ
  lineart.py             # ラスタの線抽出（生成画像用。runs_to_strokes の置き換え）
  studio/
    __init__.py
    state.py             # StudioState / PanelSpec / Script / Scene / Beat / Region / Candidate / Approval
    ops.py               # apply_studio_op と STUDIO_OPS（schema.py に登録）
    layout.py            # 段組 DSL → apply_layout、定型、ソースマップ、XY-cut 結果のギロチン化
    layouts.json         # 段組の定型
    letter.py            # フキダシの寸法（tategaki）、スロット探索、尾、顔よけ
    blocking.py          # PanelSpec の pos/scale/facing/shot → 人物の箱 mm + ポーズ配置
    poses/*.json         # 正規化 OpenPose-18 のポーズプリセット（standing_3q, turning_back, sitting, running, …）
    vocab.json           # shot / angle / 表情 / 感情の語彙（日本語 ↔ 方言）= タグ・プロンプト辞書
    recipe.py            # PanelSpec + bible + style + profile → Recipe（決定的部分）、方言レンダラ
    lint.py              # 脚本 lint、ネーム lint、見分け lint
    precheck.py          # 候補の決定的検査
    safety.py            # 内容方針、安全ネガティブ、遮断と flag の判定、監査記録
    license.py           # 部品の登録（manifest + ユーザーの components.json）、商用 preflight、来歴の検査
    similarity.py        # 候補と参照素材の類似度（知覚ハッシュ + エッジの相関。§12）
    blocklist.json       # 作家名・作品名の検査用（利用者が足せる）
    worklist.py          # next_actions(episode, jobs) → [WorkItem]（純関数。record_stage とチケットを読む）
    stages/              # bible.py sheets.py script.py name.py letter_page.py generate.py judge.py fix.py
                         # upscale.py finish.py export.py ingest.py
    jobs.py              # ファイルベースの JobStore、claim(O_EXCL) + lease、進捗、取消
    scheduler.py         # GPU スケジューラ（profile ごとにまとめる、パイロット優先）
    runner.py            # ワーカー: submit/poll/fetch → assets → commit
    ledger.py            # 費用と provider 呼び出しの記録、予算の予約（reserve → settle）
    policy.py            # gates / autonomy / locality / commercial / content / budget の判定
    service.py           # StudioService: CLI / HTTP / GUI / MCP が呼ぶ唯一の窓口
    cli.py               # `genko studio …`
    http.py              # /v1/studio/*、/v1/assets/*、/v1/jobs/*（純関数ルーター。SSE は server.py の別ハンドラ）
    review.py            # review.html（候補・比較・コピー可能なコマンドの静的な確認シート）
    prompts/*.md         # 段ごとの system prompt（版つき、バイト不変でキャッシュが効く）
    schemas/*.json       # LLM 出力の JSON Schema（bible@1, script@1, name_plan@1, recipe_slots@1, judge@1, critique@1, detection@1）
    providers/
      llm.py             # LLMProvider Protocol、LLMRequest/LLMResult、MODEL_CAPS、build_kwargs（純関数）、FakeLLM
      llm_anthropic.py   # (ai) AnthropicProvider。`import anthropic` はこの中だけ
      image.py           # ImageBackend Protocol、ImageRequest ほかの dataclass
      image_mock.py      # MockImageBackend（決定的、真の顔箱を tEXt に埋める）
      image_comfyui.py   # ComfyUIBackend（stdlib urllib。進捗の WebSocket は (comfy)）
      vision.py          # VisionAnalyzer Protocol、MockVision、XYCutDetector
      vision_claude.py   # (ai) ClaudeVision（LLMProvider 経由）
      safety_mock.py     # MockSafety（PNG tEXt の筋書きタグを読む）
      safety_local.py    # (safety) ローカル分類器。profile の ComfyUI ノードでも可
    workflows/
      manifest.json      # profile・workflow・部品（ライセンス欄）・カスタムノードの pin・VRAM 表
      sdxl_illustrious_cn.api.json   sdxl_illustrious_cn.bind.json
      sdxl_inpaint.api.json          sdxl_inpaint.bind.json
      upscale_tile.api.json          upscale_tile.bind.json
      qwen_image_t2i.api.json        qwen_image_edit.api.json      # 第2 profile（M9。§13 の決定4 で主にするなら M5）
    mcp_server.py        # (mcp)
  app/studio/            # (app) step_bar.py panel_view.py candidates.py instructions.py regions.py
                         #       history.py characters.py jobs_qt.py
tests/
  fakes/                 # FakeLLM の再生器、スタブ ComfyUI（ThreadingHTTPServer, port 0）
  fixtures/llm/<task>/<input_hash>.json
  fixtures/projects/v2_with_rasters.genko/   fixtures/projects/name16.genko/（16 ページのネーム作業、lt_convert 1 ページ）
  fixtures/safety/       # 筋書きタグ付きの PNG（sexual / violence / minor の組み合わせ）
  golden/                # 段組の矩形、写植の箱、ガイドの統計、72 dpi のページ
  perf/                  # apply・保存・GUI 相当の commit のベンチマーク（既定で走る。閾値は §10.3）
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
- **GC（`genko gc`）は `project.lock` を取って走る。** 「参照されている」とみなすもの: project.json、保持期間内のチェックポイントとそこから journal で辿れる資産、候補、recipe、実行中・`succeeded` で未コミットのジョブの入力と出力、`studio/drafts/` の成果物。`studio/quarantine/` は GC の対象外（監査のあと人間が消す）。ロックの外で書かれ、まだどこからも参照されていない新しい資産は `keep_days` の間は消さない。

### 6.3 LLMProvider

```python
# src/genko/studio/providers/llm.py（core）
@dataclass(frozen=True)
class ImageInput:
    asset: str                                   # "sha256:…"
    media_type: str = "image/png"

@dataclass(frozen=True)
class LLMRequest:
    task: str                                    # "bible" | "script" | "name" | "critic" | "recipe" | "judge" | "fix" | "read_name"
    system_stable: str                           # 段の system prompt（prompts/<task>.md、版つき）
    context_stable: str                          # 正準 JSON: style + bible + characters（sort_keys、時刻や乱数を入れない）
    payload: str                                 # 可変部（ページの beat、コマの仕様、エラー一覧）
    schema: dict                                 # JSON Schema（structured outputs の制約内）
    images_stable: tuple[ImageInput, ...] = ()   # 承認済みの顔参照など。キャッシュされる接頭辞に入れる
    images: tuple[ImageInput, ...] = ()          # 候補・比較画像など可変の画像
    model: str = "claude-opus-5"
    effort: str | None = "high"                  # low | medium | high | xhigh | max
    max_tokens: int = 16000
    stream: bool = False                         # 長い出力（脚本全体）は True

@dataclass(frozen=True)
class Usage:
    input_tokens: int
    output_tokens: int                           # adaptive thinking の思考トークンを含む（出力として課金される）
    cache_creation_input_tokens: int = 0
    cache_read_input_tokens: int = 0

@dataclass(frozen=True)
class LLMResult:
    data: dict | None                            # 検証済みの JSON（refusal や max_tokens のときは None）
    stop_reason: str                             # end_turn | max_tokens | refusal | …
    stop_details: dict | None
    requested_model: str
    served_model: str                            # response.model。refusal fallback で要求と変わりうる
    fallback_used: bool
    usage: Usage
    request_id: str | None

class LLMProvider(Protocol):
    name: str
    locality: Literal["local", "cloud"]          # 接続先から導く（netguard）。GENKO_OFFLINE=1 で cloud は生成時に例外
    def structured(self, req: LLMRequest) -> LLMResult: ...
    def count_tokens(self, req: LLMRequest) -> int: ...
    def submit_batch(self, items: Sequence[tuple[str, LLMRequest]]) -> str: ...        # custom_id → request
    def batch_status(self, batch_id: str) -> Literal["in_progress", "canceling", "ended"]: ...
    def batch_results(self, batch_id: str) -> dict[str, LLMResult | Exception]: ...   # custom_id で引く（順序は不定）
```

**request builder は純関数**（core、オフラインで試験する）:

```python
@dataclass(frozen=True)
class ModelCaps:
    adaptive_thinking: bool
    effort: bool
    cache_min_tokens: int

MODEL_CAPS = {                                   # 1か所だけで持つ互換表。実装時に Models API で再確認する
    "claude-opus-5":    ModelCaps(adaptive_thinking=True,  effort=True,  cache_min_tokens=512),
    "claude-sonnet-5":  ModelCaps(adaptive_thinking=True,  effort=True,  cache_min_tokens=1024),
    "claude-haiku-4-5": ModelCaps(adaptive_thinking=False, effort=False, cache_min_tokens=4096),
}

def build_kwargs(req: LLMRequest, locality: Locality) -> dict:
    caps = MODEL_CAPS[req.model]
    if not locality.text_to_cloud:
        raise LocalityError("text_to_cloud is off")              # そもそも呼ばない
    if (req.images_stable or req.images) and not locality.images_to_cloud:
        raise LocalityError("images_to_cloud is off")            # 画像ブロックを1つも作らない
    content: list[dict] = [image_block(i) for i in req.images_stable]
    if content:
        content[-1]["cache_control"] = {"type": "ephemeral"}     # 安定した参照画像までを接頭辞に（2つ目のブレークポイント）
    content += [image_block(i) for i in req.images]              # 可変の画像はブレークポイントの後ろ
    content.append({"type": "text", "text": req.payload})
    output_config: dict = {"format": {"type": "json_schema", "schema": req.schema}}
    if caps.effort and req.effort:
        output_config["effort"] = req.effort
    kwargs = {
        "model": req.model,
        "max_tokens": req.max_tokens,
        "system": [
            {"type": "text", "text": req.system_stable},
            {"type": "text", "text": req.context_stable, "cache_control": {"type": "ephemeral"}},
        ],
        "messages": [{"role": "user", "content": content}],
        "output_config": output_config,
    }
    if caps.adaptive_thinking:
        kwargs["thinking"] = {"type": "adaptive"}
    return kwargs
```

**`AnthropicProvider`**（`llm_anthropic.py`、extra `ai`）:

- `client = anthropic.Anthropic()`。認証は SDK の解決順（環境変数、`ant auth` のプロファイル）に任せる。project.json や ops からは読まない。`GENKO_OFFLINE=1` なら生成時に `OfflineError` を上げ、クライアントを作らない。
- **応答ごとに `response.model` と `usage` を ledger に書く。** refusal fallback が効くと、応答したモデルが要求と変わる。recipe の `compiled_by.served_model` と LLM 応答キャッシュ（§7.6）は応答したモデルで記録する。
- 通常は `client.messages.create(**build_kwargs(req))`。`stream=True` のときは `client.messages.stream(**kwargs)` で `.get_final_message()` を使う（脚本全体など長い出力）。
- **stop_reason を必ず先に見る。**
  - `refusal` なら `stop_details.category` を記録し、ノードを失敗としてチケットにする。恋愛や暴力の表現で起こりうる。
  - `max_tokens` なら、streaming で `max_tokens` を上げて1回だけ再試行する。
- 任意で server-side の refusal fallback（beta、`client.beta.messages.create(..., betas=["server-side-fallback-2026-07-01"], fallbacks="default")`）を対話的な呼び出しで有効にする。設定 `studio.models.refusal_fallback`、既定 on、無効化できる。Batches API では使えない。実装時に API 文書で形を再確認する。
- Batches: `client.messages.batches.create(requests=[{custom_id, params}, …])` → `retrieve(id).processing_status == "ended"` まで待つ → `results(id)`。**結果は custom_id で引く**（順序は保証されない）。`custom_id` は work item id にする。
- 強制 tool_choice には頼らない。構造化出力と、`strict: true` を付けた tool + `tool_choice` auto を使う（将来のモデルで強制選択が拒否されても動く）。
- `claude-haiku-4-5` には `thinking:{type:"adaptive"}` と `output_config.effort` を送らない（送ると 400）。構造化出力は使える。
- **structured outputs の制約:** 再帰スキーマ、数値範囲、文字列長、`additionalProperties:false` 以外の指定は使えない。スキーマはこの範囲で書き、範囲の検査は lint とコンパイラで行う。
- 画像は base64 の image ブロックで送る（`assets` から読む）。`policy.locality.images_to_cloud` が false なら `build_kwargs` が画像ブロックを作らずに例外を上げ、vision を使う段は止まり、人間の作業としてチケットにする（試験: images_to_cloud が false のとき、どの段の `build_kwargs` の出力にも `"type": "image"` が無い）。
- **キャッシュの置き方。** system の安定部（段の prompt → style → bible）に1つ目、承認済みの参照画像（顔、シート）の最後に2つ目の `cache_control` を置く。候補画像や比較画像のような可変の画像は2つ目のブレークポイントの後ろに来るので、接頭辞を壊さない。

**`FakeLLM`**（core）: `tests/fixtures/llm/<task>/<input_hash>.json` を再生する。input_hash は `build_kwargs` の正準 JSON の sha256。筋書き（N 回目はこれを返す）も書ける。`GENKO_LLM_RECORD=1`（live、opt-in）で実際の応答を録る。LLM 応答キャッシュ（`studio/cache/llm/`）と同じ形式。

### 6.4 ImageBackend（再開可能）

```python
# src/genko/studio/providers/image.py（core）
Mode = Literal["txt2img", "img2img", "edit", "inpaint", "upscale", "derive"]

@dataclass(frozen=True)
class Control:
    kind: Literal["lineart", "scribble", "openpose", "depth", "perspective", "segmentation"]
    asset: str
    strength: float = 0.7
    start: float = 0.0
    end: float = 1.0

@dataclass(frozen=True)
class Ref:
    role: Literal["character", "style", "background", "composition", "reference"]
    asset: str
    weight: float = 0.7

@dataclass(frozen=True)
class ImageRequest:
    mode: Mode
    profile: str                                 # "illustrious-mono@1"
    workflow: str                                # "sdxl_illustrious_cn@2"
    prompt: str
    negative: str
    width: int
    height: int
    seed: int
    batch_index: int = 0                         # runner は n 枚を「batch 1 の要求 n 個（seed + i）」に分けて投げる
    steps: int | None = None
    cfg: float | None = None
    sampler: str | None = None
    denoise: float | None = None
    init: str | None = None                      # img2img / edit / inpaint / upscale の元
    mask: str | None = None                      # 白 = 描き直す。PanelSpec の role "mask"（シルエット）もここ
    instruction: str | None = None               # edit（指示編集）
    controls: tuple[Control, ...] = ()
    refs: tuple[Ref, ...] = ()
    loras: tuple[tuple[str, float], ...] = ()

    def key(self) -> str: ...                    # 正準 JSON（seed と batch_index を含む）→ sha256（冪等キー、生成キャッシュ）

@dataclass(frozen=True)
class BackendCaps:
    modes: frozenset[str]
    controls: frozenset[str]
    ref_roles: frozenset[str]
    size_multiple: int
    max_pixels: int
    dialect: Literal["tags", "natural"]
    vram_class: Literal["8g", "12g", "16g", "24g"]
    components: tuple[str, ...]                  # manifest の部品 id（ライセンス判定に使う）

@dataclass(frozen=True)
class JobStatus:
    state: Literal["queued", "running", "done", "failed", "cancelled", "lost"]   # lost: backend が handle を知らない
    progress: float
    message: str = ""
    queue_position: int | None = None

@dataclass(frozen=True)
class ImageOutput:
    data: bytes
    seed: int
    meta: dict                                   # 指紋（checkpoint/LoRA hash、ノード版）、mock フラグ

class AssetReader(Protocol):
    def open_bytes(self, ref: str) -> bytes: ...

class ImageBackend(Protocol):
    name: str
    locality: Literal["local", "cloud"]          # 宣言ではなく接続先ホストから導く（下記）
    def capabilities(self, profile: str) -> BackendCaps: ...
    def health(self) -> dict: ...                # reachable, queue_depth, vram, missing_nodes
    def submit(self, req: ImageRequest, assets: AssetReader) -> str: ...   # handle（ComfyUI の prompt_id）
    def poll(self, handle: str) -> JobStatus: ...
    def fetch(self, handle: str) -> list[ImageOutput]: ...
    def cancel(self, handle: str) -> None: ...
```

runner は `submit` の戻り値（handle）をすぐジョブファイルに書く。クラッシュ後は handle で `poll` し直し、**handle が生きていれば再投入しない。**
ComfyUI は `/history` をメモリにしか持たないので、ComfyUI を再起動すると handle は消える。`poll` が「`/queue` にも `/history` にも無い」を猶予（既定 30 秒）の後も返したら `lost` とし、ジョブを `queued` に戻して `attempt + 1` で再投入する。冪等キーは `ImageRequest.key()` で、同じキーの結果が既に資産にあれば投げない。

**locality の導出。** `ComfyUIBackend` の locality は URL のホストを解決して決める。loopback（`127.0.0.0/8`、`::1`、`localhost`）だけが `local` で、それ以外（LAN の GPU 機、クラウド GPU）は `cloud` とみなし、`policy.locality.images_to_cloud` の対象になる。`GENKO_OFFLINE=1` では `local` でない provider はすべて生成時に例外を上げる。

**`ComfyUIBackend`**（stdlib `urllib`、進捗 WebSocket だけ extra `comfy`）:

1. URL は `GENKO_COMFY_URL`（既定 `http://127.0.0.1:8188`）。locality は上記のとおりホストから導く。
2. `health()`: `GET /system_stats`（VRAM）、`GET /queue`（キュー長）、`GET /object_info` で profile が要求するノードクラスがあるか確かめる（不足ノードを返す）。manifest で pin していないカスタムノードのクラスを workflow が使っていれば警告する。`studio doctor --comfy-dir DIR` は `custom_nodes/<repo>/.git` の commit を読んで pin と照合する。
3. `submit()`:
   1. 制御画像・参照・init・mask を `POST /upload/image` で送る。ファイル名は `<sha256>.png` にして冪等にする。
   2. `workflows/<id>.api.json` を読み、`<id>.bind.json` の対応（例 `{"prompt": ["6", "inputs.text"], "seed": ["3", "inputs.seed"], "control.openpose": ["21", "inputs.image"]}`）で値を埋める。
   3. `POST /prompt {"prompt": graph, "client_id": uuid}` → `prompt_id`。batch_size は常に 1（n 枚は n 回の submit）。
4. `poll()`: `GET /history/{prompt_id}` と `GET /queue`。`comfy` extra があれば `ws://…/ws?clientId=` で進捗を受ける。
5. `fetch()`: history の outputs から `GET /view?filename=…&subfolder=…&type=output` で取る。
6. `cancel()`: 待ち行列にあれば `POST /queue {"delete": [prompt_id]}`、実行中なら `POST /interrupt`。対象 ComfyUI の版で実装時に確認する。
7. profile の切り替えで VRAM を空けたいときは、scheduler が `POST /free`（対応版のみ）を呼ぶ。

**`MockImageBackend`**（core、決定的）:

- 乱数の種は `ImageRequest.key()`。
- 画像は、グラデーションに制御画像のエッジを描き込み、ポーズから推定した顔の位置に楕円を描いたもの。
- PNG の tEXt チャンク `genko.mock` に `{"key": …, "faces": [[x0, y0, x1, y1], …], "mode": …, "safety": {"sexual": 0.0, …}}` を埋める。安全の筋書き（プロンプトに `__mock_sexual__` などを入れると点数が上がる）で遮断と flag の経路を試験する。
- `submit` は即完了で、handle は key。`ImageOutput.meta["mock"] = True`。

**workflow の登録（`workflows/manifest.json`）:**

```json
{
  "components": {
    "ckpt.illustrious":  {"kind": "checkpoint", "file": "illustriousXL.safetensors", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "cn.openpose":       {"kind": "controlnet", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "cn.scribble":       {"kind": "controlnet", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "ipadapter.plus":    {"kind": "ip_adapter", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "clip_vision.h":     {"kind": "clip_vision", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "upscale.tile":      {"kind": "upscaler", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "detector.face":     {"kind": "detector", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null},
    "safety.local":      {"kind": "classifier", "file": "…", "sha256": null, "license": null, "commercial_ok": null, "source": null}
  },
  "custom_nodes": {
    "comfyui_controlnet_aux":  {"repo": "https://github.com/…/comfyui_controlnet_aux", "commit": "<sha>", "classes": ["OpenposePreprocessor"]},
    "ComfyUI_IPAdapter_plus":  {"repo": "https://github.com/…/ComfyUI_IPAdapter_plus", "commit": "<sha>", "classes": ["IPAdapterAdvanced"]}
  },
  "profiles": {
    "illustrious-mono@1": {
      "dialect": "tags", "size_multiple": 64, "max_pixels": 1572864, "vram_class": "12g",
      "components": ["ckpt.illustrious", "cn.openpose", "cn.scribble", "ipadapter.plus", "clip_vision.h", "upscale.tile", "safety.local"],
      "workflows": {"txt2img": "sdxl_illustrious_cn@2", "inpaint": "sdxl_inpaint@1", "upscale": "upscale_tile@1"},
      "defaults": {"steps": 28, "cfg": 5.5, "sampler": "euler_a"},
      "negative_base": "text, speech bubble, signature, watermark, color",
      "safety_negative": "nsfw, nude, sexual",
      "vram": {"8g": {"lowvram": true, "controls_max": 1, "ip_adapter": false, "target_px": 786432},
               "12g": {"controls_max": 2, "ip_adapter": true, "clip_vision_on_cpu": true, "target_px": 1048576},
               "24g": {"controls_max": 2, "ip_adapter": true, "target_px": 1048576, "tile_regen": true}}
    }
  },
  "workflows": {
    "sdxl_illustrious_cn@2": {"file": "sdxl_illustrious_cn.api.json", "bind": "sdxl_illustrious_cn.bind.json",
      "requires_nodes": ["KSampler", "ControlNetApplyAdvanced", "IPAdapterAdvanced"],
      "hash": "sha256:1a9e…"}
  }
}
```

**部品のライセンス。** Genko はモデルを同梱せず、manifest の `license` と `commercial_ok` はすべて `null`（未確認）で出荷する。利用者が配布元の条件を確かめ、`genko studio license set` でユーザー設定の `components.json` に書く（§5.3 の末尾）。判断の材料として、実装時に各配布元で確かめるべき典型例を挙げる。

- SDXL 系アニメチェックポイント（Illustrious 系を含む）とその派生マージは、版ごとに条件が違うので版ごとに確かめる。
- IP-Adapter の FaceID 系が使う InsightFace のモデルは非商用の条件で配られている。**既定の profile は FaceID 系を使わない。**
- 顔・人物検出によく使われる YOLOv8 系の検出器は AGPL のものが多い。
- ESRGAN 系のアップスケーラの重みには CC-BY-NC のものが多い。
- Qwen-Image 系は Apache-2.0 として配られている（配布元で要確認）。

`policy.commercial` が true のプロジェクトでは、`commercial_ok` が `true` でない部品を使う profile で生成しようとすると `plan_generation` が警告し、その部品を使った採用画像は書き出しの preflight で拒否される。カスタムノードは repo と commit で pin し、pin の無いノードは `health()` と `studio doctor` が警告する（悪意のあるカスタムノードが出回った例があるため）。

**VRAM の目安（M5 で実測して置き換える）。**

| profile | 8 GB | 12 GB | 24 GB |
|---|---|---|---|
| `illustrious-mono@1`（SDXL + ControlNet 2 + IP-Adapter と CLIP-ViT-H、約 1 MP） | lowvram で動く。ControlNet は1つずつ、IP-Adapter を外してキャラは参照付き inpaint で当てる。生成は約 0.8 MP | 基準の構成。限界に近いので CLIP-vision を CPU に置く設定を用意する | 余裕あり。大きいコマのタイル再生成も同じカードで |
| `qwen-image@1`（Qwen-Image / Qwen-Image-Edit） | 非対応 | 量子化版だけ（品質と速度を実測してから採否を決める） | 基準の構成 |

profile の `vram` 表から、`studio doctor` が `/system_stats` の VRAM を見て設定を選ぶ。足りない profile は投入せず `blocked:policy` にする（§7.3）。

### 6.5 VisionAnalyzer

```python
# src/genko/studio/providers/vision.py（core）
@dataclass(frozen=True)
class Detection:
    label: str                                   # face | person | text | prop
    box01: tuple[float, float, float, float]     # 画像に対する 0..1 の座標
    score: float

@dataclass(frozen=True)
class JudgeRequest:
    brief: dict
    instruction: str | None
    candidates: Sequence[tuple[str, str]]        # (candidate_id, asset)
    compare: Sequence[tuple[str, str]]          # 候補ごとのアタリ重ね画像
    character_refs: Sequence[tuple[str, str]]    # (character_id, face asset)
    keepout_boxes01: Sequence[tuple[float, float, float, float]]

@dataclass(frozen=True)
class Judgement:
    candidate_id: str
    score: float
    axes: dict[str, float]                       # composition, character.<id>, anatomy, keepout, text_in_image, safety
    verdict: Literal["accept", "fix", "reject"]
    note: str
    fix: str | None
    fix_scope: str | None

class VisionAnalyzer(Protocol):
    def detect_panels(self, page_asset: str, page_mm: tuple[float, float]) -> dict: ...  # detection@1
    def detect(self, asset: str, labels: Sequence[str]) -> list[Detection]: ...
    def read_name(self, page_asset: str) -> dict: ...                                    # 手書き台詞の OCR（M8）
    def critique_name(self, page_asset: str, plan: dict) -> dict: ...                    # critique@1
    def judge(self, req: JudgeRequest) -> list[Judgement]: ...
    def describe(self, asset: str, dialect: str) -> dict: ...                            # 画像解析・プロンプト抽出（来歴のある資産だけ）

@dataclass(frozen=True)
class SafetyVerdict:
    classifier: str                              # "safety.local@1"（manifest の部品 id と版）
    scores: dict[str, float]                     # sexual, violence, gore …（0..1）
    flagged: bool                                # policy.content の閾値を超えた
    blocked: bool                                # 未成年キャラ + sexual。候補にしない

class SafetyClassifier(Protocol):                # ローカル必須。クラウドには送らない
    def classify(self, asset: str) -> dict[str, float]: ...
```

実装と部品は次のとおり。

- `MockVision`（core）: tEXt から真の顔箱を返す。judge は筋書きの点数を返す。
- `XYCutDetector`（core、`detect_panels` だけ）: 二値化 → 行と列の射影で余白を探し、再帰的に切る。結果はギロチン木。
- `ClaudeVision`（ai）: `LLMProvider` を使い、schema `judge@1`、`critique@1`、`detection@1` を返す。`images_to_cloud` が false なら作れない。
- `MockSafety`（core）: tEXt の筋書きタグから点数を返す。
- ローカル分類器（extra `safety`、または profile の ComfyUI ノード）: 画像を外に出さずに点数を出す。**画像生成の段は分類器が無ければ動かない**（`plan_generation` が拒否する）。判定（`SafetyVerdict`）は `studio/safety.py` が `policy.content` と、そのコマに居るキャラの `age_band` から決める。
- 類似度（`studio/similarity.py`、core）: 候補と、そのコマで使った参照素材（とくに origin が `third_party` のもの）との知覚ハッシュとエッジ相関を測る。閾値を超えた候補は flag し、人間の確認に回す（§12）。

### 6.6 pyproject の extras

```toml
[project]
dependencies = ["pillow>=10.0"]                  # 変えない

[project.optional-dependencies]
app   = ["pyside6>=6.6"]
dev   = ["pytest>=8.0"]
ai    = ["anthropic"]                            # 実装時に現行メジャー版で固定する
comfy = ["websocket-client"]                     # 進捗の受信だけ。HTTP は stdlib urllib
safety = ["onnxruntime"]                         # ローカル安全分類器（モデルは利用者が入れ、ライセンスを登録する）
mcp   = ["mcp"]

[tool.hatch.build.targets.wheel]
packages = ["src/genko"]                         # 同梱フォントは src/genko/fonts/ に移し、package data として wheel に入れる
```

`genko.studio` は provider を遅延 import する（`app` の PySide6 と同じ流儀、__main__.py:161-164）。`anthropic` が無いときに Claude を使う段を呼ぶと、`{ok:false, error:"install genko-studio[ai]"}` を返す。

---

## 7. オーケストレーション

### 7.1 方式: 決定的な worklist + 有界な LLM 呼び出し

| 方式 | 採否 | 理由 |
|---|---|---|
| 決定的なステージ関数だけ | 幾何・写植・配置・仕上げ・書き出しで使う | 安く、再現でき、試験しやすい。ただし創作上の判断はできない |
| 1つの自律「監督」エージェントに全ツールを渡す | 主経路には使わない | 16 ページ × 5 コマで文脈が膨れる。高い。再開も再現も難しい |
| **混成（採用）** | 主経路 | worklist（状態の純関数）が「次に何をするか」を決める。各ステージが「どうやるか」を1回の構造化 LLM 呼び出しで決める。コンパイラが op にする。dry_run と lint が検証し、修復は最大2回 |

**エージェントループを使うところ（有界）:**

- **ネームの自己点検（S4-6）。** 描画した name ページを見て修正案を出す。最大3巡で打ち切る。
- **`studio chat`（M9）。** 自由な編集依頼（「告白シーンを見開きにして、溜めを作って」）用の対話エージェント。strict tools（`inspect`、`render_page`（画像を返す）、`propose_ops`（常に dry_run）、`plan_generation`、`submit_generation`、`list_candidates`、`adopt_candidate`）で動く。op のバッチは autonomy に従い、`assist` / `gated` では人間の確認後にコミットする。
- **外部エージェント（Claude Code など）。** CLI / HTTP / MCP から同じ道具を使う（§8.4）。

**使わないところ:** 工程の順序決定、コマ幾何、写植、配置、仕上げ、書き出し、ゲートの判定。

**LLM ステージの型（全段共通）:**

```text
context 組立（安定接頭辞 + 可変 payload）
  → structured(LLMRequest)                    # 1 呼び出し
  → schema 検証 → lint → コンパイル → apply_ops(dry_run=True)
  → エラーあり: ソースマップで DSL の位置に直し、payload に足して再呼び出し（最大 2 回）
  → なお失敗: ノード失敗としてチケット（kind=review）に回す。ループしない
  → 成功: commit（§7.4）
```

### 7.2 worklist

`worklist.next_actions(episode, jobstore) -> list[WorkItem]` は状態の**純関数**である。保存する「完了印」はない。状態が規則を満たせば項目は消える。

```json
{
  "id": "wi_3f9a51c0",
  "kind": "gen_panel",
  "role": "art_director",
  "target": {"page_id": "pg_7c1e9a0b2d41", "frame_id": "f3_p1", "label": "3-1"},
  "why": "spec present, page name_ok, no adopted candidate, no job in flight",
  "blocked_by": [],
  "est": {"llm_usd": 0.02, "gpu_s": 45},
  "priority": 30
}
```

- `id = hash(kind, target, 関係する入力の hash)` なので再起動しても同じになる。
- **純関数であるための持続記録。** 各段の結果は必ず状態に残る。
  - LLM の段は、成功しても「何も変えなかった」ときも、失敗しても、`record_stage{stage, outcome, input_hash, score?}` をコミットする（§5.3）。ネーム批評の点数は `page.plan.stages.critique_name {score, rev, input_hash}`。
  - 画像の段は `panel.gen_attempts {rounds, images, last_outcome, rev}`。
  - 規則は「同じ `input_hash` の記録があれば、その段の項目を出さない」。入力（PanelSpec、脚本、ページの内容）が変われば hash が変わり、項目が戻る。
- **チケットは項目を止める。** 対象（ページ・コマ・キャラ）に `assignee:"human"` の未解決の review / fix チケットがあれば、その対象の項目は `blocked_by: ["ticket:tk_…"]` で出る。修復2回で失敗した段はチケットを作るので、次の run で同じ項目が出直さない。
- 試験: 筋書きで LLM に毎回失敗させ、`studio run` が1回チケットを作って止まること（無限に再試行しない）。
- 規則は上から順に評価する。

```text
write_bible → [gate bible] → design_characters → gen_sheet(char) → [gate sheet(char)]
→ write_script → [gate script] → plan_pages → name_page(page) → letter_page(page)
→ critique_name(page) → [gate name(page)]
→ gen_panel(frame) → precheck_panel → judge_panel → fix_panel(ラウンド < max) → ticket（上限超過）
→ continuity_page(page) → [gate art(page)] → upscale_panel → finish_page(page) → [gate export]
```

- 次の場合、項目は `blocked_by` 付きで出る。
  - 人間がロックしたページ: `locked:human:leaf`
  - 実行中のジョブがある: `job:job_7f21`
  - 予算切れ（予約を含む）: `await_budget`
  - ゲート待ち: `await_gate`
  - 人間向けの未解決チケット: `ticket:tk_31a0`
  - 外部送信の設定で動けない段（vision の judge など）: `await_human:locality`
- `genko studio run PROJ --until gate`:
  1. worklist を計算する。
  2. 実行可能な項目を投入する（ページは並列、画像は scheduler 経由）。
  3. ジョブを待つ。
  4. 再計算する。
  5. `await_*` だけが残ったら止まり、`{"waiting_for": [{"gate": "name", "pages": [1, 2, 3]}, …]}` を出す。

### 7.3 ジョブ・キュー・進捗

- **ジョブファイル** `studio/jobs/<id>.json`:

  ```json
  {"id": "job_7f21", "kind": "generate", "project": "C:/manga/summer.genko",
   "target": {"page_id": "pg_7c1e9a0b2d41", "frame_id": "f3_p1"},
   "work_item": "wi_3f9a51c0", "request": "sha256:4b07…", "idempotency_key": "sha256:9a0c…",
   "subrequests": [{"seed": 81240, "batch_index": 0, "handle": "6f1d0c9e-…", "state": "running"}, "…"],
   "status": "running", "attempt": 1, "backend": "comfyui", "handle": "6f1d0c9e-…",
   "progress": {"done": 12, "total": 28, "message": "sampling"},
   "lease": {"worker": "host-a:4211", "until": "2026-09-24T10:44:10Z"},
   "result": null, "error": null, "cost": {"gpu_s": 0}, "created_by": "system:runner",
   "created_at": "2026-09-24T10:43:00Z", "updated_at": "2026-09-24T10:43:40Z"}
  ```

- **状態:** `queued → running → succeeded | failed | cancelled | lost → committed`。ほかに `blocked`（gate、budget、policy）がある。n 枚の生成は1つのジョブの中の n 個の subrequest（batch 1、別々の seed）で、1つずつ再投入できる。
- **書き込み:** 一時ファイル + `os.replace` で原子的に置き換える。
- **取得:** `<id>.claim` を `O_EXCL` で作る。lease は期限付き（既定 60 秒、進捗ごとに延長）。死んだワーカーの判定は **pid ではなく lease の期限切れ**で行う。Windows で `os.kill(pid, 0)` は使えないため。
- **ワーカー:**
  - CLI の `--wait` なら同じプロセス内で動く。
  - 別プロセスなら `genko studio worker PROJ`。
  - `genko serve` なら `HeadlessServer` が持つ `ThreadPoolExecutor`。
  - 並列度は provider ごとに決める。LLM 既定 4、GPU バックエンドはインスタンスあたり 1。
- **GPU スケジューラ（`scheduler.py`）:**
  - 待ち行列の画像ジョブを **profile（チェックポイント）ごとにまとめ**、同じ profile を続けて流す。Illustrious と Qwen-Image が交互に来て、12–24 GB のカードでモデルを積み直し続けるのを避ける。
  - 優先順位: パイロットページ → 読み順 → 修正（人間が待っているもの）。
  - profile の `vram_class` がカードより大きい場合は投入せず、`blocked:policy` にする。
- **進捗:**
  - ComfyUI の WebSocket（extra）または `/history` と `/queue` のポーリングから得て、ジョブファイルに書く。
  - HTTP では `GET /v1/jobs/{id}`（ポーリング）と `GET /v1/jobs/{id}/events`（SSE）で出す。SSE は `(status, bytes)` を返す純関数の `handle_request` を通らない。`server.py` の `_Handler` に流し続ける専用ハンドラを置き、認証と Origin/Host の検査は同じ関数を使う。
  - ジョブ id からプロジェクトを引くため、サーバーは `--root` の下に `job_id → project` の索引（`.genko-jobs.json`、原子的置換）を持つ。`?path=` を付けた呼び出しも受ける。
  - CLI では stderr に JSONL で出す。stdout は最後に JSON を1つだけ出す（既存の CLI 規約）。
  - GUI では QTimer でジョブファイルを見る（既存の autosave QTimer と同じ流儀）。
- **取消:** `studio cancel PROJ JOB` がジョブファイルに `cancel_requested` を立てる。ワーカーは `backend.cancel(handle)` を呼ぶ。取り消したジョブは何もコミットしない。

### 7.4 コミット手順（`commit.py`、全経路共通）

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

- **runner:** `expect_revision` を付けない。ロックの中で最新の状態から ops を作り直す。ops は冪等で、すでに入った候補 id は飛ばす。前提（frame があるか、`brief_hash`）もその場で確かめる。
- **CLI / HTTP の apply:** `--expect-revision` / `expect_revision` は任意。
- ロックの保持は数十〜数百ミリ秒で、生成、LLM 呼び出し、資産の書き込みはすべてロックの外で行う。
- **GUI は1 op ごとに commit しない（M7）。**
  - GUI はメモリ上に正本のセッション（`Episode`）を持ち、すべての変更を `apply_ops` で**メモリ上に**適用する（筆の1本も op）。画面への反映はディスクを待たない。
  - ディスクへの commit は裏のスレッドでまとめて行う。筆を離して 1 秒操作が無いとき、ページを切り替えたとき、生成の投入や承認の前に、溜まった ops を1回の commit にする。
  - commit は、セッションが読んだ revision を `expect_revision` にして `read_revision` で安く確かめる。一致すれば、変わった資産だけを書く差分保存で済む（全ラスタを書き直す今の保存、§0.5 の 25 はやめる）。
  - 衝突したら（AI が裏で commit した）、最新を読み直し、溜まっていたローカルの ops を再生（rebase）する。再生できない op は人間に見せて選ばせる。
  - **遅延の予算:** 筆の画面反映 16 ms（ディスクなし）、裏の commit 200 ms（16 ページ、差分保存）、衝突時の再読込と再生 1 秒。`tests/perf/` に GUI 相当の op 列（筆 200 本 + 台詞の移動）のベンチマークを置く。
- Windows では project.json の `os.replace` が、GUI の監視、ウイルス対策、OneDrive などのせいで `PermissionError` になることがある。間隔を広げて最大 2 秒まで再試行し、なお失敗すればロックを離して `{ok:false, error:"project.json busy"}` を返す（Windows CI で試験する）。

### 7.5 再開（resume）

- **状態はファイルにしかない。** worklist は再計算する。ジョブ、lease、handle はファイル。資産は内容アドレスで不変。
- `genko studio run --resume`（既定の動作）は次を行う。
  1. worklist を再計算する。
  2. `running` のまま lease が切れたジョブで handle があれば、`backend.poll(handle)` で**再接続**する。handle が生きていれば再投入はしない。
  3. handle がなければ、または `poll` が `lost`（backend の再起動で handle が消えた、§6.4）を返せば、その subrequest を `queued` に戻し、`attempt + 1`（最大 3、一時的なエラーは指数バックオフ）。冪等キー（`ImageRequest.key()`）の結果が既に資産にあれば投げない。
  4. 資産は書いたがコミット前に落ちたジョブ（`succeeded` かつ未 `committed`）は、コミットだけやり直す（冪等）。
  5. Batches は batch id で状態を取り直す。
- LLM の項目も冪等である。同じ項目の出力がすでにコミットされていれば、規則が満たされていて項目自体が出ない。

### 7.6 キャッシュ

1. **プロンプトキャッシュ。**
   - 並びは tools → system → messages。
   - 安定接頭辞: 段の system prompt（版つき）→ style → bible と characters（正準 JSON、`sort_keys`）。ここに1つ目の `cache_control:{"type":"ephemeral"}` を置く。
   - vision 段では、user の content の先頭に承認済みの顔参照画像（`images_stable`）を並べ、その最後の画像に2つ目の `cache_control` を置く（§6.3 の `build_kwargs`）。
   - 可変部（候補画像、比較画像、ページの beat、コマの仕様、エラー一覧）は2つ目のブレークポイントの後ろ。
   - 接頭辞に時刻、job id、並び順の不定な JSON を入れない。
   - ledger に `cache_read_input_tokens` を記録する。2回目以降も 0 なら、どこかで接頭辞が壊れている。
   - 最小接頭辞はモデルごとに違う（`MODEL_CAPS.cache_min_tokens`）。短すぎる接頭辞は黙ってキャッシュされない。
2. **LLM 応答キャッシュ。** キーは `sha256(要求モデル, prompt 版, schema hash, build_kwargs の正準 JSON)`、置き場所は `studio/cache/llm/<key>.json`。エントリには応答したモデル（`served_model`）と usage を持つ。**応答したモデルが要求と違う（refusal fallback が効いた）エントリは再利用しない**。次の実行で要求モデルに取り直し、recipe と ledger には応答したモデルを書く。変えていない段の再実行は無料で、同じファイルがテストの録画フィクスチャになる。`--refresh <stage>` で標本を取り直し、兄弟の成果物として残す。
3. **生成キャッシュ。** `ImageRequest.key()`（seed、workflow hash、入力資産の hash を含む）→ 既存の候補資産。同じ要求は backend を呼ばない。
4. **ガイドと描画のキャッシュ。** `(page 内容 hash, frame_id, kind, px)` → 一時ディレクトリ。proof と preview を速くする。600 dpi の print は毎回描く。
5. **無効化は軽く判定する。** 候補の `brief_hash`（§4.4）で「指示が変わった後の候補」を stale にする。ビルドグラフ全体のキャッシュキー管理はしない。人間が1コマの指示を直しても、下流で影響を受けるのはそのコマだけである。

### 7.7 費用の管理

- **見積りが先。** `genko studio run --until art --estimate` と `plan_generation` は何も実行しない。
  - LLM の入力: `count_tokens` × 単価表。単価表は `studio/config.json` に置き、コードに埋めない。初期値は opus-5 $5 / $25、sonnet-5 $2 / $10、haiku-4-5 $1 / $5（いずれも 1M トークンあたり、実装時に確認）。
  - LLM の出力: 見込みの出力に、**adaptive thinking の思考トークン**（出力として課金される）を足す。effort high の段では、ledger の実績の中央値から「出力の何倍か」を段ごとに学習し、初期値は保守的に出力の 2 倍を足す。refusal fallback で別モデルが走った分も実績に入る。
  - GPU: 秒数の移動平均。
- **予算は予約制。** `policy.budget`（LLM の USD、GPU の分、1コマの最大枚数、修正の巡回数）。runner は項目を投げる前に、見積りの上限を ledger に**予約**（`reserve`）し、終わったら実績で精算（`settle`）する。並列のワーカー（LLM 既定 4）は予約を含めた残高で判断するので、同時に投げて予算を超えることがない。残高が足りなければ投入せず、`await_budget`（人間のゲート）で止まる。
- **effort が第一のつまみ。** 既定は全段 `claude-opus-5` で、effort を段ごとに変える。

  | 段 | effort 既定 |
  |---|---|
  | bible / script / name | high |
  | critic | medium |
  | recipe / judge / read_name | low |

  モデルを `claude-sonnet-5` や `claude-haiku-4-5` に替えるのは利用者の選択で、自動では替えない。haiku では effort と adaptive thinking を送らない（§6.3）。
- **Message Batches（費用 50%）。** 急がない大量処理（パイロット以外のページの judge を夜間にまとめる、recipe のまとめコンパイル、全脚本の lint）に使う。`--batch` か `policy.llm_mode:"batch"` で有効にする。結果は `custom_id` で引く。
- **安い検査を先に。** 決定的な事前検査（§3.3 S5-5）で、4 枚中およそ1枚は視覚審査に回さずに済む見込み。
- **画像の節約。**
  - 下書き解像度（約 1 MP）で生成し、upscale は採用分だけ。
  - judge が絞り込み、人間は 1〜2 枚を見る。
  - 1コマの枚数と修正巡回に上限を置く。
  - 場所の establishing 画像を背景の参照に使い回す。
- **概算（M0 / M5 で実測して置き換える）。** 8 ページ・約 40 コマの1話を `claude-opus-5` で作る場合:
  - 計画系（約 20 呼び出し、入力の大半がキャッシュ、出力 約 80k トークン + effort high の思考トークン）で数ドルから十ドル前後。
  - judge（画像を送る設定のとき。40 コマ × 1.5 巡 × 入力 約 12k / 出力 約 1k + 思考）で約 $5〜10。Batches なら約半分。
  - LLM 側の合計は 1 話あたり概ね一桁ドルから二十ドル程度。画像を送らない設定では judge が無くなり、計画系だけになる。ローカル画像生成は GPU 時間だけ。
- **ledger（`studio/ledger.jsonl`）。** 呼び出しごとに `{at, node, requested_model, served_model, usage（思考を含む出力）, usd, gpu_s, cache_hit, reservation_id}` を書く。`studio status` が予算に対する消費と予約を出す。

### 7.8 自律度（`studio.policy.autonomy`）

| 値 | 意味 |
|---|---|
| `assist` | AI の書き込みはすべて proposal（`studio/proposals/prop_….json` = ops + preview 資産 + `base_rev`）になる。人間の `studio accept PROP` が今の revision で dry_run し直し、`human:*` としてコミットする（`origin: prop_…`） |
| `gated`（既定） | AI は段の中では自由にコミットする。ゲートは人間 |
| `omakase` | AI はゲートを仮承認（provisional）できる。flagged の候補を含むページは仮承認しない。本番書き出しは、人間がページごとに仮承認を確定し、export を承認するまで拒否する |

プリセット（`studio` / `quick` / `omakase`、§3.4）は gates と autonomy の組をまとめて設定する。

**外部送信の設定ごとの能力表。** どの設定でも、止まった段は人間の作業（チケット）として残り、工程は最後まで進められる。

| 段 | テキストも画像も送る | テキストだけ送る（推奨） | 何も送らない（`GENKO_OFFLINE=1` か両方 false） |
|---|---|---|---|
| bible・脚本・ネーム計画（Namer） | Claude | Claude | 人間が書く（段組 DSL と PanelSpec を GUI か JSON で入力。lint とコンパイラは動く） |
| 写植・人物配置・段組のコンパイル・lint | Genko | Genko | Genko |
| ネームの自己点検 | Claude vision | Claude（計画 JSON と lint の結果だけ） | lint だけ + 人間 |
| recipe の文（シーン文） | Claude | Claude | 決定的テンプレートだけ（品質は落ちる） |
| 画像生成（ローカル ComfyUI） | 可 | 可 | 可（loopback のときだけ） |
| 内容安全・事前検査・類似度 | Genko（ローカル） | Genko（ローカル） | Genko（ローカル） |
| judge（候補の審査）・連続性の点検 | Claude vision | 事前検査の順位 + 人間 | 事前検査の順位 + 人間 |
| アタリの台詞読み取り（M8） | Claude vision | 人間が入力 | 人間が入力 |
| 仕上げ・書き出し | Genko | Genko | Genko |

## 8. インターフェース

### 8.1 CLI

出力の規約は今のまま: stdout に JSON を1つ（`ok` 付き）、人向けのログと進捗は stderr（JSONL）。stdout は常に UTF-8 で書く（`sys.stdout.reconfigure(encoding="utf-8")`）。Windows の cp932 コンソールで表示したい場合は `--ascii`（`ensure_ascii=True`）を使う。

```bash
# 企画〜全体の実行（locality・commercial・content は必須。対話端末なら尋ね、そうでなければ省略はエラー）
genko studio init    demo.genko --premise "夏の屋上で幼馴染が5年前の約束を果たす" --pages 8 --b4 --preset studio \
                     --locality text-only|text+images|none --commercial yes|no --content default|strict \
                     [--llm anthropic|fake] [--image comfyui|mock]
genko studio doctor  demo.genko [--comfy-dir DIR]        # 認証、ComfyUI、部品とライセンス、ノードの pin、VRAM、フォント、パス長
genko studio status  demo.genko                          # 工程ロールアップ、waiting_for、予算と予約、backend、locality
genko studio next    demo.genko [--role namer] [--limit 5]
genko studio run     demo.genko --until bible|sheets|script|name|art|finish|export [--pages 1-3] \
                     [--estimate] [--batch] [--budget-usd 5] [--wait]
genko studio import-script demo.genko script.md          # 既存テキスト → Script（テキストからネーム）
genko studio import-name   demo.genko ./scans/*.png [--detect]   # アタリ取り込み（M8）
genko studio adopt-drafts  demo.genko                    # M0 のサイドカー（脚本・ネーム計画）を op で取り込む（M3）

# キャラ・資産・ライセンス
genko studio chars   demo.genko [--gen-sheets] [--adopt hina:cd_02]
genko studio asset add demo.genko ref.png --kind face --character hina \
                     --origin self|licensed|public_domain|third_party --rights-holder "…" --license "…"
genko studio props   demo.genko [--add prop.json] [--list]
genko studio license list [--profile illustrious-mono@1]
genko studio license set ckpt.illustrious --commercial-ok yes|no --license "…" --source URL --as human:leaf

# コマ単位の生成と修正（画面2の流れ）
genko studio plan    demo.genko --page 3 --frame f3_p1                      # 生成前に指示を確認（GPU なし）
genko studio gen     demo.genko --page 3 --frame f3_p1 [--n 4] [--instruction "…"] \
                     [--mode txt2img|img2img|edit|inpaint] [--profile P] [--size 1472x704] [--seed N | --reroll] [--wait]
genko studio fix     demo.genko --page 3 --frame f3_p1 --candidate cd_01 --scope person \
                     --instruction "顔をもう少し右向きに"
genko studio judge   demo.genko --page 3 [--frame f3_p1] [--batch]           # 画像を送る設定のときだけ
genko studio adopt   demo.genko --page 3 --frame f3_p1 --candidate cd_05 [--fit cover] [--safety-reason "…"]
genko studio review  demo.genko --page 3 --out review.html                  # 静的な確認シート
genko studio finish  demo.genko --page 3

# ゲート・ジョブ・保守
genko studio approve demo.genko name --page 3 --as human:leaf
genko studio approve demo.genko --pending --as human:leaf                   # 残っている仮承認をプレビュー付きで1件ずつ確定
genko studio revoke  demo.genko name --page 3 --reason "3コマ目を割り直す" --as human:leaf
genko studio accept  demo.genko prop_1a2b --as human:leaf                   # assist の提案を受理
genko studio job     demo.genko job_7f21 [--wait] ;  genko studio jobs demo.genko [--state running]
genko studio cancel  demo.genko job_7f21
genko studio backends [--health]
genko studio worker  demo.genko
genko studio log     demo.genko [--cost] [--audit]
genko studio chat    demo.genko                                             # M9

# 既存コマンドの拡張
genko apply   demo.genko ops.json --agent human:leaf [--expect-revision 128] [--dry-run]   # studio ではゲート op に --agent が要る
genko undo    demo.genko [--as human:leaf] [--force] ;  genko redo demo.genko
genko render  demo.genko --page 3 --frame f3_p1 --kind crop|guide:pose|guide:scribble|mask:text|compare --out x.png
genko inspect demo.genko [--panel 3:f3_p1 | --candidate cd_05 | --script | --bible | --studio | --job job_7f21]
genko export  demo.genko ./out --format tiff|pack|psd [--dpi N] [--proof] [--public-manifest]   # --dpi の既定は spec.dpi。studio は preflight を通す
genko doctor  demo.genko [--relink DIR]                                     # 資産の欠落と再リンク
genko gc      demo.genko [--legacy] [--archive-candidates] [--dry-run]      # project.lock を取って走る
genko serve   --root DIR [--port 8765] [--allow-origin URL]                 # --root は必須。トークンはユーザー設定の tokens.json
genko mcp     demo.genko --agent ai:claude-code                             # (mcp)
```

`studio review` は GUI（M7）ができるまでの確認画面である。静的な HTML に候補、比較の重ね画像、点数、コピーできる `genko studio adopt …` コマンドとプロンプトのコピーボタン、残っている仮承認の一覧を並べる。安く作れて、エージェントや離れた場所の確認者にも使える。

### 8.2 HTTP

`handle_request(method, path, body, ctx=None)`（server.py:39）は純関数のまま保つ。`ctx` は `{root, actor, headers}` で、サーバーの `_Handler` が認証と検査の後に作って渡す。`ctx` を省略した直接呼び出し（test_p4.py:110 の `/openapi.json` など）は、パスを取らない経路だけを受け、パスを取る経路は 403 にする。`/v1/studio/` で始まる経路は1つの分岐から `genko.studio.http.handle(method, route, query, data, ctx)` に渡す。

| メソッド / 経路 | 内容 |
|---|---|
| `GET /health` | 認証なしでは `{ok:true}` だけ。トークン付きなら providers、locality、キュー長、`/system_stats` を足す |
| `GET /v1/studio/status?path=` / `GET /v1/studio/worklist?path=` | ロールアップと次の作業 |
| `POST /v1/studio/init` | `{path, premise, pages, spec, preset, locality, commercial, content}`（locality ほかは必須） |
| `POST /v1/studio/run` → **202** `{job_id}` | `{path, until, pages?, batch?}` |
| `POST /v1/studio/plan` | `{path, page, frame_id}` → 生成計画（制御画像とマスクの縮小版は資産 URL） |
| `POST /v1/studio/generate` → **202** / `POST /v1/studio/fix` → **202** | コマ単位の生成と修正（`mode`、`profile`、`size`、`seed` を任意で） |
| `POST /v1/studio/adopt` → 200 | 同期の op（`adopt_candidate`） |
| `POST /v1/studio/approve` | human のトークンだけ |
| `GET /v1/jobs/{id}` / `GET /v1/jobs/{id}/events`（SSE）/ `POST /v1/jobs/{id}/cancel` | `{id, kind, status, progress, result, error, cost}`。ジョブ id は `--root` の下の索引でプロジェクトに引く（§7.3）。SSE は `handle_request` の外の専用ハンドラ。既存の `/v1/apply` の結果も `succeeded` のジョブとして残す（後方互換） |
| `POST /v1/assets` / `GET /v1/assets/{sha256}.png?path=` | 資産のアップロード（サイズと形式に上限、来歴の項目が必須）と取得。`place_asset` は hash だけを受けるので、HTTP ではまずここに上げる |
| `GET /v1/pages/{n}/frames/{id}.png?path=&kind=crop\|guide:pose\|guide:scribble\|mask:text\|compare` | コマ単位の画像。GUI、Claude vision、ComfyUI が使う。PNG の content-type 判定（server.py:139-143）がそのまま効く |
| `POST /v1/apply` | 既存の形 `{ok, applied, snapshot, job_id}` を保つ。`expect_revision` を足し、agent はトークンから決める |
| `POST /v1/export` | 修正: `format` と `dpi`（既定 `spec.dpi`）を尊重する（今は PNG 連番しか出さない） |

**安全（M1。既存の経路も含めて全部に、最初から入れる）:**

今のサーバーは、利用者が開いた任意の Web ページから書き込みと読み出しができる（§0.5 の 21）。AI 機能で扱うもの（未発表の脚本、キャラ、生成画像）が増えるので、互換のための猶予は置かない。

- **トークン。** `/health` の最小応答と `/openapi.json` 以外のすべての経路で `Authorization: Bearer <token>` を要求する（無ければ 401）。
  - トークンは `genko serve` の初回起動時に生成し、ユーザー設定ディレクトリの `tokens.json`（`token → actor`）に置く。project.json には置かない。
  - 人間用とエージェント用を分けて発行し、actor はトークンから決まる。
- **Origin の検査。** `Origin` ヘッダがあり、`--allow-origin` で許した値でなければ 403。ブラウザはオリジンをまたぐ POST に `Origin` を付けるので、これで CSRF を止める。
- **Host の検査。** `Host` が `127.0.0.1:<port>`、`localhost:<port>`、`[::1]:<port>` のどれでもなければ 421。DNS rebinding を止める。
- **本文は JSON だけ。** 本文のある要求は `Content-Type: application/json` でなければ 415。preflight の要らない `text/plain` の POST を受けない。
- **CORS。** `Access-Control-Allow-Origin: *` をやめる（server.py:146, 154）。既定では CORS ヘッダを出さず、`OPTIONS` は 403。`--allow-origin` を指定したときだけ、その origin を返す。
- **`--root`（必須）。** `HeadlessServer(root=…, tokens=…)` の明示の引数にする（cwd を既定にしない）。本文と query の `path`、`dest`、`out`、`put_raster` の `path` は、実パス（シンボリックリンクを解決した後）が root の下になければ 403。
- 既定で `127.0.0.1` に bind する（今と同じ）。API キーを本文で受けない。応答に出さない。
- **変える既存テスト:** `tests/test_server.py` は `HeadlessServer(root=tmp_path, tokens={"t": "human:test"})` で起動し、要求に `Authorization` を付ける。`tests/test_p4.py:110` の直接呼び出しは `ctx` なしのまま通る。
- **足す試験:** `text/plain` の POST が 415、`Origin: https://evil.example` が 403、`Host: evil.example` が 421、応答に `Access-Control-Allow-Origin` が無い、トークン無しが 401、root の外のパスが 403、シンボリックリンクで root の外へ出るパスが 403。

### 8.3 GUI（参考UIとの対応）

GUI は `StudioService` のクライアントである。

- **メモリ上の正本のセッション。** 変更はすべて `MainWindow._apply`（main.py:180-187）→ `apply_ops`（`human:<user>`）でメモリ上のセッションに適用する。ディスクへの commit は `commit.py` で裏のスレッドがまとめて行う（§7.4。1 op ごとにロード・保存しない）。
- 既存の抜け道（main.py:371 のフレーム選択、canvas.py:150-154 のドラッグ中の直接変更）は `select_frame` / `move_line` の op に直す。ドラッグ中は表示だけを動かす。
- 重い処理（生成の投入、描画、commit）は `QThreadPool` で行い、結果は Signal で GUI スレッドに渡す。
- `QFileSystemWatcher` で project.json を見張る。他の書き手の revision が進んだら再読込し、未 commit のローカル op を再生する。
- 見開きは `Page.side` に従って物理的な左右で描き、筆は `add_stroke{space:"spread"}` で送る（canvas.py:67-74 の「相手は常に右」を直す。§9.3）。
- **遅延の予算**は §7.4（筆の反映 16 ms、裏の commit 200 ms）。

**画面構成:**

- 上: 工程バー（StepBar。既存の QSplitter の上、main.py:135-140）。
- 左: ページとレイヤー（既存）+ コマサムネイル。
- 中央: キャンバス。ラスタと placed layer を `render_page(mode="proof")` の pixmap で描き、その上に領域オーバーレイを重ねる。
- 右: タブ「指示・素材」（既定）/「生成設定」（畳む）/「履歴」。
- 下: コマのメモと候補グリッド。

| 参考UIの要素 | Genko の GUI 要素（`app/studio/`） | 裏側 |
|---|---|---|
| 工程バー 1〜5 | `step_bar.py`: 工程ごとの件数バッジ（例「ネーム承認待ち 3」）。ゲートで工程が開く | `studio status` |
| 最近のプロジェクト（表紙・更新日） | 起動画面の一覧（表紙サムネイル、更新日時、locality のバッジ） | `recent.json`、journal |
| 設定 / ヘルプ、初回起動 | 初回起動ウィザード（ComfyUI の URL、profile とモデルの有無、Claude の認証状態、locality と商用の選択）。設定画面は同じ内容 | `studio doctor`、`studio init` |
| テキストからネーム | 企画タブ: premise、ページ数、プリセット、「ネーム生成」 | `studio run --until name` |
| ネーム・画像読込／判定開始／再解析 | 取り込みダイアログと、提案の重ね表示（信頼度の色） | `import-name`、dry_run の提案 |
| ページビュー + 番号バッジ + 読み順 | キャンバス上の読み順バッジと指示オーバーレイ（name / proof だけ）、読み順の文章要約（「右上 → 左上 → 下」） | `leaf_frames()`、render のオーバーレイ、`reading_summary` |
| 元画像 / 比較 / 候補 / 採用中 | `panel_view.py` の表示切替。原寸 / フィット / 100% / 全画面、拡大 | `render --frame --kind` |
| 解析データタブ | 検出ラベル（人物 / 背景 / テキスト / 小物 / コマ枠 / その他）と信頼度、事前検査、審査の軸 | `studio/analysis/`、`inspect --panel` |
| +領域 / 範囲修正 / 領域削除 / 戻す、スコープのチップ | `regions.py` の重ね書き編集。人が作ると `source:"user"` のバッジ | region op、journal undo |
| 今回の指示 / このコマのメモ | `instructions.py` のテキスト欄（フォーカスが外れたらコミット） | `set_panel` |
| タグ・プロンプト辞書 | 指示欄の補完 | `vocab.json` |
| 参照素材（役割の選択、↑、×、登録素材から追加） | 参照リスト（役割コンボ、重み、順序） | `bind_ref` / `reorder_refs` |
| 生成対象・空白の補完 | マスクのプレビュー | `guide.py mask:text` |
| 共通制約 | bible の制約エディタ（常に最初にコンパイルされる旨を表示） | `set_bible` |
| 生成前に指示を確認 | 「計画」モーダル: 日本語の仕様、コンパイル後のプロンプト、ネガティブ、サイズ、制御画像とマスクの縮小版、使う部品とライセンスの状態、送信先（local / cloud）、費用 | `plan_generation` |
| プロンプト / ネガティブ（編集可、コピー） | 「生成設定」タブのプロンプト欄。コンパイル結果を表示し、編集すると上書き（human、pinned の印）。「コンパイル結果に戻す」ボタン。コピーボタン | `set_panel{gen.prompt_override, negative_override}` |
| モデル / モード / 解像度 / シード | 「生成設定」タブ: profile、モード（txt2img / img2img / edit / inpaint）、解像度（既定はコマの比、上書き可）、seed の固定と振り直し | `set_panel{gen.*}`、`studio gen --mode --size --seed --reroll` |
| 参考画像（キャラ・背景・シルエットマスク） | 参照リストの役割コンボに `mask` を持つ。シルエット画像を落とすとマスクになる | `bind_ref{role:"mask"}` → `ImageRequest.mask` / `segmentation` |
| バリエーション生成 / 編集（インペイント）/ 参照して生成 | 生成・修正（マスクブラシ）・参照生成のボタン | ジョブ |
| 採用 / 修正指示 / 履歴を見る | `candidates.py`（点数付きグリッド）、修正欄、`history.py`（系譜ツリー + recipe） | `adopt_candidate`、`request_fix`、recipe |
| キャラクタースタジオ / キャラ登録 | `characters.py`: look、トークン、シート候補、採用、固定 | `upsert_character`、`adopt_sheet` |
| 背景・小物ライブラリ | 場所・小物・資産のブラウザ（来歴とライセンスのバッジ）。コマにドロップすると参照か `place_asset`、小物は PanelSpec の `props` に入る | `upsert_location`、`upsert_prop`、`bind_ref` |
| Qwen 生成・編集 / SD 仕上げ | モデル名のないナビ。profile の選択は「生成設定」タブに置く | profile |
| コマサムネイル | 読み順のストリップ、状態の点（empty / candidates / adopted / stale / flagged）、並べ替え（読み順 / 状態 / 点数） | snapshot |
| 右パネル / ページパネルの開閉、クイックバー（キャラ登録 / 背景・小物 / 履歴） | 表示設定。クイックバーは同じダイアログを開くだけ | – |
| ComfyUI 接続中 / VRAM、外部送信なし | ステータスバー: backend の状態、キュー、locality バッジ（「外部送信なし」「テキストのみ送信」「画像も送信」。接続先から導いた実際の値）。クリックで切り替えダイアログ（human、audit に残る） | `/health`、`set_studio{policy.locality}` |
| （新）承認箱 | `await_gate`、assist 提案、残っている仮承認の一覧。仮承認はページのプレビューを見て1件ずつ確定する（一括ボタンは無い） | `approve`、`studio accept` |

### 8.4 外部エージェント・MCP

- **CLI が第一。** `docs/AGENT.md` に「Studio loop」節を足す。
  1. `studio status` を見る。
  2. `waiting_for` と `next` に従う。
  3. `render --kind compare` や `--mode name` で**見る**（Claude Code は画像を読める）。
  4. 手直しは `apply --dry-run` → `apply --agent ai:claude-code`。**`--agent ai:<name>` は常に付ける**（studio プロジェクトで省略すると `legacy:unknown` になり、承認もロックの解除もできない）。
  5. `studio gen / fix / adopt` を使う。
  6. **ゲートで止まり、人間に聞く。**
- **HTTP。** 常駐型のエージェントはトークン付きで同じ経路を使う。
- **MCP（extra `mcp`、`genko mcp PROJ --agent ai:<name>`）。** 公開するツール:
  - `genko_inspect`、`genko_render`（画像を返す）、`genko_apply`（`dry_run` の既定は true）
  - `studio_status`、`studio_next`、`studio_run`、`studio_plan`、`studio_generate`、`studio_fix`
  - `studio_candidates`、`studio_adopt`、`studio_job`、`studio_review`
  - 入力スキーマは `schema.py` の登録簿から生成する（`strict:true` 相当、`additionalProperties:false`）。
  - **ゲートを承認するツールは出さない。**
- 役ごとの system prompt（`studio/prompts/*.md`）を文書としても配る。人間が Claude Code で Namer や Critic を手で演じられ、自動の役を直すときの最良のデバッグ手段になる。
- 同じコマを2つのエージェントが取り合わないように、任意の弱い予約 `claim_item`（worklist 項目の lease）を用意する。

---

## 9. 品質戦略

漫画として読めるかどうかは、主に6つで決まる。同じキャラに見えること、絵がネームに従うこと、読み順とめくりが崩れないこと、フキダシが顔を隠さないこと、モノクロの仕上げが揃っていること、そして審査の仕組みである。以下、それぞれに専用の仕組みを置く。

### 9.1 キャラクターの一貫性（効きの強い順）

1. **採用済みのシート参照（ゲート①）。**
   - 毎コマの生成に、そのキャラの face / sheet 参照を渡す。SDXL 系の profile では IP-Adapter または reference、指示編集系の profile（Qwen-Image-Edit 系、M9。§13 の決定4 で主にするなら M5）では複数参照画像として渡す。
   - IP-Adapter の FaceID 系は InsightFace のモデル（非商用の条件）に依存するので、既定の profile では使わない（§6.4）。
   - LoRA は任意。利用者が用意したものを bible の `lora{name, weight, trigger, origin, rights_holder, license}` に登録し、workflow が差し込む。来歴の無い LoRA は登録できず、`policy.commercial` では商用可のものだけが使える。
   - LoRA の学習は Genko の外で行う。`genko studio export-dataset --character hina` が承認済みの参照と採用コマ、方言のキャプションを書き出す。
2. **方言ごとの固定トークン。**
   - look から一度だけコンパイルし、人間が編集でき、`locked` で凍結する。
   - コマのプロンプトはキャラを言い換えない。`tokens[dialect]` をバイト単位でそのまま入れる。
   - `never[]` はネガティブに入る。
3. **モノクロでは値が個性になる。**
   - `hair_value`（beta / tone / white）とシルエットの要点をプロンプトに入れ、judge で確かめる。
   - 仕上げは全ページ同じ平網の段で行うので、「トーン髪」はどのページでも同じに見える。
4. **服のタイムライン。** コマの beat が属する scene から `outfits[scene_id]`（なければ `default`）を決める。プロンプトの揺れで服を取り違えない。
5. **seed family。** `seed = seed_base(char) + 1000 × scene 番号 + variation`（1人のコマ）。複数人のコマはコマの seed を使う。n 枚の候補は batch 1 の要求を n 個（seed, seed+1, …）投げるので、各候補の seed で1枚だけ作り直せる（指紋が一致する範囲で、§4.5）。関連するコマは近い seed を使う。
6. **judge の軸 `character.<id>`。** 閾値（既定 0.7）未満の候補は、人間に見せる前に「人物」スコープの顔インペイントを参照付きで1回だけ自動で試す。
7. **パイロットページとスタイル固定。** 1ページ目の art 承認で、profile、LoRA の重み、仕上げパラメータ、seed family を固定する。全体の一貫性を最も安く得られる。
8. **ページ・見開きの連続性の点検。** proof の見開き（§9.3 の修正後の `render_spread`）を vision で見る（画像を送る設定のとき。送らない設定では人間のチェックリスト）。服、髪、小物、目線、左右の立ち位置、時間帯、顔にかかるフキダシ。指摘はチケットにし、自動では直さない。

### 9.2 構図がネームに従う

**人物配置（`blocking.py`）。**

- 入力は PanelSpec の `pos`（left / left_third / center / right_third / right）、`scale`（0–1、コマの高さに対する比）、`shot`（ELS / LS / FS / MS / MCU / CU / ECU）、`facing`（left / right / viewer / away）。
- 出力は人物の箱と頭の箱（mm）、つまり `characters[].box_mm` と `head_mm`。
- shot ごとに見える範囲を決める。CU は頭と肩、MS は腰から上。

**ポーズプリセット（`poses/*.json`）。**

- 正規化した OpenPose-18 のキーポイントで持つ。順は nose, neck, r_shoulder, r_elbow, r_wrist, l_shoulder, l_elbow, l_wrist, r_hip, r_knee, r_ankle, l_hip, l_knee, l_ankle, r_eye, l_eye, r_ear, l_ear。
- プリセットは standing_3q、turning_back、sitting、running、face_closeup_3q など。
- `facing` で左右反転し、shot で切り出し、人物の箱に当てはめて、生成サイズで骨格画像に描く。

**既存マネキンの扱い。** 既存のマネキンは OpenPose の元にしない（§0.5 の 11: 肘・膝・首がなく、size と rot は描画で無視される）。人間が置いたマネキンは「人物の箱と向き」の情報源としてだけ使う（pos → 箱、head の yaw → facing）。マネキンを肘・膝・首まで拡張して OpenPose に変換するのは M9 以降。

**ガイドの重ね方（profile ごと）。**

| 制御 | 元 | 既定の強さ |
|---|---|---|
| openpose | 人物配置 | 0.8 |
| scribble / lineart | NAME のストロークまたはアタリの切り出し | 0.4–0.6、end 0.6（アタリは緩いので） |
| perspective / depth | prim3d の箱と ruler の消失点 | 任意、背景向け |

**`guide.py` はガイドだけを描く。**

- フキダシ、枠線、話者名、オニオンは入れない（`mode=name` の描画はこれらが混ざるので使わない）。
- NAME ストロークは mm 基準の線幅で描く（既存 render の固定 3 px、render.py:476, 479 ではない）。
- 制御の種類に応じて白地に黒か黒地に白で出す。

**PanelFrameMapping（`placement.py`）。** ガイドと配置で同じ変換を使う。

```python
@dataclass(frozen=True)
class PanelFrameMapping:
    frame_rect_mm: Rect
    pad_mm: float
    gen_w: int
    gen_h: int

    @staticmethod
    def plan(frame: Rect, pad_mm: float, caps: BackendCaps, target_px: int = 1_048_576) -> "PanelFrameMapping":
        w, h = frame.width + 2 * pad_mm, frame.height + 2 * pad_mm
        r = w / h
        m = caps.size_multiple
        gw = max(m, round((target_px * r) ** 0.5 / m) * m)
        gh = max(m, round((target_px / r) ** 0.5 / m) * m)
        return PanelFrameMapping(frame, pad_mm, gw, gh)

    def placement_mm(self) -> Rect: ...     # frame ± pad を gen の縦横比に合わせて cover
    def mm_to_gen_px(self, x: float, y: float) -> tuple[int, int]: ...
```

- 制御画像、マスク、文字よけは `mm_to_gen_px` で描く。
- 採用時の `placement_mm()` も同じ mapping から出すので、制御画像・候補・コマが画素単位で揃う。
- フレームを後で動かした候補は `stale_geometry` になり、cover で置き直すか作り直す。
- 生成サイズはコマの比から決める（画面1の「横長のコマに縦長 1024×1365」を起こさない）。

**比較画像（compare）。** アタリ（NAME の切り出し）を赤 50% で候補に重ねたもの。人間の「比較」表示と judge の入力に使う。

**ガイド追従度の事前検査。** 候補のエッジ図（縮小 + Pillow `FIND_EDGES`）と、膨張したガイドのマスクとの IoU。安く決定的で、構図が外れた候補を落とせる。

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
- **スロット探索（`letter.py`）。** ステージの中で走り、結果は `add_line` / `move_line` の明示座標になる（op の中で配置しない。journal の再生がフォントに依らない）。
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
  - 採用後は顔と人物を検出（MockVision / ClaudeVision、または ComfyUI の顔検出）し、写植器を再実行する。移動は `move_line` の提案として出し、人間は戻せる。

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
- derive ジョブとして走らせ、INK 候補として採用する。人間の既存の LT（`lt_convert`）はそのまま残す。
- 生成の目標は、mono 原稿ではグレースケールの漫画調とする（プロンプトに `monochrome, greyscale, manga`、ネガティブに `color`）。仕上げが決定的で全コマ同一なので、それ自体がスタイル一貫性の強いてこになる。

### 9.6 レビューと批評のループ

| 層 | 何を見るか | 担当 | 結果 |
|---|---|---|---|
| 1 決定的 lint | 脚本（字数、話者、予算、めくり）とネーム（密度、最小コマ、shot の連続、180度、状況説明、境界またぎ、めくり、beat を1回ずつ） | `lint.py` | エラーは LLM の修復へ、警告はチケットへ |
| 2 ネーム批評 | 描画した name ページ（読み順の迷い、窮屈さ、弱いめくり） | Claude vision（最大3巡） | 修正案の op（dry_run を通ったものだけ） |
| 3a 内容安全 | 性的・暴力の点数、未成年キャラの在否 | ローカル分類器 + `safety.py` | 遮断（候補にしない）か flag |
| 3b 事前検査 | サイズ、文字よけ、輝度、ガイド追従 | `precheck.py` | 候補を落とす |
| 3c 類似度 | 第三者由来の参照素材との近さ | `similarity.py` | flag（人間が確認） |
| 4 審査 | 比較画像、キャラの顔参照、文字よけの箱、PanelSpec | Claude vision（judge、画像を送る設定のときだけ）。送らない設定では人間 | 点数、verdict、日本語の修正指示とスコープ、`safety` 軸 |
| 5 修正 | 修正指示 → edit / inpaint | 画像モデル | 子候補（最大 `max_fix_rounds`、超えたらチケット） |
| 6 連続性 | ページと見開きの proof | Claude vision | チケット |
| 7 印刷の点検 | print の描画（画内文字、顔にかかるフキダシ、実効 dpi） | vision + 決定的 | preflight の警告 |
| 8 人間 | ゲート①〜④、いつでも修正 | 人間 | 承認、修正指示、採用の差し替え |

- judge の verdict は助言である。自動採用は `policy.auto_adopt_min_score` を設定したときだけ行い、flagged の候補は自動採用しない。
- judge の較正は M5 の受入基準で測る（ラベル付き標本 D5a で、1位と人間の選択の一致率 60% 以上）。ずれていれば axes の重みと prompt を直す。

### 9.7 解像度

- 配置した層ごとに実効 dpi = 元画像の横 px ÷（`placement_mm.width` / 25.4）を記録する。
- 例: 180×80 mm のコマを 600 dpi で刷るには約 4252×1890 px（約 8 MP）が要る（1 MP 級の生成では足りない）。
- 閾値は、グレーの絵が 350、2値の線が 600。下回れば upscale ジョブ（tile、×2〜×4）を採用分だけ出す。
- **×4 でも足りないコマはタイル再生成。** 1 MP を ×4 すると約 16 MP で、B4 断ち切りの1ページ大のコマ（600 dpi で約 6071×8598 px、約 52 MP）には届かない。必要画素が upscale ×4 を超えるコマは、採用画像を init に、同じ制御画像を分割して、タイルごとに低 denoise で再生成して継ぎ合わせる（`mode:"upscale"` の tile workflow、profile の `vram.tile_regen`）。
- 線抽出は upscale の後で行う。
- export の preflight は、コマごとに**達成した実効 dpi** を一覧で出し、閾値未満を警告する（`--force` で通すと、その旨が ai_manifest に残る）。
- 書き出しの dpi は `spec.dpi`（B4 商業原稿は 600）を既定にする。今の pack は 150 dpi に切り（pack.py:12-13）、CLI の既定も 150（__main__.py:35）なので、M6 で直す。

### 9.8 文字と効果音

- 台詞・ナレーション・心の声は Genko のベクター描画。
- ネガティブの基本形に `text, speech bubble, signature, watermark` を入れる。judge の `text_in_image` 軸が拾ったら、文字スコープのインペイントを提案する。
- 効果音（SFX）は v1 では新しい balloon 種別 `sfx` とし、Genko の装飾文字（縁取り、回転、大きさ）で描く（M6）。描き文字を生成資産にするかは未決（§12）。
- 効果（集中線・流線）はコマでクリップする（今は `_draw_effects` がコマをはみ出す、render.py:174-205）。

---

## 10. テスト戦略

### 10.1 原則

- すべてオフライン、Qt なし、リポジトリのルートから実行する（`tests/test_p1.py` は `docs/ops.schema.json` を相対パスで読む）。
- 既存のパターンを使う。メモリ上の episode + `apply_ops`、`main([...])` + `capsys`、`handle_request` の直接呼び出し、`HeadlessServer(port=0)`、Pillow で作るメモリ上の PNG。
- 基準線は 98 passed / 1 failed。落ちている `test_small_kana_sits_right_in_em_box` は、フォントが無いのではなく、同梱の DelaGothicOne で小書き仮名の重心が右に寄らないことが原因（§0.5 の 22）。M1 で `tategaki.glyph` の小書き仮名の配置（tategaki.py:84 付近）を調べて直す。字形そのものが em 箱の中央寄りのフォントなら、試験を「小書き仮名は全角より小さく、右上寄りの箱に収まる」に緩める。あわせてフォントを `src/genko/fonts/` の package data にし、`importlib.resources` で読む（wheel に入らない問題、§0.5 の 29）。
- **既存テストを変えるときは、PR ごとに一覧を書く**（§11 の各マイルストーンの「変える既存テスト」）。黙って直さない。
- **Windows CI。** `windows-latest` のジョブを足し、全試験を走らせる。Windows 固有の試験（下記 §10.7）はここでだけ走る。

### 10.2 偽物（fakes）

- `FakeLLM`: `tests/fixtures/llm/<task>/<input_hash>.json` を再生する。筋書きモード（N 回目の応答）もある。全フィクスチャはその schema で検証する。修復経路を通すための「わざと壊した」フィクスチャも置く。
- `MockImageBackend` / `MockVision`: 決定的。真の顔箱が PNG の tEXt に入る。
- スタブ ComfyUI（`tests/fakes/comfy_stub.py`、`ThreadingHTTPServer`、port 0）: `/upload/image`、`/prompt`、`/history/{id}`、`/view`、`/queue`、`/interrupt`、`/object_info`、`/system_stats` を実装し、受け取った workflow グラフを記録する。

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
| `test_ai_actor_cannot_approve` | `ai:*` の name_ok と approve は拒否され、provisional の規則に従う | M1 |
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
| `test_gc_holds_lock_and_keeps_uncommitted_job_assets` | GC がロックを取り、`succeeded` で未コミットのジョブの資産と新しい未参照資産を消さない | M2 |
| `test_advance_finish_requires_art_ok_when_strict` / `test_legacy_project_unchanged` | strict_gates の有無で挙動が分かれる | M2 |
| `test_ops_registry_complete` / `test_ops_schema_doc_is_generated` / `test_tool_schemas_lint` | 扱う op がすべて登録され、docs が生成物と一致し、`ops` 目録が残り、tool スキーマに `oneOf`・数値や文字列の制約・`false` 以外の `additionalProperties`・再帰が無い | M2 |
| `test_copy_state_covers_all_fields` | Episode の全フィールド（一時のものを除く）が commit と undo を通る | M2 |
| `test_render_spread_right_binding` / `test_set_spread_facing_pairs` / `test_spread_space_stroke` | §9.3 の試験 | M2 |
| `test_speaker_label_absent_in_print` | print に話者名の画素がない | M2 |
| `test_put_raster_rejects_non_image` | 画像でないバイト列は ApplyError | M2 |
| `test_os_replace_retries_on_permission_error`（Windows） | project.json を別プロセスが開いていても、再試行で保存できるか、`busy` で失敗を返す | M2（Windows CI） |
| `test_long_japanese_path`（Windows） | 深い日本語のパスの下で `assets/ab/<64 桁>` を読み書きできる | M2（Windows CI） |

### 10.4 単体試験と golden

- 段組 DSL → フレーム矩形（JSON の golden）。ソースマップの位置。
- `letter.py` のフキダシの箱（package data の同梱フォントで固定、JSON の golden）。
- recipe の方言文字列（tags / natural）。
- `guide.py`: 種類ごとの画素統計（黒の比率、重心）を JSON で持つ。
- `screentone.py`: 目標被覆率 ±3%（150 / 300 / 600 dpi、10〜90%）。
- `PanelFrameMapping`: mm → gen px → mm の往復、size_multiple の丸め。
- `build_kwargs`:
  - haiku に effort と thinking を入れない。
  - `cache_control` を安定接頭辞の最後と、安定した参照画像の最後に置く。可変の画像はその後ろ。
  - `images_to_cloud` が false なら、どの段でも画像ブロックが1つも無い（例外になる）。
  - `text_to_cloud` が false か `GENKO_OFFLINE=1` なら、provider を作れない。
  - `output_config.format` の形。
  - 正準 JSON が同じならキャッシュキーも同じ。
- レスポンス解析: `refusal`（stop_details を記録）、`max_tokens`（streaming で1回だけ再試行）、usage の集計（思考トークンを含む出力）、`served_model` の記録と、fallback で応答したエントリをキャッシュから再利用しないこと、Batches の結果を custom_id で引き当てる（順序を入れ替えた入力で試す）。
- 予算の予約: 並列 4 のワーカーが同時に投げても、予約を含めた合計が予算を超えない。
- locality の導出: `GENKO_COMFY_URL=http://192.168.1.20:8188` は cloud、`http://localhost:8188` は local。`GENKO_OFFLINE=1` で cloud の provider が例外を上げる。
- 内容安全: 筋書きタグ付きのフィクスチャ（`tests/fixtures/safety/`）で、未成年キャラ + sexual は候補にならず audit に残る、flagged は自動採用・仮承認・judge の事前選択から外れる、人間の `safety_reason` 付き採用だけが通る、安全ネガティブはプロンプト上書きでも外れない。
- ライセンス: `commercial:true` で、`commercial_ok` が null か false の部品を使った採用画像があると書き出しが拒否される（`--force` でも）。`studio license set` の後は通る。来歴の無い資産は `register_assets` で拒否される。
- 類似度: 参照素材そのものを候補にした筋書きで flag が付く。

### 10.5 性質試験

- 乱数の種を固定し、split / merge / resize / duplicate / delete / reorder / adopt / unadopt / set_panel の列をランダムに作る。
- 満たすべきこと:
  - `_validate` が常に通る。
  - 保存 → 読込で一致する。
  - placed art の画素が print で溝（コマ外）に出ない（bleed 指定を除く）。
  - 候補と採用の参照が解決する。
- 写植の性質:
  - フキダシはコマの中にある。
  - mock の顔と重ならない。
  - 順序の制約を守る。
- 段組の性質:
  - 葉は内枠の中にある。
  - 重ならない。
  - 溝の幅を守る。
  - 葉の順 = 計画の順。

### 10.6 E2E（オフライン）

`genko studio init demo --llm fake --image mock --preset studio` を実行し、筋書き化した人間の承認者（`human:test`）を付けて `studio run --until export` まで走らせる。

- (a) TIFF と PDF ができる。print に NAME / DRAFT / 候補の画素がない（番兵色で確かめる）。
- (b) 本番書き出しは mock の画素があるので拒否される。`allow_mock_export` で通る。
- (c) 2回目の run は provider の呼び出しが 0（LLM も画像も全部キャッシュ）。
- (d) 生成の途中で `kill -9` し、再開すると完了する。スタブ ComfyUI への `/prompt` の二重投入がない。
- (e) `ai:*` はどのゲートも本承認できない。承認者がいなければ各ゲートで止まる。
- (f) ledger の合計が合う。
- (g) omakase では proof 書き出しに透かしが入り、本番書き出しは、人間がページごとに仮承認を確定して export を承認するまで拒否される。
- (h) `--locality text-only` で走らせると、Claude への要求に画像ブロックが1つも無く、judge の段は人間のチケットになる。`GENKO_OFFLINE=1` では LLM の段がすべてチケットになり、それでも人間の筋書きで書き出しまで行ける。
- (i) 筋書きで LLM の段に毎回失敗させると、`studio run` は1回チケットを作って止まる（同じ項目を出し続けない）。
- (j) 生成の途中でスタブ ComfyUI を再起動すると（handle が消える）、ジョブは lost になり、冪等キーで1回だけ再投入される。
- (k) `commercial:true` と mock の部品では、本番書き出しがライセンスの理由で拒否される。

### 10.7 HTTP・並行性・GUI

- HTTP は `handle_request` の直接呼び出し（`ctx` 付き）と `HeadlessServer(port=0, root=tmp_path, tokens=…)` で試す。全経路でトークンが無ければ 401、§8.2 の CSRF・Origin・Host・Content-Type・CORS・root の試験、`expect_revision` の衝突は 409、`/v1/export` が形式と dpi を守ること、SSE の専用ハンドラが同じ認証を通ること。
- サービス層の並行性: GUI 相当のセッションの commit（`expect_revision` 付き、まとめ commit）と runner の commit を交互に入れ、更新が失われないこと。
- GUI は offscreen の手動チェックリスト（画面2の流れ: 指示 → 計画 → 4候補 → 採用 / 修正 → 履歴）。PySide6 は CI に入れない。GUI の遅延の予算は、Qt を使わない「セッション + 裏の commit」のベンチマーク（`tests/perf/`）で守る。
- **Windows CI（`windows-latest`）:** 全試験に加え、`os.replace` の再試行、長い日本語パス、cp932 のコンソール出力、OS ロック（`msvcrt.locking`）の排他と、プロセスが死んだときの解放。

### 10.8 live 試験と評価

- `@pytest.mark.live` を付け、`GENKO_LIVE_ANTHROPIC=1` または `GENKO_COMFY_URL` がないときは飛ばす。CI では走らせない。
- 評価セットは5つの premise。測るもの:
  - ネームの dry_run 通過率と修復回数
  - キャッシュの命中（`cache_read_input_tokens`）
  - 1話あたりの費用（思考トークンを含む）
  - 人手評価（読みやすさ、キャラの同定）
  - judge と人間の一致率
- **評価に要るデータは作業として計画する**（§11 の各マイルストーンに入れる）。
  - D0（M0）: premise 5 本、読みやすさ・テンポ・めくりのルーブリック、評価者 2 名の手順書。
  - D5a（M5）: judge と人間の一致を測るラベル。40 コマ × 候補 4〜8 枚で、評価者 2 名がそれぞれ1位を選ぶ（一致しないコマは除くか第3の評価者）。
  - D5b（M5）: 盲検のキャラ同定。10 コマ × 主要キャラ 3 人の組を、キャラの参照シートだけを見た評価者 5 名が当てる。
  - D8（M8）: スキャンしたアタリ 20 ページ以上と、その正解（コマの矩形 mm と台詞）。正解は人間が Genko 自身で op を使ってなぞり、JSON で書き出して作る。

---

## 11. ロードマップ

第2版で順序を変えた。最初の価値（Claude が書くネーム）は、v3 形式・資産ストア・journal・ロックのどれも要らない。そこで、今の v2 形式の上に薄い縦串を先に出し（M0）、既存の欠陥の修理（M1）と v3 形式（M2）をその後に置く。

見積りは「このコードベースを知る開発者1人の人週」。レビューと手戻りを含む目安で、データ作り（D…）は別に数える。

依存関係:

```text
M0（Claude ネーム, v2 のまま）─────────────┐
M1（緊急の土台: 性能・安全・ロック・actor）──┴─▶ M2（v3 形式と保存）─▶ M3（コマ配置と studio 状態）─▶ M4（オフライン縦串）─┬─▶ M5（ComfyUI 最初の1頁）─▶ M6（印刷できるモノクロ）
                                                                                                          ├─▶ M7（Studio GUI。M5 と並行可）
                                                                                                          └─▶ M8（アタリ取り込み。M4 の後いつでも）
M5 + M7 ─▶ M9（エージェント・第2 profile・カラー）
```

- M0 と M1 は並行できる（M1 は M0 に依らない）。M0 を最初に利用者へ届ける。
- M3 は M0 のサイドカー（脚本・ネーム計画）を op で project.json に取り込む。
- アタリ起点（画面2）が主な入口なら、M8 を M5 の前に引き上げる（§13 の決定1）。
- 旧版（第1版）との対応: 旧 M3 → M0（縦串部分）と M4、旧 M0 → M1 と M2、旧 M1 → M3、旧 M2 → M4、旧 M4 → M5、旧 M5 → M6、旧 M6 → M7、旧 M7 → M8、旧 M8 → M9。

合計の目安は約 37 人週 + データ作り（D0、D5a、D5b、D8）。

### M0 ネーム縦串（v2 のまま、Claude がネームを書く）— 約 3 人週 + D0

範囲: premise から、Claude が bible・脚本・ネーム（段組、コマ指示、台詞の配置）を書き、今の Genko の op にコンパイルして原稿にする。**新しい保存形式、資産ストア、新しい op は作らない。** 画像は送らない（テキストだけ）。

| PR | 内容 |
|---|---|
| M0-1 | `genko.studio` の骨組み（core）: `LLMRequest` と純関数の `build_kwargs`（locality の検査込み）、FakeLLM、schema `bible@1` / `script@1` / `name_plan@1`。`AnthropicProvider`（extra `ai`。構造化出力、adaptive thinking、effort、互換表、streaming、stop_reason、served model の記録、`GENKO_OFFLINE`） |
| M0-2 | 段組 DSL コンパイラ → 既存 op（`split_frame{frame_id, axis, ratio, gutter_mm}`、`resize_frame`）。既存の `split_frame` は子の id を指定できない（id は `new_id()`、models.py:324-337）ので、1ページを1つのロックの中で「段の分割を apply → 新しい葉を読み順で読み戻して slot に対応付け → 列の分割を apply → … → 台詞を apply」と段ごとに `apply_ops` を呼び、最後に1回だけ保存する。途中で失敗したら保存しないので、ファイルから見れば全か無か。ソースマップ |
| M0-3 | `letter.py` v1（`tategaki.compose` の寸法、`_columns` の明示改行、コマ内の貪欲な配置）→ 座標を明示した `add_line`。bible は `set_bible`、コマ指示の要約はページの `set_note` |
| M0-4 | 脚本 lint・ネーム lint・修復ループ（最大2回）、テキストだけのネーム批評。段の結果（批評の点数、失敗、変更なし）はサイドカーの `studio/drafts/stages.json` に残し、同じ入力で繰り返さない（M3 で `record_stage` に移す） |
| M0-5 | CLI: `genko studio init --locality text-only`（M0 では `text-only` と `none` だけ）、`studio run --until name`、`studio review`（name 描画 + コマ指示の一覧の HTML）。脚本・ネーム計画・PanelSpec は `studio/drafts/*.json` のサイドカー（原稿の状態ではない。M3 で取り込む）。commit はロックの中でロードし、`agent="ai:namer"` を渡す |
| M0-6 | LLM 応答キャッシュ（served model）、ledger（思考を含む出力、予約）、`--estimate` |
| D0 | premise 5 本、ルーブリック、評価者 2 名の手順書 |

受入基準:

- オフライン（FakeLLM）: 固定した premise から 8 ページのネームが決定的に出る（段組と写植の golden）。全ページが dry_run と lint を通る。
- 出力の project.json は `7e65e4c` のビルドでそのまま開ける（書式を変えていない）。
- 画像ブロックを含む要求が1つも無い。
- live（opt-in、5 premise）: 全ページが修復2回以内で通る。2ページ目以降で `cache_read_input_tokens > 0`。1話の費用を報告する。評価者 2 名で 5 話中 4 話以上が読みやすさ 4/5 以上。
- **単独でも出せる成果:** 人間の漫画家が清書できる AI ネーム。

変える既存テスト: なし。

### M1 緊急の土台（性能・安全・ロック・actor）— 約 2.5 人週

範囲: AI の書き手が増える前に、今のコードの欠陥を直す。書式は変えない。

| PR | 内容 |
|---|---|
| M1-1 | undo 履歴を deepcopy しない（`Episode.__deepcopy__`、commit 時の旧状態の移し替え）、Stroke の不変化、性能試験 |
| M1-2 | HTTP の安全（全経路のトークン、Origin / Host の検査、JSON 限定、CORS `*` の削除、`--root` 必須と閉じ込め、`handle_request` の `ctx`、SSE 用の別ハンドラの枠） |
| M1-3 | `ProjectLock`（OS ロック + token、予備経路の墓石 rename、所有者を確かめる release）、CLI と HTTP はロックの中でロード |
| M1-4 | actor の受け渡し（`--agent`、HTTP はトークンから）、studio での `legacy:unknown`、ゲート op の actor 規則、`lock_page` / `unlock_page` の所有者規則、台詞 op と `set_ticket` の page lock 検査、AGENT.md の更新 |
| M1-5 | 版ゲート R0（`version` を読む、新しすぎるファイルを拒否、未知キーを `extra` で保持）。**書式は v2 のまま単独でリリース** |
| M1-6 | フォントを package data に、小書き仮名の配置の調査と修正（または試験を緩める）、CLI の UTF-8 出力と `--ascii`、Windows CI |

受入基準:

- 既存の 98 試験 + 直した小書き仮名の試験が緑（Linux と Windows）。
- §10.3 の M1 の試験が通る（深さ 50 で 1 op 100 ms 未満、CSRF / DNS rebinding / root、ロックの競合、AI のロック奪取の拒否）。

変える既存テスト: `tests/test_server.py`（`root=tmp_path` とトークンを渡す）。`tests/test_tategaki.py::test_small_kana_sits_right_in_em_box`（調査の結果、緩める場合だけ）。

### M2 v3 形式と保存 — 約 4.5 人週

| PR | 内容 |
|---|---|
| M2-1 | 汎用 `_copy_state`、`_validate`（触れた範囲、資産の欠落は警告）、`put_raster` の画像検証、`genko doctor` |
| M2-2 | `Page.id`、v3 移行（`MIGRATIONS`、`v2_to_v3`、バックアップ）、page_locks と tickets の id 化、`_remap_page_refs`、`duplicate_page` の frame id 対応表、story と texts の同一化 |
| M2-3 | `AssetStore`、ラスタとストロークの内容アドレス保存（`name_strokes` / `ink_strokes` の重複をやめる）、差分保存と遅延読み込み、Windows の `os.replace` 再試行、`genko gc --legacy`（ロックを取る） |
| M2-4 | journal（正規化 ops + チェックポイント、上限）、`genko undo / redo`（プロセスをまたぐ） |
| M2-5 | スキーマ登録簿、生成した `OPS_SCHEMA` と `docs/ops.schema.json`（`ops` 目録を保持 + `anyOf` の機械用スキーマ）、役ごと op ごとの strict tool、tool スキーマの lint（8 op の登録漏れを解消） |
| M2-6 | `strict_gates`、`Page.side`、`set_spread` の対の検査、`add_stroke{space:"spread"}`、`render_spread` と canvas.py の見開き、print の話者名を消す、opacity 0 の修正 |

受入基準:

- §10.3 の M2 の試験が通る。
- v1 と v2 のフィクスチャが読めて、v3 で保存し直しても失うものがない。
- 16 ページのネーム作業（lt_convert 1 ページを含む）で project.json が 1 MB 未満、journal の1行が 64 KB 未満。
- 別プロセスの `genko undo` が効く。`docs/ops.schema.json` が生成物で、`ops` 目録が残る。

変える既存テスト: `tests/test_p1.py:84`（`pages/001/bg.png` ではなく `bg.raster_relpath` の資産パスを見る）。`tests/test_p1.py:118-124` は `ops` 目録を残すので変えない。見開きの既存テスト 3 本は旧プロジェクトでは警告だけなので変えない。

### M3 コマ配置と studio 状態（画素境界）— 約 3.5 人週

範囲: 生成画像を「コマに置く」ことを正しくし、studio の状態を project.json に持つ。まだ生成はしない。

| PR | 内容 |
|---|---|
| M3-1 | `LayerKind.PLACED` と Layer の新フィールド（io / migrate / snapshot）、`Episode.assets` の注入 |
| M3-2 | `placement.py` と placed layer の描画（元画像から LANCZOS、frame / bleed クリップ）。`Frame.bleed` を `_clip_mask` と `raster._clip` に実装。bleed 辺の枠線を描かない |
| M3-3 | `Frame.panel`、`studio/state.py` の `PanelSpec`、`set_panel`（human の上書き欄を含む）、寿命のルール（§4.7）、`studio adopt-drafts`（M0 のサイドカーを op で取り込む） |
| M3-4 | studio の state と ops（キャラ、場所、小物、脚本、page plan、承認、取消、領域、参照、`record_stage`、`request_fix`、`set_finish`）。来歴必須の `register_assets` |
| M3-5 | `add_candidates`（safety 必須）、`set_candidate`、`adopt_candidate`、`unadopt`、`set_placement`、`place_asset`（hash だけ） |
| M3-6 | `guide.py` の crop、`render --frame --kind crop`、`GET /v1/pages/{n}/frames/{id}.png`。GUI のキャンバスがラスタと placed layer を描く |

受入基準:

- 縦横比の違う PNG 3枚を3つのコマに採用し、600 dpi の print で溝の画素が白いこと。
- 元画像から再標本化されている（ページ引き伸ばしよりエッジが鋭い）。
- undo で採用が戻る。ページを複製しても絵が新しいフレーム id に付いている。
- project.json に base64 もストローク座標もない。print に NAME / DRAFT / 候補が出ない。
- M0 で作ったネームを取り込むと、PanelSpec が葉に付き、脚本が `studio.script` に入る。

変える既存テスト: なし（見込み）。

### M4 オフライン縦串（モックで生成側の契約を凍結）— 約 3.5 人週

範囲: 企画から書き出しまでを FakeLLM・MockImageBackend・MockVision・MockSafety で通す。

| PR | 内容 |
|---|---|
| M4-1 | `blocking.py`、ポーズプリセット、`guide.py` の全種類、`recipe.py` v1（role → 運び手の対応表）、`precheck.py` |
| M4-2 | MockImageBackend（顔箱と安全の筋書きを tEXt に）、MockVision、MockSafety、JobStore（subrequest、lost）、runner、`commit.py`、worklist（`record_stage` とチケットによる停止） |
| M4-3 | `policy.py`: locality（接続先からの導出、`GENKO_OFFLINE`、能力表）、ライセンスの preflight（mock の部品は `commercial_ok:false`）、内容安全の流れ（遮断、flag、監査）、予算の予約 |
| M4-4 | CLI `studio init / run / status / next / plan / gen / adopt / review / approve --pending / license / doctor` |
| M4-5 | export の preflight、`ai_manifest`（内部 / 公開）、mock ガード、proof の透かし、仮承認のページ単位の確定 |
| M4-6 | ネームの自己点検（vision。画像を送る設定のときだけ）、Batches |

受入基準:

- `genko studio init demo --llm fake --image mock --locality text-only --commercial no && genko studio run demo --preset omakase --until export --proof` が CI で、ネットワークなしで 8 ページの PDF を出す。所要時間の目標は 120 秒以内で、CI のマシンで計測して閾値を固定する（第1版の 60 秒は、commit ごとのロード・保存と print の描画を考えると楽観的だった）。
- ネームの golden が安定している。読み順と写植の性質試験が通る。
- 本番書き出しは mock ガードとライセンスの preflight で拒否される。
- §10.6 の (c)〜(k) が通る（再実行で provider 呼び出し 0、kill -9 と ComfyUI 再起動からの再開、失敗の筋書きで止まる、locality、内容安全）。

### M5 最初の1ページ（ComfyUI、一貫性、安全と権利）— 約 5 人週 + D5a、D5b

| PR | 内容 |
|---|---|
| M5-1 | `ComfyUIBackend`（stdlib、batch 1 × n、submit / poll / fetch / cancel、lost の検出、`/object_info` と pin の検査、hash 名アップロード、locality の導出） |
| M5-2 | `workflows/manifest.json`（部品とライセンス欄、カスタムノードの pin、VRAM 表）と最初の profile。**画像スタックは1つだけで、どれにするかは §13 の決定4 に従う**（24 GB 級なら Qwen-Image 系、12 GB なら SDXL / Illustrious 系 + ControlNet openpose / scribble + IP-Adapter。どちらも部品のライセンスを確かめてから。FaceID 系は使わない） |
| M5-3 | ローカルの安全分類器（profile のノードか extra `safety`）、judge の `safety` 軸、類似度の検査 |
| M5-4 | GPU スケジューラ、ジョブの進捗・取消、`studio worker`、HTTP `/v1/studio/*` と `/v1/jobs/*`（SSE、ジョブの索引） |
| M5-5 | キャラクタースタジオ（シート生成 → `adopt_sheet` → 固定）、ClaudeVision.judge（画像を送る設定のとき）と人間の審査画面、修正 → edit / inpaint（領域マスク）、自動の顔インペイント1回 |
| M5-6 | 複数人物のコマ（構図 → 人物ごとのインペイント）、パイロットページとスタイル固定 |
| D5a / D5b | judge と人間の一致のラベル、盲検のキャラ同定の標本（§10.8） |

受入基準:

- スタブ ComfyUI の試験がオフラインで通る（束縛、再接続、lost からの再投入、取消、不足ノード、pin の無いノードの警告）。
- live: パイロットページの全コマが採用まで行き、1コマの枚数の中央値 ≤ 8。
- D5a で judge の1位と人間の選択の一致率 ≥ 60%（画像を送る設定のとき）。
- D5b で盲検の評価者が同じキャラを 90% 以上同定する。
- 同じマシンで、指紋（部品の hash、ComfyUI とノードの版、attention と sampler の実装、GPU とドライバ）が一致すれば同じ recipe から bit 単位で同じ画像が出る。一致しなければ `fingerprint_mismatch` の印が付き、元の資産が正として残る。
- `commercial:true` で、`commercial_ok` を確かめていない部品を使った採用画像の書き出しが拒否され、確かめた後は通る。

### M6 印刷できるモノクロ — 約 4 人週

| PR | 内容 |
|---|---|
| M6-1 | `screentone.py` と render の仕上げ、`_draw_tone` の置き換え |
| M6-2 | `lineart.py` と derive ジョブ、upscale 後の線抽出、大きいコマのタイル再生成 |
| M6-3 | 顔を避けた写植の再配置（`move_line` の提案） |
| M6-4 | フキダシ描画の修正（尾、楕円の余白、ルビの揃え）、SFX 種別、効果のクリップ |
| M6-5 | 書き出しの修正: pack と print の既定を `spec.dpi` に（pack.py の 150 上限を外す、CLI `--dpi` の既定を `spec.dpi` に）、preflight のコマごとの実効 dpi、ファイル名の無害化（Windows で使えない文字）、EPUB の rtl と固定レイアウトのメタデータ |
| M6-6 | PSD をページごとに（またはページを別ファイルで）書き直す: 配置した絵・INK・写植をそれぞれ実レイヤーに、写植はラスタ化したレイヤー、レイヤー名は Unicode（`luni` ブロック）。人間の仕上げ担当（CLIP STUDIO、Photoshop）への受け渡しに使えることを実物で確かめる。確かめられない場合は PSD を書き出し形式の一覧から外す |

受入基準:

- 被覆率が ±3%。パイロットページの2値 TIFF に灰色の画素がない。pack が 600 dpi で出る。
- mock の顔フィクスチャでフキダシと顔の重なりが 0。
- PSD を CLIP STUDIO と Photoshop で開き、レイヤー名とレイヤー数が期待どおり（手動チェックリスト）。
- live のパイロットページで人間の校正確認が通る。

変える既存テスト: `tests/test_p5_psd.py`（レイヤー数の期待を「ページの実レイヤー数」に変える）、`tests/test_p5_pack.py`（600 dpi の B4 は CI で重いので `dpi=72` を明示する）。

### M7 Studio GUI — 約 5 人週

| PR | 内容 |
|---|---|
| M7-1 | メモリ上の正本のセッション、裏のまとめ commit、差分保存、`QFileSystemWatcher` の再読込と op の再生、遅延のベンチマーク |
| M7-2 | StepBar、承認箱（仮承認の1件ずつの確定）、ステータスバー（backend、locality の切り替え）、起動画面（最近のプロジェクト）、初回起動ウィザードと設定 |
| M7-3 | コマ表示（元画像 / 比較 / 候補 / 採用中、表示倍率）、候補グリッド、指示・素材パネル、コマのメモ、生成設定タブ（プロンプト上書き、モード、解像度、seed）、解析データタブ |
| M7-4 | 領域エディタ、履歴ドロワー（系譜 + recipe）、キャラクタータブ、背景・小物ライブラリ、パネルの開閉とクイックバー、サムネイルの並べ替え |
| M7-5 | GUI の抜け道（main.py:371、canvas.py:150-154）を op に直す。rebase undo。触れたページだけ複写する copy-on-write |

受入基準:

- 画面2の流れを再現する offscreen の手動チェックリストが通る。
- サービス層の試験で、並行する AI の commit を GUI の保存が消さない。
- ベンチマークで、筆の反映 16 ms、裏の commit 200 ms（16 ページ）。

### M8 アタリ取り込み（画面2の入口）— 約 3 人週 + D8

| PR | 内容 |
|---|---|
| M8-1 | `studio import-name`（一括、原本は不変資産、来歴 `self`、DRAFT に配置、ページ mm への位置合わせ） |
| M8-2 | `XYCutDetector` → `apply_layout` の提案（信頼度付き、`studio/analysis/` に全出力） |
| M8-3 | `ClaudeVision.read_name`（手書き台詞の読み取り。画像を送る設定のときだけ。送らない設定では人間の入力画面）→ 下書きの StoryLine、`replace_regions{source:"detected"}` |
| M8-4 | GUI の提案の重ね表示と確定 |
| D8 | スキャンしたアタリ 20 ページ以上と正解（§10.8） |

受入基準:

- D8 で、コマの 90% 以上が完全一致か 5 mm 以内で復元される。
- すべて dry_run の提案として出て、確定まで原稿を変えない。
- `source:"user"` の領域は再解析で消えない。

### M9 エージェントと拡張 — 約 3 人週

| PR | 内容 |
|---|---|
| M9-1 | MCP サーバー（extra `mcp`。役ごとの strict tool、ゲートを承認するツールは出さない）、AGENT.md の Studio loop、Claude Code 用の手順書 |
| M9-2 | `studio chat`（strict tools、提案の dry_run） |
| M9-3 | 第2 profile（Qwen-Image の t2i と指示編集、複数参照）と、カラーの出力 profile（§13 の決定2・4 で主にするなら M5・M6 に前倒し） |
| M9-4 | マネキンの拡張（肘・膝・首、size と rot の描画）と OpenPose 変換 |
| M9-5 | judge と recipe の Batches 既定化、候補の退避（`--archive-candidates`） |

受入基準:

- MCP のツールだけを渡された Claude Code が、「2-3 のコマをもっと低いアングルで作り直して一番良いものを採用」を手作業なしで完了する（採用は art ゲートの前まで。ゲートは人間）。

---

## 12. リスクと未決事項

### 12.1 リスク

| リスク | 対策 |
|---|---|
| キャラの一貫性は依然として最難関。参照と固定トークンだけでは長期連載で足りないかもしれない | M5 で数値で測る（D5b）。利用者の LoRA（来歴つき）を今すぐ使えるようにする。学習用データの書き出しを用意する。judge の character 軸と自動の顔インペイント |
| mono の線画の品質が商業水準に届かない | グレースケールの漫画調を直接生成する。決定的な仕上げを全コマ同一にする。任意の SD 仕上げ。ゲート③は人間 |
| 解像度の差（約 1 MP の生成と 600 dpi の印刷。B4 1ページ大は約 52 MP） | 実効 dpi をコマごとに記録する。採用分だけ upscale、×4 で足りないコマはタイル再生成。preflight で達成 dpi を示す |
| LLM の費用がコマ単位のループで膨らむ | effort を段ごとに変える、キャッシュの効く接頭辞、ページ単位の recipe、事前検査、Batches、思考トークンを含めた見積り、予約制の予算、`--estimate` |
| ComfyUI のワークフローの陳腐化（カスタムノードとモデルの版違い） | manifest で必要ノードを commit まで pin し、`/object_info` で投入前に検査する。workflow hash を recipe に残す。出荷するテンプレートは1 profile から |
| **カスタムノードの供給網（悪意のあるノードの前例がある）** | 公式に pin したノード以外を使う workflow は警告。`studio doctor --comfy-dir` で commit を照合。Genko はノードを自動で入れない |
| GPU の非決定性（ドライバや版で結果が変わる） | 再現は2段で扱う。指紋（部品 hash、ComfyUI とノードの版、attention と sampler の実装、GPU とドライバ）が一致すれば bit 一致を期待して M5 で試験し、一致しなければ `fingerprint_mismatch` の印を付けて元の資産を正とする。候補は1枚ずつの seed で作るので、1枚だけ作り直せる |
| ComfyUI の再起動で handle が消える（`/history` はメモリだけ） | `lost` として検出し、冪等キーで1回だけ再投入する（§6.4、§7.5） |
| VRAM 不足・モデルの積み直し | profile の VRAM 表（8 / 12 / 24 GB）と lowvram の構成、GPU スケジューラでまとめる、`/free`。足りない profile は投入しない |
| 構造化出力の制約（`oneOf`・再帰・範囲の制約なし） | 平らな段組 DSL、`anyOf` で書く機械用スキーマ、役ごと op ごとの strict tool、tool スキーマの lint。範囲はコンパイラと lint で検査する |
| 拒否（恋愛・暴力の表現） | `stop_reason` を先に見る。`stop_details` を記録する。任意の refusal fallback（応答したモデルを記録し、キャッシュで再利用しない）。常に手作業の道を残す |
| actor の自己申告 | ワークフローの守りと明記する。studio では名乗らない呼び出しを `legacy:unknown` にし、承認とロック解除を拒む。HTTP ではトークンで actor を固定する。本当の分離が要るなら GUI の承認を利用者の鍵で署名する（未決 12） |
| GUI とエージェントの並行書き込み | `revision`、ロック内ロード、`expect_revision`、GUI のメモリ上のセッションと裏のまとめ commit、再読込と op の再生、人間がロックしたページを worklist が飛ばし、AI はロックを外せない |
| **apply と保存の性能**（undo 履歴ごとの deepcopy、ストロークの肥大） | M1 で undo 履歴を写さない、Stroke を不変にして共有、性能試験。M2 でストロークを blob に、差分保存、project.json の大きさの試験 |
| **HTTP 経由の CSRF・DNS rebinding・任意ファイルの読み書き** | M1 で全経路にトークン、Origin / Host の検査、JSON 限定、CORS `*` の削除、`--root` の閉じ込め。試験で守る |
| 古いビルドが v3 ファイルを壊す | 版ゲートを先のリリースで出す（M1-5）。最低読み取り版を README に書く。既に入っている古いビルドの危険は残る |
| **原稿が外部に出る（未発表作品）** | `studio init` で明示選択（黙った既定なし、推奨は画像を送らない）。locality は接続先ホストから導く（LAN やクラウドの GPU は cloud）。`GENKO_OFFLINE=1` で強制停止。`build_kwargs` が画像ブロックを作らない試験。ステータスバーに実際の値を常時表示。`ai_manifest` は既定で内部用 |
| **部品のライセンス（商用利用）** | 部品ごとの `license` / `commercial_ok`（既定は未確認）、recipe への記録、`policy.commercial` での書き出しの拒否（`--force` でも通らない）。既定の profile は FaceID 系（InsightFace）、AGPL の検出器、非商用のアップスケーラを使わない構成にし、利用者が確かめるまで未確認として扱う |
| **画像の内容安全**（アニメ系の SDXL モデルは性的な画像を出しやすい。例の bible のヒロインは 16 歳） | 既定の安全ネガティブ（上書きで外せない）、ローカルの分類器を必須に、judge の `safety` 軸、bible の年齢による未成年キャラの性的画像の遮断（候補にしない、採用できない、監査記録）、flagged の自動採用と仮承認の禁止。筋書きタグ付きのフィクスチャで試験する |
| **著作権・画風の模倣** | 参照素材と LoRA の来歴（origin・権利者・license）を必須にし、商用モードでは不明なものを拒否。指示・辞書・bible の作家名と作品名をブロックリストで検査（商用では拒否）。`describe`（画像解析・プロンプト抽出）は来歴のある資産だけ。候補と第三者由来の参照との類似度を測り、閾値を超えたら人間の確認に回す。出力段の類似（特定の作品への依拠と類似）は最終的に人間が判断する |
| **AI 生成物の権利と開示** | AI だけで作った部分に著作権が認められない可能性がある。journal と ai_manifest で、コマごとの人間の関与（ネームの修正、指示、選択、加筆、仕上げ）を記録する。出版社・プラットフォームの AI 利用規定への開示は利用者が決める（§13 の決定6）。Genko は法的判断をしない |
| 資産の肥大化 | `genko gc`（ロックを取る、参照されていない、30日より古い）。却下した候補は履歴として残す。2000 件で退避する。journal とチェックポイントに上限 |
| フォント依存の決定性（写植の寸法、golden） | フォントを package data として同梱し、試験では同梱フォントを使う。写植の結果は op の明示座標として保存し、再計算しない |
| **Windows 固有の問題**（`os.replace` の `PermissionError`、長いパス、cp932 のコンソール） | 再試行つきの置換、`\\?\` の長いパス、UTF-8 の stdout と `--ascii`、Windows CI |
| 参考UIの全機能を追う範囲の膨張 | 道具が先、GUI は後（M7）。GUI は確認・比較・採用・修正に絞る |

### 12.2 実装上の未決事項（推奨既定つき）

利用者が決めること（入口、出力、外部送信、商用と画像モデル、自律度と内容、開示ほか）は §13 に分けた。ここに残すのは実装側で決めることである。

| # | 決めること | 推奨既定 |
|---|---|---|
| 1 | PanelSpec の置き場 | `Frame.panel`（型は `studio/state.py`）。ページ単位の辞書にはしない。M0 の間はサイドカー |
| 2 | 候補メタの置き場 | project.json（小さなレコード、undo が効く）。2000 件を超えたら却下分を `studio/history/` に退避 |
| 3 | 既定のプリセット | `studio`（①〜④を人間）。素早い下書きには `quick`（§13 の決定5 に従う） |
| 4 | AI（art_director）に採用権限を与えるか | `gated` では与える（ページ単位の art ③ が人間の関所）。flagged の候補は常に人間。より厳しくしたいプロジェクトは `assist` |
| 5 | 複数 actor のもとでの永続 undo の意味 | M2 は「自分の最新 commit だけ戻せる」線形 undo（チェックポイント + 再生）。M7 で正規化済み ops の再適用による rebase undo |
| 6 | 斜めコマ・多角形コマ | 当面は長方形で近似する（段組と XY-cut が大半を覆う）。多角形フレームは M9 以降、需要を見てから |
| 7 | MCP の実装 | extra `mcp` の公式 SDK。依存が重荷になったら stdlib の stdio JSON-RPC に替える |
| 8 | テキストからネームの既定のラフ | 構造的（人物配置 + ポーズプリセット + prim）。安く編集できる。AI のラフ画像はページ単位の任意 |
| 9 | mono の戦略 | グレースケールの漫画調を直接生成する。カラー生成 → 変換はテンプレートとして残す |
| 10 | LoRA の学習 | Genko の外で行う（データセット書き出しだけ用意する。来歴を引き継ぐ） |
| 11 | GUI と headless の単一ライター化 | ファイルロック + revision + GUI のメモリ上のセッションと裏の commit（M7）。`genko serve` を必須の単一ライターにするのは将来の選択肢 |
| 12 | 承認の本人性 | 当面はワークフローの守り（自己申告 + `legacy:unknown` + HTTP トークン）。必要になれば GUI の承認をユーザーの鍵で署名する |
| 13 | 省略できるゲート | `bible` と `script` は `quick` で省略可。`name` と `export` は仮承認を含めて最終的に人間がページごとに確認する |
| 14 | 見開きの開始側 | `start_side:null`（綴じ方向の既定: 右綴じは奇数が左、見開きは (2, 3) から）。雑誌の扉の都合だけ `set_meta` で上書き |
| 15 | 資産 GC の既定 | 採用中・参照中・journal の保持範囲内・未コミットのジョブは残す。どこからも参照されず 30 日より古いものだけ消す。却下した候補は既定で残す。隔離した資産は GC で消さない |
| 16 | 描き文字（SFX）を生成資産にするか | v1 は Genko の装飾文字。生成の描き文字は M9 以降に文字スコープの生成として検討 |
| 17 | 1コマの枚数と修正巡回の既定 | 下書き 4 枚（batch 1 × 4）、上限 8 枚、修正 2 巡。超えたらチケット |
| 18 | refusal fallback（beta）を既定で有効にするか | 対話的な呼び出しで有効、設定で無効化できる。Batches では使わない。応答したモデルを記録し、fallback の応答はキャッシュで再利用しない。実装時に API 文書で形を再確認 |
| 19 | ロックの方式 | OS のファイルロックを主、`O_EXCL` + token を予備。予備経路のファイルシステムでは同時実行を1プロセスに絞るよう警告 |
| 20 | 安全分類器の置き場 | profile の ComfyUI ノード（GPU 機で走る、画像を動かさない）を第一候補、extra `safety`（onnxruntime）を第二候補。どちらもローカルで、モデルのライセンスを部品として登録する |
| 21 | PSD の扱い | M6 でページごとの実レイヤーに直す。CLIP STUDIO と Photoshop で確かめられなければ形式の一覧から外す |

---

## 13. ユーザー確認事項

以下は利用者（原稿の持ち主）が決めることで、実装の順序や既定値が変わる。推奨既定は、今の Genko の強み（mono の商業原稿、縦書き写植、コマ単位の op）と、未発表原稿を扱うことを前提にした案である。決まるまでは推奨既定で進める。M0 は決定1・2・4 に依らず進められる。決定3 で「テキストを送る」（a か b）を選べば M0 は Claude でネームを書き、(c) なら段組 DSL・写植・lint の道具として人間が使う。

| # | 決めること | 選択肢 | 推奨既定 | 決定で変わること |
|---|---|---|---|---|
| 1 | **主な入口** | (a) テキスト起点: premise → 脚本 → Claude が書くネーム（「ネームから執筆」）<br>(b) 手描きアタリ起点: 描いたアタリを取り込み、コマ単位で生成（画面2） | (a)。今の保存形式の上の薄い縦串（M0）として最初に出す。アタリ取り込み（M8）はその後 | (b) を主にし、すでに手描きのアタリがあるなら、M8 を M5 の前に引き上げる。M0 は (b) でも「台詞と指示の下書き」に使える |
| 2 | **出力の形** | (a) モノクロの紙の原稿（B4、600 dpi の2値、出版社への入稿）<br>(b) カラーの webtoon / SNS 向けページ（参考UIのプレビューのような絵） | (a)。今の Genko の書き出しとスクリーントーンはこれに向けて作ってある。カラーは後から第2の出力 profile として足す | (b) を主にするなら、カラー profile（M9-3）を M5・M6 に前倒しし、screentone（M6-1）の優先度を下げる。縦スクロールの割り付け（strip）の品質作業が増える |
| 3 | **外部送信（未発表の原稿を Claude に送ってよいか）** | (a) テキストも画像も送る<br>(b) テキストだけ送る<br>(c) 何も送らない（`GENKO_OFFLINE=1`） | (b)。`studio init` で必ず明示的に選ぶ（黙った既定はない）。未発表の商業作品ではテキストだけ送り、画像は送らない。vision の審査（judge）と連続性の点検は人間の作業になる | (a) なら judge とネームの自己点検が自動になり、人手が減る（費用は増える）。(c) なら AI の段はすべて人間の作業になり、Genko は写植・段組・生成（ローカル）・仕上げの道具として働く（§7.8 の能力表） |
| 4 | **商用利用と画像モデルの構成（GPU の VRAM）** | 出版・販売するか。GPU は 8 / 12 / 24 GB のどれか。最初の profile をどれにするか | 商用なら、ライセンスを確かめて `commercial_ok` を付けた部品だけを使い、書き出しで強制する。24 GB 級なら Apache-2.0 とされる Qwen-Image 系を第一候補に（配布元で要確認）。12 GB なら SDXL / Illustrious 系を、部品ごとのライセンス確認の後で。どちらでも InsightFace 系の FaceID と非商用のアップスケーラは使わない | 最初の profile（M5-2）と VRAM の構成が決まる。Qwen-Image 系を主にするなら、第2 profile（M9-3）を M5 に入れ替える。8 GB なら lowvram の構成になり、IP-Adapter を外して品質が下がる |
| 5 | **自律度と承認、内容の方針** | 自律度: `studio`（人間が承認）/ `quick` / `omakase`（AI が仮承認）<br>内容: 恋愛・暴力の程度、未成年キャラの扱い | `studio` プリセット（キャラシート、ネーム、ページごとの絵、書き出しを人間が承認し、AI は事前選択まで）。画像の内容安全は常に厳格（性的表現なし、暴力は軽度まで、未成年キャラの性的表現は常に遮断で変更不可） | `omakase` なら人手は減るが、書き出し前にページごとの確定が要る。内容の方針（`policy.content`）を緩めると flag が減るが、未成年の遮断は緩められない |
| 6 | **AI 利用の開示と人間の関与の記録** | ai_manifest を (a) 内部だけに持つ (b) 公開版を同梱する。出版社・プラットフォームの AI 利用規定に合わせるか | (a)。記録（コマごとの部品、recipe、人間の関与）は常に内部に残し、出版社などが求めるときだけ `--public-manifest` で公開版（メモ・指示・プロンプトを除く）を出す | (b) なら書き出しに公開版が毎回入る。開示の文面と範囲は利用者が決め、Genko は記録の正確さだけを担う |
| 7 | **LLM の予算とモデル** | 1話あたりの上限（USD）。モデルは `claude-opus-5` / `claude-sonnet-5` / `claude-haiku-4-5` | `claude-opus-5` を全段で使い、effort を段ごとに変える。1話 15 USD を上限に予約制で止める。安いモデルへの切り替えは利用者の選択で、自動では替えない | 予算を下げると、`--estimate` と予約で早めに止まる。Sonnet / Haiku に替えると費用は下がるが、ネームの修復回数と品質を D0 で測り直す |
| 8 | **参照素材と LoRA の出所の方針** | (a) 自作と権利を確認した素材だけ<br>(b) 第三者の素材も参照に使う（非商用の習作） | (a)。商用モードでは (a) が強制される。第三者の画風の模倣や、作家名・作品名での指示はしない | (b) は非商用のプロジェクトだけで可能で、その資産を使った画像は商用の書き出しで拒否される |

