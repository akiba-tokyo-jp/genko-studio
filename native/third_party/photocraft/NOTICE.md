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
- PC2b（色の補正）: `crates/compose/src/adjust.rs`（106-149 行・168-181 行の色相・彩度、自然な彩度、チャンネルミキサー、レンズフィルター、白黒、カラーバランス、特定色域の選択の式、252-310 行の `hue_range_tables`・`hue_saturation`・`black_white_gray`、357-397 行の `selective_color_weights`・`selective_color`、556-594 行の HSL、試験 618-635 行・699-721 行）、`crates/doc/src/adjust.rs`（`HueRange` の `neutral`・`canonical_bounds`・`weight`、試験 268-285 行）、`crates/engine/src/adjust_params.rs`（26-47 行の `PHOTO_FILTERS`、`hue_saturation` の colorize の範囲と範囲のつまみの読み方、試験 565-620 行）、`crates/engine/src/commands.rs`（719-732 行、彩度を下げる = 色相・彩度の彩度 −100）、`crates/color/src/convert.rs`（209-212 行、`rgb_to_gray`）→ `native/src/core/adjust_color.cpp`・`adjust_color.hpp`。`crates/io/src/adjust_map.rs`（177-197 行の `hue2`、315-329 行の `selc`、374-485 行の `vibA`・`blnc`・`blwh`・`phfl`・`mixr`、508-695 行の書き方、試験 745-1013 行）と `crates/io/src/channel_map.rs`（61-84 行、色構造の色空間）→ `native/src/render/psd_adjust_color.cpp`。レンズフィルターの既定の色は上流の一覧（一般の写真用フィルターの色）。
