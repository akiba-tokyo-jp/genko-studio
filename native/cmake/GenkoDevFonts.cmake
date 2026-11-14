# fonts@1 (書体の追加 F2, render/font_files.hpp): the added lettering faces beside a build tree's program —
#   cmake -DGENKO_FONTS_SOURCE=<native/resources/fonts> -DGENKO_FONTS_DEST=<…/share/genko-studio/fonts> -P GenkoDevFonts.cmake
# Run on every build (native/src/api/CMakeLists.txt: the target genko_dev_fonts), it leaves a folder that is already right
# alone, so it costs a few file checks; a folder someone removed is made again.
#
# Where a link can be made (not Windows, and the place is free or already a link) the folder is a link to the source's.
# Otherwise — Windows, a file system without links, a real folder already there — the forty files and FONTS.json are
# copied: a font file only when it is missing or of another size (never by the files' times: a copy that keeps its
# source's time would be made again on every build, 157 MB of them), the catalogue only when its content differs (the
# six fonts compiled into the program, the licences and the screen's fonts are not copied: the program reads none of
# them from here). A font file the catalogue lists that is not in the source stops the step (GenkoFontList.cmake).
if(NOT GENKO_FONTS_SOURCE OR NOT GENKO_FONTS_DEST)
  message(FATAL_ERROR "GenkoDevFonts.cmake: GENKO_FONTS_SOURCE and GENKO_FONTS_DEST are needed")
endif()
get_filename_component(_parent "${GENKO_FONTS_DEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_parent}")

set(_copy TRUE)
if(NOT CMAKE_HOST_WIN32)
  if(IS_SYMLINK "${GENKO_FONTS_DEST}")
    file(READ_SYMLINK "${GENKO_FONTS_DEST}" _to)
    if(_to STREQUAL GENKO_FONTS_SOURCE)
      set(_copy FALSE)
    else()
      file(REMOVE "${GENKO_FONTS_DEST}")
    endif()
  endif()
  if(_copy AND NOT EXISTS "${GENKO_FONTS_DEST}")
    file(CREATE_LINK "${GENKO_FONTS_SOURCE}" "${GENKO_FONTS_DEST}" RESULT _made SYMBOLIC)
    if(_made EQUAL 0)
      set(_copy FALSE)
    endif()
  endif()
endif()

if(_copy)
  file(MAKE_DIRECTORY "${GENKO_FONTS_DEST}")
  include("${CMAKE_CURRENT_LIST_DIR}/GenkoFontList.cmake")
  genko_font_files(_fonts "${GENKO_FONTS_SOURCE}")
  foreach(_from IN LISTS _fonts)
    get_filename_component(_name "${_from}" NAME)
    set(_to "${GENKO_FONTS_DEST}/${_name}")
    set(_stale TRUE)
    if(EXISTS "${_to}" AND NOT IS_DIRECTORY "${_to}")
      file(SIZE "${_from}" _a)
      file(SIZE "${_to}" _b)
      if(_a EQUAL _b)
        set(_stale FALSE)
      endif()
    endif()
    if(_stale)
      file(COPY_FILE "${_from}" "${_to}")
    endif()
  endforeach()
  file(COPY_FILE "${GENKO_FONTS_SOURCE}/FONTS.json" "${GENKO_FONTS_DEST}/FONTS.json" ONLY_IF_DIFFERENT)
endif()
