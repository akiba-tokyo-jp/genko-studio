# Third-party libraries. Linux: the distribution's packages (Ubuntu 24.04). Windows: vcpkg (native/vcpkg.json).
# Qt: the official 6.11.2 binaries (aqtinstall), LGPL modules only — never Qt HTTP Server or other GPL-only modules.
# Every library and its licence is listed in docs/cpp-migration/THIRD_PARTY.md (SBOM source).

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network Concurrent Svg PrintSupport Test)
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

# PC4 F-a (the codecs): OpenEXR with Imath (OpenEXR files) and libavif with libaom (AVIF files; this build asks libavif
# for aom by name, to read and to write). Linux: the distribution's packages through pkg-config (libopenexr-dev,
# libavif-dev, libaom-dev); Windows: vcpkg's CMake packages (openexr, libavif[aom]).
if(WIN32)
  find_package(OpenEXR CONFIG REQUIRED)
  set(GENKO_OPENEXR OpenEXR::OpenEXR)
  find_package(libavif CONFIG REQUIRED)
  set(GENKO_AVIF avif)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GENKO_OPENEXR_PC REQUIRED IMPORTED_TARGET OpenEXR)
  set(GENKO_OPENEXR PkgConfig::GENKO_OPENEXR_PC)
  pkg_check_modules(GENKO_AVIF_PC REQUIRED IMPORTED_TARGET libavif)
  set(GENKO_AVIF PkgConfig::GENKO_AVIF_PC)
endif()

# PC4 F-f (.pcraft): zstd (its tiles and blobs; BSD-3-Clause, of its dual licence) and BLAKE3 (their names: its C
# implementation, kept in native/third_party/blake3 and built there as a static library on both systems). zstd: Linux the
# distribution's (libzstd-dev, through pkg-config), Windows vcpkg's CMake package.
if(WIN32)
  find_package(zstd CONFIG REQUIRED)
  if(TARGET zstd::libzstd_static)
    set(GENKO_ZSTD zstd::libzstd_static)
  else()
    set(GENKO_ZSTD zstd::libzstd_shared)
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GENKO_ZSTD_PC REQUIRED IMPORTED_TARGET libzstd)
  set(GENKO_ZSTD PkgConfig::GENKO_ZSTD_PC)
endif()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/blake3" "${CMAKE_CURRENT_BINARY_DIR}/third_party/blake3")
