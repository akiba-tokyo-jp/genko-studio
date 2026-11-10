# BLAKE3 1.8.7, its C implementation unmodified (PROVENANCE.json): a static library, as BLAKE3's own c/CMakeLists.txt
# builds it — x86-64: the portable code and the SSE2, SSE4.1, AVX2 and AVX-512 intrinsics, each file with its own
# instruction set and the one to use chosen at run time (blake3_dispatch.c); 64-bit ARM: the NEON intrinsics; anything
# else: the portable code alone. PC4 F-f (.pcraft's hashes, native/src/render/blake3.cpp).
add_library(genko_blake3 STATIC blake3.c blake3_dispatch.c blake3_portable.c)
target_include_directories(genko_blake3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(genko_blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _genko_b3_cpu)
if(_genko_b3_cpu MATCHES "^(x86_64|amd64|x64)$")
  target_sources(genko_blake3 PRIVATE blake3_sse2.c blake3_sse41.c blake3_avx2.c blake3_avx512.c)
  if(MSVC)
    set_source_files_properties(blake3_sse41.c PROPERTIES COMPILE_OPTIONS "/arch:AVX")
    set_source_files_properties(blake3_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    set_source_files_properties(blake3_avx512.c PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
  else()
    set_source_files_properties(blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(blake3_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
  endif()
elseif(_genko_b3_cpu MATCHES "^(aarch64|arm64)$")
  target_sources(genko_blake3 PRIVATE blake3_neon.c)
  target_compile_definitions(genko_blake3 PRIVATE BLAKE3_USE_NEON=1)
else()
  target_compile_definitions(genko_blake3 PRIVATE BLAKE3_USE_NEON=0 BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()
