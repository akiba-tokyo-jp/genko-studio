# Genko C++ 引継ぎ（2026-10-08）

## 再開位置
- 作業場所: `/home/hermes/genko-test/cpp-impl/wt/integration`
- 現在のブランチ: `native/integration`。利用者のマージ指示により、引継ぎ用branchのmerge commit `131c8e7`をfast-forwardで取り込み済み。
- 現在のintegrationは未受入WIPを含む。最後の受入済み基点は `5e07b9102b2768d062825744ecd83cfb44e64f05`であり、履歴に保持。Python stable/mainは変更していない。
- 元A: `native/m2-material-previews` / `9756f25e021e2bb2e2991118a1d2e193cf24ea14`（本体snapshot `2ec250a`、PNG保全 `9756f25`）。元B: `native/m2-user-material-cache` / `835d4dd2c75b0ac036977ee050071c93e56f42e2`。両元作業木は保全し、削除しない。

## 依頼と境界
- 最新依頼は「今の状態をマージして別のセッションで引き継げるようにして」。引継ぎを製品完成/受入済みと扱わない。
- 主担当は接続済みgpt-6.1-solによるHermesの直接実装。別AIは読取専用レビューのみ。Claudeと自己紹介した先の回答は誤りであり、モデル/担当の根拠にしない。
- 新費用/権限/認証変更/CI再有効化/cron/observerを追加しない。契約上限では停止して報告し、自動切替しない。
- 本番原稿/認証は扱わない。元CPU1/RAM2GiB/-j1、fixture/安全検査/画素閾値を維持。Windows開発中deferred、最終配布前native受入必須。waivedは合格ではない。

## 読む正本
1. `genko-cpp-migration` skill
2. `/home/hermes/genko-test/cpp-impl/PROGRESS.md` と `EXECUTION-QUEUE.json`
3. 本branchの `docs/cpp-migration/{SPEC,PLAN,ACCEPTANCE,VALIDATION,ARCHITECTURE}.md` と既存機能/改善/PhotoCraft台帳
4. PhotoCraft固定参照 `/home/hermes/genko-test/references/photocraft-v0.2.0`、commit `ad863217386440ca968fccc9bfff65ba24e61142`（変更禁止）
- `handoff/PROGRESS.snapshot.md` と `handoff/EXECUTION-QUEUE.snapshot.json` は引継ぎ時点の保全コピー。通常の更新正本は上記rootファイルであり、新台帳を増やさない。

## マージした変更と検査状態
- A: 組込み素材preview/検索/配置、FreeType字形基盤、ノンブルのGUI/描画/保存契約、素材cache本体。
- B: 高精度結合/flatten/変換、u16/f32画像↔精密ペン線、保存feature `native.color_stroke_v1`、編集時色保持、安全拒否、素材cache単体試験。
- 明示競合はmain_window.hppとmain_window_actions.cpp。nombre_dialogとlayer_operationの両方を保持して解消。共有render/core/CMakeの自動マージ部分も変更されているため、旧受入をそのまま新候補に拡張しない。
- 元snapshotの全変更パスSHAを照合済み。元ソースは各親branchに完全保全。fixture MANIFESTの70項目は実bytes一致、ノンブルPNG64枚をGitへ追加保全。
- このマージ後のcompile/製品試験/独立レビュー/V2/V3は未実施。マージ保存は受入合格ではない。

## 中断した文字作業
- `native/tests/unit/test_render_page.cpp`の未実行plainTextCases 2slotは稼働試験から外し、`handoff/unfinished-text-tests.patch`へ保存。元A snapshotにも完全に残る。
- 対応するtext-cases.jsonは未生成。RED未確認、renderer未実装。patchを今すぐapplyして全試験を起動しない。
- 元A `build/nombre-reference.py` のtext生成分岐と `build/material-v1.py` のtext phaseは準備のみ。実API/region型/registryを読んでから再開する。build失敗をREDと数えない。

## 証拠の場所（既存結果を再実行しない）
- A `/home/hermes/genko-test/cpp-impl/wt/m2-material-previews/build/`
  - `material-formal-v1/execution.json`: 旧限定Releaseの5suite/95QtPASS。後続18source変更のため現候補全体へ流用不可。
  - `material-stamp-review/`、`material-preview-review/`、`material-stamp-asan/font-glyphs-v2/`: 素材/字形の限定review・sanitizer証拠。
  - `gui-xcb-material-stamp-fixes/execution.json`: 限定GUI passed。
- B `/home/hermes/genko-test/cpp-impl/wt/m2-user-material-cache/build/`
  - `material-cache-review/result.json`: 3source限定合格、GUI library入口は対象外。
  - `m2-shared-color/precision-pen-related-v4/execution.json`: マージ前10suite/347QtPASS、fail/skip0、source不変、OOM0。
  - `m2-shared-color/gui-xcb-precision-pen/{execution.json,visual-review.json}`: マージ前の実xcb限定4QtPASS・画像所見。
  - `m2-shared-color/precision-pen-review/fixes/review.json`: 旧4P1の安全修正合格。filter/高精度pixel編集は明示拒否であり機能未完成。
- 旧ログ・build・失敗attempt・raw/JSON/画像は元作業木に保持。このGit snapshotに全build binaryは含まれない。同一ホストの別セッションで参照できる。

## 残作業と次の具体操作
- 次はこのマージ差分のcompileと関連正常/拒否/保存/画素/GUIの確認。共通境界を変えたので規定に従って対象拡大。先に差分/実APIを読み、driverを新設しない。
- M2残: ユーザー素材libraryのGUI/cache接続、頁の必要時読込、既存StartDialogの1024x640/初期preview不足分、開く→描く→Undo→保存→終了→別process再開とAC-PERF。既存E2E/perf/開始画面を作り直さず現物確認する。
- 再構成手順はrootの単一EXECUTION-QUEUE.json。M2出口の不足を閉じる→変更/競合だけreview→凍結→VALIDATIONの必要構成/実GUI/性能→現integration候補の受入を閉じる。今回の利用者指定マージを受入合格の代わりにしない。
- 続くM3/PhotoCraft: 高精度filter/画素消去・選択、色モデル/チャンネル/補正、紙質/ブラシ/ABR、残描画/アニメ/タイムラプス/plugin。M4写植/ルビ/フキダシ/全出力/PSD、M5制作サービスとMCP/HTTP/CLI、M6両OS配布、M7最終受入は未完。
- 計画/台帳を全件再生成しない。短試験→review→必須修正→凍結→規定重受入→統合の単位を保ち、合格済みの同一source/binary/config/fixture/scopeを再利用する。review1回/受入1回は目安であり、重大不具合や無効化された証拠の再確認を禁止する意味ではない。

## 他セッションへ渡す文
「Genko C++作業を引き継いでください。`/home/hermes/genko-test/cpp-impl/wt/integration/docs/cpp-migration/HANDOFF.md` を読み、native/integrationの未受入マージ候補から、既存キューのM2出口を継続してください。Python安定版・元作業木・証拠を保全し、主担当が直接実装してください。」
