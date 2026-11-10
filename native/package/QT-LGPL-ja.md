# Qt のライセンスと差し替え方

Genko Studio は画面の部品に **Qt 6.11.2** を使っています。Qt は The Qt Company の公式バイナリ（download.qt.io）を変更せずに使い、**GNU LGPL v3（LGPL-3.0-only）** の条件で配布しています。

- ライセンス文: `licenses/qt/LGPL-3.0.txt` と、LGPL v3 が条件として含む `licenses/qt/GPL-3.0.txt`。
- Qt はプログラムに静的に組み込まず、別のファイルとして入れています（動的リンク）。
  - Linux: `lib/libQt6*.so.6`、`plugins/`
  - Windows: `bin\Qt6*.dll` と、Qt のプラグインのフォルダー
- 入っている Qt のモジュール: Core、Gui、Widgets、Network、Concurrent、Svg、PrintSupport、OpenGL、OpenGLWidgets と、画面・画像形式などのプラグイン、そのプラグインが使う Qt のライブラリ（DBus、XcbQpa、WaylandClient、EglFS など。`lib/` にあるもの）。Linux では、Qt に付いている ICU（ライセンス文は `licenses/icu/LICENSE`）も入っています。Qt の GPL 専用のモジュールは使っていません。

## Qt のソースコード

Qt 6.11.2 の対応するソースコードは、次から入手できます。

- https://download.qt.io/archive/qt/6.11/6.11.2/single/ （`qt-everywhere-src-6.11.2.tar.xz`）
- https://download.qt.io/archive/qt/6.11/6.11.2/submodules/ （モジュールごと）

（download.qt.io の `official_releases/` には新しい版だけが残り、古い版は `archive/` に移ります。上は移った後の場所です）

## Qt を差し替える

利用者は、自分で手を入れた Qt や、別にビルドした互換の Qt 6.11 と差し替えて Genko Studio を動かせます。

- Linux
  - `lib/` の同じ名前のファイル（`libQt6Core.so.6` など）を置き換えます。
  - または、環境変数 `LD_LIBRARY_PATH` で別の Qt のフォルダーを先に指定します。プログラムは自分のフォルダーの `../lib` を、`LD_LIBRARY_PATH` の次に探します（RUNPATH）。
  - プラグインは `bin/qt.conf` が指す `plugins/` から読みます。
- Windows
  - `bin\` の同じ名前の DLL とプラグインを置き換えます。

## Qt が中に含む第三者のソフトウェア

Qt のライブラリとプラグインには、Qt 自身が取り込んだ第三者のソフトウェア（PCRE2、HarfBuzz、libpng、libjpeg-turbo、libtiff、libwebp、double-conversion、MD4C など）が含まれます。それぞれの名前・版・ライセンス・著作権表示・ライセンス文の全文を `licenses/qt-3rdparty/NOTICES.txt` に入れています（Qt 6.11.2 のソースの帰属表示から。Qt の文書「Licenses Used in Qt」https://doc.qt.io/qt-6/licenses-used-in-qt.html も参照）。
