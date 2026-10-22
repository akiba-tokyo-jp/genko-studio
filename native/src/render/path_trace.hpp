#pragma once

#include <utility>
#include <vector>

#include "core/vector.hpp"
#include "render/path_raster.hpp"

// PC3 C-a (SPEC.md COLOR-07「作業用パス」): a coverage → a path, PhotoCraft v0.2.0's vector/src/trace.rs (MIT,
// native/third_party/photocraft/NOTICE.md) — Make Work Path from a selection, a line of dialogue's letters as a path:
//  1. marching squares at the level 0.5 over the pixels' middles (linear along a cell's edges, a saddle settled by the
//     cell's mean) give closed contours that do not cross;
//  2. a corner where the way turns by more than 60° over ±1.5 px;
//  3. each run between corners fitted with cubic Béziers by least squares and Newton's reparameterisation, split at the
//     worst point until within the tolerance (P. J. Schneider, Graphics Gems, 1990).
// The contours are subpaths: the first combines, the others exclude (the even-odd area of nested contours that do not
// cross, whichever way each runs). Coordinates are pixels of the grid traced (y down).

namespace genko::render::path_trace {

using XY = std::pair<double, double>;

// The closed contours at 0.5 of `values` (row-major over `box`; values outside it read as `outside`), each with the
// inside on its left (y down: counter-clockwise on the screen).
std::vector<std::vector<XY>> contours(const std::vector<float>& values, const path_raster::PixelBox& box, float outside = 0.0f);
// The contours fitted within `tolerance` pixels (the first combines, the rest exclude).
core::vector::Path trace_mask(const std::vector<float>& values, const path_raster::PixelBox& box, float outside, double tolerance);

}  // namespace genko::render::path_trace
