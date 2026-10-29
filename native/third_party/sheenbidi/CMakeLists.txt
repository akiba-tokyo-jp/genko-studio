# SheenBidi 2.9.0, unmodified (PROVENANCE.json): the Unicode bidirectional algorithm (UAX #9) and script runs, a static
# library built from its one unity file (Source/SheenBidi.c with SB_CONFIG_UNITY, as its own CMakeLists builds it by
# default) on both systems. PC5 G-b (the user's decision 9: right-to-left text set across, native/src/render/text/bidi.cpp).
add_library(genko_sheenbidi STATIC Source/SheenBidi.c)
target_compile_definitions(genko_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(genko_sheenbidi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Headers" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
set_target_properties(genko_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
  target_compile_options(genko_sheenbidi PRIVATE /utf-8 /w)
else()
  target_compile_options(genko_sheenbidi PRIVATE -w)
endif()
