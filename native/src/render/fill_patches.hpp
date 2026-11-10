#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Fills kept as patches (Python's genko/fill.py: px, region_mask, mask_patch, polygon_patch; and
// selection.area_mask): a mask over its box at FILL_DPI that the renderer colours at any resolution. The patch's PNG
// holds the same pixels as Python's (its bytes are libpng's, not Pillow's).

namespace genko::render::tone_fills {

inline constexpr int kFillDpi = 300;  // FILL_DPI

// fill.px(mm, dpi): round(mm / 25.4 * dpi).
int px(double mm, int dpi = kFillDpi);

// mask_patch(mask, dpi, rgb, opacity, offset_px): a patch from a mask (cropped to what it covers; nothing when it
// covers nothing). The patch gets a new id (core::new_id).
std::optional<core::Patch> mask_patch(const Image& mask, int dpi, const std::vector<std::int64_t>& rgb, double opacity = 1.0,
                                      Point offset_px = {});

// polygon_patch(points_mm, rgb, opacity, dpi): a drawn area filled in one colour (nothing for fewer than three points
// or no size).
std::optional<core::Patch> polygon_patch(const std::vector<std::array<double, 2>>& points_mm, const std::vector<std::int64_t>& rgb,
                                         double opacity = 1.0, int dpi = kFillDpi);

// region_mask(reference, at, gap_px, threshold, expand_px, window): the fill region of a rendered reference (L) at
// pixel `at` (page-sized, L), or nothing when the click is on a line.
std::optional<Image> region_mask(const Image& reference, Point at, int gap_px = 0, int threshold = 160, int expand_px = 1,
                                 std::optional<Box> window = std::nullopt);

// selection.area_mask(area, dpi): a {"poly"} or {"mask"} area as a mask (L) and its top-left in px.
std::pair<Image, Point> area_mask(const core::Json& area, int dpi = kFillDpi);

}  // namespace genko::render::tone_fills
