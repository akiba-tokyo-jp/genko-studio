#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/exposure.hpp"
#include "core/filters.hpp"
#include "core/json.hpp"

// The colour adjustments of filters.ADJUSTMENTS (and exposure) on high-precision colour: each pixel's straight sRGB
// samples through the settings core::adjustment reads — with its refusals, by asking it first — as continuous
// functions instead of its 256-entry tables. On an 8-bit value the result is the table's within its rounding; between
// the 8-bit steps the samples keep their place; an HDR sample beyond 0..1 goes on through the same formula where the
// adjustment is a curve of its own (gamma, invert, hue, value), and is held to 0..1 where the adjustment itself maps
// onto that range (levels, a tone curve through points, posterize, a threshold, a gradient map).

namespace genko::core {

class PreciseAdjustment {
public:
    // core::adjustment(kind, params)'s exceptions for settings it refuses; PyValueError "unknown filter" otherwise.
    PreciseAdjustment(std::string_view kind, const Json& params);
    // One pixel's red, green and blue (straight sRGB samples).
    std::array<double, 3> operator()(const std::array<double, 3>& rgb) const;

private:
    enum class Way { Levels, Simple, Tone, Gamma, Brightness, Hue, Invert, Posterize, Bitonal, Threshold, Gradient, Stops, Exposure };
    double through(double u) const;  // one channel, in 8-bit units (0..255)
    Way way_ = Way::Invert;
    int channel_ = -1;  // -1: all three; 0, 1, 2: only red, green or blue
    double a_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0;
    std::vector<double> xs_, ys_, ms_;              // a tone curve's points and slopes
    std::vector<std::array<double, 3>> colours_;    // a gradient map's colours
    std::vector<NumpyInterp> stops_;                // or each channel along its stops
    Exposure exposure_;
};

}  // namespace genko::core
