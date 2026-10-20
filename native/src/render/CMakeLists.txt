# Drawing (docs/cpp-migration/ARCHITECTURE.md §4a): Pillow's libImaging with its Python layer ported, brushes, pages,
# tones, effect lines, the 3D guides. QtCore only (no QtGui): pages are drawn the same with or without a display.
qt_add_resources(GENKO_MATERIAL_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_materials.qrc")
qt_add_big_resources(GENKO_TEXT_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_text.qrc")
genko_library(genko_render
  SOURCES
    image.cpp
    color_canvas.cpp
    channels.cpp
    inspect.cpp
    draw.cpp
    png.cpp
    bmp.cpp
    tiff_webp.cpp
    gif.cpp
    psd_image.cpp
    stroke.cpp
    brushes.cpp
    paper.cpp
    panels.cpp
    composite.cpp
    page.cpp
    npcompat.cpp
    libm_float.cpp
    quantize.cpp
    tones.cpp
    effects.cpp
    fill_patches.cpp
    prims.cpp
    fill.cpp
    selection.cpp
    warp.cpp
    raster.cpp
    filters.cpp
    vectorize.cpp
    color_edit.cpp
    color_filters.cpp
    abr.cpp
    psd.cpp
    psd_descriptor.cpp
    psd_smart.cpp
    psd_text.cpp
    movie.cpp
    anim.cpp
    timelapse.cpp
    plugins.cpp
    colour.cpp
    proof.cpp
    import_profile.cpp
    ink.cpp
    ink_canvas.cpp
    ink_edit.cpp
    ink_filters.cpp
    ops_ink.cpp
    ops_channels.cpp
    raster_ops.cpp
    ops_paint.cpp
    adjust.cpp
    adjust_space.cpp
    ops_adjust.cpp
    tone_ops.cpp
    image_ops.cpp
    ops_calc.cpp
    trap.cpp
    ops_fade.cpp
    psd_adjust.cpp
    psd_adjust_color.cpp
    zip.cpp
    materials.cpp
    covers.cpp
    bookpages.cpp
    placed.cpp
    smart.cpp
    smart_decode.cpp
    ops_smart.cpp
    ops_smart_contents.cpp
    smart_filters.cpp
    ops_smart_filters.cpp
    stack.cpp
    screentone.cpp
    text/fonts.cpp
    text/tategaki.cpp
    text/lettering.cpp
    text/balloons.cpp
    # PC3 (選択・マスク・パス・変形・判型の土台: B-0, C-0)
    selection_ext.cpp
    area_kinds.cpp
    layer_masks.cpp
    mask_lending.cpp
    path_raster.cpp
    resample.cpp
    ops_select.cpp
    ops_retouch.cpp
    ops_vector2.cpp
    ops_transform.cpp
    ops_pagesize.cpp
    # PC3 B1 (選択の基本と選択範囲の編集)
    select_color.cpp
    ops_quick_mask.cpp
    erase_magic.cpp
    # PC3 C-c (変形: 自由変形・ワープ・パペットワープ・遠近法ワープ・コンテンツに応じて拡大・縮小・ゆがみ)
    puppet.cpp
    perspective_warp.cpp
    liquify_field.cpp
    seam.cpp
    own_pixels.cpp
    layer_transform.cpp
    distort_ops.cpp
    ${GENKO_MATERIAL_RESOURCES}
    ${GENKO_TEXT_RESOURCES}
  PUBLIC_DEPS genko_core Qt6::Core
  PRIVATE_DEPS genko_storage pillow_imaging PNG::PNG JPEG::JPEG ZLIB::ZLIB TIFF::TIFF Freetype::Freetype ${GENKO_WEBP} ${GENKO_LCMS2})
# (sin, cos, atan2, hypot, pow, log go through core/pymath.cpp, which calls libm exactly as CPython does)
# (numpy's float32 hypotf, atan2f: called in libm as numpy calls them; sinf, cosf, powf: glibc's, libm_float.cpp, with
# std::fma where glibc's FMA build fuses. MSVC: /fp:precise, with which these have no intrinsic forms)
if(NOT MSVC)
  set_source_files_properties(npcompat.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
endif()

# The ops that draw (render/ops_registry.hpp): each module registers its own (register_tone_ops, register_effect_ops,
# register_3d_ops) and ops_registry() holds them with core's ops; they are applied through core's CommandBus.
set(GENKO_RENDER_OPS_SOURCES ops_registry.cpp ops_tones.cpp ops_effects.cpp ops_3d.cpp import_psd.cpp)
if(GENKO_LIBRARY_KIND STREQUAL "SHARED")
  # (a sanitizer build's shared libraries, cmake/GenkoFlags.cmake: stamp_material in genko_render applies ops through
  # ops_registry(), held here — shared libraries cannot depend on each other both ways, so the two are one library)
  target_sources(genko_render PRIVATE ${GENKO_RENDER_OPS_SOURCES})
  add_library(genko_render_ops INTERFACE)
  target_link_libraries(genko_render_ops INTERFACE genko_render genko_core Qt6::Core)
else()
  genko_library(genko_render_ops SOURCES ${GENKO_RENDER_OPS_SOURCES} PUBLIC_DEPS genko_render genko_core Qt6::Core)
  # (PC3 A2: a smart object's smart filters are drawn with filter_raster's own ways, render/ops_adjust, whose local
  # corrections apply ops through ops_registry(): a page drawn needs genko_render_ops too. Two static libraries that
  # need each other: CMake links them as such, once more after each other)
  target_link_libraries(genko_render INTERFACE genko_render_ops)
endif()
