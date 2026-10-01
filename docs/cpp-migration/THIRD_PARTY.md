# 第三者ライブラリとライセンス（SBOM の元）

版: 1.0（M0）。C++ 版の配布物に入る、またはビルドに使うもの。M6 で配布物から SBOM（SPDX JSON）を自動生成し、この表と照合する。

| 名前 | 版（Linux CI / Windows vcpkg） | ライセンス | 用途 | 配布 |
|---|---|---|---|---|
| Qt 6（Core, Gui, Widgets, Network, Concurrent, Svg, PrintSupport, 画像形式プラグイン） | 6.11.2（公式バイナリ） | LGPL-3.0-only | GUI、画像、ネットワーク、印刷 | 動的リンク。Qt のライセンス文、対応ソースの入手先、差し替え方法を同梱 |
| nlohmann/json | 3.11.3 | MIT | JSON | ヘッダーのみ |
| zlib | 1.3 / 1.3.1 | Zlib | 圧縮（PNG、PDF、PSD） | 静的 |
| libpng | 1.6.43 / 1.6.x | libpng-2.0 | PNG | 静的 |
| libjpeg-turbo | 2.1.5 / 3.x | IJG AND BSD-3-Clause AND Zlib | JPEG | 静的 |
| libtiff | 4.5.1 / 4.7.x | libtiff | TIFF（CMYK、1bit） | 静的 |
| FreeType | 2.13.2 / 2.13.x | FTL（BSD 型） | 字形 | 静的 |
| HarfBuzz | 8.3.0 / 10.x | MIT | 字形の配置（縦書きの字形置換・OpenType 機能） | 静的 |
| libwebp | 1.3.2 / 1.5.x | BSD-3-Clause | WebP（タイムラプス・アニメーション） | 静的 |
| Little CMS 2 | 2.14 / 2.16 | MIT | 色変換（CMYK、ICC） | 静的 |
| Pillow の `libImaging`（C 実装のみ） | 12.3.0（sdist sha256 3b8182a7…7cce） | MIT-CMU（HPND） | 継承描画の画素処理（線・合成・変換・フィルター）。現行版と同じ画素を出すため | 静的。`native/third_party/pillow/`、ライセンス文と変更記録を同梱 |

## 同梱フォント・アイコン（現行版から継承）

| 名前 | ライセンス | 置き場所 |
|---|---|---|
| Dela Gothic One, Reggae One, Yomogi, Zen Kaku Gothic New, Zen Maru Gothic, Zen Old Mincho, IBM Plex Sans JP | SIL OFL 1.1 | `src/genko/fonts/`（ライセンス文は `fonts/licenses/`） |
| Lucide アイコン | ISC | `src/genko/app/lucide/` |

フォントは配布物に入れるが、PSD 等の出力ファイルへ埋め込まない（PSD-01）。

## 使わないもの

- Qt の GPL 専用モジュール（Qt HTTP Server、Qt Charts の GPL 版など）。HTTP API は Qt Network の `QTcpServer` 上に自前で実装する。
- FFmpeg を同梱しない。MP4 の書き出しは、利用者のコンピューターにある `ffmpeg` を現行と同じく外部コマンドとして使う（無ければ理由を表示）。WebP・GIF・PNG は自前で書く。
- 署名証明書・商用 Qt・有料 codec。

## 開発だけで使うもの（配布しない）

CMake、Ninja、ccache、aqtinstall、vcpkg、Python 3（参照比較・台帳生成・固定原稿の生成）、Pillow・numpy・psd-tools（参照比較）、Xvfb（画面試験）。
