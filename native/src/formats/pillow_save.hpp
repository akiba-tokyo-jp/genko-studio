#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "render/image.hpp"

// Pictures written as Pillow 12.3.0's Image.save writes them for the exports (PngImagePlugin, JpegImagePlugin and
// TiffImagePlugin with their C encoders), byte for byte: the same chunks, markers and tags in the same order, the same
// encoder settings and the same compression libraries' calls (zlib, libjpeg-turbo, libtiff; the reference's wheel
// compresses with the same results as this build's). render/png.hpp's write_png keeps only the pixels (libpng's own
// filters): it is for the pictures inside a book, these are for files people take elsewhere.
//
// Python's OSError ("cannot write mode … as PNG") is core::Error("io"); a picture of no pixels core::Error("value").

namespace genko::formats {

// Image.save(…, format="PNG", dpi=…, icc_profile=…, optimize=…): modes 1, L, LA, RGB, RGBA and P (a palette: 1, 2, 4
// or 8 bits a pixel by its length, a PLTE chunk; 8 bits unfiltered); the transparency the picture carries (its
// info["transparency"]) as a tRNS chunk for P, 1, L and RGB, or an RGBA palette's alpha.
struct PngSave {
    std::optional<std::array<double, 2>> dpi;  // a pHYs chunk: int(dpi / 0.0254 + 0.5) pixels a metre
    std::string icc;                           // an iCCP chunk (zlib.compress of it), when not empty
    bool optimize = false;                     // zlib level 9 and the average filter too (ZipEncode.c)
};

// What png_bytes takes from the picture itself: a P picture's bits a pixel and its PLTE chunk, the tRNS chunk's data.
struct PngPicture {
    int palette_bits = 8;
    std::string plte;
    std::optional<std::string> trns;
};

std::string png_bytes(const render::Image& image, const PngSave& params = {});

// The same file made from rows given in order (a picture too large to hold whole, such as a strip of pages):
// add() a band of rows at a time (each of the size's width and the mode given), finish() when every row is in.
class PngWriter {
public:
    PngWriter(render::Size size, std::string_view mode, const PngSave& params = {}, const PngPicture& picture = {});
    ~PngWriter();
    PngWriter(const PngWriter&) = delete;
    PngWriter& operator=(const PngWriter&) = delete;

    void add(const render::Image& rows);
    // The whole file (core::Error("value") when rows are missing).
    std::string finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Image.save(…, format="JPEG", quality=…, optimize=…, icc_profile=…): modes L and RGB (as JpegImagePlugin's RAWMODE;
// other modes core::Error("io") "cannot write mode … as JPEG"), libjpeg's defaults with jpeg_set_quality(quality,
// TRUE), 4:2:0, no density, the profile in APP2 markers after the JFIF header.
struct JpegSave {
    int quality = -1;  // -1: libjpeg's own (75)
    bool optimize = false;
    std::string icc;
};

std::string jpeg_bytes(const render::Image& image, const JpegSave& params = {});

// Image.save(…, format="TIFF", compression=…, dpi=…, icc_profile=…) through libtiff (Pillow's libtiff encoder): modes
// 1, L, RGB, CMYK; compression "tiff_lzw", "group4" (1-bit only) or "raw" (none: Image.save's default); strips of about
// 64 KiB; little-endian.
struct TiffSave {
    std::string compression = "tiff_lzw";
    std::optional<std::array<double, 2>> dpi;
    std::string icc;
};

std::string tiff_bytes(const render::Image& image, const TiffSave& params);

// color-mode@1: a CMYK or Lab page's inks as a TIFF (LZW, 8 or 16 bits, no alpha: the page is opaque): CMYK separated
// (InkSet CMYK) with the page's profile (params.icc), Lab as CIELab (L unsigned, a and b signed; 16 bits: a·256).
std::string ink_tiff_bytes(const core::InkPixels& pixels, const TiffSave& params);

// images[0].save(path, format="PDF", save_all=True, append_images=images[1:], resolution=…) (PdfImagePlugin with
// PdfParser's writer): each picture a page of its size at `resolution` dpi, stored as a JPEG (DCTDecode, as
// jpeg_bytes makes it), the document's Title `title` (the file's stem) and its CreationDate and ModDate "(D:<date>)",
// `date` being time.strftime("%Y%m%d%H%M%SZ", time.gmtime()) (core::utc_time_text). Modes L and RGB (others
// core::Error("value") "cannot save mode …"); no pictures core::Error("value").
std::string pdf_bytes(const std::vector<render::Image>& images, double resolution, std::string_view title, std::string_view date);

// colour.srgb_icc(): the sRGB profile, made once (Python's lru_cache: every file of a process carries the same bytes,
// the time they were made in them).
const std::string& srgb_icc_once();

}  // namespace genko::formats
