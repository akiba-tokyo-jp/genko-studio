#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/grid.hpp"
#include "render/image.hpp"

// Filters on a layer's pixels (Python's genko/filters.py and lineart.py): blur, sharpen, hue, levels, curve, mosaic,
// bitonal, motion / radial / zoom blur, noise, wave, twirl, line extraction, invert, posterize, threshold, gradient
// map, brightness and contrast, despeckle, glow and rain. What Python does with numpy is done in loops here, with
// numpy's types and order of operations (numpy 2's float32 arithmetic, its own float32 sine and cosine), so the
// pixels are the same.

namespace genko::render::filters {

// filters.KINDS, in Python's order.
inline constexpr std::array<std::string_view, 22> kKinds{
    "blur",   "sharpen", "hue",   "levels",  "curve",     "mosaic",    "bitonal",      "motion_blur",       "radial_blur",
    "zoom_blur", "noise", "wave", "twirl",   "lineart",   "invert",    "posterize",    "threshold",         "gradient_map",
    "brightness_contrast", "despeckle", "glow", "rain"};

// filters.apply_filter(image, kind, params) for every kind but "plugin:…" (a filter a person installed runs in the
// external runner: not here). Python's exceptions: PyValueError / PyTypeError (the op turns them into its error),
// OpKeyError, PyUncaught (ZeroDivisionError, OverflowError, IndexError …); OpError for what this build refuses (a
// non-finite number, a size past render/op_limits.hpp).
Image apply_filter(const Image& image, std::string_view kind, const core::Json& params);

// The streaks of the rain filter, on a clear "RGBA" picture of `size` (its random streaks seeded as Python seeds them).
Image rain_layer(Size size, const core::Json& params);

// filters.within: the filter only inside the selection (`mask`, "L" of the picture's size).
Image within(const Image& original, const Image& filtered, const Image& mask);

// filters._remap with float64 maps (width × height, rows first): the picture's pixels fetched from (map_x, map_y)
// for each place (bilinear; outside: transparent), an RGBA picture of the maps' size.
Image remap_area(const Image& rgba, int width, int height, const std::vector<double>& map_x, const std::vector<double>& map_y);

// filters._components: the sizes of the connected parts (8-neighbour) of a grid, as each true cell's part's size
// (0 for false cells).
std::vector<std::int64_t> component_sizes(const BoolGrid& grid);

// numpy 2's float32 np.sin / np.cos (its vectorised Cody–Waite reduction and polynomials, with fused multiply-adds;
// libm's sinf / cosf past their range).
float numpy_sinf(float x);
float numpy_cosf(float x);

}  // namespace genko::render::filters
