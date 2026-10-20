#pragma once

#include <optional>
#include <vector>

#include "core/json.hpp"
#include "render/liquify_field.hpp"

// PC3 C-c's Liquify strokes as the op reads them (render/distort_ops.cpp), shared with a smart object's Liquify kept as
// its smart filter (PC3 A5, render/smart_bends).

namespace genko::render {

// The strokes read as the op reads them ({tool, size_mm, density, pressure, rate, points, amount?}…; OpError), and the
// field they make over a page of w × h mm: a node every `mesh_mm` (none: liquify::auto_cell's; OpError past a working
// pixel to 64 of them), the strokes replayed on it.
std::vector<liquify::Stroke> read_liquify_strokes(const core::Json& strokes);
liquify::Field liquify_field(double w, double h, const std::optional<double>& mesh_mm, const std::vector<liquify::Stroke>& strokes);

}  // namespace genko::render
