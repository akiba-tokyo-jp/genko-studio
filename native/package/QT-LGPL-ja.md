# Qt のライセンスと差し替え方

Genko Studio は画面の部品に **Qt 6.11.2** を使っています。Qt は The Qt Company の公式バイナリ（download.qt.io）を変更せずに使い、**GNU LGPL v3（LGPL-3.0-only）** の条件で配布しています。

- ライセンス文: `licenses/qt/LGPL-3.0.txt` と、LGPL v3 が条件として含む `licenses/qt/GPL-3.0.txt`。
- Qt はプログラムに静的に組み込まず、別のファイルとして入れています（動的リンク）。
  - Linux: `lib/libQt6*.so.6`、`plugins/`
  - Windows: `bin\Qt6*.dll` と、Qt のプラグインのフォルダー
- 入っている Qt のモジュール: Core、Gui、Widgets、Network、Concurrent、Svg、PrintSupport、OpenGL、OpenGLWidgets と、画面・画像形式などのプラグイン、そのプラグインが使う Qt のライブラリ（DBus、XcbQpa、WaylandClient、EglFS など。`lib/` にあるもの）。Linux では、Qt に付いている ICU（ライセンス文は `licenses/icu/LICENSE`）も入っています。Qt の GPL 専用のモジュールは使っていません。

## Qt のソースコード

Qt 6.11.2 の対応するソースコードは、次から入手できます。

- https://download.qt.io/official_releases/qt/6.11/6.11.2/single/ （`qt-everywhere-src-6.11.2.tar.xz`）
- https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/ （モジュールごと）

上の場所で入手できなくなったときの受け取り方は、配布元の案内に従ってください。

## Qt を差し替える

利用者は、自分で手を入れた Qt や、別にビルドした互換の Qt 6.11 と差し替えて Genko Studio を動かせます。

- Linux
  - `lib/` の同じ名前のファイル（`libQt6Core.so.6` など）を置き換えます。
  - または、環境変数 `LD_LIBRARY_PATH` で別の Qt のフォルダーを先に指定します。プログラムは自分のフォルダーの `../lib` を、`LD_LIBRARY_PATH` の次に探します（RUNPATH）。
  - プラグインは `bin/qt.conf` が指す `plugins/` から読みます。
- Windows
  - `bin\` の同じ名前の DLL とプラグインを置き換えます。

## Qt が中に含む第三者のソフトウェア

Qt のライブラリには、Qt 自身が取り込んだ第三者のソフトウェア（PCRE2、HarfBuzz、FreeType、libpng、double-conversion、MD4C など。ビルドによって違います）が含まれます。それぞれのライセンスと帰属表示は、Qt の文書「Licenses Used in Qt」（https://doc.qt.io/qt-6/licenses-used-in-qt.html）にあります。

この配布物には、まだその全文を入れていません（既知の制限。最終の配布の前に入れます）。
