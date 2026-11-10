---
name: genko-manga
description: Genko Studio を MCP で操作して、企画から漫画のネーム（コマ割り・台詞の配置）、作画（生成した絵をコマに置く）、仕上げ、書き出しの依頼までを進める手順。漫画のネーム、コマ割り、作画、Genko の話が出たら使う。
---

# Genko で漫画を作る

Genko は漫画原稿のシステムで、MCP サーバー `genko` として接続されている（道具は `mcp__genko__<名前>`。古い Hermes（v0.21 より前）では区切りの `__` が `_` 1 つになる）。
Genko は文章も絵も作らない。企画書・脚本・ネーム計画と絵は、あなたが作る。Genko はそれを検査し、コマ割りと縦書きの写植を計算し、
絵の依頼パック（サイズ・プロンプトの下書き・ガイド画像・参照画像）を用意し、取り込んだ絵をコマに置いて仕上げる。
承認と本番の書き出しは人間が行う。この文書の最新版は resource `genko://guide/skill` で読める。

## 最初に

1. `mcp__genko__projects` でプロジェクトを確かめる。無ければ `mcp__genko__create_project`（例: name `summer.genko`、pages 8）。
2. `mcp__genko__inspect` を `target: "rules"` で呼び、ネームの規則を読む。入力の形（`target: "schemas"`）は、書いたものがエラーで返ってきたときだけ読めばよい。

## 会話の名前（session）

同じ Genko を複数の会話（Telegram のスレッドなど）から使うときは、どの道具にも `session` に会話の名前（例 スレッド番号 `"9204"`）を渡す。
変更は `ai:<名前>/<session>` で記録され、`undo` はその会話の変更だけを戻し、`next` の `claim` もその会話のものになる。
別の会話がこの 15 分に書いた原稿に書き込もうとすると、一度だけ `code: "book_in_use"` で止まる（書き込まない）。
人に確かめてから同じ呼び出しをもう一度すれば通る。別々に進めるなら、原稿を複製して使う。

## ループ

1. はじめに `mcp__genko__next` で次の作業を1つ取る（他のエージェントと並行して動くときは `claim: true`）。`tools` に使う道具の目安がある。
   書く道具の返事には次の作業が `next` に入っているので、続けて `next` を呼ばなくてよい。
2. 作業の種類ごとに下の手順で進める。
3. 書く道具はそのまま `commit: true` で呼んでよい（エラーがあれば何も書かずに返る）。`issues` に `severity: "error"` があれば、`path` の場所だけを直して送り直す。
4. 同じ指摘が 3 回直しても消えないときは、無理に続けず `mcp__genko__ask_human`（`page`、あれば `frame_id`、`item` に作業の種類）で人間に相談する。その作業は人間が閉じるまで `next` に出なくなるので、次の作業へ進む。
5. `next` の `items` が空で `waiting_for` だけになったら、人間の承認待ち。下の「承認を頼む」をする。

## ネーム

- `write_bible`: 企画書を書いて `mcp__genko__set_bible`。登場人物の `tokens_en`（英語の見た目の記述）は、絵の依頼にそのまま入るので丁寧に書く。
  - `author`: 作者名（扉に入る）。
  - `lettering`: 作品に合う文字の設定。種類（speech・thought・shout・whisper・narration・sfx・title）ごとに `font`（`inspect` の fonts の key）・`scale`（大きさの倍率）・`weight`。
    ジャンルと雰囲気で選ぶ。空なら台詞と同じ書体。叫びは 1.3 倍・太字、ささやきは 0.8 倍が既定。例:
    - 少年・青年・アクション: 叫び `gothic`、心の声 `maru`、ナレーション `mincho`。
    - シリアス・ミステリー: ナレーション `mincho`、心の声 `antique`（台詞と同じ）で静かに。
    - ギャグ・コメディ: 叫び `sfx_pop` か `gothic`、ツッコミも `gothic`、心の声 `maru`、ナレーション `maru`。
    - かわいい・日常・恋愛: 台詞 `maru`、心の声 `hand`、ナレーション `maru`。
    `weight: "heavy"` は画数の多い漢字の中の白をつぶしやすい。叫びは太い書体（`gothic`）を選び、`bold` までにする。
  - 人物の `look.clothes_value`: 服の印刷の仕方（`beta` 黒ベタ・`tone` トーン・`white` 白）。報告した人物の範囲に、どのコマでも同じに効く（黒い服が灰色になったりベタになったりしない）。
  - `props`: 何度も出る小物（id・name・desc・tokens_en）。参照画像は `apply_ops` の `attach_reference`（`target.prop_id`）で付ける。コマの `props` に id を書くと、その参照が絵の依頼に付く。
- `write_script`: 脚本を書いて `mcp__genko__set_script`。beat ごとに `page` を決める。見せ場（reveal）は偶数ページの先頭、引き（hook）は奇数ページの最後。
- `plan_page`: `mcp__genko__inspect`（target `page`）でそのページの beat を読み、ネーム計画を書いて `mcp__genko__submit_name`。
  - 見せ場は大きく: めくってすぐの見せ場は `template` の `reveal_top`・`reveal_bleed`、締めのページは `finale_bleed`、動きのある場面は `action_slant`。
  - コマの `bleed: true` で断ち切り（紙の端まで）、`slant`（mm）で次のコマとの境を斜めに。ページの `spread: true` で次のページと見開き。コマの間の広さはページの `tier_gap_mm`（段の間、既定 7）と `col_gap_mm`（横に並ぶコマの間、既定 3）。できたあとで動かすときは inspect page の `gutters`（frame_id と index）を move_gutter に渡す。
  - 1 ページ目を扉にするなら `title: true`（`template` は `title_top` か `reveal_bleed`）。題名と作者名が入る。
  - `fx` の言葉（雨・汗・集中線・水しぶきなど。知らない言葉は警告が出る）は仕上げで Genko が描くか、絵の依頼文に入る。`emphasis` も依頼文に入る。
  - 効果音（balloon `sfx`）は、コマの `sfx_at`（音の出どころ、コマの中の 0..1 の [x, y]）の近くに置かれる。
  - 台詞は話者の顔の近くに置かれ、短い尾が話者を指す（コマに話者 1 人だけ・顔のすぐ横なら尾は付かない）。
    コマの枠にかかることもある。同じ人の続く台詞は、2 つのフキダシがつながって置かれる。人物の `pos` を絵の配置どおりに書く。
  - 台詞の書き方（写植）:
    - `breaks` は意味の切れ目（文節）で折る。「まんが作りの／モヤモヤを／ズバッと解決する／このコーナー！」。1 列 5〜8 字、列の長さは揃えない。
    - 台詞の終わりの「。」は書かない（Genko も描かない）。文の途中の「。」は列を変える。「！」「？」「……」はそのまま。
    - 形は気持ちで選ぶ（ネーム計画の `balloon`）: 普通の会話 `speech`、説明・解説 `rounded`、心の声 `thought`、叫び・驚き `shout`、
      ささやき `whisper`、電話やテレビ `electric`、場面の説明 `narration`。1 ページに 1 種類だけにしない。
    - 呟き `aside`: フキダシ無しの小さな手書き文字（少し傾く）。独り言・ツッコミ・「やってもた…」のような漏れた声、絵の説明書き
      （「男女入れ替えもの」）に。1 ページに 1〜3 か所。
    - 名札: コマの人物に `tag`（「スクール担当 つら姉」）。その人物のそばに小さな角丸の囲みで横書きに入る。人物紹介・解説漫画の初登場で。
    - 飾り枠: 見出し・テーマの提示・箇条書きのメモは `fancy_box`（二重線と角飾り）、`tone_box`（薄いトーンを敷いた囲み）、
      `dotted_box`（点線の囲み）。しっぽは付かない。
    - 要点だけ大きく・太く: `{太|心地よさのこと}`・`{大|…}`（台詞の中の一部）。1 つのフキダシで 1 か所まで。
    - 短い返事（「はい」「え？」）は短いまま。長い台詞は 2 つのフキダシに分ける（同じ人なら並べて置かれる）。
- `review_name`: `submit_name` の返事に付いた画像で、読み順の迷い・窮屈なコマ・弱いめくりを確かめる（見返すときだけ `mcp__genko__render`）。直すなら `submit_name` を `replace: true` で送り直す。よければ `mcp__genko__record_review`（`input_hash` は作業項目の値）。
- 承認の依頼はまとめて出す: ネームと作画は `pages` に複数のページ、設定画は `character_ids` に複数の人物を入れて 1 回で。
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

- 絵柄はマンガの絵柄カタログ（https://manga.akiba.tokyo.jp）から選べる。`mcp__genko__style_catalog`（引数なし）で 1段目のジャンル、
  `style_id` でその絵柄の言葉と 1 つ下の段（`children`）が出る。どの段で止めてもよい。絵柄は本の印象を決めるので、
  `ask_human` で候補を見せて人に選んでもらい、`mcp__genko__use_style`（`style_id`、`commit: true`）で原稿に写す。
  以後の依頼パックの prompt・avoid・参照画像（`refs/style_catalog.png`）に入る。原稿の絵柄はサイトが変わっても変わらない。
  `style_catalog`（`project` だけ）で新しい版が出ているか（`newer`）が分かるが、写し直すのは人に確かめてから。
  白黒の絵柄はカラーの原稿（webtoon）には使われない。試しのページで絵柄が固定されたあとは人しか変えられない。
  人がサイトのページの URL（https://manga.akiba.tokyo.jp/n/…）を渡したら、そのまま `style_id` に入れてよい。
  同じ名前の絵柄が別の分類にあることがある。名前だけで指定されたら `style_catalog`（`title`）で重なりを調べ、分類（`path`）と
  id を人に確かめてから `use_style` する（`same_title_elsewhere`・警告 `style_same_title` が出たら必ず確かめる）。
  人が絵柄を変えたら、承認済みの設定画は前の絵柄のまま。描き直して承認を頼む。

- `make_sheet`: `mcp__genko__generation_request`（`character_id`）で設定画の依頼パックを受け取り、画像生成で作る。
  顔が正面を向いたアップを必ず入れる（承認時に顔の参照として切り出される）。画像を返された `inbox` のフォルダに保存し、
  `mcp__genko__import_images`（`request_id`、`images: [{file, origin}]`）で取り込む。取り込んだら人間に選んでもらう（承認を頼む）。
  要らなくなった自分の候補（取り込み直した・前の絵柄）は `apply_ops` の `withdraw_candidates`（`character_id`、`candidate_ids`）で
  承認箱から下げる。同じ画像を取り込み直すと、前の候補に顔の位置（`face_box01`）などが入り、候補は増えない。
- `gen_panel`: `mcp__genko__generation_request`（`page`, `frame_id`）で依頼パックを受け取る。参照画像（`files.references`）は大事な順に並んでいる。画像ツールが受けられる枚数が少ないときは前から渡す。
  1〜2 枚しか渡せないなら `files.references_sheet`（顔・設定画・絵柄の見本を並べた 1 枚。コマと同じ縦横比なので、出てくる絵の形もそろう）を渡す。
  - `request.prompt` は下書き。書き直してよいが、登場人物の見た目の記述（`characters[].tokens_en`）は言い換えない。
    `avoid` にあるもの（文字・フキダシ・効果音・署名・枠線、モノクロのページでは色も）は描かせない。
    `request.color` が true のページはカラーで、それ以外はモノクロ（グレースケール）で作る。
  - 参照画像（`files.references`: 設定画・顔・場所・`refs/style_pilot.png`）は、画像ツールが受け付けるなら必ず添える。
    `files.composition`（構図）と `files.pose`（人物の位置と向き）も参考として添えてよい。線をなぞらせる必要はない。
    `notes_for_agent` にマネキンの注記があるときは、`files.pose` の棒人形がポーズの指定。体の向きと手足の角度を合わせる。
  - サイズは `size.suggested_px`。画像ツールが決まったサイズしか出せないときは `size.tool_sizes` の先頭を使う（切れる方向が書いてある）。
  - `keepout` の場所（台詞が入る）は静かに空けておく。プロンプトの下書きにも書いてある。
  - ふつうは 1 枚作り、`inbox` に保存して下の `take_panel_art` で取り込む。比べて選びたいときだけ 2〜4 枚作って一度に `import_images` する。
  - `origin` には `tool_id`（例 `openai:gpt-image-1`）、`model`、実際に使ったプロンプト（`prompt`）、`params`、添えた参照（`refs_used`）を書く。
- 複数人物のコマ（依頼パックに `steps` がある）: まず全員の構図で作って採用候補を決め、`report_regions` で人物ごとの領域を報告し、
  似ていない人物だけを `generation_request`（`mode: "inpaint"`、`parent` に採用候補、`focus_character` に人物 id）で一人ずつ直す。
- 顔だけ直す: `generation_request`（`mode: "inpaint"`、`parent`、`regions: ["face:<人物 id>"]`）。マスクの白い所だけを描き直させる。
- 全体を少し直す: `mode: "edit"` と `parent`、`instruction` に直す点。`source.png` を元画像として画像ツールに渡す。
- `import_pending`: 依頼済みで画像がまだのコマ。`inbox` に画像を置いて `import_images`。
- 標準の流れ: `mcp__genko__take_panel_art`（`request_id`、`image: {file, origin}`、人物のいるコマは `regions`）で、
  取り込み → 採用 → 解像度が足りなければ拡大して採用し直し → 顔と人物の位置の報告、を 1 回で行う。止まったら `stopped_at` に
  どの段かが入る。人の承認は別。
  画像ツールが白い余白を残したら `crop01`（`[x, y, 幅, 高さ]`、絵の中の 0..1）で取り込む前に切り抜く（`regions` の `box01` は切り抜いた後の絵の中）。
  拡大しても本の解像度に届かないときは警告 `dpi_short` が出る。大きい画像で作り直すか、そのまま進めるかを決める。
- `review_candidates`（2 枚以上を比べるとき）: `mcp__genko__candidates`（metrics の `rank` が小さいほど目安が良い）と `mcp__genko__render`（`kind: "compare"`、`candidate_id`）で比べる。
  赤い線がネームの構図。構図がネームに合うか、人物が設定画に似ているか、手足の破綻、画内の文字、台詞の場所が空いているかを見て、
  `review_candidates`（`page` と `frame_id` も必須）で点数（0〜1）とメモを残し（人物のいるコマは `checks`: `likeness` 設定画に似ているか 0〜1・`hands` ok / broken / none・`text` 絵の中の文字 none / some・`cut` 顔や手が枠で切れる none / some も必須）、良いものを `mcp__genko__adopt`（`regions` も渡せば、拡大と位置の報告まで 1 回で済む）。どれも駄目なら直しの依頼を作る。
  1 コマ 8 枚・直し 2 巡を超えると、そのコマは人間の判断待ちになる。
- `report_regions`・`upscale_panel`: 作業項目に採用中の候補（`adopted`）と、ネームでの人物の目安（`figures`）が入っている。
  `inspect` を呼ばずに、絵で位置を確かめて `report_regions`、または `adopt`（`candidate_id` に `adopted`）で拡大し直す。
- `fix_panel`: 人間の指示（`comments`）どおりに直しの依頼を作る（`instruction` に指示を入れる）。絵を採用し直すとチケットは閉じる。
  絵ではない直し（台詞・線・効果など）なら `apply_ops` で直し、`mcp__genko__resolve_ticket`（`ticket_id` は `tickets` の値、`note` に何をしたか）で閉じる。
- `fix_page`: ネーム承認後のページへの人間の指示。`apply_ops` で直してから `resolve_ticket` で閉じる。閉じられるのは人からの直しの指示だけ（承認の依頼や質問は人が閉じる）。人は `genko studio reopen-ticket` で開き直せる。
- 効果の言葉: 「フラッシュ」は絵の上に放射線（ウニフラッシュ）を描く。「白で塗る」「ホワイトアウト」はコマを白で塗るので、絵のあるコマには入らない。
- 効果線と顔: `finish_page` は、報告された顔（`report_regions`）の手前で集中線・流線・フラッシュの線を止める（止まる所で細くなる）。
  顔を報告する前に置いた効果線にも、あとから同じ設定を足す。自分で置くときは `add_effect` / `edit_effect` の `params` に
  `avoid`（`[{"ellipse": [cx, cy, rx, ry]} | {"path": [[x, y], …]}]`、ページの mm）と、描く範囲を絞る `within`（`[[x, y], …]`）を渡せる。
- `report_regions`: 採用した絵の顔と人物の位置を `mcp__genko__report_regions` で報告する（`box01` は画像の中の 0..1 の `[x, y, 幅, 高さ]`）。
  人物がいない絵なら `apply_ops` の `record_review`（`kind: "regions"`、`frame_id`、`input_hash` に採用中の候補 id）。
- `upscale_panel`: 採用した絵が印刷の解像度に足りない。`mcp__genko__upscale`（`page`・`frame_id`、`method` は `inspect` の `upscalers`、既定 `genko`）で拡大した候補を作り、`adopt` で置き直す。
  画像ツールに高解像度化があれば `generation_request`（`mode: "upscale"`）でもよい。拡大しないなら `record_review`（`kind: "upscale"`、`input_hash` に採用中の候補 id）で理由を残す。
- `finish_page`: `mcp__genko__finish_page` を `commit: true` で呼ぶ（目・鼻・口にかかる台詞の移動、尾を報告した顔へ、効果・漫符・雨）。気になるときだけ先に `commit: false` で見る。
  話していない人の顔のほうが近い台詞は動かさず `suggest_move`（行き先の案 `to`）で知らせる。絵を見て、読み違えるなら `move_line` で動かす。
  漫符と雨は 1 つずつ別のレイヤー（`layer_id`）に入る。絵に同じ記号がもう描かれていたら、そのレイヤーだけ `delete_layer` で消す。
  報告した顔は、網点が薄くなって白く浮く。
- 採用した絵を差し替えると、ページは仕上げの前に戻り、顔の位置は古いものになる（`check` の `regions_stale`）。
  `report_regions` で送り直し、`finish_page` をやり直して、尾と記号を画像で確かめる。
- `check` の `cut_by_panel` は、報告した顔、または顔の報告のない人物が枠で切れている所。示された量だけ `set_placement` の `offset_mm` をずらす。
  顔が枠の中にある人物の体（足など）が切れるのは、ふつうの構図なので知らせない。
- op の鍵や型を間違えると、エラーにその op の書き方（と知らない鍵）が付く。
- コマは割る（`split_frame`・`cut_frame`）ほかに、描いて作れる: `add_frame`（`rect: [x, y, 幅, 高さ]` か `points`）。
  最初に描いたコマは基本枠と入れ替わる。`delete_frame` で 1 つだけ消せる（ほかはそのまま）。
  角を丸くするのは `set_frame` の `corner_mm`（半径 mm）。
- `add_stroke` の線の描き味: `taper_in_mm`・`taper_out_mm`（入り・抜きの長さ）、`pressure_opacity`（弱い筆圧で薄く）、
  `stabilize_speed`（速い所ほど補正）、`post_fit`（この mm までのゆれを除いて曲線に置き換え）。
- 塗りつぶし（`fill`）: `tolerance`（色の誤差 0〜100、大きいほどうすい線を越える）、`expand_mm`（線の下へ広げる）、
  `ignore: ["draft", "text"]`（下描き・台詞を壁にしない）。`fill_enclosed`（`poly`）は囲んだ中の線で閉じた所だけを塗る。
- `add_stroke` の `snap_lines_mm`: 線の端が、その距離までの近い線にくっつく（形を閉じる・線をつなぐ）。
  `erase` の `texture`（soft / rough）と `snap_ruler`（定規に沿って消す）。
- フキダシ: style の `line_rgb`・`fill_rgb`・`fill_opacity`（線と中の色）、`text_dx_mm`・`text_dy_mm`（文字だけずらす）。
  しっぽの `vias`（折れ線の角）、`width_mm`。`cut_balloon`（`points`、`width_mm`）でフキダシの一部を削る。
- 写植: 台詞の書き方 `{#3060c0|…}`（色）・`{×1.3|…}`（大きさ）・`{縦中横|12}`。style の `align: "justify"`（均等）、
  `ruby_scale`・`mono_ruby`、`below_layer`（そのレイヤーの下に描く）。
- 描いた線の直し: `trace_edit`（`action` widen / narrow / redraw / redraw_width / join / simplify、`points` はなぞった線）。
  1 点の太さは `vector_edit`（`action: set_pressure`、`index`、`pressure`）。
- ペン入れと新しいペン・ペイントのレイヤーの線は、描き始めたコマの中だけに出る（`panel_each`）。
  コマをまたいで引く線は、そのレイヤーを `set_layer panel_each: false` にする。
  `tail_hidden` は尾がフキダシの中に埋もれている所（`move_line` で大きさを変えると尾は外へ出し直される）。
- 取り込んだ絵のレイヤーにも `set_layer_mask`・`paint_mask` で範囲を付けられる。
- 線をくっきりさせたいコマは `mcp__genko__derive`（`kind: "lineart"`）で採用中の絵から線を抜き出し、`adopt`（`to: "ink"`）で絵の上に置く。
  モノクロの原稿では、グレーの絵は印刷のときに Genko が網点に変える。画像はグレースケールで、トーンを描き込みすぎずに作る。
- 効果音はネーム計画の台詞で `balloon: "sfx"` にする（Genko が大きな縁取り文字で描く。画像に描かせない）。
- 台詞の見た目は `apply_ops` の `edit_line` で直せる: `balloon`（speech・rounded・box・cloud・thought・shout・flash・whisper・narration・sfx・none）、`style`（`font`: antique・gothic・mincho・maru・hand・sfx・sfx_pop、`size_mm`、`outline_mm` など）、`tails`（`[{to, via}]`、曲がったしっぽ・複数）。人間が直した見た目は変えない。

## 人と同じ道具で描く・直す

人が画面でできることは、`mcp__genko__apply_ops` の op で全部できる。エラーがあれば何も書かずに返るので、そのまま `commit: true` でよい。
- op の名前の一覧は `mcp__genko__inspect`（`target: "ops"`）、引数の形は同じく `op` に名前（カンマ区切りで複数）を渡して引く。
- ページ・レイヤー・線と塗り・ブラシ・線の編集・変形・コマ・文字・フキダシ・効果線・定規・素材・3D・本（表紙・帯・作品の結合）・
  アニメーション・PSD の読み込み・図形・範囲の使い方の詳しい説明は `inspect`（`target: "drawing"`）で必要なときに読む。
- 調べる: `inspect` の `snapshot`（レイヤーと台詞の今の設定。`page` でそのページだけ）、`materials`・`fonts`・`brushes`・`plugins`。
- 見る: `mcp__genko__render`（`mode: print` は印刷と同じ見え方、`layer_id` はそのレイヤーだけ）。
- 点検: `mcp__genko__check`（人の「入稿前の点検」と同じ一覧）。取り消し: `mcp__genko__undo`（自分の最後の変更だけ）。
- 書き出し: `mcp__genko__export`（`format` の既定は png、`dpi` の既定は 300。ほかの形式は人に頼まれたときだけ）。承認は要らない。書き出し先は原稿の `exports/`。
  時間がかかると `job` が返るので、`mcp__genko__export_status`（`job`。最大 60 秒、終わるまで待ってから返す）で結果を取る。

## 承認を頼む

`waiting_for` の内容ごとに `mcp__genko__request_approval` を 1 回出す。

- `name` / `art`: `gate` と `pages`。
- `sheet`: `gate: "sheet"` と `character_id`。候補が複数あれば `note` におすすめを書く。
- `export`: 全ページの仕上げが済んだら、`mcp__genko__preflight` で止めている理由を確かめ、`mcp__genko__export_proof` で校正を出してから依頼する。

依頼を出したら `mcp__genko__review_page` で確認ページ（review.html）を作り、その場所（`path`）と何を見てほしいかを、
メッセージで人間に知らせる。review.html は絵を中に入れた 1 ファイルなので、そのまま渡す（zip にまとめない）。承認するのは人間で、Genko アプリの承認箱（または確認ページにあるコマンド）で行う。

チャットでの承認: 人がチャットではっきり承認したら、`mcp__genko__record_chat_approval`（`gate`・`pages` か `character_id` と
`candidate_id`・`message` に人の言葉をそのまま・`person` にチャットでのその人の名前）で人の名前で記録する。許可や設定は要らない。
人がその原稿で止めていれば `chat_approval_off` が返るので、承認箱で承認してもらうよう伝える。止めた設定を AI が戻さない。
承認のためのシェルのコマンドやスクリプトを人に流させない。

## してはいけないこと

- 人がはっきり承認していないのに承認を記録しない（`record_chat_approval` は人の言葉があるときだけ）。
- 人間が承認したページのコマ割りを変えない。直す必要があれば人間に頼む。（`cut_frame`・`move_gutter`・`set_frame` の `poly` も同じ）
- 人間が描いた線・塗り（`fill`・`fill_area`・`transform_area`・`delete_area`・`paste`・`reshape_stroke` で触れるもの）を、頼まれずに動かしたり消したりしない。
- 人間が固定した欄（pinned）・人間が描いた領域・承認済みの設定画を変えようとしない。
- 台詞や効果音を「絵の中の文字」として描かせない。文字は Genko が描く。
- 画像のバイト列を道具の引数に書かない。画像はファイル（`inbox`）かアップロードで渡す。
- 正式な書き出し（書き出しの承認として記録するもの）はしない。それは人間だけ。`export` で各形式に書き出して見せるのはよい。
