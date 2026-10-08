#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "render/image.hpp"

// Colour for print (Python's genko/colour.py, CMYK・カラープロファイル): RGB pages as CMYK through the printer's ICC
// profile (Little CMS, as Pillow's ImageCms) or, without one, a plain conversion that prints black lines on the black
// plate only and keeps the total ink under a limit; a proof of how the CMYK will look on screen (色校正); the sRGB
// profile embedded in RGB files. Python's ValueError: core::PyValueError.

namespace genko::render::colour {

inline constexpr int kInkLimit = 320;  // % total ink (C+M+Y+K) most coated-paper printers ask for at most

// The sRGB profile (Little CMS's built-in one) as ICC bytes.
std::string srgb_icc();
// The profile's description, or the file's name.
std::string profile_name(const std::filesystem::path& icc);
bool is_cmyk_profile(const std::filesystem::path& icc);

// The picture in CMYK ("CMYK"). With `icc` (a CMYK output profile) through it, black point compensated; without,
// grey component replacement: neutral greys and black print on K alone, the rest keeps under `ink_limit`%.
// intent: perceptual | relative | saturation | absolute.
Image to_cmyk(const Image& image, const std::optional<std::filesystem::path>& icc = std::nullopt, int ink_limit = kInkLimit,
              std::string_view intent = "relative");
Image from_cmyk(const Image& image, const std::optional<std::filesystem::path>& icc = std::nullopt, std::string_view intent = "relative");
// How the picture will print in CMYK, on screen: colours out of the printer's reach come back duller. RGBA keeps its
// transparency.
Image proof(const Image& image, const std::optional<std::filesystem::path>& icc = std::nullopt);
// The most ink anywhere on a CMYK picture, in %.
double ink_coverage(const Image& image);

}  // namespace genko::render::colour
