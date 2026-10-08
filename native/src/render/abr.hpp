#pragma once

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
// AbrError is Python's ValueError: core::PyValueError with its words here.

namespace genko::render::abr {

struct Tip {
    std::string name;  // UTF-8
    Image image;       // "L", the ink white
    int spacing = 25;  // %
};

// abr.read(data): the sampled tips. PyValueError "the file ends too early", "a tip has no size", "only 8-bit tips are
// read (this one is N-bit)", "a section does not start with 8BIM", "version N brush files are not read", "the file has
// no picture tips".
std::vector<Tip> read(std::string_view data);

// abr.tip_png(image, longest): the tip kept small (thumbnail to `longest`), as the base64 PNG a brush carries.
std::string tip_png(const Image& image, int longest = 256);

// abr.brushes_from(data, prefix): brush definitions (define_brush / the library) from the file's tips:
// {"label", "base": "gpen", "tip": "image", "tip_png", "spacing", "min_pressure": 0.3, "taper": false}.
std::vector<core::Json> brushes_from(std::string_view data, std::string_view prefix = {});

// abr.tip_from_picture(path): a tip from any picture (dark marks on light paper, or marks on transparency).
// PyValueError "the picture could not be read"; the file's own errors as Image.open raises them.
std::string tip_from_picture(const std::filesystem::path& path);
std::string tip_from_picture(const Image& image);

}  // namespace genko::render::abr
