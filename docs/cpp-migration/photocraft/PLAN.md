# PhotoCraft全機能取込み・実装境界

依頼: 「このリポジトリの機能を調べてください Genko-studioにない機能を全て取り込んでください」

## 適用方針

- C++20/QtのGenko本体へ直接実装する。既存Python/原稿/未完修正を保全する。
- 別アプリの起動、メニューの登録だけ、8bitへの不可逆な平坦化を高精度/非破壊機能の完成として扱わない。
- MIT移植箇所には原著作権・本文を残す。商標/ブランドは移植しない。
- 新費用・新権限・外部AI・認証変更・新自動継続・CI再有効化をしない。
- 利用者は「漫画原稿へ全機能を統合し、C++版の原稿形式を拡張する」を選択済み。原本を保全し、feature/capabilityで新機能を識別、未対応readerでは編集を拒否する。
- 「カラーもC++では可能にします」を正式要求COLOR-01としてSPECへ追加。C++版はカラー原稿の作成・着彩・編集・表示・保存・再読込・出力まで対象とし、モノクロ制作も維持する。承認の実結果はDECISIONS.jsonに記録。

## ローカル参照元

PhotoCraftのGitリポジトリを `/home/hermes/genko-test/references/photocraft-v0.2.0` に保全する。履歴・タグ取得済み、参照pinは `ad863217386440ca968fccc9bfff65ba24e61142`（v0.2.0）。PhotoCraft由来機能の作業時は `SOURCE.md` に従い、対象の実装・テスト・仕様をこのコピーから読み直す。参照コピーは変更せず、Genko側で実装する。

## 全件正本

`feature-ledger.json`（961行）を正本とし、既存部分対応と本体実装/受入済みを区別する。名前・件数はPythonで集計。新しい発見は追記し、未完行を削除しない。

## 工程

1. 保存/精度基盤の設計方針は利用者の選択で確定。カラー制作を含む漫画原稿への統合として実装する（製品ソースの直接実装・開発検証中）。
2. 8/16/32bitと色/チャンネル、素材保存、feature識別、Undo/Redo/復旧/GC、予算。
3. 調整/色管理を個別の縦断TDDで完成。
4. スマートオブジェクト/フィルター・選択修復・パス/変形。
5. ブラシ/フィルター群・RAW/形式互換。
6. 制作補助/バッチ/タイムライン/プラグイン/安全なMCP接続。
7. 台帳全件の同等性・実画面・保存・独立レビュー・統合受入・配布。

## 初回開発検証の記録（2026-10-06 08時台・履歴）

- RGBA16/32保存・再読込、露光量調整層、feature識別をC++へ直接実装中。
- 初期の実xcb GUIで日本語の露光量操作、素材不変、メモリUndo/Redoが合格。修正版のGUI回帰は別途実施する。
- 独立レビューv1の4件はRED→GREENの回帰試験を追加して対処。精度を失う旧結合・変換は拒否、HDRの補間はstd::lerp、role=toneの拒否、同じopen handleによるサイズ・読込量上限を導入。現在のカラー単体はQt 11 PASS/0 FAIL/0 SKIP（初期化・終了を含む）。
- 修正版の独立読取専用レビューv2と、CLI/保存/描画/GUIの回帰を実施中。正式disk Undo、復旧、GC、出力、上流画素対照、ASan/全Release統合受入は未完了。
- 作業中の報告は15分ごと・重要節目。最終報告2026-10-06 08:29 JST。新cron/observer/AIによる自動継続を追加しない。
- 未コミット・未統合・未公開。旧review-core-v1.jsonの不合格を改変しない。

## 修正前の限定V1結果（履歴）

- `build/photocraft-v1/regression-v6/execution.json`: 関連8系列Qt148 PASS/0 FAIL/0 SKIP、実CLIのRGBA16/32元bytes・PNG画素、保存済みUndo/Redo、GC、拒否batchの全保存files不変（released_atのみ正常変化として扱いagent/releasedを確認）、素材の読込中伸長を注入して24+1bytesで拒否。source/binary/driver前後一致、1CPU/2GiB/OOM kill増分0。
- ノンブルの既存未移植を避けるため合成入力のnumero=falseをfixture生成で設定。set_nombreの合格ではない。初回driverの未実装依存とinspectの応答形の誤りは失敗artifactのまま保全。
- canonical履歴のmetadataキー順依存によるdisk Undo停止を再現し、順序を無視した完全なキー/値検査へ訂正。非カラー/非表示の不正exposure metadataもread_only化、GUIのparse例外も安全な日本語拒否へ訂正。
- `gui-settled-probe/visual-observations.json`: 実Canvasの非同期タイル完了・現在DPIの独立proof render全画素一致・GUI4 PASS。原稿とサムネイルの暗い素材表示を目視確認。以前の白い画像は旧タイルの撮影で不合格のまま保全。新probeは製品sourceを変えず現在のlibにリンクした一時検証。
- 露光量ダイアログの日本語ラベル・数値・追加/キャンセルは可読で文字切れなし。レイヤー一覧は未確認。
- 最終の第2再レビュー`review-core-v3.json`はpassed=true。レビュー対象34・依存6・全native526ファイルのSHA256が現在と一致することを主担当が照合済み。未コミット・未統合・未公開。Release/ASan全体受入・復旧・汎用入力・上流全同等性・Windowsは残る。
- 961行を保持し、`layer.newAdjustmentLayer.exposure`と`document:color-depth-model`のみ部分実装へ更新。台帳件数やQt PASS数を完成機能数として数えない。

## 次の実作業

現在はM3のRGBA16/32素材・露光量補正の初回機能群を検証中。保存基盤とCLI/GUIの直接実装、disk Undo/Redo、GC、拒否時不変の限定検証は実施済みであり、最初のREDをやり直す段階ではない。

- 初回ASan/UBSan全58試験は55合格・3不合格で完走。元ログを保全した。
- 読取レビュー合格の互換性修正を本体へ適用し、同じ全58計画で再検証している。ASan/UBSan合格後にRelease全58へ進む。候補sourceを途中で変更せず、次機能を混ぜない。
- 修正後本体の先行2 CTest/Qt16は合格。正式CLIの20操作ではRGBA16/32元bytes、印刷/校正PNG中心画素、disk Undo/Redo、GC、読戻し、拒否batchの全保存files不変が合格。証跡は`build/photocraft-full-repaired/current-color-cli/execution.json`。全体合格ではない。
- 旧4操作の高精度拒否と別頁の正常対照は、同一actorの正式保存基準を使用し、全体試験の空きメモリが確保できる時に実行する。CPU/RAM条件を増やしたり、元fixtureを減らして通さない。
- レビュー対象34・依存6・native526・fixture109を照合し、現在の2ファイル差分が承認された修正コピーのbytesと一致。`build/photocraft-full-repaired/combined-review-source-check.json`に記録。
- 最新の実行状態・保留・最終報告時刻は`IMPLEMENTATION-PROGRESS.json`が正本。Windows開発中保留、配布前実機受入必須、Actions無効は維持する。

調査レポート `REPORT.md` に対応範囲、出典、限定動作確認、互換性の停止条件を保存済み。Genko製品への取込みはRGBA16/32保存と露光量調整層から着手済み。全機能完了ではない。
