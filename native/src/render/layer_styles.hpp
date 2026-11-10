#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/patterns.hpp"

// PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」): a layer drawn with its style — PhotoCraft v0.2.0's
// crates/compose/src/effects.rs (the CPU renderer of layer effects: the distance fields, the tent blurs, each effect's
// coverage, Photoshop's order of drawing them) and lib.rs's Blend If, MIT: native/third_party/photocraft/NOTICE.md — on any
// of the three ways a page is composited: the 8-bit page (sRGB values), a page in precise colour (the canvas's linear
// light) and a page of inks (its plates' values), through the way's own blend formulas and colours (Mixing).
//
// Order, bottom to top: drop shadows, outer glows, an outer bevel's or emboss's outer half, the strokes' outer parts (onto
// what is beneath, each in its own mode), then the layer itself — its pixels at its fill opacity with the pattern,
// gradient and colour overlays, satin, inner glows, inner shadows, the strokes' inner parts and an inner bevel over them,
// taking its own shape's alpha — in the layer's blend mode; the layer's opacity over all of it (and the alpha of the layer it
// is clipped to). The effects' shapes come from the layer's alpha (after its mask): sizes and distances in mm, at the
// drawing's resolution. Several effects of a kind: the first listed on top.

namespace genko::render::layer_styles {

// How the page's way holds colours and mixes them.
struct Mixing {
    unsigned n = 3;  // colour channels (straight values, then the alpha)
    // B(below, above): the blend's colour where both are (n values each).
    std::function<void(std::string_view mode, const float* below, const float* above, float* out)> blend;
    // An sRGB colour (0..1) as the way's values (n).
    std::function<void(const std::array<double, 3>& srgb, float* out)> colour;  // (sRGB 0..1)
    // A pattern's values: the target it is laid for, and (in place, n colours) how its values become the way's.
    patterns::Target pattern_target = patterns::Target::srgb();
    std::function<void(float* values)> pattern_in;
    // Blend If: a pixel's values on 0..255 — its grey, then its own channels (blend_if_names) —, none: not applied here.
    std::function<void(const float* px, float* out)> blend_if_values;
    std::vector<std::string> blend_if_names;
    bool clamp = true;  // (values kept in 0..1: not a page of HDR values)
};

// The 8-bit page's: sRGB values 0..1 and PhotoCraft's blend formulas (a precise canvas's, on encoded values).
Mixing srgb_mixing();

// What a style is drawn with.
struct Inputs {
    const core::Json* style = nullptr;
    std::array<double, 2> light{120.0, 30.0};  // the book's global light (angle, altitude)
    double dpi = 200;
    const core::Document* book = nullptr;  // (the patterns its overlays name)
    Box wide;    // the content's box (page pixels: area grown by margin_px, cut to the page)
    Box area;    // the backdrop's box
    std::optional<Box> layer_box;  // the layer's box (page pixels; gradients aligned with the layer), none: the page's
    Size page;
    std::string blend = "normal";  // the layer's mode
    double opacity = 1.0;          // the layer's opacity
    // a shape (a pen layer of shape parts alone, written to a PSD as Photoshop's shape layer): an outer stroke lies only
    // around it, not beneath it too (upstream effects.rs's vector_shape, review P3-11)
    bool vector_shape = false;
};

// How far beyond `area` the content must be drawn for the effects within it to be exact (pixels at `dpi`).
int margin_px(const core::Json& style, double dpi);

// The layer (`content`: its picture over in.wide, n + 1 values a pixel, straight; its alpha its shape after its masks) with
// its style laid over `backdrop` (in.area, n + 1 values a pixel, straight; changed in place). `weight` (in.area, one a
// pixel): what multiplies the layer's opacity there — the alpha of the layer it is clipped to; null: 1.
void composite(const Inputs& in, const Mixing& mixing, const std::vector<float>& content, std::vector<float>& backdrop,
               const std::vector<float>* weight = nullptr);

// Is an effect of the style drawn beneath the layer (レイヤーを作成: a layer of its own below it, not clipped) — a drop
// shadow, an outer glow, a stroke outside, an outer bevel or an emboss — or over it (clipped to it)?
bool drawn_below(const core::Json& effect);

// How many effects' maps were made (tests).
std::size_t maps_made();

}  // namespace genko::render::layer_styles
