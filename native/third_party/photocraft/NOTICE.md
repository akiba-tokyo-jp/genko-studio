# PhotoCraft由来の算法

露光量の式・パラメータ境界は PhotoCraft v0.2.0 (`ad863217386440ca968fccc9bfff65ba24e61142`) の `crates/compose/src/adjust.rs` と `crates/engine/src/adjust_params.rs` を参照してC++へ実装した。著作権と利用条件は同梱 `LICENSE-MIT` を参照。ブランド・上流バイナリ・別アプリは製品へ同梱しない。

## 内蔵の ICC プロファイル（color-model@1、PC1a）

- `native/src/core/data/profiles/coated-cmyk.icc` は PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`）の `crates/cms/profiles/photocraft-coated-cmyk.icc` をそのまま同梱したもの（sha256 `f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc`）。上流がこのファイルを CC0-1.0（パブリックドメイン）として公開している（`ATTRIBUTION.md`、`crates/cms/README.md`、`crates/cms/src/builtin.rs` 冒頭）。
- 内蔵プロファイルの ID・別名・説明の文字列と、RGB・グレーの原色・白色点・トーンカーブの選び方は `crates/cms/src/builtin.rs`（18-102 行、229-268 行、MIT）による。RGB・グレーのプロファイルのバイト列そのものは Genko が開発用の道具 `tools/migration/make_profiles.cpp` で Little CMS により作ったもの（CC0-1.0 として扱う）。

## カラー設定・色の校正・色域外警告・表示（PC1b）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT。`LICENSE-MIT`）の次の部分を C++ へ移した。

- `crates/engine/src/proof_sim.rs`（32-73 行、118-225 行、341-418 行の試験）: 校正の種類（版・CMY 版・旧 Macintosh の RGB・P 型と D 型の色覚）、sRGB の符号化、Viénot・Brettel・Mollon（1999）の行列と色域の縮め方 → `native/src/render/proof.cpp`、`native/tests/unit/test_proof.cpp`。
- `crates/cms/src/gamut.rs`（10-77 行）: 色域外の判定（校正のプロファイルへ行って Lab に戻した CIE ΔE 1976、既定のしきい値 4、RGB 33³・グレー 256・CMYK 17⁴ の格子）。計算は Little CMS で行う → `native/src/render/proof.cpp`。
- `crates/cms/src/clut.rs`（1-6 行、63-152 行）: 格子の間の補い方（四面体。4 入力は二つの四面体の間を直線で）→ `native/src/render/proof.cpp`（`tetra`・`Gamut::delta_e`）。
- `crates/cms/src/transform.rs`（115-120 行）: 校正の変換の二段（校正のプロファイルへは意図・黒点の補正、画面へは相対・黒点補正あり、紙色では絶対・黒点補正なし）→ `native/src/render/proof.cpp`（`apply`）。
- `crates/engine/src/color_cmds.rs`（680-697 行）: 色域外の判定に文書のプロファイルの値を使うこと（`gamut_mask`・`composite_profile`）→ `native/src/app/tiles.cpp`。
- `crates/engine/src/color_cmds.rs`（70-198 行、391-480 行、648-676 行、708-744 行）: カラー設定の項目と既定（作業用の空間・方針・聞く・意図・黒点の補正・ディザ・文字のガンマ・モニター）、開くときの方針、校正の設定 → `native/src/app/color_settings_dialog.cpp`、`native/src/app/main_window_color.cpp`。
- `crates/engine/src/display_color.rs`（相対的な色域を維持・黒点補正のモニターの変換、恒等の判定）→ `native/src/app/display_color.cpp`。
- `crates/engine/src/prefs.rs`（443-490 行）: 透明部分の市松の大きさ・色の組と色域外警告の色・不透明度の既定 → `native/src/app/preferences.cpp`。
- `crates/compose/src/psblend.rs`（文字のガンマ `composite_gamma`、既定 1.45 と試験の値）→ `native/src/render/text/lettering.cpp`（`paste_in_gamma`）。

## チャンネル（channels@1、PC1c）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を参照して C++ へ移した。

- `crates/engine/src/channel_cmds.rs`（781-1296 行）: 新規・特色・複製・削除・名前の変更・並べ替え・オプション・特色を合成・分割・統合の既定値と名前（Alpha n、上流の Spot Color n は「特色 n」）、番号の付け替え、特色を合成の式 `c + (s·ink + (1−s)·c·ink − c)·v`（1149-1198 行）。Genko の op（`native/src/render/ops_channels.cpp`）と画素の op の対象（`native/src/render/channels.cpp`）。
- `crates/compose/src/multichannel.rs`（14-38 行）: 特色の表示の式 `(1 − s·v)·p·(1 − v + v·c) + s·v·c` を参照した。Genko の画面の特色（`native/src/render/channels.cpp` の print_spots）は、合成の結果と同じに見せるため、上の特色を合成の式に替えた（マルチチャンネルのページは PC1d）。
- `crates/ui-egui/src/channel_view.rs`（64-128 行）: チャンネル パネルの表示（色の版の 1 枚をグレーで、隠した版は 0、アルファの重ねの覆い方 `overlay_coverage`）。
- `crates/io/src/channel_map.rs`（1-40 行）と `crates/io/src/psd_export.rs`（448-470 行、654-656 行）: PSD の追加チャンネルの名前（1006・1045）と DisplayInfo（1077）の形（`native/src/formats/psd_export.cpp`、読み戻しは `native/src/render/psd.cpp`）。Genko は 1077 の記録を 13 バイト（上流が種類の後に書く埋めのバイトは書かない）、特色と「選択範囲」のアルファの標本を Photoshop の向き（色の付く所が暗い）で書く。

## 特別な色モード（color-model@1、PC1d）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を C++ へ移した。

- `crates/algo/src/quantize.rs`（1-658 行と試験）: パレットの作り方（selective・perceptual・adaptive・exact・systemMac・systemWindows・web・uniform、強制する色、メディアンカットと知覚の重み、Lab の変換）、量子化とディザ（誤差拡散・8×8 の bayer・ノイズのハッシュ）、2 階調（50% の閾値・パターン・誤差拡散・網点の 6 つの形）を、f32 の演算の順まで同じに移した → `native/src/render/quantize.cpp`（pow・cbrt は `native/src/render/libm_float.cpp`）、`native/tests/unit/test_quantize.cpp`（上流の試験と、上流の Rust で計算した固定のハッシュ）。
- `crates/doc/src/mode.rs`（12-117 行と 193-210 行の試験）: カラーテーブルの最も近い色（同じ距離は小さい番号）、ダブルトーンのインキと曲線（`curve_eval`）と白の上に刷る式 → `native/src/core/color_model.cpp`（`ColorTable::nearest`）、`native/src/core/ink.cpp`（`curve_at`・`duotone_print`）。
- `crates/engine/src/mode_cmds.rs`（182-208 行、229-252 行、254-321 行、325-353 行、355-412 行と試験）: インデックスカラーの引数と既定、表のプリセット、表の付け替え（古い表の番号 → 新しい表の色）、2 階調の引数と既定、ダブルトーンの既定のインキ（Black・Warm Brown・Gold・Slate Blue）と種類 → `native/src/render/ops_ink.cpp`、`native/src/render/quantize.cpp`（`preset_table`）。
- `crates/engine/src/multichannel_cmds.rs`（25-29 行、97-119 行、151-199 行と `tests.rs`）: 版の表示色（CYAN・MAGENTA・YELLOW・BLACK）、モードごとの版の作り方と名前、戻すときの読み替えと刷った見え → `native/src/render/ops_ink.cpp`（`multichannel_of`）、`native/src/render/ink.cpp`。
- `crates/compose/src/multichannel.rs`（14-38 行）: 版を白の上に刷る式 `(1 − s·v)·p·(1 − v + v·c) + s·v·c` → `native/src/core/ink.cpp`（`print_plates`。マルチチャンネルのページの表示。特色チャンネルの表示は PC1c のとおり合成の式）。
- `crates/engine/src/multichannel_cmds/tests.rs`（`mismatched_channel_count_converts_the_printed_look`。上流は版を文書から直接減らす）: 版の数がモードと合わないマルチチャンネルを戻すと刷った見えになること。色の版の削除そのもの（`delete_channel` の `to_multichannel`、残りの版でマルチチャンネルにする）は Photoshop の動きに合わせた Genko のもの（上流の `channel.delete` はアルファだけを消す）→ `native/src/render/ops_channels.cpp`（`delete_plate`）。

## 補正・色管理（adjustments@1、PC2）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を参照して C++ へ移した。各群（PC2a〜PC2d）が式を移すときは、この節にその部分を足す。

- `crates/engine/src/adjust_params.rs`（19-65 行、67-286 行、294-563 行）・`crates/engine/src/adjust_cmds.rs`（24-64 行、207-222 行）・`crates/algo/src/tone.rs`（34-65 行）・`crates/doc/src/adjust.rs`（`HueRange::neutral`、`LevelsChannel`）: 補正の種類ごとの設定の既定と範囲（範囲外は切る、型の誤りは断る）、レンズフィルターの既定の名前、Hue/Saturation の 6 つの範囲の既定、レベル補正の組の範囲（白は黒 + 2 以上）とトーンカーブの点の読み方（入力の順、0.5 以内の重なりは先のもの、2〜19 点）→ `native/src/core/adjust_kinds.cpp`（種類の表・`read_params`・`read_tone_channels`）。
- `crates/compose/src/lib.rs`（878-902 行、1127-1140 行）: 補正レイヤーの描画モード（下の絵と補正後の絵を描画モードで重ね、不透明度 × マスクで混ぜ、α はそのまま）→ `native/src/render/color_canvas.cpp`（`ColorCanvas::adjust`・`expose`）、`native/src/render/ink_canvas.cpp`（`InkCanvas::recolour`・`adjust_planes`）、`native/src/render/adjust.cpp`（`blend_8bit`。8 bit の式は Genko の `render._blend_over`）。
- `crates/compose/src/lib.rs`（426-440 行、`adjustment_quantum`）: 8 bit の文書の補正を 8 bit に丸める考え → `native/src/render/adjust.cpp`（`apply_8bit`）。
- `crates/algo/src/tone.rs`（1-364 行と試験 366-445 行）: シャドウ・ハイライト（ぼかした輝度の重み・持ち上げのガンマ・彩度と中間調・クリップで伸ばす）、色の置き換え、Lab の統計と色の一致、HDR トーン（局所適応）を f32 の演算の順まで同じに移した → `native/src/render/tone_ops.cpp`、`native/tests/unit/test_tone_ops.cpp`（PC2c）。
- `crates/algo/src/fxutil.rs`（`gauss_box_radii`・`box_rows_n`・`box_cols_n`・`gauss_blur_n`）: 3 回の箱で近似したガウスのぼかし（和は f64）→ `native/src/render/tone_ops.cpp`（`gauss_blur`）。
- `crates/algo/src/selection.rs`（360-385 行、`color_range`）と `crates/algo/src/other2.rs`（42-87 行、HSL）: 色域の量と HSL の変換 → `native/src/render/tone_ops.cpp`。
- `crates/color/src/convert.rs`（150-212 行）: sRGB の伝達関数と D50 の Lab の式（色の一致の統計）→ `native/src/render/tone_ops.cpp`。
- `crates/algo/src/trap.rs`（1-125 行）と `crates/engine/src/trap_cmds.rs`（19-43 行）: トラッピング（インキのある画素の下へ各インキを最大値で広げる）と試験。広げ方は同じ結果の van Herk の最大値フィルターにした → `native/src/render/trap.cpp`、`native/tests/unit/test_image_calc.cpp`。
- `crates/color/src/blend.rs`（151-342 行）: 分離できる描画モードの式・分離できないモード（W3C）・`composite` → `native/src/render/image_ops.cpp`（`blend_channel`・`blend_rgb`・`composite`）。
- `crates/engine/src/channel_cmds.rs`（200-427 行、1298-1393 行）: 画像操作・演算の元の読み方（合成・版・アルファ・透明部分・マスク・反転）と `Blending`（add・subtract の scale と offset）、画像操作の重み（不透明度 × マスク × 選択範囲、透明部分の保護）と演算の 3 つの結果 → `native/src/render/ops_calc.cpp`、`native/src/render/image_ops.cpp`。
- `crates/engine/src/edit_menu_cmds.rs`（50-79 行、152-218 行）: フェードできる操作と、前の画素へ前乗算で補間する `fade_surface` → `native/src/render/ops_fade.cpp`、`native/src/render/image_ops.cpp`（`fade_values`・`fade_rgba`）、`native/src/app/image_dialogs.cpp`。
- `crates/engine/src/adjust_cmds.rs`（171-322 行）: 局所の補正と HDR トーンの引数・既定・範囲 → `native/src/render/tone_ops.cpp`、`native/src/core/adjust_kinds.cpp`（PC2-0 の表）。
- PC2b（色の補正）: `crates/compose/src/adjust.rs`（106-149 行・168-181 行の色相・彩度、自然な彩度、チャンネルミキサー、レンズフィルター、白黒、カラーバランス、特定色域の選択の式、252-310 行の `hue_range_tables`・`hue_saturation`・`black_white_gray`、357-397 行の `selective_color_weights`・`selective_color`、556-594 行の HSL、試験 618-635 行・699-721 行）、`crates/doc/src/adjust.rs`（`HueRange` の `neutral`・`canonical_bounds`・`weight`、試験 268-285 行）、`crates/engine/src/adjust_params.rs`（26-47 行の `PHOTO_FILTERS`、`hue_saturation` の colorize の範囲と範囲のつまみの読み方、試験 565-620 行）、`crates/engine/src/commands.rs`（719-732 行、彩度を下げる = 色相・彩度の彩度 −100）、`crates/color/src/convert.rs`（209-212 行、`rgb_to_gray`）→ `native/src/core/adjust_color.cpp`・`adjust_color.hpp`。`crates/io/src/adjust_map.rs`（177-197 行の `hue2`、315-329 行の `selc`、374-485 行の `vibA`・`blnc`・`blwh`・`phfl`・`mixr`、508-695 行の書き方、試験 745-1013 行）と `crates/io/src/channel_map.rs`（61-84 行、色構造の色空間）→ `native/src/render/psd_adjust_color.cpp`。レンズフィルターの既定の色は上流の一覧（一般の写真用フィルターの色）。
- PC2a（階調の補正・3D LUT）:
  - `crates/compose/src/adjust.rs`（55-105 行、150-200 行、205-250 行、313-357 行、414-535 行）: 明るさ・コントラストの今の式と従来の式、階調化の段（floor）、グラデーションマップの逆とディザ、4×4 の Bayer、レベル補正（ガンマ > 1 の柔らかい足）、トーンカーブの自然 3 次スプライン（4096 の表）、合成とチャンネルの順・組の空間（`tone_luts`・`tone_in_space`）、3D LUT の三線形・四面体の引き方 → `native/src/core/adjust_tone.cpp`、`native/src/core/lut3d.cpp`（`sample`・`bayer4`）、`native/src/render/adjust_space.cpp`。
  - `crates/cms/src/lutfile.rs`（1-268 行）: .cube・.3dl・.look の読み方と断りの言葉、.cube の書き方、作った 8 つの見た目（上流で CC0 とされる手続きの表）→ `native/src/core/lut3d.cpp`。
  - `crates/engine/src/edit_cmds.rs`（223-301 行）: 自動トーン補正・自動コントラスト・自動カラー補正（両端 0.1% を切る黒と白、中央値のガンマ）→ `native/src/core/adjust_tone.cpp`（`auto_levels`）、`native/src/render/ops_adjust.cpp`（`auto_adjust`）。
  - `crates/engine/src/extra_cmds.rs`（156-187 行）: 平均化（R・G・B をまとめた度数の累積）→ `native/src/core/adjust_tone.cpp`（`equalize_table`）、`native/src/render/ops_adjust.cpp`。
  - `crates/engine/src/file_cmds.rs`（828-882 行）: 補正レイヤーを .cube に焼く（格子・TITLE・LUT_3D_SIZE・DOMAIN）→ `native/src/formats/exporting.cpp`（`bake_cube`）、`native/src/api/cli.cpp`（`export --format cube`）。
  - `crates/io/src/adjust_map.rs`（21-110 行、160-316 行、331-375 行、487-640 行）・`crates/io/src/gradient_bake.rs`: PSD の調整レイヤー（`levl`・`curv`・`brit`/`CgEd`・`post`・`thrs`・`nvrt`・`expA`・`grdm`・`clrL`）の読み書きと記述子、グラデーションの焼き方（なめらかさ・中間点・Oklab）→ `native/src/render/psd_adjust.cpp`。
- `crates/engine/src/color_cmds.rs`（496-506 行、746-762 行）: プロファイルの情報の項目（説明・色空間・種類・版・変換の表・意図、内蔵の一覧）→ `native/src/render/proof.cpp`（`profile_info`・`builtin_profile_list`・`profile_info_of`。白色点とトーンカーブは Genko が足した）、`genko inspect --profile`、編集 → プロファイルの情報…（PC2d）。
- `crates/engine/src/color_cmds.rs`（880-911 行）: 埋め込みプロファイルの不一致の答え（そのまま・変換・破棄・作業用を割り当て）の考え → `native/src/render/import_profile.cpp`（`put_raster`・`import_psd` の `embedded_profile`: keep・convert・assign。PC2d）。
- `crates/ui-egui/src/tone.rs`（20-120 行、157-180 行、213-275 行）: ヒストグラムの数え方（透明でない画素、RGB は 3 つの平均、大きな山で潰れないよう高い段で目盛る）と数値（平均・標準偏差・中央値・画素数）→ `native/src/app/info_panels.cpp`（`histogram_of`・`stats_of`・`HistogramPanel`。輝度とカラーは Genko が足した。PC2d）。
- `crates/ui-egui/src/panels.rs`（831-889 行）: 情報パネルの項目（ポインターの下の色・位置・選択範囲の大きさ）→ `native/src/app/info_panels.cpp`（`InfoPanel`。値は Genko の `render::pixel_at`、校正の値はそのプロファイルへの変換。PC2d）。

## 非破壊（PC3 節 A: スマートオブジェクト、smart-objects@1）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を参照して C++ へ移した。各群（A0〜A5）が移すときは、この節にその部分を足す。

- A0（土台と絵の取り込み）:
  - `crates/engine/src/smart_cmds.rs`（214-223 行、`place`）: 中身を変形で置くとき、整数画素の移動だけなら画素をそのまま写し（変換・描き直しが劣化しない）、ほかは 1 回の再標本化で置く考え → `native/src/render/smart.cpp`（`whole_shift`・`lay`・`lay_picture`。核の正規化と縁の被覆は Genko のもの）。
  - `crates/engine/src/smart_cmds.rs`（395-399 行、`snap_affine`）: 合成した変形の行列の浮動小数の誤差を丸めて、変形を戻すと元の置き方にちょうど戻る考え → `native/src/core/smart.cpp`（`snapped`。1e-9 の格子に寄せる）。
  - `crates/engine/src/smart_cmds.rs`（476-539 行、`layer_to_smart`）: 1 枚の層を包むとき、表示・不透明度・描画モード・クリップを SO が引き継ぎ、中の層は普通に戻す → `native/src/render/ops_smart.cpp`（`smart_convert`）。
  - `crates/engine/src/file_cmds.rs`（291-331 行、`place_bytes`・`place`）: 埋め込みを配置の置き方（ファイルの解像度でページの中央、大きければ収める、倍率と中心の指定）→ `native/src/render/ops_smart.cpp`（`smart_place`）。
  - `crates/engine/src/smart_cmds/tests.rs`（61-93 行、220-251 行）: 変換してもページの画素が変わらないこと、縮めて戻すと元と同じ（2 度標本化しない）ことの試験 → `native/tests/unit/test_smart_render.cpp`（Genko の 3 つの道・白黒の仕上げ・部分描画に広げた）。
- A1（中身とリンク）:
  - `crates/engine/src/smart_cmds.rs`（582-601 行、`set_source`・`replace_contents`）と 840-880 行（`relinkToFile`・`updateModifiedContent`・`convertToEmbedded`・`convertToLinked` の振る舞い）: 中身を差し替えても変形とフィルターはそのまま、PSD の記録は外す、埋め込みに変えるときは記録を残す、リンクに変えるときは中身をファイルに書いてからリンクする → `native/src/render/ops_smart_contents.cpp`（Genko ではリンクした中身もいつも原稿に写しを持ち、op はパスを読み書きしない）。
  - `crates/engine/src/print_cmds.rs`（401-449 行、`package`）: 原稿をフォルダーに写し、リンクしたファイルを `Links/` に集めて写しのリンクをそこへ向ける、同じ名前は番号を付ける → `native/src/formats/smart_files.cpp`（`package`。Genko は原稿の中の写しを書くので欠けたリンクは無い）。
  - `crates/engine/src/layer_menu_cmds.rs`（343-372 行、`reveal_in_finder`）: リンクしたファイルのあるフォルダーを開く（埋め込みにはファイルが無い）→ `native/src/app/main_window_smart.cpp`（`reveal_contents`）。
- A2（スマートフィルター）:
  - `crates/engine/src/smart_cmds.rs`（236-308 行、`blend_surfaces`・`mask_mix`・`apply_smart_filters`）: 見えているフィルターを下から順に前の結果にかけ、描画モードと不透明度で前の結果に重ねる（通常・100% は置き換え）、フィルターマスクでフィルターの前と後の絵をアルファを掛けた値で混ぜる → `native/src/render/smart_filters.cpp`（`on_8bit`・`on_precise`・`on_inks`・`mix_through`。描く解像度・3 つの道・作業用のページと層で Genko のフィルターを使う所は Genko のもの）。
  - `crates/engine/src/smart_cmds.rs`（367-388 行、`add_smart_filter`）: フィルターを一番上に足し、選択範囲があってフィルターマスクが無ければ選択範囲をフィルターマスクにする → `native/src/render/ops_smart_filters.cpp`（`smart_filter_add`）。
  - `crates/engine/src/smart_cmds.rs`（692-779 行、880-953 行）: スマートフィルターの番号（0 が一番下、既定は一番上）、設定を重ねる・描画オプション（1 を超える不透明度は %）・表示の反転・並べ替え・削除・クリア（フィルターとマスク）・スマートフィルターとフィルターマスクの無効化の振る舞いと断り → `native/src/render/ops_smart_filters.cpp`（`smart_filter_set`・`smart_filter_move`・`smart_filter_delete`・`smart_filter_state`）。
  - `crates/engine/src/filters.rs`（199-207 行）: SO にフィルターをかけると、画素を変えずにスマートフィルターとして記録する → `native/src/app/layer_panel.cpp`（`apply_filter`）、`native/src/app/image_dialogs.cpp`（シャドウ・ハイライト）。
  - `crates/engine/src/smart_cmds/tests.rs`（125-211 行、253-264 行）: 設定を変えても中身から描き直す・同じ設定なら同じ画素・隠せば元の画素、並び・描画モード・不透明度・無効化・削除・クリア、選択範囲がフィルターマスクになる、変形に付いて来ることの試験 → `native/tests/unit/test_smart_filters.cpp`（Genko の 3 つの道・200 dpi の `rasterize_layer`＋`filter_raster` との比べ・部分描画に広げた）。
- A3（PSD のスマートオブジェクト）:
  - `crates/io/src/linked.rs`（1-128 行）: 全体のブロック `lnk2`・`lnk3`・`lnkD` の項目（8 バイトの長さ・4 バイトにそろえる・種類 `liFD`/`liFE`/`liFA`・版・uuid・ファイル名・開く記述子）の読み方と uuid で引くこと、`liFD`（版 2、開く記述子なし）の書き方 → `native/src/render/psd_smart.cpp`（`read_linked`・`linked_item`）、`native/src/render/psd.cpp`（`File::linked`）。試験 114-148 行 → `native/tests/unit/test_psd_smart.cpp`（`linkedFilesRoundTrip`）。
  - `crates/io/src/blocks.rs`（344-421 行、`parse_placed_warp`・`parse_smart`）: `SoLd`・`SoLE` の記述子の `Idnt`・`Trnf`・`Sz` から中身の id と置き方、`warp` の形の既定・曲げ・歪み・向き・範囲とカスタムの 4×4 のメッシュ、何も変えないワープは無いとすること → `native/src/render/psd_smart.cpp`（`read_placed`。`nonAffineTransform`・`PlLd` の読み方と Genko の形への写しは Genko のもの）。試験 472-512 行・546-562 行 → `native/tests/unit/test_psd_smart.cpp`（`placedBlocksRead`）。
  - `crates/io/src/psd_import.rs`（195-243 行）: `SoLd`・`PlLd`・`SoLE` のある層を SO にし、Photoshop の絵を描き直すまでの絵として持ち、ブロックを書き戻すために持つこと → `native/src/render/import_psd.cpp`（`smart_contents_of`・`smart_settings`）、`native/src/render/smart.cpp`（`shown_8bit`・`shown_inks`）。
  - `crates/io/src/psd_export.rs`（269-274 行）: 読んだ配置レイヤーのブロックを変えずに書き戻すこと → `native/src/formats/psd_export.cpp`（`PlacedLayers`。Genko で作った SO の `SoLd`・`PlLd`・`lnk2` を書くこと、場所が同じ間だけ書き戻すことは Genko のもの）。
  - `crates/engine/src/smart_cmds/tests.rs`（359-397 行、`psd_placed_layer_renders_from_lnk2_data`）: PSD の配置レイヤーが `lnk2` の中身から描き直されるまで Photoshop の絵を見せる試験 → `native/tests/contract/test_contract_psd_smart.cpp`（`placedLayerImported`）、`native/tests/unit/test_psd_smart.cpp`（`importedAndWrittenBack`）。

## 選択・マスク（masks@1、PC3 節 B）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を参照して C++ へ移した。各群（B1〜B4）が算法を移すときは、この節にその部分を足す。

- `crates/algo/src/selection.rs`（80-655 行）: 選択の共通の算法（範囲の箱と覆い `Region`、魔法の杖と隣接・色の近さ、色域の量、正確なユークリッド距離変換、拡張・縮小・境界線・滑らかに・ぼかし、AA の多角形の覆い、明るさ・色相の範囲）→ `native/src/render/selection_ext.cpp`（PC3-0）。区域の修飾の順（拡張・縮小・境界線・滑らかに・反転・ぼかし）→ `native/src/render/area_kinds.cpp`。

## パス・ベクトル・変形・判型（vector@1・pen-shapes@1・transform@1・page-spec@1、PC3 節 C）

PhotoCraft v0.2.0（同上、MIT）の次の部分を参照して C++ へ移した。各群（C-a〜C-e）が移すときは、この節にその部分を足す。

- `crates/doc/src/vector.rs`: パス・サブパス・アンカーとハンドル・合わせ方・塗りの決まり・線の設定・生きた形の型 → `native/src/core/vector.hpp`・`vector.cpp`（単位は mm、y は下向き、ページの座標にした。PC3-0）。
- `crates/vector/src/flatten.rs`（1-132 行）・`raster.rs`（1-419 行）・`stroke.rs`（1-400 行）・`shapes.rs`（1-153 行）・`lib.rs`（40-110 行）と `crates/vector/src/tests.rs` の試験の考え: ベジエを平らにする誤差、画素ごとの正確な面積の走査（巻き数・偶奇）、サブパスの合わせ方、線の輪郭（端・角・マイター・破線）、生きた形（角丸の長方形・楕円・多角形と星・ライン）→ `native/src/render/path_raster.cpp`、`native/tests/unit/test_path_raster.cpp`（PC3-0）。
- `crates/algo/src/transform.rs`（1-270 行と試験 271 行から）・`warp.rs`（1-290 行と試験 293 行から）・`resample.rs`（1-180 行）: 射影の変形（前乗算の標本化、縮小の前の縮め）、メッシュのワープ（三角形）、画像解像度の 1 次元のフィルター → `native/src/render/resample.cpp`、`native/tests/unit/test_resample.cpp`（PC3-0）。
- `crates/engine/src/canvas_geom.rs`（200-235 行、`Turn`）と `crates/engine/src/canvas_geom/tests.rs` の試験の考え: 90°・180° の回転と反転の画素の並べ替えと座標の写し方 → `native/src/render/resample.cpp`（`turn`）・`native/src/core/page_geometry.cpp`（`turn_mapping`）、`native/tests/unit/test_page_geometry.cpp`（PC3-0）。
- `crates/geom/src/warp.rs`（1-620 行）: ワープの形の一覧（円弧・アーチ・旗・波・魚形・膨張・絞り込み・ねじれなど）と曲げ・ゆがみ、ベジエのメッシュ（当てはめ・分割・分割の削除）→ `native/src/core/page_geometry.cpp`（`StyleWarp`・`BezierMesh`・`Warp`。PC3-0）。
