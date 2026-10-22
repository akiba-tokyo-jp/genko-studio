# The SBOM of an installed Genko Studio (SPDX 2.3, JSON), made from what was installed (M6, AC-PACKAGE): the program and
# every library file in the folder (with its SHA-256), each under the package it belongs to; the versions from the
# program itself (`genko --version`: the libraries it was built with); what is built into the program (FreeType,
# Pillow's libImaging, the ported CPython re and Arm routines, the fonts and icons); and, on Linux, the libraries the
# computer itself provides (not in the folder). The test test_package compares it with docs/cpp-migration/THIRD_PARTY.md.
#
# Run at install time with GENKO_SBOM_PREFIX (the installed folder), GENKO_SBOM_VERSION and GENKO_SBOM_PROGRAM set.

cmake_minimum_required(VERSION 3.24)

execute_process(COMMAND "${GENKO_SBOM_PROGRAM}" --version OUTPUT_VARIABLE _out RESULT_VARIABLE _rc
                ENCODING UTF-8 TIMEOUT 120)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "the SBOM needs `genko --version` of the installed program (exit ${_rc})")
endif()
string(JSON _build GET "${_out}" build)

function(_version key out)
  string(JSON _v ERROR_VARIABLE _err GET "${_build}" ${key})
  if(_err)
    set(_v "NOASSERTION")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

# (id ~ name ~ version: build_info's key, or "=" and the version ~ licence ~ where it is: bundled files (a regex on
# their names), "built-in" (inside the program) or "system" (the computer's own, not in the folder))
set(_packages
  "qt~Qt 6~qt~LGPL-3.0-only~bundled:(^|/)(lib)?Qt6|^plugins/|(^|/)(opengl32sw|d3dcompiler_47)\\.dll$"
  "icu~ICU (Qt's)~=73.2~Unicode-DFS-2016 AND ICU AND BSD-3-Clause AND NAIST-2003~bundled:^lib/libicu(data|i18n|uc|io|tu|test)\\.so"
  "xcb-extras~libxcb utility libraries (Qt's xcb platform)~=NOASSERTION~MIT~bundled:^lib/libxcb-(cursor|icccm|image|keysyms|render-util|util)\\.so"
  "nlohmann-json~nlohmann/json~nlohmann_json~MIT~built-in"
  "freetype~FreeType~freetype~FTL~built-in"
  "pillow-libimaging~Pillow libImaging~=12.3.0~MIT-CMU~built-in"
  "optimized-routines~Arm optimized-routines~=v25.01~MIT~built-in"
  "photocraft~PhotoCraft (ported: the exposure adjustment, colour models and modes, quantization, proofing, channels)~=v0.2.0~MIT~built-in"
  "photocraft-coated-cmyk~PhotoCraft Coated CMYK (the built-in ICC profile coated-cmyk)~=v0.2.0~CC0-1.0~built-in"
  "icc-profiles~Built-in RGB and grey ICC profiles (made by Genko with Little CMS)~=NOASSERTION~CC0-1.0~built-in"
  "cpython-re~CPython re (ported)~=3.12.3~PSF-2.0 AND Unicode-3.0~built-in"
  "fonts~Bundled fonts (Dela Gothic One, Reggae One, Yomogi, Zen Kaku Gothic New, Zen Maru Gothic, Zen Old Mincho, IBM Plex Sans JP)~=NOASSERTION~OFL-1.1~built-in"
  "lucide~Lucide icons~=NOASSERTION~ISC~built-in")
if(CMAKE_HOST_WIN32)
  list(APPEND _packages
    "zlib~zlib~zlib~Zlib~built-in" "libpng~libpng~libpng~libpng-2.0~built-in" "libjpeg-turbo~libjpeg-turbo~libjpeg_turbo~IJG AND BSD-3-Clause AND Zlib~built-in"
    "libtiff~libtiff~libtiff~libtiff~built-in" "harfbuzz~HarfBuzz~harfbuzz~MIT~built-in" "libwebp~libwebp~webp~BSD-3-Clause~built-in"
    "lcms2~Little CMS 2~lcms2~MIT~built-in"
    "openexr~OpenEXR (with Imath)~openexr~BSD-3-Clause~built-in" "libavif~libavif~libavif~BSD-2-Clause AND Apache-2.0~built-in"
    "aom~libaom~aom~BSD-2-Clause~built-in"
    "libdeflate~libdeflate (OpenEXR's)~=1.26~MIT~built-in" "openjph~OpenJPH (OpenEXR's)~=0.32.0~BSD-2-Clause~built-in"
    "libyuv~libyuv (libavif's)~=1916~BSD-3-Clause~built-in"
    "msvc-runtime~Microsoft Visual C++ runtime~=NOASSERTION~LicenseRef-MSVC-Redistributable~bundled:(^|/)(msvcp|vcruntime|concrt)[0-9_]*\\.dll$")
else()
  list(APPEND _packages
    "zlib~zlib~zlib~Zlib~system" "libpng~libpng~libpng~libpng-2.0~system" "libjpeg-turbo~libjpeg-turbo~libjpeg_turbo~IJG AND BSD-3-Clause AND Zlib~system"
    "libtiff~libtiff~libtiff~libtiff~system" "harfbuzz~HarfBuzz~harfbuzz~MIT~system" "libwebp~libwebp~webp~BSD-3-Clause~system"
    "lcms2~Little CMS 2~lcms2~MIT~system"
    "openexr~OpenEXR (with Imath)~openexr~BSD-3-Clause~system" "libavif~libavif~libavif~BSD-2-Clause AND Apache-2.0~system"
    "aom~libaom~aom~BSD-2-Clause~system")
endif()

# a file of the folder: its name, its id and its checksums (SPDX 2.3 asks for SHA1; SHA256 for the person)
function(_file_entry file index out)
  file(SHA1 "${GENKO_SBOM_PREFIX}/${file}" _sha1)
  file(SHA256 "${GENKO_SBOM_PREFIX}/${file}" _sha256)
  set(${out} "{\"fileName\": \"./${file}\", \"SPDXID\": \"SPDXRef-File-${index}\", \"checksums\": [{\"algorithm\": \"SHA1\", \"checksumValue\": \"${_sha1}\"}, {\"algorithm\": \"SHA256\", \"checksumValue\": \"${_sha256}\"}]}" PARENT_SCOPE)
endfunction()

_version(genko _genko_version)
string(TIMESTAMP _now "%Y-%m-%dT%H:%M:%SZ" UTC)
set(_doc "{}")
string(JSON _doc SET "${_doc}" spdxVersion "\"SPDX-2.3\"")
string(JSON _doc SET "${_doc}" dataLicense "\"CC0-1.0\"")
string(JSON _doc SET "${_doc}" SPDXID "\"SPDXRef-DOCUMENT\"")
string(JSON _doc SET "${_doc}" name "\"genko-studio-${GENKO_SBOM_VERSION}\"")
string(JSON _doc SET "${_doc}" documentNamespace "\"https://genko.invalid/spdx/genko-studio-${GENKO_SBOM_VERSION}-${_now}\"")
string(JSON _doc SET "${_doc}" creationInfo "{\"created\": \"${_now}\", \"creators\": [\"Tool: genko-studio package/sbom.cmake\"]}")
string(JSON _doc SET "${_doc}" packages "[]")
string(JSON _doc SET "${_doc}" files "[]")
string(JSON _doc SET "${_doc}" relationships "[]")
if(CMAKE_HOST_WIN32)
  # (a licence SPDX has no id for: named and described here)
  string(JSON _doc SET "${_doc}" hasExtractedLicensingInfos "[{\"licenseId\": \"LicenseRef-MSVC-Redistributable\", \"name\": \"Microsoft Visual C++ Redistributable\", \"extractedText\": \"The Microsoft Visual C++ runtime files listed as Distributable Code in the Microsoft Software License Terms for Visual Studio 2022 (https://visualstudio.microsoft.com/license-terms/).\"}]")
endif()

set(_root "{\"SPDXID\": \"SPDXRef-genko-studio\", \"name\": \"Genko Studio\", \"versionInfo\": \"${_genko_version}\", \"downloadLocation\": \"NOASSERTION\", \"licenseConcluded\": \"NOASSERTION\", \"filesAnalyzed\": false}")
string(JSON _doc SET "${_doc}" packages 0 "${_root}")
string(JSON _doc SET "${_doc}" relationships 0 "{\"spdxElementId\": \"SPDXRef-DOCUMENT\", \"relationshipType\": \"DESCRIBES\", \"relatedSpdxElement\": \"SPDXRef-genko-studio\"}")

# every file of the folder (but the notes), and which package it is
file(GLOB_RECURSE _files RELATIVE "${GENKO_SBOM_PREFIX}" "${GENKO_SBOM_PREFIX}/*")
list(SORT _files)
set(_claimed "")
set(_p 1)
set(_r 1)
set(_f 0)
foreach(_entry IN LISTS _packages)
  string(REPLACE "~" ";" _fields "${_entry}")
  list(GET _fields 0 _id)
  list(GET _fields 1 _name)
  list(GET _fields 2 _key)
  list(GET _fields 3 _licence)
  list(GET _fields 4 _where)
  if(_key MATCHES "^=")
    string(SUBSTRING "${_key}" 1 -1 _ver)
  else()
    _version(${_key} _ver)
  endif()
  set(_comment "")
  if(_where STREQUAL "system")
    set(_comment "the computer's own library: linked dynamically, not in the folder")
  elseif(_where STREQUAL "built-in")
    set(_comment "built into the program")
  endif()
  set(_pkg "{\"SPDXID\": \"SPDXRef-Package-${_id}\", \"name\": \"${_name}\", \"versionInfo\": \"${_ver}\", \"downloadLocation\": \"NOASSERTION\", \"licenseConcluded\": \"${_licence}\", \"licenseDeclared\": \"${_licence}\", \"copyrightText\": \"NOASSERTION\", \"filesAnalyzed\": false, \"comment\": \"${_comment}\"}")
  string(JSON _doc SET "${_doc}" packages ${_p} "${_pkg}")
  math(EXPR _p "${_p} + 1")
  set(_relation "CONTAINS")
  if(_where STREQUAL "system")
    set(_relation "DEPENDS_ON")
  endif()
  string(JSON _doc SET "${_doc}" relationships ${_r} "{\"spdxElementId\": \"SPDXRef-genko-studio\", \"relationshipType\": \"${_relation}\", \"relatedSpdxElement\": \"SPDXRef-Package-${_id}\"}")
  math(EXPR _r "${_r} + 1")
  if(_where MATCHES "^bundled:")
    string(SUBSTRING "${_where}" 8 -1 _pattern)
    foreach(_file IN LISTS _files)
      if(_file MATCHES "${_pattern}" AND NOT IS_SYMLINK "${GENKO_SBOM_PREFIX}/${_file}")
        _file_entry("${_file}" ${_f} _entry)
        string(JSON _doc SET "${_doc}" files ${_f} "${_entry}")
        string(JSON _doc SET "${_doc}" relationships ${_r} "{\"spdxElementId\": \"SPDXRef-Package-${_id}\", \"relationshipType\": \"CONTAINS\", \"relatedSpdxElement\": \"SPDXRef-File-${_f}\"}")
        math(EXPR _f "${_f} + 1")
        math(EXPR _r "${_r} + 1")
        list(APPEND _claimed "${_file}")
      endif()
    endforeach()
  endif()
endforeach()

# the program itself (with Genko's own libraries, and Pillow's libImaging, beside it in a sanitizer build, where they
# are shared: GenkoFlags.cmake)
foreach(_file IN LISTS _files)
  if(_file MATCHES "^lib/libpillow_imaging\\.so$")
    _file_entry("${_file}" ${_f} _entry)
    string(JSON _doc SET "${_doc}" files ${_f} "${_entry}")
    string(JSON _doc SET "${_doc}" relationships ${_r} "{\"spdxElementId\": \"SPDXRef-Package-pillow-libimaging\", \"relationshipType\": \"CONTAINS\", \"relatedSpdxElement\": \"SPDXRef-File-${_f}\"}")
    math(EXPR _f "${_f} + 1")
    math(EXPR _r "${_r} + 1")
    list(APPEND _claimed "${_file}")
  endif()
  if(_file MATCHES "^bin/genko(\\.exe)?$" OR _file MATCHES "^lib/libgenko_[a-z_]+\\.so$")
    _file_entry("${_file}" ${_f} _entry)
    string(JSON _doc SET "${_doc}" files ${_f} "${_entry}")
    string(JSON _doc SET "${_doc}" relationships ${_r} "{\"spdxElementId\": \"SPDXRef-genko-studio\", \"relationshipType\": \"CONTAINS\", \"relatedSpdxElement\": \"SPDXRef-File-${_f}\"}")
    math(EXPR _f "${_f} + 1")
    math(EXPR _r "${_r} + 1")
    list(APPEND _claimed "${_file}")
  endif()
endforeach()

# a library in the folder that no package claims would go unlisted: refused here, so the SBOM never leaves one out
foreach(_file IN LISTS _files)
  if(_file MATCHES "\\.(so(\\.[0-9]+)*|dll)$" AND NOT IS_SYMLINK "${GENKO_SBOM_PREFIX}/${_file}")
    list(FIND _claimed "${_file}" _at)
    if(_at EQUAL -1)
      message(FATAL_ERROR "the SBOM has no package for ${_file}: add it to package/sbom.cmake and THIRD_PARTY.md")
    endif()
  endif()
endforeach()

set(_target "${GENKO_SBOM_PREFIX}/share/doc/genko-studio/sbom.spdx.json")
file(WRITE "${_target}" "${_doc}\n")
message(STATUS "SBOM: ${_target} (${_p} packages, ${_f} files)")
