genko_library(genko_api
  SOURCES buildinfo.cpp cli.cpp studio_cli.cpp prefs_cli.cpp server_cli.cpp http_server.cpp headless_server.cpp
          mcp_tools.cpp mcp_server.cpp mcp_stdio.cpp mcp_http.cpp
  PUBLIC_DEPS genko_core genko_storage genko_render genko_render_ops genko_formats genko_service Qt6::Network
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
# fonts@1 (書体の追加 F2, render/font_files.hpp): the program finds the added lettering faces in ../share/genko-studio/fonts
# from the folder it is in — a package installs them there (GenkoPackage.cmake) —, so a build tree has that folder too:
# a link to native/resources/fonts, or a copy of the forty files where a link cannot be made (cmake/GenkoDevFonts.cmake).
# Looked at on every build (it is cheap when the folder is right), so a folder that was removed comes back.
add_custom_target(genko_dev_fonts ALL
  COMMAND "${CMAKE_COMMAND}" "-DGENKO_FONTS_SOURCE=${GENKO_SOURCE_ROOT}/resources/fonts"
          "-DGENKO_FONTS_DEST=$<TARGET_FILE_DIR:genko>/../share/genko-studio/fonts"
          -P "${GENKO_SOURCE_ROOT}/cmake/GenkoDevFonts.cmake"
  VERBATIM)
# (after the program is built — its folder is the program's —; the tests' support library waits for it: tests/CMakeLists.txt)
add_dependencies(genko_dev_fonts genko)
