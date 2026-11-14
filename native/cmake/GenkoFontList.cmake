# fonts@1 (書体の追加 F2): the files of the forty added lettering faces, as the catalogue lists them.
#
#   genko_font_files(<out> <folder>)
#
# sets <out> to the files (in <folder>, in the catalogue's order) of the faces whose storage is "file", read from
# <folder>/FONTS.json. It stops (FATAL_ERROR) when the catalogue does not list forty of them — GenkoPackage.cmake,
# package/sbom.cmake and the tests name forty — or when one of the files is not there: a package configured without a
# font file would only fail at `cmake --install`, and a build tree's link would only be noticed by the tests.
#
# Run as a script it checks a folder and says nothing else:
#   cmake -DGENKO_CHECK_FONTS=<native/resources/fonts> -P GenkoFontList.cmake
function(genko_font_files out folder)
  if(NOT EXISTS "${folder}/FONTS.json")
    message(FATAL_ERROR "the font catalogue is not there: ${folder}/FONTS.json")
  endif()
  file(READ "${folder}/FONTS.json" _catalogue)
  string(JSON _count LENGTH "${_catalogue}" fonts)
  math(EXPR _last "${_count} - 1")
  set(_files "")
  foreach(_i RANGE ${_last})
    string(JSON _storage GET "${_catalogue}" fonts ${_i} storage)
    if(_storage STREQUAL "file")
      string(JSON _file GET "${_catalogue}" fonts ${_i} file)
      if(NOT EXISTS "${folder}/${_file}" OR IS_DIRECTORY "${folder}/${_file}")
        message(FATAL_ERROR "a font file the catalogue lists is not there: ${folder}/${_file}")
      endif()
      list(APPEND _files "${folder}/${_file}")
    endif()
  endforeach()
  list(LENGTH _files _listed)
  if(NOT _listed EQUAL 40)
    message(FATAL_ERROR "the font catalogue lists ${_listed} added faces, not 40: GenkoPackage.cmake, package/sbom.cmake and the tests name forty")
  endif()
  set(${out} "${_files}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND GENKO_CHECK_FONTS)
  genko_font_files(_checked "${GENKO_CHECK_FONTS}")
endif()
