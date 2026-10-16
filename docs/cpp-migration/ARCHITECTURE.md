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
    studio/   スタジオの op 以外の部品：同席・道具の記録・作業の確保・依頼フォルダー・画像生成の道具の一覧・入力スキーマ・
              原稿から読む state（スタジオの op ＝企画・脚本・ネーム・依頼・候補・採用・領域・仕上げ・承認・相談は core の op）
    service/  スタジオの道具（StudioService・HumanService：エージェントと人が呼ぶ 41 の道具と人だけの承認・正式な書き出し）と
              その部品（依頼と下絵の案内・取り込み・アタリ・点検・確認ページ・バックグラウンドの仕事・高解像度化・評価）、
              利用者向けの言葉（wording）。`genko studio`・MCP・HTTP・アプリが同じものを呼ぶ
    api/      CLI（genko）、HTTP、MCP、設定変換、エラー整形
    app/      Qt Widgets 画面、ペン、IME、案内（GUI だけが Qt Widgets に依存）。スタジオの画面（承認箱・工程・コマの詳細・資料・
              AI と作る・絵柄を選ぶ・genko:// のリンク）も。承認箱の中身（review_model）と AI と作る の Qt を使わない部分（ai_link・
              client_config）は画面から分けて Python と照合する。AI への返事の下書き（reply_box）は v4 の原稿の
              studio/reply_drafts/ に置く（原稿の変更・履歴ではない。schema-v4「返事の下書き」）
  tests/      unit/  contract/  e2e/  perf/  data/（固定の小さな試験資産）
tools/migration/   台帳生成・固定原稿生成・Python 参照比較（開発用。製品に入れない）
```

依存は `core ← storage ← render ← text ← formats ← service ← api ← app` と `core ← storage ← studio ← service` の一方向。スタジオの op は、ほかの op と同じく core に置き（`core/ops_studio.cpp`）、絵の画素を読む部分（素材の画像の大きさと、配置の前に絵全体を読めるかの確かめ）だけを render が `core::StudioHooks` で差し込む（`render::ops_registry()`。core は render に依存しない）。`studio`（`genko_studio`）は core と storage だけに依存し、render・formats には依存しない。`service`（`genko_service`）は formats・render・studio・storage・core の上にあり、道具を Python のメソッドと同じ引数で受け（`StudioService::call`：位置引数とキーワード引数の JSON。束ねられないものは Python の TypeError）、`ToolResult::to_dict()` の JSON で答える（MCP・HTTP・`genko studio` はこれを送るだけ）。Python のキーワード専用の `confine=`（`import_image`・`import_name` が --root の外を読むか）は `call` の引数ではなく `StudioService::set_confine` で、コマンドラインだけが使う（MCP・HTTP からは渡せない）。v1〜v3 の本には、書き込む道具を始める前に（`Session`・HumanService・adopt-drafts も）`needs_migration` で断る。formats は service を知らないので、アプリの正式な書き出しは `formats::RunOptions::official_export` に service が HumanService.export を差し込む（`service::run_export`）。`core`・`storage` は **QtCore だけ**に依存してよい（QCryptographicHash・QRandomGenerator・QStringConverter 等。QtGui/Widgets は不可）。`render` 以降は QtGui の QImage/QPainter を使ってよいが、ディスプレイ接続を要求しない（headless で動く）。GUI 部品は `app` だけ。

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
- CMYK・Lab のページ（color-mode@1、SPEC.md COLOR-02）: 色モードの形と GKIR の符号は `core/ink`、Little CMS の変換（RGB の色をインキへ当てる規則、インキを sRGB へ、ページどうし、取り込む絵）は `render/ink`、合成は `render/ink_canvas`（double の値。CMYK はインキ 0..1、Lab は符号化を 0..1 にしたもの。保存の標本 ÷ 最大値と同じ）。`render_page` は CMYK・Lab のページを `InkCanvas` で合成して画面の sRGB を返し、`RenderOptions::inks` でインキも返す（書き出しが使う）。8 bit で描く既存の要素（線・塗り・文字・フキダシ・トーン・効果線・枠線・ノンブル）は手を加えず、層の RGBA の絵を色の規則でインキに当てるか、黒と白の 2 枚に描いて被覆と色を取り出して重ねる。RGB のページの描画（Python と同じ画素）は通らない。1 メガ画素を超える範囲は高精度のページと同じく 512 px のタイルで描く。
- Little CMS の変換は、プロファイル（の参照）・意図・向き・形式ごとに一度だけ作り、一画素のキャッシュなしで描画の各スレッドから使う。Little CMS の版が違えば変換の結果が 1 違うことがある（照合は許容差 1）。
- 色モデル（color-model@1、SPEC.md COLOR-03）: 色空間ごとの違い（名前・GKIR の番号・版の数・明るさか・HDR を持てるか・アルファ）は `core/ink` の `SpaceTraits` の表にまとめ、`InkPixels` は版の数を表から（マルチチャンネルは自分の数）、標本を 8・16 bit は整数、32 bit は float で持つ（`value`・`set` が 0..1 の値で読み書きする）。グレーは明るさ 1 版の `InkCanvas`（インキの k = 1 − 明るさ。Little CMS のグレーの float も明るさ）。プロファイル付きの RGB のページは `render/color_canvas` の `RgbSpace`（ページのプロファイルの行列とトーンカーブ。パラメトリックのカーブは double で解析的に、ほかは Little CMS の float）で `ColorCanvas` が直線の光で合成し、8 bit で描く要素の sRGB の色はページの意図・黒点補正で入れ、表示は相対的な色域を維持・黒点補正で sRGB へ。`RgbSpace` が sRGB のときは前と同じコード（同じバイト）。8 bit の sRGB の絵（紙・8 bit の層・`draw_8bit`）は色ごとに一度だけ変換する（24 bit の値で覚え、行の新しい色をまとめて Little CMS に渡す）。プロファイル付きのページの 8 bit の画素がページの値かどうかは `core::holds_page_values` の一つの規則で決め、変換（`render/ops_ink`）・描画（`render/page`）・結合（`composite_color_layers`）・焼き込み（`raster::bake_vectors`）・書き出し（`page_layers`）が同じ答えを使う。新しい色を 8 bit の道具で描く op は `render/ops_registry` の `RgbLending` がページを sRGB のページとして貸し、戻すときに触れなかった層・画素・色の元の値を残す。持ち上げた画素（`selection::lift`）はプロファイルの鍵（`rgb_profile`、保存しない）を持ち、op の後に `color_edit::bake_page_values` が同じプロファイルの画素へ描き込むか sRGB にする。内蔵のプロファイル（`core/color_model` の表、`core/data/profiles/*.icc` をビルドに埋め込み、表の sha256 と試験で照合する）は `tools/migration/make_profiles.cpp` で作った CC0 のもの（`coated-cmyk` は PhotoCraft の CC0 の ICC）。書き出しの値（PSD・TIFF・PDF の 16・32 bit）は `RenderOptions::precise` の GKCR f32（層の値に、上に 8 bit で描いたものの色をページの空間にして入れたもの）から作る。
- 表示だけの色（PC1b、SPEC.md COLOR-03）: 色の校正・校正の設定・色域外警告の式は `render/proof`（上流 proof_sim.rs・gamut.rs から。画面の sRGB の 8 bit を入力に、上流と同じ二段の校正の変換・版の分解・色覚の行列。ΔE76 の格子は上流の Clut と同じ四面体で読み、入力はページの値（`render_page` の `precise` とインキ。無ければ画面の sRGB）。画面のタイル（`app/tiles`）は、ページを描いた絵に校正を掛けたもの（スポイトと `compose` が読む）と、色域外の印・モニターのプロファイル（`app/display_color`）を重ねた画面用の絵を分けて持つ（後者は印かモニターの変換があるときだけ）。どれも原稿・履歴・書き出しに入らない。文字のガンマだけは本の値（`text_gamma`）で、`render_page` がその間のスレッドに置き（`render/text` の `TextGamma`）、字を置くところ（`paint_text`）が使う。
- チャンネル（channels@1、SPEC.md COLOR-03）: モデルは `core::Channel`（`core/model`。規則と JSON は `core/channels`）、画素は GKIR 版 2 の Mask（`SpaceTraits` の 1 版・アルファ無し）。描画の側は `render/channels`: 特色の刷り込みとパネルの見え方（`RenderOptions` の `spots`・`planes`・`overlays`）は `render_page` の最後に画素ごとに掛け（ページの座標で双一次に読むので部分と全体が同じ画素。素材の復号は素材のバイトごとに数枚を、その精度のまま（8 bit は 1 画素 1 バイト）合わせて 192 MiB まで覚える。GKIR は `core::read_ink_raster_rows` で 1 行ずつ読み、全体を展開しない。パネルの縮小図も 1 行ずつ縮めて読み、覚えない）、画素の op の `channel` は `render::ops_registry` の包みで扱う（チャンネルは 200 dpi の見えない画素レイヤーとして貸して、op が変えた画素だけをチャンネルの解像度へ戻す。色の版は op の後でほかの版を戻す）。チャンネルの op は `render/ops_channels`、選択範囲の `{"channel"}` は `render/selection`。
- 特別な色モード（PC1d、color-model@1、SPEC.md COLOR-03）: 形と規則（インデックスの `ColorTable`、2 階調の `BitmapLook`、ダブルトーンの `DuotoneInk` と曲線 `curve_at`・刷り `duotone_print`、マルチチャンネルの `Plate` と刷り `print_plates`）は `core/color_model`・`core/ink`。インデックスは RGB の系（`RgbMode::table`）、2 階調・ダブルトーンはグレーの `InkCanvas`、マルチチャンネルは N 版（1〜32）の `InkCanvas`（版ごとに分離できる描画モードの式。`SpaceTraits` の Multichannel は版の数をページの `plates` から）。表示の写像（インデックスは最後の合成を表の最も近い色へ、2 階調は 50% で 2 値、ダブルトーン・マルチチャンネルは白の上に刷る）は画素ごとなので部分と全体が同じ画素（`render/page`・`render/ink_canvas`）。マルチチャンネルの要素の色・変換は `render/ink`（`from` のモードの規則）。量子化（パレット・ディザ・2 値化。上流 algo/src/quantize.rs をそのまま移したもの）は `render/quantize`（f32 の式と pow・cbrt は `render/libm_float` で決定的）。変換の op は `render/ops_ink`（`to_indexed`・`set_color_table`・`to_bitmap`・`set_duotone`・`set_color_mode` の multichannel。統合は `render/ops_channels` の `flatten_page` が `merge_visible` の flatten を走らせる。特色の合成と同じ）、色の版の削除は `render/ops_channels`。op の後の寄せ（2 階調の 2 値・インデックスの表の色・16/32 bit の RGB のページの新しい 8 bit の画素を GKCR へ正確に広げる）は `render/ops_registry` の包みの `keep_page_modes`（op の前のページと比べ、変わった層だけ）。書き出しは `formats/export`（`native_mode`・`indexed_picture`）・`formats/psd_export`（色モード 0・2・7）・`formats/pdf`（`/Indexed`・1 bpc）・`formats/pillow_save`（TIFF のパレット）。画面は `app/mode_dialogs`（インデックス・カラーテーブル・2 階調・ダブルトーン）とステータスのポインターの下の値（`render::pixel_at`、`genko inspect --pixel` と同じ）。

## 5. CommandBus と op

- `CommandBus::apply(const Document&, const Json& ops, const Actor&, ApplyOptions)` は**全部成功したときだけ**新しい `Document` を返す。途中で失敗したら元の `Document` は変わらず、エラーに `ops[i] <op名>: <理由>` を返す（Python の `ApplyError` と同じ位置情報）。
- op は名前で登録する（`OpRegistry`）。各 op は引数検証・権限判定・ページロック・`strict_gates`・承認ゲートを Python と同じ順で行う。人間専用の op を AI に開かない（`can_approve`：`human:` 接頭辞、または旧来の無名呼出し `genko`）。
- `dry_run` は検証と結果の予測だけで保存しない。`for_pages` の展開、`area` の解決は Python と同じ規則。
- op の一覧（引数の説明文を含む）は `docs/ops.schema.json` と `genko schema` の出力を一致させる。
- スタジオの op（M5：`set_studio`・`approve`・`adopt_candidate`・`place_asset` など 46 種）は、ほかの op と同じく core の op として `core/ops_studio.cpp` に登録する（Python の `apply_ops` が `studio_ops` へ渡すのと同じ位置で、同じ検査順）。素材の画像の大きさと、配置の前の絵の確かめ（絵全体を展開する。C++ だけ）は画素の読込が要るので、`render::ops_registry()` が `core::StudioHooks`（`render::asset_picture_size`・`render::check_asset_picture`）を差し込んで登録し直す（core だけのバスは大きさが要る所で `not_yet_ported`、絵は確かめない）。`studio/`（`genko_studio`）は op 以外のスタジオの部品：同席（`studio/presence.json`）・道具の記録（`studio/logs/tools.jsonl`）・作業の確保（`studio/claims/`）・依頼フォルダー（`studio/requests/`）・画像生成の道具の一覧（設定フォルダーの `tools.json`）・入力スキーマと jsonschema_lite・原稿から読む state。配置画像の描画（`_placed_raster`・`_finish_placed`）は `render/placed.cpp` と `render/screentone.cpp`。

## 6. 保存（v4）と履歴

形式は `schema-v4.md`。要点だけ再掲する。

- 書込みは `project.lock` の OS 排他（POSIX は `flock`、Windows は先頭 1 バイトの `LockFileEx`。現行 Python 版と同じ排他）で直列化する。
- `project.json` の `revision` は**単調増加の確定世代**。Undo・Redo・復旧の採用も新しい世代を割り当て、過去の値を再利用しない。`base_revision` 照合はこの世代で行う。
- 1 回の保存は `prepare` 記録 → 素材の確定 → `project.json` の原子的置換 → 監査記録 → `commit` 記録の順。各境界での強制終了から、起動時・保存失敗時・次の書込み前に整合点へ戻せる。
- 旧形式（v1/v2/v3）は**別の `.genko` へ変換**する。原本は書き換えない。旧履歴は保全し、旧スナップショット→v4 状態の対応表で、移行境界を越えた Undo/Redo を v4 の新しいトランザクションとして行う。

## 7. スレッド

- GUI（QWidget・QPixmap）はメインスレッドだけ。描画・保存・出力・フィルターはワーカーで、不変の `Document` と独立した画像バッファを扱い、結果は世代 ID 付きでキュー経由で返す。古い世代の結果は捨てる。
- 原稿の変更は 1 本の実行列で順序付ける。ワーカーから生ポインタでモデルを書き換えない。
- AI と作る の「テスト接続」は Genko の MCP サーバーを QProcess で起動し、メインスレッドのイベントで読み書きする（GUI を止めない。時間切れでサーバーを止める）。画面の人の操作（承認・差し戻し・返事・採用）は、ウィンドウのセッション（`human:<名前>`）で op として行い、承認はすぐ保存する（Python の apply_and_commit）。正式な書き出しは `service::run_export`（HumanService.export）。
- スタジオのバックグラウンドの仕事（`service/jobs`：`export`・`export_proof`・`upscale` の `background`）は、仕事ごとのスレッドで、同じ root と actor の StudioService を新しく作って動かす（呼んだ側の StudioService を共有しない）。本への書き込みは本のロックで順序付く。スレッドはすべて登録して join し、デタッチしない（登録の場所はスレッドを作る前に取るので、作った後で登録に失敗しない）。前面（`genko studio`、MCP・HTTP のサーバ）は終わる前に `jobs::wait_all()` を呼び、仕事が終わるまで待つ（Python はプロセスと一緒に仕事が消える。SPEC COMP-01a）。呼び忘れても、QCoreApplication の終わり（`qAddPostRoutine`。静的な物を壊す前）に待つ。スレッドを始められない仕事は、始めた鼓動を止めて記録を failed にし、Python の RuntimeError を投げる。試験用の id の数え方（`ScopedIdSource`）と時刻（`ScopedClock`）は、待っている間に仕事のスレッドから読まれてよいが、仕事の途中で変えない。

## 8. エラー

- `core::Error`（`code` 文字列と英語の `message`、必要なら JSON ポインター `path`）を例外として投げる。CLI/HTTP/MCP は `{"ok": false, "error": …, "code": …}` に整形する。日本語の利用者向け文言は `service/wording.cpp` の文言表（Python の `app/wording.py` 相当。スタジオの道具の `fail()` とアプリが同じ表を使う）で付ける。
- スタジオの道具が Python で投げるもの（ApplyError・ValueError・TypeError・FileNotFoundError・JSONDecodeError と、Python がつかまえずトレースバックになるもの）は、同じ型の C++ の例外（`core::ApplyError`・`core::PyValueError`・`core::PyTypeError`・`core::Error("not_found"/"json")`・`core::PyUncaught(型名)`）として投げる。`genko studio` は Python と同じく `{"ok": false, "error": str(exc)}` を出し、`code` を足す。Python ではトレースバックで止まるものは `{"ok": false, "error": "<型>: <言葉>", "code": "python_error"}`。
- 保存・入出力の失敗は握りつぶさない。失敗を成功扱いで返さない。

## 9. 試験の約束

- 試験は CTest に登録する（`genko_test()`）。ラベル: `unit`（速い単体）、`contract`（Python 版の挙動・形式との照合）、`e2e`（CLI/GUI/API の通し）、`perf`（性能。通常の CI では実行しない）、`gui`（offscreen の画面試験）。
- Python 参照: 開発用コンテナの `/opt/pyref/bin/python` と、リポジトリの `src/genko`（`PYTHONPATH=/src/src`）で現行版の挙動・出力を作り、C++ 版と照合してよい。製品・配布物は Python を必要としない。
- MCP・HTTP の契約試験（`test_contract_mcp`）の Python 参照は別の venv: 環境変数 `GENKO_MCP_PYTHON`（未設定なら `/opt/pyref-mcp/bin/python`）。中身は `tools/migration/mcp_reference_requirements.txt`（`uv.lock` の `mcp` extra と、参照と同じ Pillow 12.3.0・numpy 2.4.6）で、`python3.12 -m venv /opt/pyref-mcp && /opt/pyref-mcp/bin/pip install -r tools/migration/mcp_reference_requirements.txt` で作る。ほかの契約試験の参照 Python と同じく、どちらも無いと QSKIP になり、検証（`validation_run.py`）はその SKIP を予期しない SKIP として不合格にする。だから検証の環境では `GENKO_PYREF` と並べて必ず用意する。
- 描画の照合は、同じ DPI の Python 参照画像に対し、全画素の平均絶対差 ≤ 2/255 かつ 99% 以上の画素の差 ≤ 32/255 を基本とする。超える場合は理由（Pillow とのアンチエイリアス差など）を試験に書き、差分画像を残し、台帳の備考に記録する。機能の欠落を許容差で隠さない。
- 原稿の固定試験資産は `FIXTURES.md` に従って生成し、ハッシュを固定する。本番原稿（`/home/hermes/manga` 等）を使わない。

## 10. 書式

- C++20。`#include` は `"core/…"` のようにモジュール名から。名前空間は `genko::core`、`genko::storage` 等。
- 警告ゼロ（CI は `-Werror` / `/WX`）。RAII、所有権が明確なスマートポインタ。生の `new`/`delete` を使わない。
- コードのコメントは英語（既存 Python と同じ）。文書・コミットメッセージ・PR 本文・利用者向け文言は日本語。
