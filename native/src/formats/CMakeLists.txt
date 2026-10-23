# The exports (docs/cpp-migration/ARCHITECTURE.md §1 formats/): PNG, TIFF, PDF, CMYK, the layers, strip, webtoon, SNS,
# EPUB, Kindle, the submission pack and layered PSD, written as Python's genko/export.py, profiles.py, pack.py and
# psd.py write them (the encoders as Pillow's: pillow_save.hpp), and the dispatcher the command line and the app share
# (app/exporting.py's run), and 入稿前の点検 (genko/checks.py, which the export dialog runs first). Above render: the
# pages are drawn by render_page.
genko_library(genko_formats
  SOURCES
    pillow_save.cpp
    output.cpp
    pdf.cpp
    export.cpp
    profiles.cpp
    pack.cpp
    psd_export.cpp
    psd_adjust.cpp
    psd_text.cpp
    exporting.cpp
    checks.cpp
    smart_files.cpp
    # PC4 F-0 (形式の土台: ページ・層をその深さの絵に、ファイルから 1 ページの本)
    page_picture.cpp
    open_file.cpp
    # PC4 F-b (ファイルの入口: 本のファイル情報をファイルに、コピーを保存)
    file_xmp.cpp
    copy.cpp
    # PC4 F-f (.pcraft: a page written as PhotoCraft's document)
    pcraft_write.cpp
  PUBLIC_DEPS genko_render genko_core Qt6::Core
  PRIVATE_DEPS genko_storage ZLIB::ZLIB JPEG::JPEG TIFF::TIFF ${GENKO_LCMS2})
