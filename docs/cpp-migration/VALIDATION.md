# 検証計画・実行頻度（正本）

版: 1.1 / 2026-10-04 / 利用者の整理指示を反映

本書はSPEC.md・PLAN.md・ACCEPTANCE.mdの検証頻度を具体化する。製品機能、保存・権限の保護、画素許容値、固定fixture、生成系列、性能合格値は減らさない。変更するのは **いつ、どの範囲を、どの構成で検査するか**。過去の全体実行記録は履歴として保持し、新しい作業の手順に流用しない。

## 1. 段階と実行のタイミング

### V0 文書・台帳だけ

仕様説明、計画、進捗だけの変更は方針の回帰試験・台帳整合・差分検査のみ。C++をbuildしない。実装を伴う仕様変更はV1以降が必要。文書だけの変更を製品受入成功とは呼ばない。

### V1 開発中・関連変更をまとめる前

- 不具合・新機能は対象のRED→GREENを確認する。
- 変更箇所と依存先のunit、正常対照、無効入力、拒否時の原稿不変、権限、Undo/Redo、保存再開、CLIを選択する。
- 通常はLinux Debugの関連試験。数値・画像・所有権・非同期経路と保存安全をLinux ASan/UBSanでも確認する。CIの開発候補はLinux Debug・ASanとWindows Debugの関連試験。
- 描画変更は該当領域のPython契約・画素比較を含む。例: raster_ops/selectionはラスタ契約・CLI・保存・描画対照、トーン/定規は該当契約と混合操作、3Dは該当契約と混合操作。
- 共通モデル・保存・権限・数値変換・共有描画・oracle・依存・CMake・未知の変更範囲は安全側でV2相当の全体へ拡大する。単純な設定保存でも共通CommandBusを直接変える場合、機械選択は保守的に全体とする。最小化のために危険な依存を隠さない。
- 小さなopごとに公開・全体3構成を繰り返さない。同じ依存境界の関連機能を、完結した候補単位でまとめる。作業者の開発試験とHermesの独立確認は役割を分けるが、独立確認も同じ関連範囲でよく、毎回全体に広げない。

### V2 統合候補の一本化後

- 完結した機能群をnative/integrationへ統合し、その実ソースを一度検証する。全体受入前は候補であり、次の機能を混ぜず同じソースを保持する。
- 通常の全契約・全画素比較はLinux Releaseで一度実行する。
- Linux ASanは非契約の安全対照と変更に関係する契約を実行する。共通境界の変更なら全契約へ拡大する。Releaseの故障注入OFFによる未実施は同ソースASan（Debug）の同slot PASSと対応付ける。対応が欠ければgate不合格。
- Windows Debug/Releaseで実build・関連試験（全体候補では全非契約試験）を実行する。従来どおりPython契約はWindows未実施と明記し、Linuxの結果と混同しない。
- 元CPU/RAM・故障注入・警告・sanitizer設定を維持する。ローカルはgdevの1 CPU/2 GiB、-j1。CI実行機は実環境を記録し、ローカルの資源合格に代用しない。
- 同じソース・実行物・構成・依存・fixture・検査範囲の証跡は再利用する。CIとローカルの同じ一式を機械的に重ねない。ソース/実行物/依存/fixture変更、統合競合、失敗時は該当証跡が無効になり再実行する。
- Windows固有確認や実GUI・性能確認をLinux unitの証跡で代用しない。単なるHEAD違いでも実製品バイトが一致すれば説明・照合を残し再利用可。未照合の別版には流用不可。

### V3 工程出口・配布候補

- M2/M3/M4/M5/M6の出口とM7最終候補で、Linux Debug/Release/ASanの全通常試験＋CLI23、Windows Debug/Releaseの全非契約試験を行う。
- V2の対象と実ソース・構成・実行物・全対象が一致する構成は証跡を再利用できる。V2の関連試験をV3の全体合格へ拡大しない。
- 実GUI・固定の全画面/DPI条件・正式API実通信・全出力・配布物検査は該当工程の出口で行う。GUI変更・保存/終了保護変更があれば、その画面と操作はV1から確認する。
- 性能改修中は該当PERFだけ再測定。工程出口で影響するPERF、M7で全PERFと2時間試験を実施。短い性能slot PASSでp95/p99や2時間試験を代用しない。
- 最終配布物そのもののM7受入は省略しない。waived項目と代替検査はACCEPTANCE.md §6のまま。

## 2. テスト項目の整理

試験資産を削除せず、実行対象を次に分類する。

- **常時保護の関連対照**: JSON/原稿モデル、保存・排他・履歴・transaction、CommandBus、session、CLI/保存/変換E2E、GUI保存・終了・レイアウト。新opの拒否原子性と既存正常操作を含む。
- **領域の契約**: ラスタ、筆跡、選択/マスク、トーン/効果線/定規、3D、描画、保存、CLI。変更領域の契約はV1、全領域はV2 ReleaseとV3。
- **画面・操作の受入**: 実GUI、Undo/再開、全画面と倍率、文字の見え方。自動試験と画像所見を別証拠にする。
- **長時間・性能・配布物**: perfラベルを通常CIから分離したまま、指定された工程で必ず実行する。頻度の変更をnot_runの隠蔽に使わない。
- **CI自身の変更**: 方針/ログ監査のunitに加えLinux Release/ASan・Windows Debug/Releaseの全非契約試験で実ワークフローを確認する。製品・oracleが不変なら全画素契約を重ねない。

名前の登録件数だけで受入しない。成功、拒否無変更、権限、Undo、保存再開、GUI/API入口の台帳は維持。固定・乱数・保存比較のケース数/DPI/許容値も維持。

## 3. CIの起動と重複の除去

- featureブランチのpushは保管だけ。native/integration/mainへのpushまたはそれらへのPRで実行する。feature pushとPRの二重起動を避ける。integration→mainの公開候補PRは別の境界として検査する。ブランチ名だけで検査を省略しない。PRの全jobはmerge treeではなくhead SHAそのものをcheckoutする。実workflow定義がhead側と異なれば停止しrebaseする。統合後にtarget由来の製品差分があれば別の全体候補として検査する。今回の基盤変更は独立レビュー後、工程出口PRの全構成で受入し、全treeが同一の統合pushは証跡を再使用する。
- 通常PRはV1、統合pushはV2。`native/milestone/<工程名>`からのPRはV3の全5構成。現mainにはnative workflowがないため、default branchへの反映前はこのPR経路を使う。mainへの反映後は手動起動 `phase=milestone` も可能。新cron・observer・夜間自動実行は作らない。
- 差分から `validation_policy.py` が対象・構成・除外理由をJSONへ記録する。統合pushの基点は同じbranchの前回合格runの祖先SHA、または全構成合格の工程出口PR（祖先SHAとworkflow/script/CMake/preset/依存制御が同一）とし、前のCI失敗分を直前commit差分だけで落とさない。通常PR・別の制御・不明な履歴は再利用しない。候補決定時だけ公開CI metadataを読み、新認証/permission/cronは追加しない。基点の合格を照合できなければ全体へ拡大する。合格工程出口PRと全treeまで同一のマージpushはrun URLを記録して再buildしない。差分取得失敗は停止、それ以外の差分空/分類不能は全体へ拡大する。変更されたテストを除外しない。
- Python pytestはsrc/tests/依存設定/公開op schema/Python workflow変更時だけLinux/Windowsで実行する。C++専用変更のたびにPython全pytestを実行しない。ただし必要なPython参照契約はnative試験に残す。
- CTestをverboseで一度実行し、Windowsでは同じCTest登録から`-o file,txt`で全選択QtTestのrawログも出す。その一回のrawログでQt Totals/FAIL/SKIPを検査する。ログ取得のための再実行と、失敗後の全exe再実行を廃止する。
- 0試験、欠落/重複Totals、未完走、未知SKIP、sanitizer診断、構成artifact欠落を成功にしない。Releaseの既知SKIPだけを許可し、Debug/ASan側の同slot PASSを総合auditで要求する。
- CIは自動で実行中候補をcancelしない。手動V3は通常push/PRとconcurrencyを分離する。不要になった同じ検査対象の候補のみ、理由とログを保存して明示的に停止する。cancelは未受入であり成功に数えない。ローカルの意図的停止は理由・元ログ・状態を保存する。

## 4. 実行入口

計画を作る（baseは依存境界を固定した実コミット）:

```sh
python3 tools/ci/validation_policy.py --base <基点SHA> --phase development --output build/validation-plan.json
```

対応するCMake presetをconfigureし、JSONにある構成だけ実行する:

```sh
python3 tools/ci/validation_run.py --plan build/validation-plan.json --preset linux-debug --build build/linux-debug --output build/validation/linux-debug
```

全構成の `result.json` を同じディレクトリへ集めた後:

```sh
python3 tools/ci/validation_run.py --plan build/validation-plan.json --pair --output build/validation
```

ローカルは既存gdevの資源境界を使う。過去のreviews/run_*_full_matrix.pyは当時の証拠を再現する履歴用であり、新opごとの既定手順には使わない。新しい候補ごとにdriverを複製しない。

## 5. 現在の作業のまとめ方

- 統合木596f0f7の全体3構成・CI合格は保持。
- オニオン/ライトテーブル設定の対象試験と実CI成功を保持し、同じものを再実行しない。
- m3lの未完了ローカル全体3構成は、本整理指示に基づき停止。元exit137・rawログを保全し意図的停止を別記録する。合格には変えない。
- 先に進んでいたm3o ASanは、既存証拠を捨てて再起動しないため完走分を回収する。完走しても別版の全体合格へ流用しない。
- この設定変更は独立したCI方針候補として検査する。製品機能の作業木を変更しない。
- 次の製品統合では設定関連を候補単位でまとめ、V2を一本の統合木で実施する。プレビュー描画の未実装を設定保存の合格で閉じない。
