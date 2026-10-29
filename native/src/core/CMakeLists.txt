# The op catalogue (`genko schema`) is embedded from data/ops_schema.json; a change to the file re-runs CMake.
set(GENKO_OPS_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/ops_schema.json")
file(READ "${GENKO_OPS_SCHEMA_FILE}" _genko_schema_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," GENKO_OPS_SCHEMA_BYTES "${_genko_schema_hex}")
set(_b "0x[0-9a-f][0-9a-f],")
string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n"
       GENKO_OPS_SCHEMA_BYTES "${GENKO_OPS_SCHEMA_BYTES}")
configure_file(ops_schema_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ops_schema_data.cpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GENKO_OPS_SCHEMA_FILE}")

# The Unicode names of Python's unicodedata.lookup (core/pyre's \N{…}), embedded from data/unicode_names.bin
# (tools/migration/pyre_tables.py writes it); a change to the file re-runs CMake.
set(GENKO_PYRE_NAMES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/unicode_names.bin")
file(READ "${GENKO_PYRE_NAMES_FILE}" _genko_names_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," GENKO_PYRE_NAMES_BYTES "${_genko_names_hex}")
string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n"
       GENKO_PYRE_NAMES_BYTES "${GENKO_PYRE_NAMES_BYTES}")
configure_file(pyre_names_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/pyre_names_data.cpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GENKO_PYRE_NAMES_FILE}")

# The built-in ICC profiles (color-model@1: core/color_model.cpp's table), embedded from data/profiles/<id>.icc
# (tools/migration/make_profiles.cpp makes the RGB and grey ones; coated-cmyk is PhotoCraft's, CC0-1.0); a change to a
# file re-runs CMake.
set(GENKO_PROFILE_IDS display-p3 adobe-rgb-compat prophoto-compat linear-srgb rec2020 gray-gamma-2.2 sgray coated-cmyk)
set(GENKO_PROFILES_DATA "")
foreach(_id IN LISTS GENKO_PROFILE_IDS)
  set(_file "${CMAKE_CURRENT_SOURCE_DIR}/data/profiles/${_id}.icc")
  file(READ "${_file}" _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n" _bytes "${_bytes}")
  string(MAKE_C_IDENTIFIER "${_id}" _name)
  string(APPEND GENKO_PROFILES_DATA "extern const unsigned char kProfile_${_name}[] = {\n${_bytes}\n};\n"
         "extern const std::size_t kProfile_${_name}_size = sizeof(kProfile_${_name});\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
endforeach()
configure_file(builtin_profiles_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles_data.cpp" @ONLY)

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
    config_dir.cpp
    prefs_table.cpp
    base64.cpp
    geometry.cpp
    model.cpp
    color_raster.cpp
    ink.cpp
    color_model.cpp
    lut3d.cpp
    channels.cpp
    exposure.cpp
    ops_color.cpp
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
    filters.cpp
    color_adjust.cpp
    adjust.cpp
    adjust_kinds.cpp
    filter_kinds.cpp
    filter_upstream.cpp
    exif.cpp
    lens.cpp
    vanishing.cpp
    gallery_kinds.cpp
    adjust_tone.cpp
    adjust_color.cpp
    rulers.cpp
    command_bus.cpp
    ops_util.cpp
    ops_book.cpp
    ops_frames.cpp
    ops_pages.cpp
    ops_strokes.cpp
    ops_layers.cpp
    ops_rulers.cpp
    anim.cpp
    ops_anim.cpp
    ops_vector.cpp
    linalg3.cpp
    mesh3d.cpp
    prim3d.cpp
    persp3d.cpp
    mannequin.cpp
    poses.cpp
    areas.cpp
    ops_arrange.cpp
    pyre.cpp
    pyre_parse.cpp
    pyre_compile.cpp
    pyre_match.cpp
    pyre_unicode.cpp
    ruby.cpp
    ops_lines.cpp
    ops_bookpages.cpp
    clock.cpp
    stylecat.cpp
    ops_studio.cpp
    smart.cpp
    # PC3 (選択・マスク・パス・変形・判型の土台: masks@1, vector@1, pen-shapes@1, transform@1, page-spec@1)
    vector.cpp
    masks.cpp
    page_geometry.cpp
    # PC3 C-d (表示 → スナップ・スナップ先)
    snap.cpp
    # PC4 F-0 (形式の土台: file-info@1, image-grid@1, psd-keep@1)
    file_info.cpp
    psd_keep.cpp
    # PC4 E-0 (描く道具の土台: brushes@1, patterns@1)
    brush_dabs.cpp
    patterns.cpp
    # PC4 F-e (カメラ RAW: camera-raw@1 の現像の設定)
    raw_develop.cpp
    # PC4 E6 (レイヤースタイル: layer-styles@1)
    layer_styles.cpp
    # PC5 G-0 (文字: type@1 の記録・属性の表・解決・範囲の追従)
    type_text.cpp
    # PC5 I-b (グループ: groups@1 の印・確かめ・層の木)
    groups.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/ops_schema_data.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/pyre_names_data.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles_data.cpp"
  PUBLIC_DEPS Qt6::Core nlohmann_json::nlohmann_json
  PRIVATE_DEPS ZLIB::ZLIB)

# CPython calls libm directly: so must we (no pow(x, 2.0) → x*x, no sin+cos → sincos), for the same last bits. Every
# call of libm for Python's math module is in pymath.cpp; linalg3.cpp calls libm's hypot as numpy does. (MSVC: the
# same with /fp:precise, and the intrinsics turned off by #pragma function in each file.)
if(NOT MSVC)
  set_source_files_properties(pymath.cpp linalg3.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
endif()
