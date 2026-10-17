#pragma once

#include <string_view>

#include "core/adjust.hpp"
#include "render/image.hpp"

// PC2 (補正・色管理, SPEC.md COLOR-04; ARCHITECTURE.md §4a): the 8-bit way of a correction (core/adjust.hpp). The two
// other ways are the precise canvas (ColorCanvas::adjust, ::expose) and the ink canvas (InkCanvas::recolour,
// ::adjust_planes). A correction of Python's form is not drawn here: render._adjusted lays Python's tables as Python does
// (render/composite.cpp), and only its native.blend comes here.

namespace genko::render::adjust {

// A correction's change on an 8-bit RGBA picture (its alpha kept): the function's tables when it has them (each channel
// through its 256 values), else each pixel's colour (v / 255) through the function and rounded to 8 bits (PhotoCraft
// rounds an 8-bit document's adjustment so: compose adjustment_quantum) — each colour computed once, unless where the
// pixel is changes it (a dither: `origin` is the picture's top left on the page).
Image apply_8bit(const Image& rgba, const core::AdjustFunction& f, Point origin = Point{0, 0});

// The corrected picture laid over the one it corrected in a blend mode (a correction's native.blend): their colours
// mixed as render._blend_over mixes a layer's (the 8-bit page's formulas), the alpha of the one under it kept. normal:
// the corrected picture as it is.
Image blend_8bit(const Image& below, const Image& corrected, std::string_view mode);

}  // namespace genko::render::adjust
