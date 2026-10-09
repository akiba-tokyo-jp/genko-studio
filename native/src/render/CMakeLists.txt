# Drawing (docs/cpp-migration/ARCHITECTURE.md §4a): Pillow's libImaging with its Python layer ported, brushes, pages,
# tones, effect lines, the 3D guides. QtCore only (no QtGui): pages are drawn the same with or without a display.
qt_add_resources(GENKO_MATERIAL_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_materials.qrc")
qt_add_big_resources(GENKO_TEXT_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_text.qrc")
genko_library(genko_render
  SOURCES
    image.cpp
    color_canvas.cpp
    draw.cpp
    png.cpp
    bmp.cpp
    gif.cpp
    stroke.cpp
    brushes.cpp
    panels.cpp
    composite.cpp
    page.cpp
    npcompat.cpp
    libm_float.cpp
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
    movie.cpp
    anim.cpp
    timelapse.cpp
    plugins.cpp
    colour.cpp
    raster_ops.cpp
    ops_paint.cpp
    zip.cpp
    materials.cpp
    covers.cpp
    bookpages.cpp
    text/fonts.cpp
    text/tategaki.cpp
    text/lettering.cpp
    text/balloons.cpp
    ${GENKO_MATERIAL_RESOURCES}
    ${GENKO_TEXT_RESOURCES}
  PUBLIC_DEPS genko_core Qt6::Core
  PRIVATE_DEPS genko_storage pillow_imaging PNG::PNG JPEG::JPEG ZLIB::ZLIB Freetype::Freetype ${GENKO_WEBP} ${GENKO_LCMS2})
# (sin, cos, atan2, hypot, pow, log go through core/pymath.cpp, which calls libm exactly as CPython does)
# (numpy's float32 hypotf, atan2f: called in libm as numpy calls them; sinf, cosf, powf: glibc's, libm_float.cpp, with
# std::fma where glibc's FMA build fuses. MSVC: /fp:precise, with which these have no intrinsic forms)
if(NOT MSVC)
  set_source_files_properties(npcompat.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
endif()

# The ops that draw (render/ops_registry.hpp): each module registers its own (register_tone_ops, register_effect_ops,
# register_3d_ops) and ops_registry() holds them with core's ops; they are applied through core's CommandBus.
genko_library(genko_render_ops
  SOURCES
    ops_registry.cpp
    ops_tones.cpp
    ops_effects.cpp
    ops_3d.cpp
    import_psd.cpp
  PUBLIC_DEPS genko_render genko_core Qt6::Core)
