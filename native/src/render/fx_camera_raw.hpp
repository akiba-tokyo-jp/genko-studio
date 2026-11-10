#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/json.hpp"

// PC4 節 D5 (SPEC.md COLOR-08「フィルター」): フィルター → Camera Raw フィルター… — filter_raster's kind camera_raw (its kernel
// in render/fx_camera_raw.cpp, on the three ways of render/fx) and what its window (app/camera_raw_window: CameraRawPanel,
// 節 F's RAW development too) and tests read of it: its settings as numbers, PhotoCraft v0.2.0's curve and hue band helpers
// (algo/src/camera_raw.rs, MIT: native/third_party/photocraft/NOTICE.md).

namespace genko::render::camera_raw {

// A colour grading wheel (degrees 0..360, 0..100, −100..100).
struct Wheel {
    float hue = 0, sat = 0, lum = 0;
    bool neutral() const { return sat == 0 && lum == 0; }
};

// camera_raw's settings (core/filter_kinds: Adobe Camera Raw's sliders in its units) as the formula reads them.
struct Settings {
    float temperature = 0, tint = 0, exposure = 0, contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
    float texture = 0, clarity = 0, dehaze = 0, vibrance = 0, saturation = 0;
    float curve_highlights = 0, curve_lights = 0, curve_darks = 0, curve_shadows = 0;
    std::array<float, 3> curve_splits{25, 50, 75};
    // [in, out] in 0..255; empty: the straight line
    std::vector<std::array<float, 2>> point_curve, point_curve_red, point_curve_green, point_curve_blue;
    // red, orange, yellow, green, aqua, blue, purple, magenta
    std::array<float, 8> hsl_hue{}, hsl_sat{}, hsl_lum{};
    Wheel grade_shadows, grade_midtones, grade_highlights, grade_global;
    float grade_blending = 50, grade_balance = 0;
    float sharpen_amount = 0, sharpen_radius = 1, sharpen_detail = 25, sharpen_masking = 0;
    float noise_luminance = 0, noise_luminance_detail = 50, noise_color = 0, noise_color_detail = 50;
    float grain_amount = 0, grain_size = 25, grain_roughness = 50;
    float vignette_amount = 0, vignette_midpoint = 50, vignette_roundness = 0, vignette_feather = 50, vignette_highlights = 0;
    std::string vignette_style = "highlight_priority";  // highlight_priority, color_priority, paint_overlay
    std::uint32_t seed = 0;

    bool wb_neutral() const;
    bool tone_neutral() const;
    bool hsl_neutral() const;
    bool grade_neutral() const;
    bool curves_neutral() const;
    // Every control at its default: the filter changes nothing.
    bool identity() const;
};

// The settings of camera_raw's normal form (core::normalize_filter); a key not given (or not of its type): its default.
Settings settings_of(const core::Json& params);

// The Color Mixer's hue band centres (degrees) and the weights of the eight bands at a hue (hat functions on the circle:
// they sum to 1 everywhere).
inline constexpr std::array<float, 8> kHslBands{0, 30, 60, 120, 180, 240, 270, 300};
std::array<float, 8> band_weights(float hue);

// A monotone cubic (Fritsch–Carlson) through [in, out] points in 0..255, as a table of `n` values 0..1 over 0..1 (fewer
// than two distinct points: the straight line).
std::vector<float> curve_lut(const std::vector<std::array<float, 2>>& points, std::size_t n);

// Is a point curve the straight line (fewer than two points, or each on the diagonal)?
bool is_linear(const std::vector<std::array<float, 2>>& points);

}  // namespace genko::render::camera_raw
