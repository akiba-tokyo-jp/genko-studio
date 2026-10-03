# Genko での変更

## 元のファイル（`libImaging/`）

Pillow 12.3.0 の sdist を基準に、`Imaging.h` と `Storage.c` に画像確保の予算フック、`Geometry.c` に確保失敗時の所有権と固定小数点経路の安全検査を追加した。

- `Geometry.c`: nearest補助表の確保失敗時、借用された出力画像をC側では解放しない（所有者のC++が一度だけ解放する）。16.16係数・補正切片・量子化後の全加算範囲がintに収まる場合だけ既存の固定小数点経路を使い、それ以外は既存の浮動経路へ進む。正常な大きい切片の90度回転を一律拒否しない。

- `Imaging.h`: 各画像に予算状態への参照と予約バイト数を追加。
- `Storage.c`: 行・画素領域の確保前に `linesize × ysize` バイトを予約し、最終 `ImagingDelete` で解放する。C内部のChops・blur・resizeの一時画像も同じ経路で計数する。
- `compat/imaging_glue.c/.h`: スレッド単位のスコープ付き予算。再帰は共有し、予算状態は画像の最終解放まで参照計数で保持する。限界を超える確保はNULLと予算エラーを返し、C++側でOpErrorへ変換する。確保後の検査ではない。
- 選択領域処理は480,000,000バイトのlive画素領域上限を使う（従来のL-mask画素上限を実バイト計数へ接続）。RGBの内部4バイト/画素も数える。アリーナの解放済みキャッシュ、行テーブル、PNG圧縮バイトや補間係数等の補助領域はこの画素領域予算とは別であり、プロセス全RAMのhard limitと呼ばない。受入は1 CPU・2 GiBの実cgroupでOOMも測る。

## 足したファイル

- `CMakeLists.txt`: 静的ライブラリ `pillow_imaging`。画素処理のファイルだけをビルドする（「ビルドしないもの」を参照）。
- `compat/Python.h`: libImaging が `ImPlatform.h` から読む `Python.h` の代わり（include パスで先に見つかる）。宣言に要る型（`Py_ssize_t`、中身のない `PyObject`）、ビルドするファイルが呼ぶ Python の関数、`PyMutex` だけを持つ。
  - `Py_GIL_DISABLED` を定義する（Pillow の free-threaded ビルドと同じ）。これで Imaging.h・Storage.c の `MUTEX_LOCK` が働き、共有のメモリアリーナ（`ImagingDefaultArena`）と画像の参照数がロックで守られる。Genko は複数のスレッドで描くため。画素の計算は変わらない。
- `compat/imaging_glue.c`・`compat/imaging_glue.h`: `_imaging.c` と Python が持つ関数の代わり。
  - `ImagingError_MemoryError`・`_Mismatch`・`_ModeError`・`_ValueError`: `_imaging.c` は Python の例外を立てて NULL を返す。ここではエラーの種類とメッセージをスレッドごとに残して NULL を返す（C++ 側は `genko_imaging_error_kind`・`genko_imaging_error_message` で読む）。
  - `ImagingSectionEnter`・`ImagingSectionLeave`: `_imaging.c` はここで GIL を外す。GIL がないので何もしない。
  - `PyErr_SetString`（ColorLUT.c）・`PyErr_Clear`（Storage.c）: 上のエラーに入れる・消す。`PyCapsule_GetPointer`（Storage.c の Arrow 取り込み。Genko は使わない）: 常に失敗を返す。
  - `PyMutex_Lock`・`PyMutex_Unlock`: 小さなスピンロック（アトミック交換と `sched_yield`、Windows は `SwitchToThread`）。
- `compat/resample_region.c`・`compat/resample_region.h`: `genko_ImagingResampleRegion`。拡大縮小した画像の一部（出力の列 [x0, x1)・行 [y0, y1)）だけを計算する。`RenderRegion` で描画範囲だけを描くため（docs/cpp-migration/ARCHITECTURE.md §4a）。
  - `Resample.c` を未改変のまま `#include` し、その static 関数（係数の計算、水平・垂直の各パス）をそのまま使う。各画素は、`ImagingResample` が全体を計算するときと同じ係数・順序・丸めで出る。単体試験（`test_image`）と契約試験（`test_contract_render` の regions）で、全体を描いて切り出したものと全画素が一致することを確かめている。
  - このため `Resample.c` は単独ではビルドしない（同じ関数が二度定義されないように）。
- ビルド時に作る `ImagingCxx.h`（ビルドディレクトリの `third_party/pillow/cxx/`）: C++ から読むための Imaging.h。CMake が構成時に、Imaging.h から文字列の置換で作る。
  - C++ では `typedef struct ImagingMemoryArena {…} *ImagingMemoryArena;`（同じ名前の struct と typedef）が書けない。そこで struct のタグ名を `ImagingMemoryArenaInstance` に替える（`extern struct ImagingMemoryArena ImagingDefaultArena;` も合わせる）。
  - Imaging.h の `extern "C"` の外で読まれる `Mode.h` を `extern "C"` で囲む。
  - それ以外は元と同じ。C のファイルは元の Imaging.h を読む。Imaging.h が変わって置換が当たらなくなると、構成が止まる（FATAL_ERROR）。

## ビルドしないもの

- 画像コーデック: `*Decode.c`・`*Encode.c`。PNG は libpng で読む（`native/src/render/png.cpp`）。
- `Arrow.c`、`codec_fd.c`、`Dib.c`（Windows の画面表示）、`Quant*.c`（減色）。
- `Resample.c`: `compat/resample_region.c` から読む（上記）。

## ビルドの設定

- C11。`NDEBUG`（CPython が拡張モジュールに付ける）。ビッグエンディアンでは `WORDS_BIGENDIAN`。
- GCC・Clang:
  - `-ffp-contract=off -fno-fast-math`: Genko の浮動小数の決まり。FMA に縮めず、x86-64 の wheel と同じ丸めで計算する。
  - `-fwrapv`: CPython 3.12 が拡張モジュールに付ける `-fno-strict-overflow` と同じく、符号つき整数のあふれを 2 の補数で折り返す（wheel の Pillow と同じ動き）。
  - `-fno-strict-aliasing`: 画素の行を別の型のポインタで読む箇所があるため。保守的に付けている（画素は変わらない）。
  - `-w`: 第三者のコードなので警告は出さない。`-Werror` は付けない。
  - Debug でも `-O2`: wheel と同じく最適化する。上の決まりがあるので、画素は最適化の段階によらない。Debug ビルドの描画試験の時間を抑えるため。
- MSVC: `/fp:precise /W1 /utf-8`。
- 実行時（`native/src/render/image.cpp`）: アリーナが、空いたブロック（16 MB）を 32 個まで取っておく（Pillow の `PILLOW_BLOCKS_MAX` に当たる。参照の Python は 0）。ページは何枚ものページ大の画像を通して描くので、毎回新しいメモリを取るより速い。再利用するブロックも、新しいブロックと同じく 0 で埋められる（`ImagingNewDirty` は埋めないが、呼び出し側が全画素を書く）ので、画素は変わらない。

## サニタイザ（linux-asan: `address,undefined`）

- libImaging も Genko のコードと同じく計装し、UBSan の最初の報告で止まる（`-fno-sanitize-recover=undefined`）。報告があれば、その試験は失敗する。
- linux-asan の全試験（unit・contract・e2e・perf）で、ASan・UBSan の報告はなかった。抑制リストは作っていない。
- `-fwrapv` のもとでは、符号つき整数のあふれは未定義動作ではないので、UBSan（GCC）はこれを検査しない。そこで、libImaging だけ `-fwrapv` を外した調査用のビルド（別のビルドディレクトリで、コンパイル規則の末尾に `-fno-wrapv` を足したもの。ソースとこの CMakeLists.txt は変えない）でも、描画の試験を UBSan の報告で止まる設定で流した。test_image・test_png・test_render_page・test_contract_draw・test_contract_brushes・test_contract_render・test_contract_render_cli のすべてで、あふれを含め報告はなかった。`-fwrapv` は wheel と同じ動きにするためのもので、報告を隠すためではない。
- 将来、libImaging の中で直せない報告が出た場合は、次のようにする。Genko 自身のコードは抑制しない。
  1. そのチェックだけを回復可能に戻す（`-fsanitize-recover=<チェック>`）。
  2. `sanitizer-suppressions.txt` を作り、試験の環境で `UBSAN_OPTIONS=suppressions=…:halt_on_error=1` として読ませる。
  3. ここに理由を記録する。
