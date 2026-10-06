# PhotoCraftのローカル参照リポジトリ

利用者の指定: 「PhotoCraftリポジトリも置いて、作業時に再確認できるようにしてください」

## 保存場所

- ソースリポジトリ: `/home/hermes/genko-test/references/photocraft-v0.2.0`
- 上流: `https://github.com/storytold/photocraft.git`
- 固定するリリース: `v0.2.0`
- 固定するコミット: `ad863217386440ca968fccc9bfff65ba24e61142`
- Gitの浅い取得を解除済み。過去履歴とタグも参照できる。
- 配布物と限定実行用CLI: `/home/hermes/genko-test/references/photocraft-release-v0.2.0`

上流ソースは参照専用として扱う。Genkoへの修正はGenkoのworktreeで行い、この参照コピーを直接変更しない。自動更新・自動pullはしない。調査対象を変更するときは別の保存場所へ取得し、現在のpinと既存証跡を保全する。履歴取得時もcheckoutはv0.2.0から変えていない。

## 作業時の再確認

PhotoCraft由来の機能を実装・修正する際は、その機能のコード・テスト・仕様をこのリポジトリから読み直す。メニュー名や以前の要約だけから動作を推測しない。

1. `git -C /home/hermes/genko-test/references/photocraft-v0.2.0 rev-parse HEAD` で上記pinと一致することを確認する。
2. `git -C /home/hermes/genko-test/references/photocraft-v0.2.0 status --porcelain` が空であることを確認する。変更があれば無断resetせず、差分を確認する。
3. `feature-ledger.json`の対象IDと、上流のコマンド・算法・保存形式・テストを照合する。
4. 参照したファイル/位置/commitをGenkoの受入証跡へ結び付ける。上流の成功をGenkoの受入へ流用しない。

## 最初に読むファイル

- `README.md`: 機能全体。ただし数値は実コード/実登録一覧と照合する。
- `docs/architecture.md`: エンジン・描画・UI・形式の境界。
- `crates/engine/src/commands.rs`: 登録操作とパラメータ。
- `crates/engine/src/adjust_cmds.rs`: 調整層の操作。
- `crates/ui-egui/src/menu_catalog.rs`: メニューの出典。
- `crates/ui-egui/src/state.rs`: 道具の列挙。
- `crates/codecs/src/format.rs`: codec能力と形式。
- `docs/plugins.md`: プラグインの仕様。
- `LICENSE-MIT`, `LICENSE-APACHE`, `NOTICE`, `ATTRIBUTION.md`: 移植・資産の利用条件。

## 検証済みの保存状態

この配置作業では、上流URL、v0.2.0のcommit、一致するタグ、作業木無変更、履歴の非shallow状態、`git fsck --no-reflogs`の成功を確認した。製品の実装・ビルド・受入完了を意味しない。
