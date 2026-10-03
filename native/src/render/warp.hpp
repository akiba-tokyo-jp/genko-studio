#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Free transform (Python's genko/warp.py): an area's corners pulled anywhere (遠近, perspective) or a grid of points
// pulled to bend it (メッシュ). Pen lines move point by point (long segments split first so they bend with the area);
// fills and pixels are redrawn through the same mapping, piece by piece.
//
// A warp is {"perspective": [[x, y] × 4]} — where the top-left, top-right, bottom-right and bottom-left of the area's
// box go — or {"mesh": [[x, y] × 9]} (a 3×3 grid over the box, row by row), {"mesh": […], "grid": [across, down]}
// for another grid (2 to 9 points each way).
//
// Python solves the perspective's homography with numpy's SVD and each piece's affine map with numpy.linalg.solve
// (OpenBLAS, whose kernels use fused multiply-adds): here they are solved by Gaussian elimination with partial
// pivoting. The maps are the same to the last few bits, not bit for bit (the lines of a mesh warp, plain arithmetic in
// Python, are bit for bit the same).

namespace genko::render::warp {

using Go = std::function<std::pair<double, double>(double, double)>;

// warp.mapping(box, warp): the warp as a function (x, y) mm → (x', y') mm over the area's box (x, y, w, h). Throws
// OpError with WarpError's words; Python's other errors (unpacking a box that is not four numbers, float()).
Go mapping(const std::vector<double>& box, const core::Json& warp);

// warp.warp_stroke: a line through the warp (points every 1.5 mm first; its width scaled by the warp's growth at its
// middle point; its id, kind, colour and opacity kept).
core::Stroke warp_stroke(const core::Stroke& stroke, const Go& go);

struct Warped {
    Image image;
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
};

// warp.warp_image: a picture lying at (x0, y0) (pixels at dpi) redrawn through the warp, as cells × cells pieces of two
// triangles each: the picture and its new corner. OpError "the transform stretches the area too far".
std::optional<Warped> warp_image(const Image& image, std::int64_t x0, std::int64_t y0, const Go& go, int dpi, int cells = 14,
                                 Resample resample = Resample::Bilinear);

}  // namespace genko::render::warp
