# Pillow libImaging（同梱）

- 取得元: Pillow 12.3.0 の sdist（PyPI。sha256 `3b8182a766685eaa002637e28b4ec8d6b18819a0c71f579bf0dbaa5830297cce`）の `src/libImaging/`。
- ライセンス: `LICENSE`（MIT-CMU / HPND）。
- 目的: 現行の Python 版（Pillow 12.3.0）と同じ画素で原稿を描くため、画素処理の C 実装をそのまま使う（docs/cpp-migration/ARCHITECTURE.md §4a）。
- ビルドするのは `native/third_party/pillow/CMakeLists.txt` に列挙したファイルだけ。Python・Arrow・画像コーデックに依存する部分は使わない。
- Python の代わり（`compat/Python.h`・`compat/imaging_glue.c`）、描画範囲だけの拡大縮小（`compat/resample_region.c`）、C++ から読む Imaging.h（ビルド時に作る `ImagingCxx.h`）は `compat/` と `CMakeLists.txt` にあり、`libImaging/` は取り込んだときのまま。詳しくは `GENKO_CHANGES.md`。
- 変更は `GENKO_CHANGES.md` に記録する。元のファイルを書き換える場合も、その理由と差分を残す。
- `fonts/load_default.ttf`: Pillow 12.3.0 の `ImageFont.load_default()` が FreeType で開くフォント（Aileron Regular の、Pillow が `PIL/ImageFont.py` に base64 で持つ字の少ない版。12,676 バイト、sha256 `69853909b940023570964e29cffe30da95aea8de3627736b5cd15ab30143169f`）を、そのバイトのまま取り出したもの。Aileron は Sora Sagano（dotcolon）作、CC0。スタジオの依頼の手引き（`guide.pose` の人物の id）と入稿前の点検の図が Pillow と同じ画素になるように使う（`render::TrueTypeFonts::load_default`）。
