#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

// Photoshop brush files (.abr, Python's genko/abr.py): the sampled tips inside, as image-tip brushes. Old files
// (version 1 and 2) list the brushes one after another; newer ones (6, 7, 10) keep the tips in an "8BIM samp"
// section. Only sampled (picture) tips of 8 bits are read, as other programs do. Every number is big-endian.
//
// AbrError is Python's ValueError: core::PyValueError with its words here. Beyond Python: a version 1 / 2 record that
// points back to where it began ("a brush record points back into the file", where Python reads it again from there)
// and a file whose tips unpack to more than 2**30 pixels together (core::Error "abr_too_large", "the file's tips are
// too large to read") are refused, so a small broken file cannot unpack without end; the tips are made small one by one
// as they are read.

namespace genko::render::abr {

struct Tip {
    std::string name;  // UTF-8
    Image image;       // "L", the ink white
    int spacing = 25;  // %
};

// The pixels the tips of one file may unpack to together (32 tips of 5792 × 5792, larger than Photoshop's largest
// brushes).
inline constexpr std::int64_t kMaxPixels = std::int64_t{1} << 30;

// abr.read(data): the sampled tips. PyValueError "the file ends too early", "a tip has no size", "only 8-bit tips are
// read (this one is N-bit)", "a section does not start with 8BIM", "version N brush files are not read", "the file has
// no picture tips".
std::vector<Tip> read(std::string_view data, std::int64_t max_pixels = kMaxPixels);

// abr.tip_png(image, longest): the tip kept small (thumbnail to `longest`), as the base64 PNG a brush carries.
std::string tip_png(const Image& image, int longest = 256);

// abr.brushes_from(data, prefix): brush definitions (define_brush / the library) from the file's tips:
// {"label", "base": "gpen", "tip": "image", "tip_png", "spacing", "min_pressure": 0.3, "taper": false}.
std::vector<core::Json> brushes_from(std::string_view data, std::string_view prefix = {}, std::int64_t max_pixels = kMaxPixels);

// abr.tip_from_picture(path): a tip from any picture (dark marks on light paper, or marks on transparency).
// PyValueError "the picture could not be read"; the file's own errors as Image.open raises them.
std::string tip_from_picture(const std::filesystem::path& path);
std::string tip_from_picture(const Image& image);

}  // namespace genko::render::abr
