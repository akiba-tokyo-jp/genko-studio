# The op catalogue (`genko schema`) is embedded from data/ops_schema.json; a change to the file re-runs CMake.
set(GENKO_OPS_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/ops_schema.json")
file(READ "${GENKO_OPS_SCHEMA_FILE}" _genko_schema_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," GENKO_OPS_SCHEMA_BYTES "${_genko_schema_hex}")
set(_b "0x[0-9a-f][0-9a-f],")
string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n"
       GENKO_OPS_SCHEMA_BYTES "${GENKO_OPS_SCHEMA_BYTES}")
configure_file(ops_schema_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ops_schema_data.cpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GENKO_OPS_SCHEMA_FILE}")

genko_library(genko_core
  SOURCES
    version.cpp
    error.cpp
    json.cpp
    pynum.cpp
    pymath.cpp
    pyconv.cpp
    ids.cpp
    paths.cpp
    base64.cpp
    geometry.cpp
    model.cpp
    frames.cpp
    strokes.cpp
    covers.cpp
    pyrandom.cpp
    stroke_geom.cpp
    ops_schema.cpp
    actor.cpp
    gates.cpp
    pyops.cpp
    placement.cpp
    stroke_tools.cpp
    brushes.cpp
    rulers.cpp
    command_bus.cpp
    ops_util.cpp
    ops_book.cpp
    ops_frames.cpp
    ops_pages.cpp
    ops_strokes.cpp
    ops_layers.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/ops_schema_data.cpp"
  PUBLIC_DEPS Qt6::Core nlohmann_json::nlohmann_json)

# CPython calls libm directly: so must we (no pow(x, 2.0) → x*x, no sin+cos → sincos), for the same last bits.
if(NOT MSVC)
  set_source_files_properties(pymath.cpp pyrandom.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
endif()
