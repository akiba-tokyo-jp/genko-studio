#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/grid.hpp"
#include "render/image.hpp"

// Python's genko/fill.py: the region under a click (closing small gaps in the lines), or a drawn area, kept as a
// patch (a mask over its box, at FILL_DPI) that the renderer colours at any resolution.

namespace genko::render::fills {

inline constexpr int kFillDpi = 300;

// fill.px: round(mm / 25.4 * dpi), as the whole number it is (Python's int has no bound). PyValueError for NaN,
// PyUncaught OverflowError for an infinity (round()'s errors).
double px(double mm, int dpi = kFillDpi);

// fill.region: the pixels connected to (sx, sy) through free pixels (4-neighbour); all false when the seed is outside
// or not free.
BoolGrid region(const BoolGrid& free, double sx, double sy);

// fill.dilate: true pixels grown by r (a square); the grid itself for r <= 0.
BoolGrid dilate(const BoolGrid& grid, double r);

// fill.region_mask: the fill region of an "L" reference at the pixel `at`: dark pixels (< threshold) are walls, gaps
// up to gap_px closed, the region grown by expand_px + gap_px under the lines; `window` (x0, y0, x1, y1 in pixels, as
// the op computed them) limits the search. Nothing when the click finds nothing; a page-sized "L" mask otherwise.
std::optional<Image> region_mask(const Image& reference, double at_x, double at_y, double gap_px, int threshold,
                                 double expand_px, const std::optional<std::array<double, 4>>& window);

// fill.mask_patch: a patch ({"id", "box", "mode": "mask", "png", "rgb", "opacity"}) of a page-aligned "L" mask, cropped
// to what it covers; nothing when it covers nothing. `offset` (pixels) moves it.
std::optional<core::Patch> mask_patch(const Image& mask, int dpi, const std::vector<std::int64_t>& rgb, double opacity,
                                      std::pair<double, double> offset = {0.0, 0.0});

// fill.polygon_patch: a drawn area (an area's "poly", in mm) filled in one colour; nothing for fewer than three points
// or an area less than two pixels across. `what` names the op's key in the errors ("<what> must be a finite number").
std::optional<core::Patch> polygon_patch(const core::Json& points_mm, const std::vector<std::int64_t>& rgb, double opacity,
                                         int dpi = kFillDpi, std::string_view what = "area");

// The PNG bytes of a picture an op keeps (Python's save(format="PNG", optimize=True): the bytes differ from Pillow's,
// the pixels are the same). A transparent colour the picture carries (Pillow's im.info["transparency"], kept through
// convert, crop, filter and the like) is written as Pillow writes it (a tRNS chunk), so the picture reads back the
// same; PyValueError where Pillow refuses one (an RGB colour for an "L" picture, say).
std::string png_data(const Image& image);
core::Bytes png_bytes(const Image& image);

}  // namespace genko::render::fills
