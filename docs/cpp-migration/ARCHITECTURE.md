# C++版の構成と実装の約束（ARCHITECTURE）

版: 1.0（M0）/ 対象: `native/` 以下の製品コードと、その作業者。仕様の正本は `SPEC.md`、受入は `ACCEPTANCE.md`、保存形式は `schema-v4.md`。

## 1. 置き場所と依存の向き

```
native/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  cmake/
  src/
    core/     原稿モデル（不変・構造共有）、ID・単位、JSON、Actor・権限、CommandBus、op 登録
    storage/  旧形式の読込（v1/v2/v3）、v4 読書き、素材ストア、排他、トランザクション、履歴、復旧、GC、変換器
    render/   描線・ブラシ・タイル・レイヤー合成・トーン・効果線・3D・色管理（画面と出力で共通）
    text/     縦書き・横書き、禁則、ルビ、傍点、字体処理（FreeType+HarfBuzz）、フキダシ
    formats/  PNG/TIFF/PDF/PSD/EPUB/Kindle/strip/webtoon/SNS/pack/layers/timelapse/animation、PSD/PSB/ABR 読込
    studio/   企画・脚本・ネーム・依頼・候補・採用・領域・仕上げ・承認・相談・作業一覧
    api/      CLI（genko）、HTTP、MCP、設定変換、エラー整形
    app/      Qt Widgets 画面、ペン、IME、案内（GUI だけが Qt Widgets に依存）
  tests/      unit/  contract/  e2e/  perf/  data/（固定の小さな試験資産）
tools/migration/   台帳生成・固定原稿生成・Python 参照比較（開発用。製品に入れない）
```

依存は `core ← storage ← render ← text ← formats ← studio ← api ← app` の一方向。`core`・`storage` は **QtCore だけ**に依存してよい（QCryptographicHash・QRandomGenerator・QStringConverter 等。QtGui/Widgets は不可）。`render` 以降は QtGui の QImage/QPainter を使ってよいが、ディスプレイ接続を要求しない（headless で動く）。GUI 部品は `app` だけ。

変更は必ず `CommandBus` を通す。app・api が原稿 JSON やファイルを直接書き換える経路を作らない。

## 2. 原稿モデル（core）

- `Document`（Python の `Episode`）は値型。ページは `std::shared_ptr<Page>` の配列で、**参照数による書込み時複製（COW）**をする: `Document` のコピーはページのポインターを写すだけ。変更は `doc.edit_page(i)` で行い、そのページが他と共有されていれば（use_count > 1）複製してから書き換える。1 回の op の束は `Document` のコピー（作業用）に対して行い、成功したらそれを新しい状態にする。前の `Document` がそのまま Undo 用の旧状態になる（深いコピーをしない）。作業用のコピーを他のスレッドへ渡さない。
- 層の筆跡は `std::shared_ptr<const StrokeList>`（不変の `std::shared_ptr<const Stroke>` の配列）。筆跡を変える op は新しい配列を作る（変わらない線のポインターは共有）。ラスターの PNG バイト列も `std::shared_ptr<const Bytes>`。
- 層は読み込んだ筆跡ブロブの参照（`strokes_ref`）とラスターの参照（`raster_ref`）を覚え、内容が変わっていなければ保存で再利用する（Python の `blobcache` と同じ効果）。
- 筆跡は開くときに全部読んでよい（ページ単位で並列に復号）。PERF-A を満たせない場合に限り、ページの必要時読込に切り替える。
- Python の型付きフィールドは C++ でも型付きで持つ。Python で自由形式の dict（`panel`、`plan`、`studio`、`style`、`effects`、`prims`、`rulers`、`tone`、`fill`、`adjust`、`effect`、`screen`、`source`、`finish`、`bible.characters`、`tickets`、`nombre`、`brush.custom`、パッチの属性等）は `core::Json`（= `nlohmann::ordered_json`）で持ち、キー順を保つ。
- 未知のキーは各階層の `extra`（`Json` object）に保持し、保存で書き戻す（Python の `episode.extra`・`page.extra` と同じ）。
- 台詞は `Document::story` だけに持つ（Python の `page.texts` は `story` のページ別の見え方。二重に持たない）。
- 座標は mm の `double`、筆圧・回転も `double`。`float` へ縮めない。
- ID は Python と同じ 12 桁の 16 進（`uuid4().hex[:12]` 相当）。ページ ID は `pg_` 接頭辞。乱数は暗号論的乱数源（QRandomGenerator::system）から取る。
- `PageSpec`・用紙プリセット・綴じ・ページの左右・基本枠の計算は `models.py` と同じ値を返す（単体試験で固定値を照合）。
- Python の `round(x, n)` は `core::py_round`（`std::to_chars` の固定小数で丸めて読み戻す。Python と同じ結果）、`random.Random` は `core::PyRandom`（MT19937 と Python と同じ seed 展開・`random()`・`uniform()`）で再現する。

## 3. JSON・数値・文字列

- `core::Json` = `nlohmann::ordered_json`。整数は int64 で保持し、リビジョン・ID・件数を double へ変換しない。
- 書き出す JSON は UTF-8、LF。`project.json` はインデント 2・キーは保持順（Python の `json.dumps(indent=2, ensure_ascii=False)` と同等の見た目）。素材として保存する JSON（筆跡ブロブ・op 記録）は**正準形**（キーを辞書順、区切り `,` `:`、空白なし、`ensure_ascii=false`）。数値の書式はロケール非依存。
- NaN・±Infinity を書かない。旧形式の読込で見つけた場合は null に置き換えて JSON ポインターを変換報告に記録し、利用者の明示（`--accept-repairs`）がない限り v4 を書かない。
- 文字列は UTF-8 の `std::string`。Qt との境界でだけ `QString` に変換する。パスは `std::filesystem::path`。UTF-8 との変換は `core/paths.hpp` の関数だけで行う（Windows で ANSI コードページを経由しない）。
- パスを文字列にするとき（エラー文言・報告・GC の `removed` 等）は、どの OS でも区切りを `/` にする（`generic_u8string`）。Python は Windows で `\` を使うが、出力を OS で変えないためにそろえる（Windows は `/` のパスも受け付ける）。
- CLI の標準出力・標準エラーはどの OS でも UTF-8・LF（Windows でもバイナリモードにし、CRLF へ変換しない）。
- Windows の実行ファイルは長いパス対応を宣言する（`longPathAware`。MAX_PATH を超えるパスは Windows の LongPathsEnabled 設定が有効なときに使える。Python と同じ条件）。

## 4. 決定性

- 浮動小数の縮約（FMA）と fast-math を禁止（`cmake/GenkoFlags.cmake`）。同じ入力から Windows・Linux で同じ原寸画素を出す。
- 乱数を使う描画（効果線・手描き風の揺れ・紙質・ノイズ）は、入力から決まる seed と自前の決定的な乱数器を使う。`std::uniform_*_distribution` は実装依存なので使わない。Python と同じ乱数列が必要な箇所は Mersenne Twister（MT19937）と Python の `random` と同じ変換を実装し、試験で数列を照合する。
- 並列化しても結果の画素が変わらないこと（タイル順・合成順を固定）。

## 4a. 画素処理の土台（render）

- 継承する描画（線・ブラシ・コマ枠・合成・マスク・トーン・フィルター等）は、現行版が使う **Pillow 12.3.0 の C 実装 `libImaging`** を `native/third_party/pillow/` に同梱してそのまま使う（MIT-CMU ライセンス。変更点は同フォルダーの `GENKO_CHANGES.md` に記録）。Python 版の `ImageDraw`・`ImageChops`・`Image` の Python 層（`line(joint="curve")`、`alpha_composite`、`resize` の既定補間など）は C++ に移植する。これにより現行の原稿が**同じ画素**で描かれる。
- `libImaging` の Python 依存部（`Python.h`、Arrow、コーデック）は使わない。小さな互換ヘッダーで置き換える。PNG の読み書きは libpng、JPEG は libjpeg-turbo、TIFF は libtiff（いずれも画素は同じ。PNG のバイト列は Pillow と異なってよい）。
- numpy を使う現行処理は C++ のループへ移す。numpy と同じ演算順・型変換（float64 計算→`astype(uint8)` の切捨て等）にする。
- 画面用の部分描画: `render_page` は出力範囲（原点と大きさ、画素）を受け取り、全体を描いて切り出した結果と**同じ画素**を返す（タイル描画・拡大表示の精細化に使う）。
- 未移植の描画要素（その工程で移植しない機能）を含むページは、黙って省かず `NotYetPorted` 例外で止める。

## 5. CommandBus と op

- `CommandBus::apply(const Document&, const Json& ops, const Actor&, ApplyOptions)` は**全部成功したときだけ**新しい `Document` を返す。途中で失敗したら元の `Document` は変わらず、エラーに `ops[i] <op名>: <理由>` を返す（Python の `ApplyError` と同じ位置情報）。
- op は名前で登録する（`OpRegistry`）。各 op は引数検証・権限判定・ページロック・`strict_gates`・承認ゲートを Python と同じ順で行う。人間専用の op を AI に開かない（`can_approve`：`human:` 接頭辞、または旧来の無名呼出し `genko`）。
- `dry_run` は検証と結果の予測だけで保存しない。`for_pages` の展開、`area` の解決は Python と同じ規則。
- op の一覧（引数の説明文を含む）は `docs/ops.schema.json` と `genko schema` の出力を一致させる。

## 6. 保存（v4）と履歴

形式は `schema-v4.md`。要点だけ再掲する。

- 書込みは `project.lock` の OS 排他（POSIX は `flock`、Windows は先頭 1 バイトの `LockFileEx`。現行 Python 版と同じ排他）で直列化する。
- `project.json` の `revision` は**単調増加の確定世代**。Undo・Redo・復旧の採用も新しい世代を割り当て、過去の値を再利用しない。`base_revision` 照合はこの世代で行う。
- 1 回の保存は `prepare` 記録 → 素材の確定 → `project.json` の原子的置換 → 監査記録 → `commit` 記録の順。各境界での強制終了から、起動時・保存失敗時・次の書込み前に整合点へ戻せる。
- 旧形式（v1/v2/v3）は**別の `.genko` へ変換**する。原本は書き換えない。旧履歴は保全し、旧スナップショット→v4 状態の対応表で、移行境界を越えた Undo/Redo を v4 の新しいトランザクションとして行う。

## 7. スレッド

- GUI（QWidget・QPixmap）はメインスレッドだけ。描画・保存・出力・フィルターはワーカーで、不変の `Document` と独立した画像バッファを扱い、結果は世代 ID 付きでキュー経由で返す。古い世代の結果は捨てる。
- 原稿の変更は 1 本の実行列で順序付ける。ワーカーから生ポインタでモデルを書き換えない。

## 8. エラー

- `core::Error`（`code` 文字列と英語の `message`、必要なら JSON ポインター `path`）を例外として投げる。CLI/HTTP/MCP は `{"ok": false, "error": …, "code": …}` に整形する。日本語の利用者向け文言は `app`・`api` の文言表（Python の `app/wording.py` 相当）で付ける。
- 保存・入出力の失敗は握りつぶさない。失敗を成功扱いで返さない。

## 9. 試験の約束

- 試験は CTest に登録する（`genko_test()`）。ラベル: `unit`（速い単体）、`contract`（Python 版の挙動・形式との照合）、`e2e`（CLI/GUI/API の通し）、`perf`（性能。通常の CI では実行しない）、`gui`（offscreen の画面試験）。
- Python 参照: 開発用コンテナの `/opt/pyref/bin/python` と、リポジトリの `src/genko`（`PYTHONPATH=/src/src`）で現行版の挙動・出力を作り、C++ 版と照合してよい。製品・配布物は Python を必要としない。
- 描画の照合は、同じ DPI の Python 参照画像に対し、全画素の平均絶対差 ≤ 2/255 かつ 99% 以上の画素の差 ≤ 32/255 を基本とする。超える場合は理由（Pillow とのアンチエイリアス差など）を試験に書き、差分画像を残し、台帳の備考に記録する。機能の欠落を許容差で隠さない。
- 原稿の固定試験資産は `FIXTURES.md` に従って生成し、ハッシュを固定する。本番原稿（`/home/hermes/manga` 等）を使わない。

## 10. 書式

- C++20。`#include` は `"core/…"` のようにモジュール名から。名前空間は `genko::core`、`genko::storage` 等。
- 警告ゼロ（CI は `-Werror` / `/WX`）。RAII、所有権が明確なスマートポインタ。生の `new`/`delete` を使わない。
- コードのコメントは英語（既存 Python と同じ）。文書・コミットメッセージ・PR 本文・利用者向け文言は日本語。
