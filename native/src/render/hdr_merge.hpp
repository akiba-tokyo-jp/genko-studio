#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render/codecs/image.hpp"
#include "render/image_ops.hpp"

// HDR Pro に統合 (file.automate.mergeToHdrPro, PC5 I-h; SPEC.md AUTO-01「HDR Pro に統合」): exposures of one scene made
// into one picture of linear radiance, and its 16/8-bit tone mappings — PhotoCraft v0.2.0's algo/src/hdr.rs (MIT,
// native/third_party/photocraft/NOTICE.md), its formulas as it computes them:
//   - the relative exposures estimated from the pictures (consecutive ones by brightness, on pixels well exposed in both);
//   - the alignment: G. Ward's median threshold bitmaps (JGT 2003), XOR-counted over a pyramid — a translation;
//   - the camera's response: Debevec & Malik (SIGGRAPH 1997), g(Z) = ln E + ln Δt by weighted least squares with a
//     smoothness term and g(128) = 0, per channel, on stratified sample pixels, made monotonic;
//   - the merge: the hat-weighted mean of g(Z) − ln Δt; ghosts (where the exposures disagree) from one base exposure, the
//     mask feathered; the result scaled so the base exposure's well-exposed pixels keep their linear values;
//   - the tone mappings: Local Adaptation (Genko's hdr_toning, image_ops — PC2c's), Exposure and Gamma, Highlight
//     Compression (Reinhard et al. 2002, global) and Equalize Histogram (of log luminance, after Ward Larson et al. 1997).
// Genko's own (design 節 I 4.4): a camera RAW file comes in as linear light (LibRaw's development, PC4 F-e) — no response
// is estimated for it (its values are radiance already, the weights from its display values) —; the exposures are held as
// the files have them (8 or 16 bits, or floats), never all as floats at once, and the merge reads them pixel by pixel
// (an exposure's alignment is where it is read, not a moved copy), so the memory is the files' and the result's.

namespace genko::render::hdr {

using Rgba = image_ops::Rgba;

// sRGB's curve (IEC 61966-2-1), as PhotoCraft's photo_util.
float srgb_to_linear(float v);
float linear_to_srgb(float v);

// One exposure as it is merged: RGB of 8 or 16 bits (display values, an encoding curve — sRGB's taken) or floats (linear
// light: a RAW file's development, a 32-bit picture), w × h.
class Shot {
public:
    Shot() = default;
    // `picture` RGB of U8, U16 or F32 (core::Error("value") for another; codecs' layouts of grey or with an alpha are made
    // RGB first by rgb_of).
    Shot(std::string name, codecs::CodecImage picture, std::optional<double> exposure = std::nullopt);

    std::string name;
    // The camera's exposure, as a linear factor (time × ISO/100 / N²): its EXIF's, a RAW file's.
    std::optional<double> exposure;
    // Where it is read from: the pixel (x, y) is the picture's (x − dx, y − dy), its edges carried on (the alignment).
    int dx = 0, dy = 0;

    std::uint32_t width() const { return picture_.width(); }
    std::uint32_t height() const { return picture_.height(); }
    bool linear() const { return picture_.sample() == codecs::Sample::F32; }
    const codecs::CodecImage& picture() const { return picture_; }
    // The level each channel of its linear values clips at (its 99.9th percentile; 1 for a picture of display values).
    float white(int c) const { return white_[static_cast<std::size_t>(c)]; }
    // Channel c of the pixel (x, y) as held (display values 0..1, or linear light), its alignment applied.
    float value(std::int64_t x, std::int64_t y, int c) const;
    // The same as a display value 0..1 (linear light: sRGB's curve over value / its channel's white).
    float display(std::int64_t x, std::int64_t y, int c) const;
    // How near the pixel is to the clip, as a display value 0..1: its channels' largest display value (linear light — a
    // sensor clips a channel and the colour of the others with it); for display values, channel c's own.
    float level(std::int64_t x, std::int64_t y, int c) const;
    // Linear light (display values through sRGB's curve; linear ones as they are).
    float light(std::int64_t x, std::int64_t y, int c) const;

private:
    codecs::CodecImage picture_;
    std::array<float, 3> white_{1.0f, 1.0f, 1.0f};
};

// A picture of grey or RGB, with or without an alpha, of any sample, as RGB a Shot takes (U8, U16 as they are, F16 and
// F32 as F32); core::Error("value") for CMYK (no light is merged from inks).
codecs::CodecImage rgb_of(codecs::CodecImage picture);
// The exposure factor of a camera's setting: time × ISO/100 / N² (none without a time; N 1 and ISO 100 when not said).
std::optional<double> exposure_factor(std::optional<double> seconds, std::optional<double> f_number, std::optional<double> iso);

// The shot made smaller (a box mean, its kind kept) so its long side is at most `max_side` — the window's preview.
Shot reduced(const Shot& shot, int max_side);

// --- the steps (each as hdr.rs) ----------------------------------------------------------------------------------------

// Relative exposures (linear factors, the darkest 1) estimated from the shots themselves.
std::vector<double> estimate_exposures(const std::vector<const Shot*>& shots, const std::function<bool()>& stopped = {});
// The translation (dx, dy) that aligns the grey picture `img` to `reference` (Ward's MTB), up to 2^levels px.
std::pair<int, int> mtb_offset(std::size_t w, std::size_t h, const std::vector<float>& reference, const std::vector<float>& img, int levels);
// The shots aligned to the middle exposure (by `exposures`) on a grey copy made at most 1536 px a side: each one's dx, dy.
void align(std::vector<Shot>& shots, const std::vector<double>& exposures, const std::function<bool()>& stopped = {});
// Sample pixels for the response: a grid of pixels flat in `middle`, stratified by their intensity (count at most).
std::vector<std::size_t> sample_pixels(const Shot& middle, std::size_t count);
// Debevec–Malik: z[j][i] the code value (0..255) of sample i in exposure j; g(0..255) with g(128) = 0, smoothed by `lambda`.
std::vector<double> response_curve(const std::vector<std::vector<std::uint8_t>>& z, const std::vector<double>& log_dt, double lambda);

struct MergeOptions {
    std::vector<double> exposures;  // linear factors, one a shot
    bool remove_ghosts = false;
    std::optional<std::size_t> ghost_base;  // none: the shot with most well-exposed pixels
    // per channel (none: recovered from the shots — never for shots of linear light)
    std::optional<std::array<std::vector<double>, 3>> response;
    std::function<bool()> stopped;  // asked between rows: true stops the work (render::Cancelled)
};

struct Merged {
    std::uint32_t width = 0, height = 0;
    std::vector<float> rgb;  // linear radiance, RGB interleaved
    std::optional<std::array<std::vector<double>, 3>> response;  // none: the shots were linear light
    std::size_t ghost_base = 0;
    double ghost_fraction = 0;  // of the pixels handled as ghosts
    double stops = 0;           // dynamic range (1st..99.9th percentile of the luminance)
};

// The aligned shots (all of one size, all of display values or all linear) merged. Exposures all alike (no response can be
// recovered from them) take sRGB's curve; light past what a float holds is refused (core::Error("value")). Its memory (the result, the ghosts'
// mask) is held in the imaging budget while it is made (image_ops::Held: refused past it).
Merged merge(const std::vector<Shot>& shots, const MergeOptions& options);

// --- the tone mapping (16/8 bits) ----------------------------------------------------------------------------------------

struct Tone {
    enum class Method { LocalAdaptation, ExposureGamma, HighlightCompression, EqualizeHistogram };
    Method method = Method::LocalAdaptation;
    // ローカル適応 (Photoshop's defaults, as PhotoCraft's merge_to_hdr)
    image_ops::HdrToning local{7, 0.52f, 1, 0, 30, 0, 0, 0, 20, {}};
    // 露光量とガンマ
    float exposure = 0, gamma = 1;
};
// local_adaptation, exposure_gamma, highlight_compression, equalize_histogram (none for another).
std::optional<Tone::Method> method_from(std::string_view text);
const char* method_name(Tone::Method method);

// Linear radiance tone mapped to display values (sRGB's curve) in 0..1, in place.
void tone_map(std::vector<Rgba>& px, std::size_t w, std::size_t h, const Tone& tone, const std::function<bool()>& stopped = {});

}  // namespace genko::render::hdr
