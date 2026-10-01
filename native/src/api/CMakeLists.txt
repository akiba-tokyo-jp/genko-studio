genko_library(genko_api
  SOURCES buildinfo.cpp cli.cpp
  PUBLIC_DEPS genko_core
  PRIVATE_DEPS ZLIB::ZLIB PNG::PNG JPEG::JPEG TIFF::TIFF Freetype::Freetype ${GENKO_HARFBUZZ} ${GENKO_WEBP} ${GENKO_LCMS2})

add_executable(genko main.cpp)
target_link_libraries(genko PRIVATE genko_api)
genko_target_flags(genko)
