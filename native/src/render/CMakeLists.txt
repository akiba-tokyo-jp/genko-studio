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
    prims.cpp
  PUBLIC_DEPS genko_core Qt6::Core
  PRIVATE_DEPS pillow_imaging PNG::PNG)
# (sin, cos, atan2, hypot, pow, log go through core/pymath.cpp, which calls libm exactly as CPython does)

# The ops that draw (each module's ops_*.cpp registers them in ops_registry.cpp), applied through core's CommandBus.
genko_library(genko_render_ops
  SOURCES
    ops_registry.cpp
    ops_3d.cpp
  PUBLIC_DEPS genko_render genko_core Qt6::Core)
