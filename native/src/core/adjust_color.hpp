#pragma once

#include <array>
#include <string_view>
#include <vector>

// PC2b (色の補正, SPEC.md COLOR-04): the formulas of the colour corrections (PhotoCraft v0.2.0 compose/src/adjust.rs,
// doc/src/adjust.rs, engine/src/adjust_params.rs; attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md) — what the kinds' hooks (core/adjust_color.cpp: color_hooks), the PSD's
// adjustment layers (render/psd_adjust_color.cpp) and the windows (app/adjust_forms_color.cpp) share. Colours are the
// page's encoded RGB values (0..1; beyond for HDR), as core::AdjustFunction takes them.

namespace genko::core::color_adjust {

using Rgb = std::array<double, 3>;

// The six hue ranges (Hue/Saturation's, Black & White's weights) and Selective Color's nine, in their stored order.
inline constexpr std::array<std::string_view, 6> kHueRanges{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
inline constexpr std::array<std::string_view, 9> kSelectiveRanges{"reds",  "yellows", "greens",   "cyans", "blues",
                                                                  "magentas", "whites", "neutrals", "blacks"};

// Photo Filter's presets (PhotoCraft's PHOTO_FILTERS: common photographic filter colours), in their order.
struct PhotoFilterPreset {
    std::string_view id;
    std::array<int, 3> rgb;
};
const std::vector<PhotoFilterPreset>& photo_filters();
const PhotoFilterPreset* photo_filter(std::string_view id);

// Grey as PhotoCraft takes it (Rec. 601 in floats: 0.299, 0.587, 0.114 — not Pillow's integer 299/587/114 of
// Python's kinds).
double grey_of(const Rgb& c);
// HSL: hue in turns (0..1), saturation and lightness 0..1 (a grey: hue and saturation 0).
std::array<double, 3> rgb_to_hsl(const Rgb& c);
Rgb hsl_to_rgb(double h, double s, double l);

// One of Hue/Saturation's six ranges: its change (hue in degrees −180..180, saturation and lightness in percent
// −100..100) and the four hue degrees where it starts fading in, is fully on, starts fading out and ends.
struct HueRange {
    double hue = 0, saturation = 0, lightness = 0;
    std::array<double, 4> bounds{};
    bool neutral() const { return hue == 0 && saturation == 0 && lightness == 0; }
    // How much a colour of hue `deg` (degrees) belongs to it, 0..1 (measured from its start: the reds wrap round 0).
    double weight(double deg) const;
};
// The neutral range `i` (0 reds … 5 magentas): fully on 15° each side of its centre (60°·i), gone 45° from it.
HueRange neutral_range(int i);
// Four range sliders as one: increasing, within one turn of the first, the centre in −30..330 (the default reds are
// −45, −15, 15, 45 whichever way they were written).
std::array<double, 4> canonical_bounds(const std::array<double, 4>& b);

// Hue/Saturation on one colour: hue shift in degrees, saturation and lightness −1..1; colorize sets the hue (degrees)
// and saturation (at least ¼) instead.
Rgb hue_saturation(const Rgb& c, double hue, double saturation, double lightness, bool colorize);
// Black & White's grey: the smallest channel, plus the secondary colour's part (middle − smallest) and the primary's
// (largest − middle) weighted by their percentages (reds, yellows, greens, cyans, blues, magentas), 0..1.
double black_white_grey(const Rgb& c, const std::array<double, 6>& weights);
// How much a colour belongs to each of Selective Color's ranges (by its chroma in its hue sector; whites, neutrals and
// blacks by its lightness), and the correction: each range moves the cyan, magenta and yellow inks (1 − R, G, B) by
// its percentages and its black on every ink, relative (to the ink there: white stays white) or absolute.
std::array<double, 9> selective_color_weights(const Rgb& c);
Rgb selective_color(const Rgb& c, bool relative, const std::array<std::array<double, 4>, 9>& changes);

}  // namespace genko::core::color_adjust
