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
struct PdfPage {
    std::string mode;
    std::string_view profile;
    PdfBoxes boxes;
};

std::filesystem::path write_pdf(const std::filesystem::path& path, const std::vector<PdfPage>& pages,
                                const std::function<render::Image(std::size_t)>& picture);

}  // namespace genko::formats
