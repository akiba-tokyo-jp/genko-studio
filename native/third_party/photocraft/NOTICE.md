# PhotoCraft由来の算法

露光量の式・パラメータ境界は PhotoCraft v0.2.0 (`ad863217386440ca968fccc9bfff65ba24e61142`) の `crates/compose/src/adjust.rs` と `crates/engine/src/adjust_params.rs` を参照してC++へ実装した。著作権と利用条件は同梱 `LICENSE-MIT` を参照。ブランド・上流バイナリ・別アプリは製品へ同梱しない。

## 内蔵の ICC プロファイル（color-model@1、PC1a）

- `native/src/core/data/profiles/coated-cmyk.icc` は PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`）の `crates/cms/profiles/photocraft-coated-cmyk.icc` をそのまま同梱したもの（sha256 `f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc`）。上流がこのファイルを CC0-1.0（パブリックドメイン）として公開している（`ATTRIBUTION.md`、`crates/cms/README.md`、`crates/cms/src/builtin.rs` 冒頭）。
- 内蔵プロファイルの ID・別名・説明の文字列と、RGB・グレーの原色・白色点・トーンカーブの選び方は `crates/cms/src/builtin.rs`（18-102 行、229-268 行、MIT）による。RGB・グレーのプロファイルのバイト列そのものは Genko が開発用の道具 `tools/migration/make_profiles.cpp` で Little CMS により作ったもの（CC0-1.0 として扱う）。

## カラー設定・色の校正・色域外警告・表示（PC1b）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT。`LICENSE-MIT`）の次の部分を C++ へ移した。

- `crates/engine/src/proof_sim.rs`（32-73 行、118-225 行、341-418 行の試験）: 校正の種類（版・CMY 版・旧 Macintosh の RGB・P 型と D 型の色覚）、sRGB の符号化、Viénot・Brettel・Mollon（1999）の行列と色域の縮め方 → `native/src/render/proof.cpp`、`native/tests/unit/test_proof.cpp`。
- `crates/cms/src/gamut.rs`（10-77 行）: 色域外の判定（校正のプロファイルへ行って Lab に戻した CIE ΔE 1976、既定のしきい値 4、33³ の格子）。計算は Little CMS で行う → `native/src/render/proof.cpp`。
- `crates/engine/src/color_cmds.rs`（70-198 行、391-480 行、648-676 行、708-744 行）: カラー設定の項目と既定（作業用の空間・方針・聞く・意図・黒点の補正・ディザ・文字のガンマ・モニター）、開くときの方針、校正の設定 → `native/src/app/color_settings_dialog.cpp`、`native/src/app/main_window_color.cpp`。
- `crates/engine/src/display_color.rs`（相対的な色域を維持・黒点補正のモニターの変換、恒等の判定）→ `native/src/app/display_color.cpp`。
- `crates/engine/src/prefs.rs`（443-490 行）: 透明部分の市松の大きさ・色の組と色域外警告の色・不透明度の既定 → `native/src/app/preferences.cpp`。
- `crates/compose/src/psblend.rs`（文字のガンマ `composite_gamma`、既定 1.45 と試験の値）→ `native/src/render/text/lettering.cpp`（`paste_in_gamma`）。
