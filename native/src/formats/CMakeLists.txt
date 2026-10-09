# The exports (docs/cpp-migration/ARCHITECTURE.md §1 formats/): PNG, TIFF, PDF, CMYK, the layers, strip, webtoon, SNS,
# EPUB, Kindle, the submission pack and layered PSD, written as Python's genko/export.py, profiles.py, pack.py and
# psd.py write them (the encoders as Pillow's: pillow_save.hpp), and the dispatcher the command line and the app share
# (app/exporting.py's run). Above render: the pages are drawn by render_page.
genko_library(genko_formats
  SOURCES
    pillow_save.cpp
    output.cpp
    pdf.cpp
    export.cpp
    profiles.cpp
    pack.cpp
    psd_export.cpp
    exporting.cpp
  PUBLIC_DEPS genko_render genko_core Qt6::Core
  PRIVATE_DEPS genko_storage ZLIB::ZLIB JPEG::JPEG TIFF::TIFF ${GENKO_LCMS2})
