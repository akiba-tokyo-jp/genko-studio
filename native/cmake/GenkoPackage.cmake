# The distribution (M6, I03, AC-PACKAGE): `cmake --install` puts Genko Studio into one folder that runs without
# Python, uv or the development tools — the `genko` program (the app, the command line and the MCP server in one),
# the Qt it runs on with its plugins (and, on Windows, every library it was built with), the licences,
# the notes for the person (supported systems, no signature, known limits, going back to the old version) and the
# SBOM made from what was installed (package/sbom.cmake). `cpack` packs that folder: a .tar.gz on Linux (with a
# per-user install.sh and uninstall.sh), a .zip on Windows.

include(GNUInstallDirs)

# (a folder of its own: the distribution is never installed over /usr/local by a plain `cmake --install`)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  if(WIN32)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/genko-studio-${PROJECT_VERSION}-windows-x64" CACHE PATH "" FORCE)
  else()
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/genko-studio-${PROJECT_VERSION}-linux-x86_64" CACHE PATH "" FORCE)
  endif()
endif()

if(UNIX AND NOT APPLE)
  # (the libraries beside the program: ../lib, whatever folder it is put in)
  set_target_properties(genko PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
endif()
install(TARGETS genko RUNTIME DESTINATION bin)

# Qt's libraries and plugins (platforms, image formats, styles…) with the ICU they come with, and the six small xcb
# utility libraries Qt's xcb platform needs that a desktop may lack (MIT). Every other library Genko was linked with
# (the C library, the C++ runtime, zlib, libpng, libjpeg, libtiff, libwebp, Little CMS, HarfBuzz, the graphics and
# window system) stays the computer's own: Ubuntu 24.04's desktop has them (package/README-ja.md names the packages),
# and a library the system builds with others (libtiff with JBIG-KIT, GPL) is never put in the folder. (Qt writes the
# regular expressions into its script unquoted: one plain expression a library, no parentheses or bars.)
qt_generate_deploy_app_script(
  TARGET genko
  OUTPUT_SCRIPT GENKO_DEPLOY_SCRIPT
  NO_UNSUPPORTED_PLATFORM_ERROR
  POST_INCLUDE_REGEXES "/libxcb-cursor[.]so" "/libxcb-icccm[.]so" "/libxcb-image[.]so" "/libxcb-keysyms[.]so"
                       "/libxcb-render-util[.]so" "/libxcb-util[.]so")
install(SCRIPT "${GENKO_DEPLOY_SCRIPT}")

# the licences of what is inside, and the notes
set(_genko_doc "${CMAKE_INSTALL_DATADIR}/doc/genko-studio")
install(FILES "${GENKO_SOURCE_ROOT}/package/README-ja.md" "${GENKO_SOURCE_ROOT}/package/QT-LGPL-ja.md" DESTINATION "${_genko_doc}")
install(FILES "${GENKO_REPO_ROOT}/docs/cpp-migration/THIRD_PARTY.md" DESTINATION "${_genko_doc}")
install(DIRECTORY "${GENKO_REPO_ROOT}/src/genko/fonts/licenses/" DESTINATION "${_genko_doc}/licenses/fonts")
install(FILES "${GENKO_REPO_ROOT}/src/genko/app/lucide/LICENSE" DESTINATION "${_genko_doc}/licenses/lucide")
install(FILES "${GENKO_SOURCE_ROOT}/package/licenses/qt/LGPL-3.0.txt" "${GENKO_SOURCE_ROOT}/package/licenses/qt/GPL-3.0.txt"
        DESTINATION "${_genko_doc}/licenses/qt")
if(UNIX AND NOT APPLE)
  install(FILES "${GENKO_SOURCE_ROOT}/package/licenses/icu/LICENSE" DESTINATION "${_genko_doc}/licenses/icu")
  install(DIRECTORY "${GENKO_SOURCE_ROOT}/package/licenses/xcb-util/" DESTINATION "${_genko_doc}/licenses/xcb-util")
endif()
install(FILES "${GENKO_SOURCE_ROOT}/third_party/photocraft/LICENSE-MIT" DESTINATION "${_genko_doc}/licenses/photocraft")
install(FILES "${GENKO_SOURCE_ROOT}/third_party/pillow/LICENSE" DESTINATION "${_genko_doc}/licenses/pillow")
install(FILES "${GENKO_SOURCE_ROOT}/third_party/freetype/FTL.TXT" "${GENKO_SOURCE_ROOT}/third_party/freetype/LICENSE.TXT"
        DESTINATION "${_genko_doc}/licenses/freetype")
install(FILES "${GENKO_SOURCE_ROOT}/third_party/cpython/LICENSE" "${GENKO_SOURCE_ROOT}/third_party/cpython/LICENSE-SRE"
              "${GENKO_SOURCE_ROOT}/third_party/cpython/LICENSE-Unicode" DESTINATION "${_genko_doc}/licenses/cpython")
install(FILES "${GENKO_SOURCE_ROOT}/third_party/optimized-routines/LICENSE-MIT" DESTINATION "${_genko_doc}/licenses/optimized-routines")
if(UNIX AND NOT APPLE)
  install(PROGRAMS "${GENKO_SOURCE_ROOT}/package/linux/install.sh" "${GENKO_SOURCE_ROOT}/package/linux/uninstall.sh" DESTINATION ".")
  # (a template for install.sh, which writes the menu item with the folder it put Genko in: not itself a menu item)
  install(FILES "${GENKO_SOURCE_ROOT}/package/linux/genko-studio.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/genko-studio"
          RENAME "genko-studio.desktop.in")
endif()

# the SBOM, from what was installed (last: after Qt's deployment)
install(CODE "set(GENKO_SBOM_PREFIX \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
set(GENKO_SBOM_VERSION \"${PROJECT_VERSION}\")
set(GENKO_SBOM_PROGRAM \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/genko${CMAKE_EXECUTABLE_SUFFIX}\")
include(\"${GENKO_SOURCE_ROOT}/package/sbom.cmake\")")

set(CPACK_PACKAGE_NAME "genko-studio")
set(CPACK_PACKAGE_VENDOR "Genko")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Genko Studio — マンガの原稿を、ネームから入稿まで")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_FILE_NAME "genko-studio-${PROJECT_VERSION}-windows-x64")
else()
  set(CPACK_GENERATOR "TGZ")
  set(CPACK_PACKAGE_FILE_NAME "genko-studio-${PROJECT_VERSION}-linux-x86_64")
endif()
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
