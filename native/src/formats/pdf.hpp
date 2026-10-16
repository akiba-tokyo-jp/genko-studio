#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "render/image.hpp"

// The print PDF (Python's export.write_pdf and export._pdf_boxes), byte for byte: each page one picture, stored
// losslessly (Flate, zlib level 6), in its own colour (1-bit and 8-bit grey, RGB or CMYK, with an ICC profile when
// given: one object for each different profile), every page with its MediaBox, BleedBox and TrimBox.

namespace genko::formats {

// A page's boxes in points from the bottom left, each number round(…, 3): MediaBox (the area written: paper, bleed or
// trim), BleedBox and TrimBox, cut to the MediaBox.
struct PdfBoxes {
    std::array<double, 4> media{};
    std::array<double, 4> bleed{};
    std::array<double, 4> trim{};
};

PdfBoxes pdf_boxes(const core::Page& page, std::string_view area);

// One page of the PDF: the picture's mode ("1", "L", "RGB" or "CMYK"), its profile (empty for none; used for RGB and
// CMYK only; the bytes are the caller's, kept while the PDF is written) and boxes. The pictures are made one at a time
// by `picture(n)` (of that mode), so the pages are never all held at once; the file is written beside `path` and moved
// into place when it is whole.
// channels@1: a spot channel of a page written as a separation over its picture, overprinting (/Separation with the
// ink's name, its colour in sRGB as the alternate space; an ExtGState with /OP and /OPM 1).
struct PdfSpot {
    std::string name;
    std::array<int, 3> rgb{};
};

struct PdfPage {
    std::string mode;
    std::string_view profile;
    PdfBoxes boxes;
    // channels@1: its spot channels, each one's density made by `spot(k)` ("L" of the picture's size, 255 the full ink)
    std::vector<PdfSpot> spots = {};
    std::function<render::Image(std::size_t)> spot = {};
};

// color-model@1: an RGB page's values (GKCR: its profile's encoded values) as a PDF image of `bits` (8 or 16) a channel.
struct PdfValues {
    std::string values;
    int bits = 8;
};

// color-mode@1: a page of mode "INK" is a CMYK or Lab page's inks, made by `inks(n)` (8 or 16 bits): CMYK with its
// profile (ICCBased, N 4) or DeviceCMYK, Lab as /Lab (D50, a and b -128..127); color-model@1: a grey page's, its
// lightness, with its profile (ICCBased, N 1) or DeviceGray. A page of mode "VALUES" is an RGB page's values, made by
// `values(n)`, with its profile (ICCBased, N 3) or DeviceRGB.
std::filesystem::path write_pdf(const std::filesystem::path& path, const std::vector<PdfPage>& pages,
                                const std::function<render::Image(std::size_t)>& picture,
                                const std::function<core::InkPixels(std::size_t)>& inks = {},
                                const std::function<PdfValues(std::size_t)>& values = {});

}  // namespace genko::formats
