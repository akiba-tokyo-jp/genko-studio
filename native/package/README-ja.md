# Genko Studio（C++ 版）について

マンガの原稿を、ネームから入稿まで作るアプリです。この版は Python を使わずに動きます（Python・uv・開発用の道具は要りません）。

## 動くコンピューター

| OS | 状態 |
|---|---|
| Ubuntu 24.04 LTS（x86_64）のデスクトップ | 確かめた環境です。`.tar.gz` を展開して `install.sh` で入れます。 |
| Windows 10 / 11（x64） | ZIP を用意します。**Windows の実機での受け入れはまだです**（配布の前に実機で確かめます）。 |

Linux では、次のライブラリはコンピューターに入っているものを使います（配布物には入れていません）。Ubuntu 24.04 のデスクトップには最初から入っています。起動したときに「ライブラリが見つからない」と出たら、管理者に次のパッケージを入れてもらってください。

```
sudo apt install libgl1 libegl1 libopengl0 libfontconfig1 libfreetype6 libglib2.0-0t64 libdbus-1-3 \
  libx11-6 libx11-xcb1 libxcb1 libxkbcommon0 libxkbcommon-x11-0 libxcb-glx0 libxcb-randr0 libxcb-render0 \
  libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libpng16-16t64 libjpeg-turbo8 libtiff6 \
  libwebp7 libwebpdemux2 libwebpmux3 liblcms2-2 libharfbuzz0b libgssapi-krb5-2 zlib1g \
  libopenexr-3-1-30 libimath-3-1-29t64 libavif16 libaom3 libzstd1
```

Qt（画面の部品）と、Qt が X11 の画面で使う小さなライブラリ（libxcb-cursor など）、カメラ RAW を読む LibRaw（`lib/libraw_r.so.25`。ライセンスと差し替え方は `LIBRAW-LGPL-ja.md`）は配布物の中に入っています。

## 署名について

この配布物には**電子署名がありません**。

- Windows では「Windows によって PC が保護されました」（SmartScreen）や「発行元を確認できません」と出ることがあります。警告が出ないことを前提にしないでください。
- 開く前に、配布元のページに載っている SHA-256 の値と、手元のファイルの値が同じかを確かめてください。
  - Linux: `sha256sum genko-studio-<版>-linux-x86_64.tar.gz`
  - Windows（PowerShell）: `Get-FileHash genko-studio-<版>-windows-x64.zip -Algorithm SHA256`
- 値が違うとき、配布元がわからないときは開かないでください。
- OS の保護を切ったり、管理者の権限を与えたりする必要はありません。

## 入れ方・外し方

### Linux

```
tar xf genko-studio-<版>-linux-x86_64.tar.gz
cd genko-studio-<版>-linux-x86_64
./install.sh
```

- `~/.local/opt/genko-studio-<版>` に入り、`~/.local/bin/genko-studio` とアプリのメニューの項目ができます。管理者の権限は要りません。
- 起動: メニューの「Genko Studio」、または `genko-studio`（`~/.local/bin` が PATH にないときは `~/.local/opt/genko-studio-<版>/bin/genko`）。
- 入れずに、展開したフォルダーの `bin/genko` をそのまま起動することもできます。
- 外す: `~/.local/opt/genko-studio-<版>/uninstall.sh`。外すのはプログラムのフォルダーとメニューの項目・起動のリンクだけです。**原稿・設定・旧版（Python 版）は消しません。**
- 新しい版にする: 新しい版の `install.sh` で入れます（版ごとのフォルダーに入り、メニューには版の付いた項目が並びます。`genko-studio` の起動のリンクは新しい版を指します）。新しい版で困らなければ、古い版の `uninstall.sh` で古い版を外します（原稿・設定はそのまま使えます）。

### この版をふだん使う版にする（Linux）

`install.sh` は、旧版（Python 版）の起動のしかたを変えません。この版をふだん使う版にするときは、次を自分で行います（どれも旧版を消しません。戻すときは、足したものを消すか元に戻します）。

1. 先に一度、この版のアプリ（`genko-studio`）を起動して、旧版の設定を引き継ぎます（上の「設定と、旧版（Python 版）の設定」）。
2. アプリの起動: メニューの「Genko Studio <版>」か `genko-studio` を使います。
3. 端末で `genko` と打ったときもこの版にしたいときは、シェルの設定（`~/.bashrc` など）に次の 1 行を足します。旧版の `genko` はそのまま残ります。
   ```
   alias genko=genko-studio
   ```
4. AI（MCP）のクライアント: クライアントの設定の `mcpServers` の `genko` の `command` を、この版のプログラム `~/.local/opt/genko-studio-<版>/bin/genko` に書き換えます（`args` の `mcp` などはそのまま）。新しい版を入れたら、版の部分も書き換えます。
5. サイトの「この絵柄を使う」（`genko://` のリンク）: この版で `genko-studio register-links` を一度実行するか、アプリの絵柄の画面の「サイトのボタンで開けるようにする」を押します（旧版の登録と置き換わります。旧版に戻すときは、旧版で登録し直します）。

### Windows

ZIP を好きなフォルダーに展開して、`bin\genko.exe` を起動します。外すときは、そのフォルダーを消します（原稿と設定は残ります）。

## 設定と、旧版（Python 版）の設定

- この版は、自分の設定を旧版と別の場所に持ちます。
  - Linux: `~/.config/genko-studio`
  - Windows: `%APPDATA%\GenkoStudio`
- 置くもの: 環境設定、最近の原稿、自作のブラシ、色、ポーズ、素材、オートアクション、AI 接続の鍵、プラグイン。
- 旧版と並べて使えます。旧版の設定（Linux `~/.config/genko`・`~/.config/Genko`、Windows `%APPDATA%\genko` とレジストリ）は、この版では**読むだけで、変えません**。
- 初めて起動したときに旧版の設定が見つかると、「引き継ぎますか？」と一度だけ聞きます。
  - 「はい」のときだけ写します。
  - 写さないもの: 復旧用のコピー、キャッシュ（絵の縮小・書体の一覧）、ロック。
  - この版にすでにあるものの上には写しません（写さなかったものは知らせます）。
  - リンクは、ファイルへのリンクなら指す先の中身を写し、フォルダーへのリンクは写さずに知らせます（この版の変更が旧版のファイルに及ばないように）。
  - 写せないものがあったときは知らせて、次の起動でもう一度聞きます。
- コマンドラインや MCP（AI）だけで使う場合も、先に一度アプリ（`genko-studio`）を起動して引き継いでください。引き継ぐ前は、コマンドラインがそのことを知らせます（AI 接続の鍵も、そのときに写せます）。
- 設定の場所を変えたいときは、環境変数 `GENKO_CONFIG_DIR` にフォルダーを指定します（そのフォルダーを使い、引き継ぎは聞きません）。

## 原稿と、旧版に戻すとき

- この版が保存する原稿は新しい形式（v4）です。
- 旧版の原稿を開くと、元の原稿は書き換えずに、別の原稿として新しい形式に写します。
- **旧版は新しい形式の原稿を開けません。** 旧版に戻すときは、写す前の元の原稿を旧版で開いてください。
  - この版で加えた変更は、元の原稿には入っていません。
  - 必要なら、この版で PNG・PSD などに書き出して、旧版で取り込んでください。
- この版の原稿に、旧版の `genko gc`・`genko undo` を使わないでください。
- 旧版はアンインストールしなくても、そのまま使えます。

## 知られている制限

- Windows の実機での受け入れ（入れ方・起動・ペンタブレット・画面の倍率・genko:// のリンク）は、まだ済んでいません。
- 色を面ごとに分けて持つ TIFF（PlanarConfiguration 2）のうち、1 画素の深さが 8 ビットの倍数でないものは読めません。読めないことを知らせ、別の絵を描くことはしません。
- 旧版（Pillow）だけが読み書きする画像の形式（PPM などの netpbm、ICO・CUR、icns、QOI、DDS、SGI、JPEG 2000、AVIF）は、この版では読めません。画像を置くときに断り、旧版で置いた原稿のその画像は描かずに知らせます。`genko render --out` でも、GIF・ICO・PPM・TGA・JPEG 2000 などには書けません（PNG・JPEG・TIFF・PDF・WebP・BMP・DIB に書けます）。
- 旧版の設定の引き継ぎは、アプリの初回の問いからだけ行えます（コマンドラインだけで引き継ぐ方法はありません）。
- Lab のページの文字の色は、PSD に RGB の値で書きます。
- 書き出した PSD が Photoshop で同じに見えることは、まだ確かめていません。
- Qt が中に含む第三者のソフトウェアの帰属表示は、配布物に全文を入れていません。Qt の文書「Licenses Used in Qt」（6.11）を見てください（QT-LGPL-ja.md）。
- MCP の `inspect` の説明文にあるプラグインの置き場所の例は、旧版の場所のままです。実際の場所は `inspect`（target: plugins）が返す `folder` です。
- アプリのアイコンはまだありません（メニューの項目は名前だけで出ます）。
- `genko://` のリンクを開くアプリとして登録すると、旧版の登録と置き換わります（アプリの中で選んだときだけ登録します。`uninstall.sh` は、外すプログラムを指している登録だけを外します）。

## 中に入っているもの

- プログラム `bin/genko`。アプリ、コマンドライン、MCP サーバーがこの一つに入っています。
- Qt のライブラリとプラグイン（`lib/`、`plugins/`）。
- `share/doc/genko-studio/` に、次のものが入っています。
  - この説明と `QT-LGPL-ja.md`（Qt のライセンスと差し替え方）
  - `THIRD_PARTY.md`（第三者ライブラリの一覧）
  - `licenses/`（ライセンス文）
  - `sbom.spdx.json`（配布物の中身の一覧、SPDX 2.3）
