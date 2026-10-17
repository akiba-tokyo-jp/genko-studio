# 第三者ライブラリとライセンス（SBOM の元）

版: 1.1（M6）。C++ 版の配布物に入る、またはビルドに使うもの。配布物の SBOM（SPDX 2.3 JSON、`share/doc/genko-studio/sbom.spdx.json`）は `cmake --install` が入れたものから作り（`native/package/sbom.cmake`）、試験 `test_package` がこの表と照合する。

「配布」の列の Linux の「コンピューターのもの」は、Ubuntu 24.04 のパッケージのライブラリに動的にリンクし、配布物に入れないもの（SBOM では DEPENDS_ON、comment「the computer's own library」）。Ubuntu の libtiff は JBIG-KIT（GPL-2.0-or-later）とリンクしているため、システムのライブラリを配布物に入れない。

| 名前 | 版（Linux CI / Windows vcpkg） | ライセンス | 用途 | 配布 |
|---|---|---|---|---|
| Qt 6（Core, Gui, Widgets, Network, Concurrent, Svg, PrintSupport, OpenGL, OpenGLWidgets, 画面・画像形式などのプラグイン） | 6.11.2（公式バイナリ） | LGPL-3.0-only | GUI、画像、ネットワーク、印刷、GPU のキャンバス表示 | 動的リンク。`lib/`・`plugins/`（Windows は `bin\`）。Qt のライセンス文（`licenses/qt/`）、対応ソースの入手先、差し替え方法（`QT-LGPL-ja.md`）を同梱。Qt が中に含む第三者のソフトウェアの帰属表示とライセンス文の全文は `licenses/qt-3rdparty/NOTICES.txt` |
| ICU（Qt の公式バイナリに付くもの） | 73.2 / なし | Unicode-DFS-2016 AND ICU AND BSD-3-Clause AND NAIST-2003（辞書・時刻帯のデータの条件を含む。全文は `licenses/icu/LICENSE`） | Qt の文字処理（Linux） | Linux: 動的リンク、`lib/libicu*.so.73` を同梱 |
| libxcb の補助ライブラリ（libxcb-cursor・icccm・image・keysyms・render-util・util） | 0.1.4・0.4.1・0.4.0・0.4.0・0.3.9・0.4.0（Ubuntu 24.04）/ なし | MIT | Qt の X11（xcb）の画面プラグインが使う。デスクトップに無いことがある | Linux: 動的リンク、`lib/` に同梱。著作権表示は `licenses/xcb-util/` |
| Microsoft Visual C++ の再頒布可能ランタイム | なし / Visual Studio 2022 | LicenseRef-MSVC-Redistributable | Windows の実行時ライブラリ | Windows: `bin\` に同梱（Windows の実機での受け入れは未了） |
| nlohmann/json | 3.11.3 | MIT | JSON | ヘッダーのみ（プログラムに組み込まれる。ライセンス文は `licenses/nlohmann-json/`） |
| zlib | 1.3 / 1.3.1 | Zlib | 圧縮（PNG、PDF、PSD） | Windows: 静的。Linux: コンピューターのもの |
| libpng | 1.6.43 / 1.6.x | libpng-2.0 | PNG | Windows: 静的。Linux: コンピューターのもの |
| libjpeg-turbo | 2.1.5 / 3.x | IJG AND BSD-3-Clause AND Zlib | JPEG | Windows: 静的。Linux: コンピューターのもの |
| libtiff | 4.5.1 / 4.7.x | libtiff | TIFF の書き出し（CMYK、1bit）と読み込み（M6: Pillow と同じモード・画素） | Windows: 静的。Linux: コンピューターのもの |
| FreeType | 2.14.3（署名検証済みの固定archive＋メモリ予算接続override、ソースは両OS共通） | FTL（BSD 型） | 字形。参照Pillowと同じrasterizerで画素互換を保つ | 静的。`native/third_party/freetype/`に原archive・SHA256・来歴・ライセンス |
| HarfBuzz | 8.3.0 / 10.x | MIT | 字形の配置（縦書きの字形置換・OpenType 機能） | Windows: 静的。Linux: コンピューターのもの |
| libwebp（libwebp・libwebpdemux・libwebpmux） | 1.3.2 / 1.5.x | BSD-3-Clause | WebP の書き出し（タイムラプス・アニメーション）と読み込み（M6: Pillow と同じ画素。アニメーションは最初のコマ） | Windows: 静的。Linux: コンピューターのもの |
| Little CMS 2 | 2.14 / 2.16 | MIT | 色変換（CMYK、ICC） | Windows: 静的。Linux: コンピューターのもの |
| Pillow の `libImaging`（C 実装のみ） | 12.3.0（sdist sha256 3b8182a7…7cce） | MIT-CMU（HPND） | 継承描画の画素処理（線・合成・変換・フィルター）。現行版と同じ画素を出すため | 静的。`native/third_party/pillow/`、ライセンス文と変更記録を同梱 |
| Arm optimized-routines（https://github.com/ARM-software/optimized-routines）の `math/sinf.c`・`cosf.c`・`sincosf.h`・`sincosf_data.c`・`powf.c`・`powf_log2_data.c`・`exp2f_data.c` | v25.01（コミット 3752b981a6f3b89b5468476a87568a29fd9e0822。これらのファイルは v25.07 も同じ）。著作権表示は各ファイルの通り（Copyright (c) 2018-2024, Arm Limited: sinf.c・cosf.c・sincosf.h／2018-2019: sincosf_data.c／2017-2024: powf.c／2017-2019: powf_log2_data.c／2017-2018: exp2f_data.c） | MIT OR Apache-2.0 WITH LLVM-exception（MIT で利用） | numpy の float32 sin・cos・power が参照環境で使う C ライブラリの関数を、どの OS でも同じビットで計算する（`native/src/render/libm_float.cpp` に移植。参照環境の C ライブラリ glibc 2.39 の結果とビット単位で照合する。glibc の同梱版は LGPL のため使わない） | 静的（ソースに移植）。`native/third_party/optimized-routines/LICENSE-MIT`（同リポジトリの `LICENSE` の MIT の部分そのまま） |
| PhotoCraft（露光量の式とパラメータの境界 `crates/compose/src/adjust.rs`・`crates/engine/src/adjust_params.rs`。PC1 の色モデル・カラー設定・色の校正と色域外警告・チャンネル・量子化と特別な色モード。移した部分と行は `native/third_party/photocraft/NOTICE.md`） | v0.2.0（コミット ad863217386440ca968fccc9bfff65ba24e61142） | MIT | 露光量の補正（`native/src/core/exposure.cpp`）、色モデル・校正・チャンネル・量子化（`native/src/core/color_model.cpp`・`core/ink.cpp`・`render/proof.cpp`・`render/quantize.cpp`・`render/ops_ink.cpp`・`render/ops_channels.cpp` ほか）に移植 | 静的（ソースに移植）。`native/third_party/photocraft/LICENSE-MIT` と `NOTICE.md`、配布物の `licenses/photocraft/` |
| CPython の `re`（`Lib/re/_parser.py`・`_compiler.py`・`_casefix.py`・`_constants.py`、`Modules/_sre/sre_lib.h`・`sre.c`、`Modules/unicodedata.c` の名前の引き方）を C++ に移植したもの、Unicode 15.0.0 の文字の性質と名前の表（参照の Python 3.12 から `tools/migration/pyre_tables.py` で作る。別名は Unicode の NameAliases.txt 15.0.0） | CPython v3.12.3 | PSF-2.0（CNRI の Python 1.6.1 の許諾を含む）、sre は Secret Labs AB の著作権で CNRI's Python 1.6 license により再配布、Unicode の表は Unicode License v3（Unicode-3.0） | 台詞の一括置換・検索の正規表現を Python と同じ結果にする（`native/src/core/pyre*.cpp`） | 静的。`native/third_party/cpython/`に `LICENSE`（Python）・`LICENSE-SRE`（Secret Labs AB の表示）・`LICENSE-Unicode`（全文）と来歴。移植したソースの冒頭にも表示 |

## 同梱データ（プログラムに組み込む）

| 名前 | 出典 | ライセンス | 置き場所 |
|---|---|---|---|
| PhotoCraft Coated CMYK（合成 ICC プロファイル、sha256 f7ba9320…a54cc。内蔵プロファイル `coated-cmyk`） | PhotoCraft v0.2.0 `crates/cms/profiles/photocraft-coated-cmyk.icc`（pin ad86321） | CC0-1.0 | `native/src/core/data/profiles/coated-cmyk.icc`（プログラムに組み込む。色モデル color-model@1） |
| 内蔵の RGB・グレーの ICC プロファイル（display-p3・adobe-rgb-compat・prophoto-compat・linear-srgb・rec2020・gray-gamma-2.2・sgray） | Genko が `tools/migration/make_profiles.cpp` で Little CMS により作ったもの（原色・白色点・トーンカーブは公開の規格の値。選び方と ID・説明は PhotoCraft v0.2.0 `crates/cms/src/builtin.rs`、MIT） | CC0-1.0 | `native/src/core/data/profiles/`（プログラムに組み込む。sha256 は `core/color_model.cpp` と試験 `test_color_model` で照合） |

## 同梱フォント・アイコン（現行版から継承）

| 名前 | ライセンス | 置き場所 |
|---|---|---|
| Dela Gothic One, Reggae One, Yomogi, Zen Kaku Gothic New, Zen Maru Gothic, Zen Old Mincho, IBM Plex Sans JP | SIL OFL 1.1（OFL-1.1） | `src/genko/fonts/`（ライセンス文は `fonts/licenses/`） |
| Lucide アイコン | ISC | `src/genko/app/lucide/` |

フォントは配布物に入れるが、PSD 等の出力ファイルへ埋め込まない（PSD-01）。

## 試験データ（配布物に入れない）

| 名前 | 出典 | ライセンス | 置き場所 |
|---|---|---|---|
| PhotoCraft Coated CMYK（合成 ICC プロファイル、sha256 f7ba9320…a54cc） | PhotoCraft v0.2.0 `crates/cms/profiles/photocraft-coated-cmyk.icc`（pin ad86321） | CC0-1.0 | `native/tests/data/colour/`（CMYK 変換・色校正の契約試験） |

## 使わないもの

- Qt の GPL 専用モジュール（Qt HTTP Server、Qt Charts の GPL 版など）。HTTP API は Qt Network の `QTcpServer` 上に自前で実装する。
- FFmpeg を同梱しない。MP4 の書き出しは、利用者のコンピューターにある `ffmpeg` を現行と同じく外部コマンドとして使う（無ければ理由を表示）。WebP・GIF・PNG は自前で書く。
- 署名証明書・商用 Qt・有料 codec。

## 開発だけで使うもの（配布しない）

CMake、Ninja、ccache、aqtinstall、vcpkg、Python 3（参照比較・台帳生成・固定原稿の生成）、Pillow・numpy・psd-tools（参照比較）、Xvfb（画面試験）。
