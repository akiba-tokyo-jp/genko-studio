#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "render/image.hpp"

// The colour work of CMYK and Lab pages (color-mode@1, SPEC.md COLOR-02) through Little CMS: RGB colours into a page's
// inks (its table of inks, Genko's blacks and white, greys on K alone, the rest through its profile), inks out to sRGB
// for the screen, and whole rasters from one space into another (a page's conversion).
//
// Values: CMYK inks 0..1 (no ink .. 100%), Lab L 0..100 and a, b -128..127 (D50), sRGB 0..1 (encoded). Transforms are
// made once for a profile, an intent and a direction, without Little CMS's one-pixel cache (they run on the drawing
// pool's threads).

namespace genko::render::ink {

// Can these bytes be a CMYK page's profile? Nothing when they can, else why not (in English).
std::optional<std::string> profile_problem(std::string_view icc);
// The profile's description, or "".
std::string profile_description(std::string_view icc);

// The ink of an RGB colour on the page (schema-v4.md「色モード」: the table, Genko's blacks and white, other greys,
// then the profile).
std::vector<double> ink_of(const core::PageColor& color, const std::array<int, 3>& rgb);
// The ink of a precise sRGB colour (0..1 each; HDR values are clamped) through the profile.
std::vector<double> ink_of_srgb(const core::PageColor& color, const std::array<double, 3>& srgb);

// Many 8-bit RGB colours (rgb: 3 bytes each) as inks (out: channels values each), by the same rules; `alpha` (one byte
// each, or null) skips the transparent ones (their ink is left as it is in `out`).
void inks_of(const core::PageColor& color, const unsigned char* rgb, const unsigned char* alpha, double* out, std::size_t count);

// Inks (channels values each) as sRGB for the screen (3 floats each, 0..1): relative colorimetric, black point
// compensation.
void to_srgb(const core::PageColor& color, const double* inks, float* srgb, std::size_t count);
std::array<double, 3> srgb_of(const core::PageColor& color, const std::vector<double>& ink);

// The raster's pixels ("RGBA" or "RGB"; 8-bit) as ink pixels of the page's mode (its depth), through the profile and
// intent; `gray_on_k`: pixels exactly grey (r = g = b) on K alone (CMYK).
core::InkPixels from_rgb(const Image& image, const core::PageColor& color, bool gray_on_k = false);
// Precise sRGB pixels (straight RGBA, 0..1, rows from the top; HDR clamped) as ink pixels of the page's mode.
core::InkPixels from_srgb(const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height, const core::PageColor& color,
                          bool gray_on_k = false);
// Ink pixels as sRGB: "RGBA" 8-bit, or (`precise`) straight RGBA floats 0..1.
Image to_rgba8(const core::InkPixels& pixels, const core::PageColor& color);
std::vector<float> to_srgb_floats(const core::InkPixels& pixels, const core::PageColor& color);
// Ink pixels of one mode as another's: CMYK → CMYK (another profile: black preserved), CMYK ↔ Lab, and the depth.
core::InkPixels convert(const core::InkPixels& pixels, const core::PageColor& from, const core::PageColor& to);

// The CMYK pixels of a CMYK JPEG or PSD (Pillow's "CMYK" image: 0 is no ink) as the page's inks: through the
// picture's own profile into the page's (black preserved), or taken as the page's when it has none.
core::InkPixels from_cmyk_image(const Image& cmyk, const Image* alpha, std::string_view picture_icc, const core::PageColor& color);
// Lab pixels (Pillow's "LAB" image: L, a+128, b+128 bytes) as the page's inks.
core::InkPixels from_lab_image(const Image& lab, const Image* alpha, const core::PageColor& color);
// A "LAB" image as Pillow's Image.convert makes it "RGB", "RGBA" or "RGBX": ImageCms from Little CMS's Lab profile to
// sRGB, perceptual, into a new image, and the LAB image's fourth byte copied as it is (pyCMScopyAux) — its alpha:
// 255 for a LAB image made from colours or bytes, 0 for one merged from bands (a PSD's).
Image lab_to_rgb_as_pillow(const Image& lab, std::string_view mode);

// The ICC profile a picture's file carries (a JPEG's APP2 ICC_PROFILE chunks, a PSD's image resource 1039), or "".
std::string picture_icc(std::string_view bytes);
// A picture's file (any this build opens) as the page's ink pixels: CMYK from its own profile (or the page's), Lab,
// else RGB (its alpha kept).
core::InkPixels picture_inks(std::string_view bytes, const core::PageColor& color);

// Are two inks the same at the page's depth (as stored)?
bool same_inks(const core::PageColor& color, const std::vector<double>& a, const std::vector<double>& b);
// An RGB colour to give these inks on the page: a grey whose rules already give them (Genko's black 20 for K 100%
// alone, white for no ink, a grey for K alone), which every page prints so; else the one the screen shows them as
// (its table then holds the pair, set_inks), moved by a step or a few when that one is a colour the rules keep
// (Genko's blacks, white) or one the table holds for other inks. core::Error("value") when none near is free.
std::array<int, 3> rgb_for_inks(const core::PageColor& color, const std::vector<double>& ink);
// How far (CIE ΔE 1976) an sRGB colour moves through a CMYK page's profile and back: past about 3, out of what the
// page prints (色域外). A Lab page holds every colour: 0.
double gamut_distance(const core::PageColor& color, const std::array<int, 3>& rgb);

// One sample of an ink as a stored value (0..max) and back.
std::uint16_t encode_sample(core::InkSpace space, unsigned channel, double value, int depth);
double decode_sample(core::InkSpace space, unsigned channel, std::uint16_t sample, int depth);

}  // namespace genko::render::ink
