# 人と同じ道具で描く・直す（op の使い方の詳しい説明）

案内書（SKILL.md）から分けた、`apply_ops` の op と書き出しの細かい説明。`mcp__genko__inspect` の `target: "drawing"` で読める。
op ごとの引数の形は `inspect` の `target: "ops"`（`op` に名前）で引く。

人が画面でできることは、`mcp__genko__apply_ops` の op で全部できる（一覧は resource `genko://ops`）。エラーがあれば何も書かずに返るので、そのまま `commit: true` でよい。

- ページ: `add_page`（`after` を省くと本文の最後、表紙より前）・`delete_page`・`duplicate_page`・`reorder`・`set_spread`（見開き）・`set_page_spec`（原稿用紙を変えるとコマや台詞も合わせて動く）。
- 調べる: `mcp__genko__inspect` の `target` で `snapshot`（レイヤーの名前・種類・不透明度・合成・マスク・表示色・フォルダ・参照、台詞の書式とフキダシ、3D）、`materials`（貼れる素材の id）、`fonts`（`style.font` に使える書体）、`brushes`（`kind` に使えるブラシ）。
- 見る: `mcp__genko__render` の `mode: print` は印刷と同じ見え方、`layer_id` はそのレイヤーだけ。
- レイヤー: `add_layer`・`duplicate_layer`・`merge_down`（ペン同士は線のまま）・`delete_layer`・`set_layer`（`exportable: false` で下描き＝書き出さない、`color` で画面だけの表示色、`reference: true` で参照レイヤー）。
  マスクは `set_layer_mask`（`area` の所だけ見せる・`fill`・`invert`・`enabled`・`delete`）と `paint_mask`（`show: true` で見せる、`false` で隠す）。
- 線と塗り: `add_stroke`（`kind` はブラシ。自作のブラシは `define_brush` で定義してから。コマの外に描いた線はコマの形で切られて見えない。そのときは返事の `results` に `outside_panels` が出る）、`erase`、`fill`・`fill_area`（`fill` の `reference: "reference"` は参照レイヤーの線だけを見て塗る）、`gradient_fill`（`from`・`to`・色・不透明度、`shape: radial` で円・`ellipse`＋`ratio` で楕円、`repeat: repeat|mirror` で繰り返し、多色は `stops: [[位置 0〜1, [r,g,b], 不透明度], …]`（2〜16 色））、`filter_raster`（`levels`・`curve`・`hue`・`blur`…。`area` を渡すとその範囲の中だけ。`levels` は `gamma`（中間）・`out_black`・`out_white`・`channel`、`curve` は `points: [[元, 後], …]`（0〜255）、`brightness_contrast`、ゴミ取りは `despeckle`（`size_mm`・`what: ink|holes|both`）、スキャンの線画抽出は `lineart`（`threshold`・`radius`・`min_px`・`drop_blue` で水色の下描きを消す・`keep_solid`・`rgb`）、`sharpen` は `amount`、光彩拡散は `glow`（`radius`・`amount`・`threshold`）、雨は `rain`（`count`・`length`・`angle`・`width`・`rgb`・`opacity`）、`gradient_map` は `stops: [[位置, [r,g,b]], …]` で好きな色の並び）。
- ブラシ: 入っているものは `inspect` の `brushes`（G ペン・筆・スプレー・点描・点線・破線・レース・草むら・木の葉・ハート・星・カリグラフィ・水彩など）。`define_brush` で `tip`（`round`・`flat`・`image`＋`tip_png`）・`pattern`・`spacing`・`scatter`・`stamp_size`・`size_jitter`・`turn_jitter`・`count`・`speed`・`post_smooth`・`aa` も決められる。下地混色は `mix`（0〜1: 同じレイヤーにある色を混ぜて描く）、色延びは `stretch`（0〜1: 拾った色を線の先まで引きずる）。`add_stroke` でも一本ごとに渡せる。色を混ぜる・ぼかすのは `smudge`（`mode`: `blur`・`smudge`・`blend`）。線を丸ごと消すのは `erase` の `mode: "whole"`。
- 線の編集: `vector_edit`（`action`: `move_point`・`add_point`・`delete_point`・`connect`・`cut`・`recolor`・`delete`。線の id は `inspect` の `snapshot` か `render`）。塗り残しは `fill_gaps`。
- レイヤー: `add_layer` の `kind` に `fill`（ベタ塗り・`rgb`）・`gradient`（`gradient`）・`adjust`（色調補正・`adjust: {kind, …}`、下の絵の色を変える。あとから `set_layer` で直せる）。`set_layer` の `effect`（`border` フチ・`water_edge` 水彩境界）と `color_prints`（表示色を印刷にも）。合成モードは比較（暗・明）・焼き込み・覆い焼き・ソフトライト・差の絶対値・色相・輝度なども。まとめて: `merge_layers`・`merge_visible`（`copy`）・`group_layers`・`move_layers`・`set_layers`・`convert_layer`（`to`: `paint`・`pen`）。用紙の色は `set_paper`。
- 変形: `liquify`（`mode`: `push`・`pinch`・`bloat`・`twirl_cw`・`twirl_ccw`）、`transform_area` の `interp`（`nearest` でドットをぼかさない）と、`warp.mesh` の格子の数 `grid: [横の点, 縦の点]`（2〜9。省くと 3×3）。フィルターは移動・放射・ズームぼかし、ノイズ、波形、渦巻き、線画抽出、反転、階調化、しきい値、グラデーションマップも。
- コマ: `set_frame` の `curves`・`bow`（辺を曲げる）と `line`（枠線: `solid`・`double`・`dashed`・`dotted`・`rough`、`rgb`）。
- 文字の `style`: `scale_x`（長体・平体）・`gradient`・`fill_png`（画像で塗る）・`warp`（4 隅で遠近・ゆがみ）・`text_path`（パスに沿わせる）・`features`（字形）・`yakumono`（約物の詰め）。異体字はテキストに異体字セレクタを入れる。フキダシは `picture`（画像のフキダシ）、`spike_jitter`・`bumps`、しっぽの `kind`（`zigzag`・`fade`・`bubbles`）。
- 効果線: 流線の `path`・`spread_mm`・`spacing_mm`（間隔）、集中線の `inner_path`・`twist`・`length_mm`（線の長さ）。どちらも `bundle`（1 束の本数）・`bundle_gap`、乱れを種類ごとに `jitter_length`・`jitter_position`・`jitter_width`、入り抜き `taper`（`in`・`out`・`both`・`false`）。トーン: 柄（`check`・`brick`・`wave`・`grid`・`hatch`・`star`・`sand`・`image`）、レイヤーのトーン化は `set_layer` の `screen`（`shape`・`offset_mm` も）、点検の `tone_moire`。網の形は `add_tone`・`set_tone` の `dot_shape`（`round`・`square`・`diamond`・`ellipse`）、網をずらすのは `offset_mm: [右, 下]`（`move_by_mm` で足す）。2 値で書き出すとき、トーン化していないグレーを網点にするのは `export` の `screen: {lpi, shape}`。
- 定規: `parallel_curve`・`multi_curve`・`radial_curve`、`layer_id`（レイヤー専用）、パースの `lock_horizon`・`horizon_y`・`fixed`、定規ペンは `ruler_to_layer`。描き文字の素材は `stamp_material`（kind `lettering`）。
- 素材: `inspect` の `materials` に種類（トーン・効果線・画像・パーツ〔漫符・小物・背景の線画〕・描き文字・ブラシ・3D）とタグ。`stamp_material` でパーツは `layer_id` の `x_mm`・`y_mm` に、描き文字は台詞として、ブラシは原稿に加わり（`add_stroke` の `kind` に使える）、3D は置いた所に。
- 3D: 体型と関節のある人形は `add_figure`（`body`: `heads` 等身・`shoulders`・`hips`・`build`・`legs`、`preset`、`hands`）と `pose_figure`（`joints` の x・y・z、`drag`。`drag` に `ik: true` で手首・足首を引くと腕・脚ごと曲がる）。体型の男女は `body.sex`（`male`・`female`）。指を 1 本ずつ曲げるのは `hands` の `{pose, curls: [親指, 人差し指, 中指, 薬指, 小指]}`（0〜1）、手だけのモデルは `pose_figure` の `curls`。頭部 `add_head`、手 `add_hand`（`pose`）、球・円錐・小物は `add_prim3d` の `kind: sphere|cone|prop`（`prop`: `chair`・`desk`・`table`・`bed`・`door`・`window`・`shelf`・`car`）、OBJ は `import_model`。ページのカメラ `set_camera`、光 `set_light`。線と陰の面にするのは `render_prims`（`surfaces` は既定で true＝陰を灰色で入れる。線だけなら `lines: true, surfaces: false`。`tone` で面を網点に）。
- 本: 表紙・裏表紙・カバー（背と袖）・帯（`kind: obi`、`height_mm` 15〜200、幅はカバーと同じ）は `add_cover`（ページの最後に入り、ノンブルなし。本のプレビュー・EPUB・Kindle では表紙が先頭、裏表紙が末尾。PDF・TIFF・PNG はページの順のままで、ファイル名が `cover_front` など）。全ページの台詞の置換は `replace_text`、同じ操作を全ページに `for_pages`、担当は `set_assignee`。ほかの原稿のページを取り込む（作品の結合）のは `import_pages`（`from` は --root の中の別の原稿、`pages`・`after`。台詞も一緒に、絵のファイルも写る）。バックアップ（別のフォルダーへ定期的に zip）は画面の環境設定で人が決める。
- ペンの軸の回転（アートペン）: `add_stroke` の `rotation`（点ごとの角度）。`tip_rotation` のブラシ（カリグラフィ）は先端が回る。
- フィルターのプラグイン（置き場所は `inspect` の `plugins` の `folder`）: 一覧にあれば、`filter_raster` の `kind` に `plugin:<key>`（設定は `PARAMS` のとおり）。
- アニメーション: `set_animation`（`fps`・`frames`・`loop`）でページを短いアニメーションに。`add_anim_folder` がタイムラインの 1 行、`add_cel`（`folder`・`at`）がセル（描くのは `add_stroke` の `layer_id` にセルの id）。どのフレームにどのセルを出すかは `set_exposure`（`frame`・`cel`、null で空）か `set_exposures`（全部）。カメラワークは `set_camera_key`（`rect`）、いつも薄く見るセルは `set_light_table`。書き出しは `export` の `format: "animation"`（`pages` に 1 ページ、`movie`: gif・webp・png・mp4・frames）。
- ファイル: PSD／PSB をレイヤーのまま読み込むのは `import_psd`（`path` は原稿のフォルダからの相対パスか --root の中、`fit`: `paper`・`bleed`・`trim`。フォルダー・マスク・不透明度・合成モード・クリッピング・表示もそのまま）。制作過程の記録は `set_timelapse`（`on`）。
- 図形: `add_shape`（`shape`: `line`・`polyline`・`curve`・`rect`・`ellipse`・`polygon`、`line`・`fill` で線と塗り）。
- 範囲（`area`）: どの op の `area` にも、`poly`・`mask` のほかに `rect`・`ellipse`・`layer`（そのレイヤーの描いてある所）・`color`（その色の所）・`all`・`saved`（`store_area` で残した範囲）と、`union`・`intersect`・`subtract` の組み合わせ、`invert`・`grow_mm`（負で縮める）・`feather_mm` が使える。ガイド線は `add_ruler` の `kind: "guide"`（`axis`・`at`）。図形定規は `kind: "rect"`・`"ellipse"`（`points` は対角の 2 点、`angle`）・`"polygon"`（角 3 つ以上）。パース定規の地面のグリッドは `grid`（線の数）。背景の 3D に合わせてパース定規を置くのは `ruler_from_3d`、逆にパース定規にカメラを合わせるのは `camera_from_ruler`。
- 範囲の変形: `transform_area` の `matrix`（移動・拡大・回転・反転）か `warp`（`perspective` で 4 隅、`mesh` で 3×3 の点）。レイヤーを丸ごと動かすのは、ページ全体を `area` にした `matrix`。
- 台詞: `add_line`・`edit_line` の
  - `ruby_runs`（ルビ）、`emphasis_runs`（傍点）、`style_runs`（一部を大きく・小さく・太く・色を変える: `[["本当", {"scale": 1.4, "weight": "heavy"}]]`）。
  - `style`: `rotate_deg`（フキダシごと回す）、`skew_deg`・`arc`（描き文字の傾き・弓なり）、`weight`（`normal`・`bold`・`heavy`）・`italic`、`outline_rgb`（フチの色）、`latin`（`rotate` で 4 文字以上の英数字を寝かせる／`upright`）、`emphasis_mark`（`sesame`・`dot`）、`wobble`（手描き風の揺れ）・`double`（二重線）・`spikes`・`spike_depth`（叫びのトゲ）。
  - `balloon` の形に `electric`（電子音: 電話・テレビの声。角のあるギザギザの縁と稲妻のしっぽ）。
  - `path`（手で描いた形のフキダシ、`set_balloon_path` でも）。
  - フキダシの大きさと位置は `move_line` の `w_mm`・`h_mm`・`x_mm`・`y_mm`（`edit_line` では変わらない）。
- 3D: `add_prim3d` の `kind` は `box`・`cylinder`・`stairs`・`floor`（パースの格子）。背景は `add_scene`（`kind`: `room`・`classroom`・`corridor`・`street`）で、壁・床・窓・机・建物をまとめて置き、`edit_prim`・`delete_prim`・`trace_prims` は id 1 つで効く。人形は `add_mannequin`。`trace_prims` で線にする。
- 点検: `mcp__genko__check` で、人の「入稿前の点検」と同じ問題の一覧を受け取る（`preflight` の結果の `checks` にも入る）。
- 取り消し: `mcp__genko__undo` で自分の最後の変更を取り消す。最後の変更が人のもの・承認が変わる・`project.json` が Genko の外で書き換えられた、のどれかなら断る。
- 書き出し: `mcp__genko__export`（`format` の既定は png。ほかの形式は人に頼まれたときだけ: pdf・tiff・cmyk・layers・psd・pack・epub・kindle・strip・webtoon・sns・timelapse、`pages`、`dpi`、`area`: paper・bleed・trim）。承認は要らない。書き出し先は原稿の `exports/`。
  `dpi` の既定は 300（見せる・確かめる用。印刷用の本番は人が書き出す）。40 秒で終わらない書き出しは `job` を返す。書き出しは続いているので、`mcp__genko__export_status`（`job`。既定で最大 60 秒、終わるまで待ってから返す）で結果を取る。
  書き出しは Genko（MCP サーバ）の中で動く。呼び出しごとにサーバを立ち上げて閉じるつなぎ方では、閉じたときに書き出しも止まる。そのときは 90 秒ほどで `status: "lost"` になるので、同じつなぎのまま `export` と `export_status` を続けて呼び直す。
  - `color` の既定 `auto`: モノクロの原稿はグレー（劣化なし）、カラーは RGB。`bitonal` で白黒 2 階調。PDF には仕上がり線（TrimBox）と裁ち落とし（BleedBox）が入る。
  - `cmyk`（CMYK の TIFF）と pdf の `color: "cmyk"` は、`icc` に印刷所の CMYK プロファイル（.icc のパス）を渡すとそれで変換する。無ければ黒は K 版だけ・総インキ量 320% 以内。`color: "gray"` も。
  - `epub`・`kindle` は仕上がりで切り、トーンを網点にせずグレーで描く（読むときに縮小されてもモアレが出ない）。印刷と同じ網点にするなら `dots: true`。
  - `layers` はレイヤーを 1 枚ずつ透明な PNG に。`kindle` は Kindle 用の固定レイアウト（`long_edge` 既定 2560）。
  - `timelapse` は `set_timelapse` で記録した制作過程（`movie`: webp・gif・png・mp4、`fps`、`seconds`、`pages` は 1 ページだけ）。
