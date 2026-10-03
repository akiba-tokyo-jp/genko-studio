#pragma once

#include <vector>

#include "core/model.hpp"
#include "render/image.hpp"

// Pixels into pen lines (Python's genko/vectorize.py, ラスター → ベクター): the marks are thinned to their middle lines,
// which are followed into polylines, simplified, and given the marks' width and colour.

namespace genko::render::vectorize {

inline constexpr int kTraceDpi = 150;

// vectorize.trace_layer: pen lines (new ids, kind "mili") for the marks of an RGBA picture drawn at dpi; lines
// shorter than min_mm are left out. The lines are found in the order Python finds them: from the ends of the middle
// lines in the order a CPython 3.12 set of their pixels holds them, then the rest in sorted order.
std::vector<core::Stroke> trace_layer(const Image& picture, int dpi, double min_mm = 0.8);

}  // namespace genko::render::vectorize
