# Pillow 12.3.0's libImaging (C, MIT-CMU), the pixel code the Python baseline draws with
# (docs/cpp-migration/ARCHITECTURE.md §4a). Only the pixel-processing files are built: no codecs (*Decode.c,
# *Encode.c), no Arrow, no codec_fd.c, no quantizer. compat/ stands in for Python.h and _imaging.c.
#
# Third-party code: built with its own warnings (never -Werror), but with Genko's floating-point rules, so a pixel
# comes out the same as in the Pillow wheel the baseline uses: no FMA contraction, no fast-math; -fwrapv and NDEBUG
# as CPython 3.12 builds its extensions (-fno-strict-overflow: signed overflow wraps); no strict aliasing, as the
# pixel rows are read through pointers of other types.

set(_pillow_dir "${CMAKE_CURRENT_SOURCE_DIR}/libImaging")
# (Resample.c is built through compat/resample_region.c, which includes it unchanged and adds region resampling.)
set(_pillow_sources
  Access AlphaComposite Bands Blend BoxBlur Chops ColorLUT Convert ConvertYCbCr Copy Crop Draw Effects Fill
  Filter Geometry GetBBox Histo Matrix Mode ModeFilter Negative Offset Pack Palette Paste Point RankFilter
  Reduce Storage UnpackYCC Unpack UnsharpMask)
list(TRANSFORM _pillow_sources PREPEND "${_pillow_dir}/")
list(TRANSFORM _pillow_sources APPEND ".c")
list(APPEND _pillow_sources compat/resample_region.c)

# Imaging.h as C++ reads it (ImagingCxx.h, made here): C++ does not allow `typedef struct ImagingMemoryArena {…}
# *ImagingMemoryArena;` (a struct and a typedef of one name), so the struct's tag is renamed; and Mode.h, which
# Imaging.h includes before its extern "C" block, is included with C linkage. Nothing else changes, and the C files
# still include the original.
file(READ "${_pillow_dir}/Imaging.h" _imaging_h)
set(_imaging_cxx "${_imaging_h}")
string(REPLACE "typedef struct ImagingMemoryArena {" "typedef struct ImagingMemoryArenaInstance {"
       _imaging_cxx "${_imaging_cxx}")
string(REPLACE "extern struct ImagingMemoryArena ImagingDefaultArena;"
       "extern struct ImagingMemoryArenaInstance ImagingDefaultArena;" _imaging_cxx "${_imaging_cxx}")
string(REPLACE "#include \"Mode.h\"\n" "extern \"C\" {\n#include \"Mode.h\"\n}\n" _imaging_cxx "${_imaging_cxx}")
string(FIND "${_imaging_cxx}" "struct ImagingMemoryArena " _left)
string(FIND "${_imaging_cxx}" "extern \"C\" {\n#include \"Mode.h\"" _mode_wrapped)
if(NOT _left EQUAL -1 OR _mode_wrapped EQUAL -1 OR _imaging_cxx STREQUAL _imaging_h)
  message(FATAL_ERROR "libImaging/Imaging.h changed: update the C++ header in native/third_party/pillow/CMakeLists.txt")
endif()
set(_cxx_dir "${CMAKE_CURRENT_BINARY_DIR}/cxx")
file(WRITE "${_cxx_dir}/ImagingCxx.h.new"
     "/* Made by native/third_party/pillow/CMakeLists.txt from libImaging/Imaging.h: do not edit. */\n${_imaging_cxx}")
configure_file("${_cxx_dir}/ImagingCxx.h.new" "${_cxx_dir}/ImagingCxx.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pillow_dir}/Imaging.h")

add_library(pillow_imaging STATIC ${_pillow_sources} compat/imaging_glue.c)
# SYSTEM: Imaging.h is C and is not written for Genko's warning flags.
target_include_directories(pillow_imaging SYSTEM PUBLIC
  "${_cxx_dir}"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${_pillow_dir}")
target_compile_definitions(pillow_imaging PRIVATE NDEBUG)
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  target_compile_definitions(pillow_imaging PUBLIC WORDS_BIGENDIAN)
endif()
set_target_properties(pillow_imaging PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(pillow_imaging PRIVATE /fp:precise /W1 /utf-8)
  target_compile_definitions(pillow_imaging PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(pillow_imaging PRIVATE -ffp-contract=off -fno-fast-math -fno-strict-aliasing -fwrapv -w)
  # (optimized in Debug builds too, as the wheel is: the pixel loops are not debugged here, and the drawing tests
  # would take far longer; the rules above keep every pixel the same at any level)
  target_compile_options(pillow_imaging PRIVATE $<$<CONFIG:Debug>:-O2>)
  find_library(GENKO_LIBM m)
  if(GENKO_LIBM)
    target_link_libraries(pillow_imaging PUBLIC ${GENKO_LIBM})
  endif()
  if(GENKO_SANITIZE)
    # (instrumented as Genko's code is, and a report stops the program, so a test fails: the tests find no undefined
    # behaviour in these files; GENKO_CHANGES.md says what to do if a report ever has to be suppressed)
    target_compile_options(pillow_imaging PRIVATE -fsanitize=${GENKO_SANITIZE} -fno-omit-frame-pointer
                           -fno-sanitize-recover=undefined)
    target_link_options(pillow_imaging PUBLIC -fsanitize=${GENKO_SANITIZE})
  endif()
endif()
