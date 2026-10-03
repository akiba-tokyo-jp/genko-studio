genko_library(genko_api
  SOURCES buildinfo.cpp cli.cpp
  PUBLIC_DEPS genko_core genko_storage genko_render genko_render_ops
  PRIVATE_DEPS ZLIB::ZLIB PNG::PNG JPEG::JPEG TIFF::TIFF Freetype::Freetype ${GENKO_HARFBUZZ} ${GENKO_WEBP} ${GENKO_LCMS2})

add_executable(genko main.cpp)
target_link_libraries(genko PRIVATE genko_api)
# `genko` alone and `genko app [book]` start the desktop app when it is built (app/app_main.hpp)
if(GENKO_BUILD_APP)
  target_link_libraries(genko PRIVATE genko_app)
  target_compile_definitions(genko PRIVATE GENKO_WITH_APP=1)
else()
  target_compile_definitions(genko PRIVATE GENKO_WITH_APP=0)
endif()
genko_target_flags(genko)
