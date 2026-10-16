# PhotoCraft由来の算法

露光量の式・パラメータ境界は PhotoCraft v0.2.0 (`ad863217386440ca968fccc9bfff65ba24e61142`) の `crates/compose/src/adjust.rs` と `crates/engine/src/adjust_params.rs` を参照してC++へ実装した。著作権と利用条件は同梱 `LICENSE-MIT` を参照。ブランド・上流バイナリ・別アプリは製品へ同梱しない。

## 内蔵の ICC プロファイル（color-model@1、PC1a）

- `native/src/core/data/profiles/coated-cmyk.icc` は PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`）の `crates/cms/profiles/photocraft-coated-cmyk.icc` をそのまま同梱したもの（sha256 `f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc`）。上流がこのファイルを CC0-1.0（パブリックドメイン）として公開している（`ATTRIBUTION.md`、`crates/cms/README.md`、`crates/cms/src/builtin.rs` 冒頭）。
- 内蔵プロファイルの ID・別名・説明の文字列と、RGB・グレーの原色・白色点・トーンカーブの選び方は `crates/cms/src/builtin.rs`（18-102 行、229-268 行、MIT）による。RGB・グレーのプロファイルのバイト列そのものは Genko が開発用の道具 `tools/migration/make_profiles.cpp` で Little CMS により作ったもの（CC0-1.0 として扱う）。
