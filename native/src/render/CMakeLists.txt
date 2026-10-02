# Drawing (docs/cpp-migration/ARCHITECTURE.md §4a): Pillow's libImaging with its Python layer ported, brushes, pages.
# QtCore only (no QtGui): pages are drawn the same with or without a display.
genko_library(genko_render
  SOURCES
    image.cpp
    draw.cpp
    png.cpp
    stroke.cpp
    brushes.cpp
    panels.cpp
    composite.cpp
    page.cpp
    npcompat.cpp
    tones.cpp
    effects.cpp
    fill_patches.cpp
  PUBLIC_DEPS genko_core Qt6::Core
  PRIVATE_DEPS pillow_imaging PNG::PNG)
# (sin, cos, atan2, hypot, pow, log go through core/pymath.cpp, which calls libm exactly as CPython does)
# (numpy's float32 sinf, cosf, hypotf, atan2f: called in libm as numpy calls them)
if(NOT MSVC)
  set_source_files_properties(npcompat.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
endif()
# The ops that need pictures (genko_render_ops: tones, effects): their objects go into genko_render, where the page
# drawing keeps them linked and registered with core::OpRegistry::builtin() (render/render_ops.hpp).
add_library(genko_render_ops OBJECT
  render_ops.cpp
  ops_tones.cpp
  ops_effects.cpp)
target_link_libraries(genko_render_ops PUBLIC genko_core Qt6::Core)
genko_target_flags(genko_render_ops)
target_link_libraries(genko_render PRIVATE genko_render_ops)
