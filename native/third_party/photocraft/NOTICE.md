# PhotoCraft由来の算法

露光量の式・パラメータ境界は PhotoCraft v0.2.0 (`ad863217386440ca968fccc9bfff65ba24e61142`) の `crates/compose/src/adjust.rs` と `crates/engine/src/adjust_params.rs` を参照してC++へ実装した。著作権と利用条件は同梱 `LICENSE-MIT` を参照。ブランド・上流バイナリ・別アプリは製品へ同梱しない。

## 内蔵の ICC プロファイル（color-model@1、PC1a）

- `native/src/core/data/profiles/coated-cmyk.icc` は PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`）の `crates/cms/profiles/photocraft-coated-cmyk.icc` をそのまま同梱したもの（sha256 `f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc`）。上流がこのファイルを CC0-1.0（パブリックドメイン）として公開している（`ATTRIBUTION.md`、`crates/cms/README.md`、`crates/cms/src/builtin.rs` 冒頭）。
- 内蔵プロファイルの ID・別名・説明の文字列と、RGB・グレーの原色・白色点・トーンカーブの選び方は `crates/cms/src/builtin.rs`（18-102 行、229-268 行、MIT）による。RGB・グレーのプロファイルのバイト列そのものは Genko が開発用の道具 `tools/migration/make_profiles.cpp` で Little CMS により作ったもの（CC0-1.0 として扱う）。

## チャンネル（channels@1、PC1c）

PhotoCraft v0.2.0（`ad863217386440ca968fccc9bfff65ba24e61142`、MIT、同梱 `LICENSE-MIT`）の次の部分を参照して C++ へ移した。

- `crates/engine/src/channel_cmds.rs`（781-1296 行）: 新規・特色・複製・削除・名前の変更・並べ替え・オプション・特色を合成・分割・統合の既定値と名前（Alpha n、上流の Spot Color n は「特色 n」）、番号の付け替え、特色を合成の式 `c + (s·ink + (1−s)·c·ink − c)·v`（1149-1198 行）。Genko の op（`native/src/render/ops_channels.cpp`）と画素の op の対象（`native/src/render/channels.cpp`）。
- `crates/compose/src/multichannel.rs`（14-38 行）: 特色の表示の式 `(1 − s·v)·p·(1 − v + v·c) + s·v·c`（`native/src/render/channels.cpp` の print_spots）。
- `crates/ui-egui/src/channel_view.rs`（64-128 行）: チャンネル パネルの表示（色の版の 1 枚をグレーで、隠した版は 0、アルファの重ねの覆い方 `overlay_coverage`）。
- `crates/io/src/channel_map.rs`（1-40 行）と `crates/io/src/psd_export.rs`（448-470 行、654-656 行）: PSD の追加チャンネルの名前（1006・1045）と DisplayInfo（1077）の形（`native/src/formats/psd_export.cpp`、読み戻しは `native/src/render/psd.cpp`）。Genko は 1077 の記録を 13 バイト（上流が種類の後に書く埋めのバイトは書かない）、特色と「選択範囲」のアルファの標本を Photoshop の向き（色の付く所が暗い）で書く。
