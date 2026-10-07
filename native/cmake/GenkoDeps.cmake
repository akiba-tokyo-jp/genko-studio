# Third-party libraries. Linux: the distribution's packages (Ubuntu 24.04). Windows: vcpkg (native/vcpkg.json).
# Qt: the official 6.11.2 binaries (aqtinstall), LGPL modules only — never Qt HTTP Server or other GPL-only modules.
# Every library and its licence is listed in docs/cpp-migration/THIRD_PARTY.md (SBOM source).

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Svg PrintSupport Test)
qt_standard_project_setup()

find_package(nlohmann_json 3.11 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/freetype" "${CMAKE_CURRENT_BINARY_DIR}/third_party/freetype")

find_package(harfbuzz CONFIG QUIET)
if(TARGET harfbuzz::harfbuzz)
  set(GENKO_HARFBUZZ harfbuzz::harfbuzz)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GENKO_HB REQUIRED IMPORTED_TARGET harfbuzz)
  set(GENKO_HARFBUZZ PkgConfig::GENKO_HB)
endif()

find_package(WebP CONFIG QUIET)
if(TARGET WebP::webp AND TARGET WebP::libwebpmux)
  set(GENKO_WEBP WebP::webp WebP::libwebpmux WebP::webpdemux)
elseif(TARGET WebP::webp AND TARGET WebP::webpmux)
  set(GENKO_WEBP WebP::webp WebP::webpmux WebP::webpdemux)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GENKO_WEBP_PC REQUIRED IMPORTED_TARGET libwebp libwebpmux libwebpdemux)
  set(GENKO_WEBP PkgConfig::GENKO_WEBP_PC)
endif()

find_package(lcms2 CONFIG QUIET)
if(TARGET lcms2::lcms2)
  set(GENKO_LCMS2 lcms2::lcms2)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GENKO_LCMS2_PC REQUIRED IMPORTED_TARGET lcms2)
  set(GENKO_LCMS2 PkgConfig::GENKO_LCMS2_PC)
endif()
