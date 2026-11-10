#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PC2a (階調の補正, SPEC.md COLOR-04): what the tone corrections' formulas (core/adjust_tone.cpp: the hooks of levels,
// curve, brightness_contrast, posterize, gradient_map and color_lookup in the table of correction kinds) share with the
// ops, the drawing and the windows.

namespace genko::core {

// A colour of a page into the records' space and back (levels' and a tone curve's channel records in another space than
// the page's, design 4.2): `into` the page's encoded RGB value (0..1; a page of inks: the sRGB colour its inks mean) as
// the space's channel tones — CMYK each plate's brightness 1 − ink through the built-in coated-cmyk (relative, black
// point compensation), Lab L/100, (a + 128)/255, (b + 128)/255 (D50) —, `back` the tones as the page's value. The
// drawing build makes them (Little CMS, render/adjust.cpp installs it); without it such records are refused where they
// would be drawn (core::Error "not_yet_ported").
struct ToneBridge {
    std::function<std::vector<double>(const std::array<double, 3>& rgb)> into;
    std::function<std::array<double, 3>(const std::vector<double>& tones)> back;
};
using ToneBridgeMaker = std::function<ToneBridge(const Page& page, std::string_view space)>;
void set_tone_bridge(ToneBridgeMaker maker);

// PhotoCraft's formulas of levels and a tone curve (the user's decision 4: native.method "upstream"; compose/adjust.rs
// 460-535): a levels record's value at v (0..1; its settings in 0..255 as Genko stores them: black, white, gamma,
// out_black, out_white) — gamma > 1 with Photoshop's soft toe —, and a curve's 4096 samples over 0..1 (a natural cubic
// spline through its points [in, out] in 0..255, flat past its ends) and its value at v (interpolated as PhotoCraft's
// lut()).
double upstream_levels(const Json& record, double v);
std::vector<double> upstream_curve(const std::vector<std::array<double, 2>>& points);
double upstream_lut(const std::vector<double>& table, double v);

// Image › Auto Tone / Auto Contrast / Auto Color (design 3.3, PhotoCraft engine/edit_cmds.rs 223-301): from a layer's
// red, green and blue histograms (8-bit bins), levels' channel records {red, green, blue} — each {black, white, gamma,
// out_black, out_white} — that clip 0.1% at each end: "tone" each channel by its own, "contrast" all by the histograms
// together (the colours kept), "color" each by its own with a gamma that moves its median onto the medians' mean.
Json auto_levels(const std::array<std::array<std::uint64_t, 256>, 3>& histograms, std::string_view mode);

// Image › Adjustments › Equalize (PhotoCraft engine/extra_cmds.rs 156-187): one CDF over the red, green and blue values
// counted (the 256 bins: a value v of any depth falls in round(v · 255)), the table each bin goes to (0..1); none when
// nothing was counted.
std::optional<std::array<double, 256>> equalize_table(const std::array<std::uint64_t, 256>& counts);

}  // namespace genko::core
