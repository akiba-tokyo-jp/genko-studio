# LibRaw のライセンスと差し替え方

Genko Studio はカメラ RAW の読み込みと現像に **LibRaw 0.22.2**（LibRaw LLC、https://www.libraw.org）を使っています。LibRaw は **GNU LGPL v2.1（LGPL-2.1-only）と CDDL 1.0 のどちらかを選べる**ライセンスで、Genko Studio は **LGPL v2.1** の条件で配布しています（LibRaw の中の DCB・FBDD・X3F の部分は BSD 型のライセンスです）。

- ライセンス文: `licenses/libraw/COPYRIGHT`（LibRaw の著作権表示とライセンスの選び方）、`licenses/libraw/LICENSE.LGPL`、`licenses/libraw/LICENSE.CDDL`（どれも LibRaw 0.22.2 の配布物のもの）。
- LibRaw はプログラムに静的に組み込まず、別のファイルとして入れています（動的リンク）。
  - Linux: `lib/libraw_r.so.25`（libraw.org の LibRaw 0.22.2 のソースを、OpenMP・JasPer・Little CMS を使わずに組んだもの。JPEG と zlib はコンピューターのものを使います）
  - Windows: `bin\raw_r.dll`（vcpkg の libraw 0.22.2。中に JasPer 4.2.9（JasPer License 2.0、`licenses/jasper.copyright`）と、Little CMS・zlib・libjpeg-turbo を含みます）
- LibRaw に手は入れていません。

## LibRaw のソースコード

LibRaw 0.22.2 の対応するソースコードは、次から入手できます。

- https://www.libraw.org/download （`LibRaw-0.22.2.tar.gz`、sha256 `de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa`）

Linux の配布物の LibRaw は、Genko のソースの `tools/ci/build_libraw.sh` のとおりに組んだものです（`configure --enable-shared --disable-static --disable-openmp --disable-jasper --disable-lcms --disable-examples --enable-zlib --enable-jpeg`）。

## LibRaw を差し替える

利用者は、自分で手を入れた LibRaw や、別に組んだ互換の LibRaw 0.22（同じ `libraw_r` の ABI のもの）と差し替えて Genko Studio を動かせます。

- Linux
  - `lib/libraw_r.so.25` を置き換えます。
  - または、環境変数 `LD_LIBRARY_PATH` で別の LibRaw のフォルダーを先に指定します。プログラムは自分のフォルダーの `../lib` を、`LD_LIBRARY_PATH` の次に探します（RUNPATH）。
- Windows
  - `bin\raw_r.dll` を置き換えます。

`genko --version` の `build` の `libraw` に、動いている LibRaw の版が出ます。
