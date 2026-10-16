#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Colour reduction for an indexed page and a bitmap page (color-model@1, PC1d; SPEC.md COLOR-03): the palettes
// (exact, system, web, uniform, perceptual, selective, adaptive), the forced colours, the dithers (none,
// Floyd–Steinberg diffusion, an ordered 8×8 pattern, noise) and the 1-bit methods (threshold, pattern, diffusion, a
// halftone screen). Ported from PhotoCraft v0.2.0 crates/algo/src/quantize.rs (MIT; third_party/photocraft/NOTICE.md),
// in single precision as it computes, its powf, sinf, cosf and cbrtf the same bits everywhere (render/libm_float): the
// same picture always gives the same pixels.
//
// Pictures are straight RGBA floats 0..1, rows from the top.

namespace genko::render::quantize {

using Rgb8 = std::array<std::uint8_t, 3>;
using Rgba = std::array<float, 4>;

enum class PaletteKind { Exact, SystemMac, SystemWindows, Web, Uniform, Perceptual, Selective, Adaptive };
enum class Forced { None, BlackWhite, Primaries, Web };
enum class Dither { None, Diffusion, Pattern, Noise };

// By their ids (PhotoCraft's: "selective", "systemMac", "blackWhite", "diffusion", …), or none.
std::optional<PaletteKind> palette_kind(std::string_view id);
std::optional<Forced> forced_of(std::string_view id);
std::optional<Dither> dither_of(std::string_view id);

// The 216 web-safe colours; the classic Mac OS palette (256); Windows' (256); up to n on an equal RGB grid.
std::vector<Rgb8> web_palette();
std::vector<Rgb8> mac_palette();
std::vector<Rgb8> windows_palette();
std::vector<Rgb8> uniform_palette(std::size_t n);
// Color Table's presets (PhotoCraft's mode_cmds.rs preset_table): grayscale, blackBody, spectrum, systemMac (system),
// systemWindows, web; none for another name.
std::optional<std::vector<Rgb8>> preset_table(std::string_view name);

// The palette for a picture: `colors` (2..256) for the kinds it computes, the forced colours first and counted in.
// core::Error("value") when Exact meets more than 256 colours.
std::vector<Rgb8> build_palette(const std::vector<Rgba>& px, PaletteKind kind, std::size_t colors, Forced forced);
// Every pixel that shows mapped onto the palette (in place) with the dither, its index returned per pixel. A pixel of
// alpha under ½ becomes `transparent` (alpha 0) when there is one; else its alpha is kept (with one, the others are
// opaque). `amount` (0..1) is the dither's strength.
std::vector<std::uint8_t> quantize(std::vector<Rgba>& px, std::size_t width, const std::vector<Rgb8>& palette, Dither dither, float amount,
                                   std::optional<std::size_t> transparent);
// 8×8 Bayer threshold in -0.5..0.5.
float bayer8(std::size_t x, std::size_t y);

enum class HalftoneShape { Round, Ellipse, Line, Square, Diamond, Cross };
HalftoneShape halftone_shape(std::string_view id);  // (an unknown one: round, as PhotoCraft)
struct BitmapMethod {
    enum class Kind { Threshold, Pattern, Diffusion, Halftone } kind = Kind::Diffusion;
    float cell = 6;  // a halftone's cell in pixels (at least 2)
    float angle = 45;
    HalftoneShape shape = HalftoneShape::Round;
};
// Grey values (0..1, rows `width` wide) made 0 or 1, in place.
void to_bitmap(std::vector<float>& grey, std::size_t width, const BitmapMethod& method);

// CIE L*a*b* (D50) of an sRGB colour and back (PhotoCraft's color/src/convert.rs, single precision).
std::array<float, 3> srgb_to_lab(const std::array<float, 3>& rgb);
std::array<float, 3> lab_to_srgb(const std::array<float, 3>& lab);

}  // namespace genko::render::quantize
