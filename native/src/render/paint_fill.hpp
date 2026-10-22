#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "render/image.hpp"

// PC4 E4 (SPEC.md COLOR-09「塗りつぶし・グラデーション」, design section E 0.4 and 3.3): a fill or a gradient laid into a paint
// layer's own values on its own grid — an 8-bit layer's bytes (with the dither of PhotoCraft v0.2.0's algo/src/paint.rs,
// MIT: native/third_party/photocraft/NOTICE.md), a precise layer's samples in float, an ink layer's inks by the page's rules
// (render/ink correct: a colour left as it was keeps its inks) — with a blend and an opacity, as upstream's composite_area
// lays them (the W3C general formula, image_ops::composite). The ops gradient_fill and fill take this way for a precise
// or ink layer and for their new keys; their calls without them on an 8-bit layer keep the pictures they always laid.

namespace genko::render::paint_fill {

// Where a fill reaches and how much of each pixel: an "L" picture laid on the page's pixels at `dpi` (its top-left at
// `offset`), or an area (an op's resolved {poly} or {mask}) drawn at the layer grid's resolution. None: the whole layer.
struct Coverage {
    std::optional<core::Json> area;
    Image picture;
    Point offset;
    int dpi = 0;
};

// The colour laid at a place (mm, a pixel's centre): straight sRGB 0..1 (a precise page's values past 1 too) and its
// alpha 0..1, in rgba.
using Colour = std::function<void(double x_mm, double y_mm, double rgba[4])>;

struct How {
    std::string blend = "normal";  // (one of image_ops::is_blend's)
    double opacity = 1.0;          // (0..1, times the coverage)
    bool dither = false;           // (an 8-bit layer: one level of noise before its bytes are rounded)
};

// The colours composited into the paint layer `layer_id` of the page at `at` where the coverage covers (its lines and the
// pictures laid on it become its pixels first, image_ops::layer_pixels; its lock_alpha keeps each pixel's alpha), worked in
// bands of rows when the part is large (a precise B4 page at 600 dpi is some 820 MB of floats). False when the coverage
// covers nothing (the layer as it was).
bool paint(core::OpContext& c, std::size_t at, const std::string& layer_id, const std::optional<Coverage>& coverage, const Colour& colour,
           const How& how);

// The dither's noise at a pixel: 0..1 from its place (PhotoCraft's dither_noise: a hash, no pattern to see).
float dither_noise(int x, int y);

}  // namespace genko::render::paint_fill
