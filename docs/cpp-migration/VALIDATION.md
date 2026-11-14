# 検証計画・実行頻度（正本）

版: 1.3 / 2026-11-14 / 工程出口の範囲（利用者決定 2026-11-13）と差分の機械的な分類（利用者決定 2026-11-14）を反映。1.2（2026-10-05）: 利用者の整理・Windows検証保留指示

## 0. 開発中のWindows検証の扱い（利用者指示）

GitHub Actionsはリポジトリ側で無効とし、検証のために再有効化しない。既存Windows実行環境が未確保の間、**開発中のV1/V2のWindows Debug/Releaseは保留し、Linuxの検証・移植を継続する**。2026-10-05に利用者がこの進め方を明示選択した。

- 元の検証計画のWindows対象・構成・除外範囲は保持し、`deferred`（保留）を別記録する。Linux成功、旧版Windows成功、静的な可搬性レビューをWindows実build/test成功へ置き換えない。
- LinuxのV2全通常Release・必要なASan/UBSan・CLI・画素・保存安全の合格条件は減らさない。Linuxで失敗している候補へ次機能を混ぜない。
- Windows保留を理由に開発を止めないが、OSを限定した開発ゲート成功と両OSの最終受入を区別する。
- **配布前には、その配布候補そのもののWindows実機build・実行・受入を必ず行う。** M6/M7・最終配布物のWindows要件、性能値、別途waivedになっている実ペン等の扱いは維持する。Windows受入なしに両OS配布完了としない。ここでの実機検証はWindowsネイティブ環境での配布物の起動・保存・出力・GUI／注入入力を指し、実施不要となっている実ペン・撮影・筆圧／傾き／IMEの手動実操作を復活させない。

以下のV1/V2とCI入口に記載したWindows構成は本補足の対象となる。V3・最終配布前の実検証は保留を合格にせず、確保した実行場所で実施する。新CI・新権限・新費用・自動schedulerを追加する許可ではない。

本書はSPEC.md・PLAN.md・ACCEPTANCE.mdの検証頻度を具体化する。製品機能、保存・権限の保護、画素許容値、固定fixture、生成系列、性能合格値は減らさない。変更するのは **いつ、どの範囲を、どの構成で検査するか**。過去の全体実行記録は履歴として保持し、新しい作業の手順に流用しない。

## 1. 段階と実行のタイミング

### V0 文書・台帳だけ

仕様説明、計画、進捗だけの変更は方針の回帰試験・台帳整合・差分検査のみ。C++をbuildしない。実装を伴う仕様変更はV1以降が必要。文書だけの変更を製品受入成功とは呼ばない。

### V1 開発中・関連変更をまとめる前

- 不具合・新機能は対象のRED→GREENを確認する。
- 変更箇所と依存先のunit、正常対照、無効入力、拒否時の原稿不変、権限、Undo/Redo、保存再開、CLIを選択する。
- 通常はLinux Debugの関連試験。数値・画像・所有権・非同期経路と保存安全をLinux ASan/UBSanでも確認する。CIの開発候補はLinux Debug・ASanとWindows Debugの関連試験。
- 描画変更は該当領域のPython契約・画素比較を含む。例: raster_ops/selectionはラスタ契約・CLI・保存・描画対照、トーン/定規は該当契約と混合操作、3Dは該当契約と混合操作（名前の規則。2026-11-13 までのまま）。app の変更は test_gui_*・test_session・test_ops_e2e（同じく名前の規則）。
- それ以外で、前はフォルダーごとに全体へ送っていた変更（core・storage・service・api・formats・studio・試験の補助とデータ・埋め込みデータ・名前に系統の無い render）は、§2a の規則（ソースの閉包・子プロセス・データ）で関係する試験を決める。同じ差分で、前より多くの試験を選ぶことはない。
- 依存・CMake・oracle（tools/migration・tools/acceptance）・規則が説明できない変更は安全側で全体へ拡大する。共通モデル・保存のように全試験の閉包に入るコードは、規則の結果として全体になる。最小化のために危険な依存を隠さない。
- 小さなopごとに公開・全体3構成を繰り返さない。同じ依存境界の関連機能を、完結した候補単位でまとめる。作業者の開発試験とHermesの独立確認は役割を分けるが、独立確認も同じ関連範囲でよく、毎回全体に広げない。

### V2 統合候補の一本化後

- 完結した機能群をnative/integrationへ統合し、その実ソースを一度検証する。全体受入前は候補であり、次の機能を混ぜず同じソースを保持する。
- 通常の全契約・全画素比較はLinux Releaseで一度実行する。
- Linux ASanは常時保護の対照（§2）と、差分に関係する試験（§2a）を実行する。全体へ拡大した差分なら全通常試験。Releaseの故障注入OFFによる未実施は同ソースASan（Debug）の同slot PASSと対応付ける。対応が欠ければgate不合格。
- Windows Debug/Releaseで実build・関連試験（全体候補では全非契約試験）を実行する。従来どおりPython契約はWindows未実施と明記し、Linuxの結果と混同しない。
- 元CPU/RAM・故障注入・警告・sanitizer設定を維持する。ローカルはgdevの1 CPU/2 GiB、-j1。CI実行機は実環境を記録し、ローカルの資源合格に代用しない。
- 同じソース・実行物・構成・依存・fixture・検査範囲の証跡は再利用する。CIとローカルの同じ一式を機械的に重ねない。ソース/実行物/依存/fixture変更、統合競合、失敗時は該当証跡が無効になり再実行する。
- Windows固有確認や実GUI・性能確認をLinux unitの証跡で代用しない。単なるHEAD違いでも実製品バイトが一致すれば説明・照合を残し再利用可。未照合の別版には流用不可。

### V3 工程出口・配布候補

- **利用者の決定（2026-11-13）**: 「変更分と影響箇所だけを見る」。工程の出口は、`validation_policy.py` が差分から選ぶ範囲を ASan と Release の 2 構成で流す。**Debug は最終配布の前に 1 回だけ。** 同じ試験を 3 構成で重ねて流さない。合格値・監査（SKIP・Totals・sanitizer 診断・pins）は変えない。流さなかった構成・試験は合格に数えない。
- **利用者の決定（2026-11-14）**: 「分類されていない変更は全試験」の規則を見直し、差分の分類を細かくする（§2a）。
- **工程出口**（M2〜M6 の出口、機能の工程の出口）は `--phase milestone`:
  - linux-release: 全通常試験（契約を含む）＋CLI23。全部で約 20 分と安いので毎回全部流す。分類が外れたときの安全網。
  - linux-asan: 常時保護の対照（§2）と、差分に関係する試験（§2a）＋CLI23。全体へ拡大した差分（依存・CMake・説明できない変更・関係する試験が全試験に及ぶ差分）は全通常試験。
  - Windows Debug/Release: ASan と同じ集合から契約を除いたもの（全体へ拡大した差分は全非契約試験）。§0 の保留はそのまま。
  - linux-debug は入れない。
  - 構成ごとの範囲とその理由は、計画の `reasons`（構成ごとの行）と `classification`（変更パスごとの扱い）に残る。
- **最終配布候補**（M7。配布の前に一度）は `--phase final`: Linux Debug/Release/ASan の全通常試験＋CLI23、Windows Debug/Release の全非契約試験。2026-11-13 までの `--phase milestone` と同じ中身。
- Release の故障注入 OFF による SKIP は、同じ計画の ASan の同 slot PASS と対にする（`--pair`）。故障注入の試験は常時保護の対照に入っているので、ASan が関係する試験だけの工程出口でも対になる（方針の回帰試験で固定）。
- V2の対象と実ソース・構成・実行物・全対象が一致する構成は証跡を再利用できる。V2の関連試験をV3の全体合格へ拡大しない。工程出口（milestone）の ASan の合格を、最終配布候補（final）の ASan の合格に数えない。
- 実GUI・固定の全画面/DPI条件・正式API実通信・全出力・配布物検査は該当工程の出口で行う。GUI変更・保存/終了保護変更があれば、その画面と操作はV1から確認する。
- 性能改修中は該当PERFだけ再測定。工程出口で影響するPERF、M7で全PERFと2時間試験を実施。短い性能slot PASSでp95/p99や2時間試験を代用しない。
- 最終配布物そのもののM7受入は省略しない。waived項目と代替検査はACCEPTANCE.md §6のまま。

## 2. テスト項目の整理

試験資産を削除せず、実行対象を次に分類する。

- **常時保護の関連対照**: JSON/原稿モデル、保存・排他・履歴・transaction、CommandBus、session、CLI/保存/変換E2E、GUI保存・終了・レイアウト。新opの拒否原子性と既存正常操作を含む。
- **領域の契約**: ラスタ、筆跡、選択/マスク、トーン/効果線/定規、3D、描画、保存、CLI。変更に関係する契約（§2a）はV1、全領域はV2・V3 の Release と最終配布候補。
- **画面・操作の受入**: 実GUI、Undo/再開、全画面と倍率、文字の見え方。自動試験と画像所見を別証拠にする。
- **長時間・性能・配布物**: perfラベルを通常CIから分離したまま、指定された工程で必ず実行する。頻度の変更をnot_runの隠蔽に使わない。
- **CI自身の変更**: 方針/ログ監査のunitに加えLinux Release/ASan・Windows Debug/Releaseの全非契約試験で実ワークフローを確認する。製品・oracleが不変なら全画素契約を重ねない。

名前の登録件数だけで受入しない。成功、拒否無変更、権限、Undo、保存再開、GUI/API入口の台帳は維持。固定・乱数・保存比較のケース数/DPI/許容値も維持。

### 2a. 差分の分類（どの試験が関係するか。利用者決定 2026-11-14）

`validation_policy.py` が、木に置かれたソースだけから決める（ビルドの前に計画を作る。Linux と Windows の checkout で同じ答え）。試験を消したり SKIP にしたりはしない。決めるのは、ASan・Windows・V1 の構成にどの試験を入れるかだけ。

- **どの規則が決めるか**: 名前の規則（下の「名前の規則」）が当たるパスは、2026-11-13 までと同じ集合。**閉包が置き換えたのは、フォルダーごと・未分類として「全試験」に送っていた変更だけ**（core・storage・tests/support・service・api・formats・studio・埋め込みデータと手引き・試験のデータ・名前に系統の無い render）。計画の `classification` に、パスごとにどちらが決めたかが出る（`名前の規則（2026-11-13 までのまま）: …` か `ソースの閉包` など）。
- **前より多く選ばない**: 同じ差分・同じ段階で、ASan を含むどの構成も、2026-11-13 の方針が選んだ試験の部分集合になる（方針の回帰試験が、領域ごとの代表のファイルで、古い規則を書き出して比べる）。前より増えるのは、前の方針が落としていた次の 3 つだけ: (1) `resources/studio/` の手引き（`*.md`）は service に埋め込まれるのに、文書として何も流していなかった → service の閉包の試験、(2) 試験が名指しで読んで比べる文書（`docs/ops.schema.json`、`docs/PSD_TEXT_SUPPORT.md`、`THIRD_PARTY.md`、integrations の `SKILL.md`）→ その試験 1 本、(3) 2 つの試験が同じソースから作られる場合（`test_contract_plugins.cpp`）→ 両方。開発段階（V1）で閉包が決めた差分は、Debug・ASan・Windows Debug の関係する試験になる（前は Release・ASan・Windows 2 構成の全試験。構成は替わるが、流す試験の延べは減る）。
- **名前の規則**（2026-11-13 までのまま。前から受け入れられている選び方で、工程出口では Release が全試験を流す）: `native/src/render/` のファイルは、名前に `image`・`png`・`render`・`stroke`・`raster`・`ops`・`pynum`・`pyrandom` を含む非契約の試験に加えて、ファイル名が `raster_ops`・`selection` なら `raster`・`select`・`mask` を含む試験、`tone`・`effect`・`ruler` を含むなら `tone`・`ruler` を含む試験、`3d`・`mannequin` を含むなら `3d` を含む試験。`native/src/app/` は `test_gui_*`・`test_session`・`test_ops_e2e`。これらのパスに閉包を当てると前より多く選ぶ（`raster_ops.cpp` で 45 本 → 256 本、app で 79 本 → 141 本）ので、当てない。

- **ソースの閉包**: 試験の閉包は、`native/tests/CMakeLists.txt` の SOURCES から `#include "…"` をたどり（インクルードした側の隣、`native/src`、`native/tests/support` の順に探す。木に書かれた綴りそのまま）、ヘッダーが出てきたら、そのヘッダーが宣言した名前を行頭で定義している同じライブラリ（`native/src/<名前>`、試験の木は `native/tests`）の実装ファイルを足し、そのファイルのインクルードをまたたどったもの。
  - 「宣言した名前を定義している」: 実装ファイルの行頭にある定義（`型 名前(`、`クラス::名前(`、`operator`、値の定義）の名前がヘッダーに現れ（関数は `名前(` の形）、修飾（`クラス::`・`名前空間::`）があればその名前の class・struct・namespace がヘッダーで開かれていること。同じ名前の .cpp（`a.hpp`→`a.cpp`）も、分割した実装（`main_window.hpp`→`main_window_*.cpp`）も、別名の .cpp で定義した関数も、この一つの規則で入る。`static` の定義は数えない。
  - 実行物のソース（試験の SOURCES、`add_executable` のソース）は、その実行物だけのもの。ヘッダーからは連れて来ない。
  - Q_OBJECT のヘッダーは、ライブラリごとに一まとまり（AUTOMOC の生成物が 1 ファイルで全クラスを参照するため）。
  - どのヘッダーも連れて来ない実装ファイルは「説明できない」として全体へ拡大する。
- **子プロセス**: 試験が動かす実行物（`genko`、`genko_lock_helper`、`genko_argv_helper`、`genko_test_native_plugin`、`genko_fake_mcp`）は、試験の名前ではなく、CMake の定義（`GENKO_CLI="$<TARGET_FILE:genko>"` など）のマクロをソースが使っているか（補助関数・クラスごしを含む）と、`add_dependencies(試験 実行物)` から決める。動かす試験は、その実行物の閉包に入る変更すべてに関係する（`genko` を動かす試験は、製品のほぼ全ソース）。
- **ビルドが埋め込むデータ**: CMake が雛形（`*_data.cpp.in`）へ埋めるファイル（`core/data/ops_schema.json`・`commands.json`・`unicode_names.bin`、`resources/studio/` の手引きと表）は、雛形が定義する名前を使うソースの閉包へ。qrc に並ぶファイルと qrc 自身は、`Q_INIT_RESOURCE(その名前)` を書いたソースの閉包へ。埋め込み方を読み取れないもの（`data/profiles/*.icc`、qrc にも CMake にも無い resources、名前を CMake が作る雛形）は全体へ。
- **試験のデータ・木の外のファイル**: 試験の木のソースが文字列で名指しするもの（ファイル名、`/` を含む部分パス、`"legacy/book-" + 版` のような途中まで）を、そのソースを閉包に持つ試験へ。名指しが無い試験のデータは全体へ。文書（`docs/`、`*.md`）は、名指しする試験があればその試験、無ければ文書だけの変更（V0）。文書でも試験の木でもないものは、`/` を含む部分パスで名指しされたときだけ。製品のソースを文字として読む試験（`test_gui_ledger_a` の `"/native/src/"`）も、この規則で関係づく。
- **CMake の変更**（基点の中身と比べられるときだけ。比べられなければ全体）: `native/tests/CMakeLists.txt` は、試験ごとの文（`genko_test(...)`、その試験を第 1 引数に取る文）だけが変わったなら、その試験。共通の部分（補助の実行物・定義・関数・if・foreach）が変わったら全体。`native/src/<名前>/CMakeLists.txt` は、ソースの並び（`*.cpp` の語）だけが変わったなら、足されたソースの閉包。ほかが変わったら全体。
- **全体へ拡大するもの**（従来どおり）: `native/cmake/`・`native/third_party/`・`native/CMakeLists.txt`・`CMakePresets.json`・`vcpkg.json`・`.gitattributes`、`tools/migration/`・`tools/acceptance/`、`native/wasm-host/`・`native/package/` などの分類の無い native の変更、消えた・名前の変わったソース、native の外の分類の無い変更、差分なし・基点不明。`tools/ci/`・`native.yml` は「CI自身の変更」（非契約の全試験）。関係する試験が全試験に及ぶ差分も全体として扱う。
- **確かめ方**: `tools/ci/validation_closure_check.py --build build/linux-release`（手で走らせる。計画を作るのには要らない）。静的ライブラリの Release のビルドでは、実行物に入るのは参照が届くオブジェクトだけなので、実行物ごとに nm の記号で「入っているオブジェクト」を調べ、そのソースが全部その試験の閉包にあること（閉包 ⊇ リンクされたもの）を確かめる。生成物（埋め込みデータ・qrc・moc）は、上の規則が説明できることを確かめる。漏れが 1 つでもあれば不合格。実行物より新しいオブジェクトがあるビルド（作り直しの途中）も不合格にする。**工程出口で Release のビルドができたら走らせ、合格を HANDOFF に書く。不合格なら、規則を直すまで計画に `--all-tests` を付け、ASan も全通常試験で流す（構成は増えない）。**
  - 2026-11-14 の実測（main da6acebd に書体の作業中を足した Release のビルド）: 試験 273 本と実行物 5 本の 278 本、漏れ 0。リンクされた製品のソース 延べ 110,761 に対し閉包 延べ 120,934（余分 9.2%）。ライブラリの単位では閉包とリンクがほぼ同じ集合になる。
- **どれだけ減るか**（2026-11-14、ASan の 1 本ごとの秒は凍結 a72bd1e8 の出口の記録、全 267 本で 17,927 秒）: 代表の 1 ファイルの変更で ASan に入るのは（前は全 267 本だったもの）、api 85 本・5,859 秒、service 160 本・8,434 秒、studio 161 本・8,434 秒、`resources/studio` の表 160 本・8,434 秒、tests/support 175 本・14,909 秒、formats 181 本・13,714 秒、試験のデータ（`data/type/plain_lines.json`）19 本・1,197 秒、名前に系統の無い render 256 本・17,801 秒、storage 263 本、core 266 本。名前の規則のまま変わらないもの: app 79 本・4,027 秒、`raster_ops.cpp` 45 本・2,446 秒、`tones.cpp` 40 本・2,159 秒、`ops_3d.cpp` 39 本・2,137 秒。**機能の工程の差分はどれも共通モデル（`core/model.hpp`）・本の読み書き（`storage/reader.cpp`・`writer.cpp`）・描画のコードに触れるので、ASan も全通常試験になる**（フキダシ・ネーム・コマ割り・書体の 4 つの差分で確かめた）。減るのは、service だけ・api だけ・formats だけ・試験のデータだけのような狭い差分。重いフィルター・スマートオブジェクト・高精度の試験は描画と本の読み書きのコードを丸ごと持つので、リンクの関係からは外せない。外すには、試験ごとに実際に通ったコードの記録（カバレッジ）が要る（未着手。利用者に相談する）。
- **残る危険**（どれも、Release は毎回全試験を流すので、見落とすのは「選ばれなかった試験が ASan でだけ落ちる」場合に限られる）: 部品から組み立てたパスで読むデータ（文字列に名前も `/` も出ない）、CMake の書き方が変わって埋め込み先を取り違える場合、行頭に無い定義・マクロで作った定義・ヘッダー無しの `extern` 宣言（突き合わせの道具が見つける）、ASan の共有ライブラリだけで走る静的初期化（そのライブラリを読む選ばれた試験と常時保護の対照が同じ初期化を通る）。

## 3. CIの起動と重複の除去

- featureブランチのpushは保管だけ。native/integration/mainへのpushまたはそれらへのPRで実行する。feature pushとPRの二重起動を避ける。integration→mainの公開候補PRは別の境界として検査する。ブランチ名だけで検査を省略しない。PRの全jobはmerge treeではなくhead SHAそのものをcheckoutする。実workflow定義がhead側と異なれば停止しrebaseする。統合後にtarget由来の製品差分があれば別の全体候補として検査する。今回の基盤変更は独立レビュー後、工程出口PRの全構成で受入し、全treeが同一の統合pushは証跡を再使用する。
- 通常PRはV1、統合pushはV2。`native/milestone/<工程名>`からのPRはV3の工程出口（`--phase milestone` の 4 構成: Release は全通常試験、ASan と Windows は差分に関係する試験）。最終配布候補の全 5 構成（`--phase final`）は手元で計画を作る（無効にしてある native.yml の手動起動には `final` の選択肢が無い）。現mainにはnative workflowがないため、default branchへの反映前はこのPR経路を使う。mainへの反映後は手動起動 `phase=milestone` も可能。新cron・observer・夜間自動実行は作らない。
- 差分から `validation_policy.py` が対象・構成・除外理由をJSONへ記録する（`reasons`: 構成ごとの範囲と理由、`classification`: 変更パスごとの扱い）。統合pushの基点は同じbranchの前回合格runの祖先SHA、または全構成合格の工程出口PR（祖先SHAとworkflow/script/CMake/preset/依存制御が同一）とし、前のCI失敗分を直前commit差分だけで落とさない。通常PR・別の制御・不明な履歴は再利用しない。候補決定時だけ公開CI metadataを読み、新認証/permission/cronは追加しない。基点の合格を照合できなければ全体へ拡大する。合格工程出口PRと全treeまで同一のマージpushはrun URLを記録して再buildしない。差分取得失敗は停止、それ以外の差分空/分類不能は全体へ拡大する。変更されたテストを除外しない。
- Python pytestはsrc/tests/依存設定/公開op schema/Python workflow変更時だけLinux/Windowsで実行する。C++専用変更のたびにPython全pytestを実行しない。ただし必要なPython参照契約はnative試験に残す。
- CTestをverboseで一度実行し、Windowsでは同じCTest登録から`-o file,txt`で全選択QtTestのrawログも出す。その一回のrawログでQt Totals/FAIL/SKIPを検査する。ログ取得のための再実行と、失敗後の全exe再実行を廃止する。
- 0試験、欠落/重複Totals、未完走、未知SKIP、sanitizer診断、構成artifact欠落を成功にしない。Releaseの既知SKIPだけを許可し、Debug/ASan側の同slot PASSを総合auditで要求する。
- 途中で止まった回・落ちた回は `--resume` で続きから走らせる（利用者決定 2026-11-14。機械の再起動で数時間の全体実行を失うのを止める）。同じ出力先に残った前回の `result.json` と、計画・ソース・実行物・構成が全部一致するときだけ、合格済みの試験を引き継ぎ、残りと落ちた試験だけを走らせる。一つでも違えば引き継がずに最初から走らせ、理由を `resume.reason` に残す。コードを直せばソースが変わるので、全部走らせ直しになる。
  - 引き継ぐのは、CTestの結果行が一つだけでPassed、その試験自身の行にQt失敗・sanitizer診断・未知SKIPがなく、Totalsが一つだけの試験。結果行の無い試験（走っている途中で止まったもの）は走らせ直す。止まる前に自分の失敗を出していた試験は、落ちた試験として残す。
  - 続けないで最初から走らせる場合: 前回のログの指紋が記録と合わない、ログがこのビルドのものでない、どの試験のものか分からない診断の行がある、合格済みの試験が後の回でも走っている、全部合格しているのに最後の回のCTestが「走り切った」と書いていない。合格で終わっている記録に `--resume` を付けても書き換えない。
  - 各回のrawログは消さない（1回目 `ctest-output.log`、続きの回 `ctest-output.attempt-N.log`。CLI23は `cli23.log` と前の回の `cli23.attempt-N.log`）。`result.json` は各回の時刻・並列数・機械・落ちた試験・終わらなかった試験・不合格の理由を `attempts` に持つ。`--pair` は全部の回のログを読み直して照合し、記録にないログが出力先にあれば不合格にする。最後の回のログには、その回に始めた試験が全部合格で走り切ったというCTestの行が要る。
  - 前の回で落ちて後の回で合格した試験は `audit.rerun_after_failure` に、落ちた回のログ名と理由が残る。`--pair` の結果の `resumed` にも、回の数・その試験・前の回の不合格の理由が出る。原因（機械のメモリ不足など、コードの外にあるもの）を調べてHANDOFFに書く。原因を書けない落ち方を、走らせ直しの合格で済ませない。
  - 出力先は一つの実行だけが使う（出力先の `.running` を握り、ctestと試験のプロセスにも渡す）。道具だけが落ちてctestや試験が残っているあいだは、次の回は始まらない。
  - 実行物の指紋には、試験・genko・排他の補助に加えて、ビルドの中の共有ライブラリを入れる（sanitizerの構成では製品のコードがそこにある）。
  - Windows構成は続きから走らせない（QtTestのファイル記録を回ごとに残していない）。CIのjobは機械ごと作り直すので、続きからは手元の実行だけ。

## 4. 実行入口

計画を作る（baseは依存境界を固定した実コミット。分類は今の木のソースを読むので、head を checkout した所で作る。`--head` が checkout と違えば全体へ拡大する。数秒）:

```sh
python3 tools/ci/validation_policy.py --base <基点SHA> --phase development --output build/validation-plan.json
```

`--phase` は `development`（V1）、`integration`（V2）、`milestone`（V3 の工程出口: linux-release 全通常試験、linux-asan と Windows は差分に関係する試験、Debug なし）、`final`（最終配布候補: 全通常試験を Debug を含む全構成で）。`--all-tests` は、分類によらずその段階の全構成で全通常試験にする（§2a の突き合わせが不合格のあいだ）。工程出口と最終配布候補:

```sh
python3 tools/ci/validation_policy.py --base <前の工程の統合SHA> --phase milestone --output build/validation-plan.json
python3 tools/ci/validation_policy.py --base <基点SHA> --phase final --output build/validation-plan.json
```

工程出口で Release のビルドができたら、分類の規則をそのビルドと突き合わせる（§2a。約 20 秒、nm が要る。漏れがあれば終了コード 1）:

```sh
python3 tools/ci/validation_closure_check.py --build build/linux-release --output build/validation/closure-check.json
```

GPU と CPU の表示の一致（render:GPU-CPU-parity、PC5 H-a）を Xvfb と Mesa の OpenGL で比べる試験 `test_gui_canvas_gpu`（`genko_xvfb_test`、ラベル gui・gpu、Linux だけ）は、計画に `--gpu` を付けたときだけ入る（アプリの試験を流す Linux の構成に）。機械に xvfb-run と Mesa が要る。xvfb-run が無ければ SKIP ではなく失敗する。CI（native.yml）はまだ `--gpu` を付けない。offscreen の `test_gui_canvas` は、OpenGL の無い所で CPU に戻る道を毎回確かめる。一致の許し（利用者の決定 2026-10-29）: 再標本しない表示は全画素 1/255 以内。回転と画面に合わせた倍率は、2/255 を超える画素が 2‰ まで、差は 12/255 まで（用紙の縁と影の 14 画素は比べない）。

Python参照の契約試験は、参照Pythonを環境変数で渡して実行する: `GENKO_PYREF`（Pillow 12.3.0・numpy 2.4.6・psd-tools 1.10.9）と、MCP・HTTPの契約（`test_contract_mcp`）用の `GENKO_MCP_PYTHON`（`tools/migration/mcp_reference_requirements.txt` のvenv。作り方はARCHITECTURE.md §9）。未設定だと契約試験はSKIPになり、auditが不合格にする。

対応するCMake presetをconfigureし、JSONにある構成だけ実行する:

```sh
python3 tools/ci/validation_run.py --plan build/validation-plan.json --preset linux-debug --build build/linux-debug --output build/validation/linux-debug
```

止まった回・落ちた回の続きから走らせるときは、同じ `--output` のまま `--resume` を付ける（前回の記録が無ければ普通に走る。出力先を消さない）:

```sh
python3 tools/ci/validation_run.py --plan build/validation-plan.json --preset linux-asan --build build/linux-asan --output build/validation/linux-asan --resume
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
