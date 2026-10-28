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
// or ink layer and for a blend; on an 8-bit layer in the 通常 mode they lay pictures (gradient_fill's new keys too, at the
// layer's grid: the coordinator's decision after the review — its lines stay lines).

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
    bool keep_transparency = false;  // (each pixel's alpha kept, as the layer's lock_alpha keeps it: preserve_transparency)
};

// (PC4 E3's pattern fill, its pixels' way after the merge) Values laid at the pixels of the layer's grid, made once the
// layer is known: `make(kind, grid_dpi)` gives the values at a pixel (x, y) of a grid of that resolution over the page —
// Rgb: an RGB layer's (8-bit or precise) values in the page's space, straight, then the alpha; Inks: an ink layer's plates'
// values (the ink canvas's) then the alpha, in the 通常 mode; Srgb: straight sRGB then the alpha, an ink layer's in a
// blend.
struct LaidValues {
    enum class Kind { Rgb, Inks, Srgb };
    std::function<std::function<void(int x, int y, float* v)>(Kind kind, double grid_dpi)> make;
};

// The colours composited into the paint layer `layer_id` of the page at `at` where the coverage covers (its lock_alpha
// keeps each pixel's alpha). An 8-bit layer's lines and the pictures laid on it become its pixels first
// (image_ops::layer_pixels); an ink layer's in the 通常 mode stay as they are — its inks alone painted, under them, as
// before E4 (review P2-1) —, in another mode they become its inks first. The layer is read and written once (a precise
// layer's samples through one edit, an ink layer's inks, an 8-bit layer's picture) and worked in bands of rows in floats
// (a precise B4 page at 600 dpi is some 820 MB of floats: review P3-6); only the pixels painted are written. False when
// the coverage covers nothing (the layer as it was).
// (After E3's merge, its rule kept for every fill: a layer the panels cut has its coverage cut at the panels, as the page
// draws it.)
bool paint(core::OpContext& c, std::size_t at, const std::string& layer_id, const std::optional<Coverage>& coverage, const Colour& colour,
           const How& how);
// The same with values of the layer's own kind (E3's pattern): laid as E3 laid them — an ink layer's plates over its own
// in the 通常 mode (its lines kept as lines, as paint's), through the colours they mean in a blend; with the transparency
// kept, B2's fill_keeping_alpha (a precise layer's in linear light); else image_ops::composite. False when the coverage
// covers nothing of the layer (a pixel it covers left as it was — a clear one with the transparency kept — is not).
bool paint_values(core::OpContext& c, std::size_t at, const std::string& layer_id, const Coverage& coverage, const LaidValues& values, const How& how);

// Clear precise pixels for a layer becomes_precise() names (the page's working grid and precision; the book's
// color-raster feature): what paint() and E3's pattern fill start such a layer with.
void make_precise(core::Document& doc, std::size_t at, const std::string& layer_id);

// Whether a fill or a gradient on this layer makes it precise first: an empty paint layer (no pixels, lines or pictures) of
// an RGB page of 16 or 32 bits that keep_page_modes would make precise (render/ops_registry: not a tone layer, no effect,
// screen or colour of its own; cut at the panels or not — PC5-0, the user's decision of 2026-10-28: a new paint layer is
// at the page's depth). paint() gives it clear precise pixels at the page's working grid before it lays the colours, so
// they stay finer than 8 bits.
bool becomes_precise(const core::Page& page, const core::Layer& layer);

// The dither's noise at a pixel: 0..1 from its place (PhotoCraft's dither_noise: a hash, no pattern to see).
float dither_noise(int x, int y);

}  // namespace genko::render::paint_fill
