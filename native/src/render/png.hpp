#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "render/image.hpp"

// PNG files with libpng. Reading gives the mode, pixels and transparency Pillow's PngImagePlugin gives for the same
// file (Image.open(…) then load()): grey 1 bit → "1", 2/4/8 bits → "L", 16 bits → "I;16"; RGB 8/16 → "RGB";
// palette → "P" with its palette and tRNS; grey+alpha 8 → "LA", 16 → "RGBA"; RGBA 8/16 → "RGBA". The raw rows are
// unpacked with Pillow's own unpackers. Writing keeps the pixels (the bytes may differ from Pillow's).

namespace genko::render {

struct PngLimits {
    // Refused above this many pixels (width × height), before anything is decoded.
    std::int64_t max_pixels = 400'000'000;
    // Refused above this many pixels on either side, before anything is decoded (0: no such limit). PNG, JPEG, BMP and
    // GIF (its screen and its first frame) say so; the paper texture's pictures use it (render/paper.hpp).
    std::int64_t max_side = 0;
};

// Whether a picture of w × h pixels is beyond limits.max_side (when it has one).
inline bool beyond_side(std::int64_t w, std::int64_t h, const PngLimits& limits) {
    return limits.max_side > 0 && (w > limits.max_side || h > limits.max_side);
}

// What Pillow's Image.open refuses (DecompressionBombError above twice MAX_IMAGE_PIXELS): the drawing opens the
// pictures of a book with this, as the Python baseline does.
inline constexpr PngLimits kPillowOpenLimits{2 * kMaxImagePixels};

// Throws core::Error("unidentified_image") for bytes that are not a PNG or whose chunks before the image data are
// broken (where Pillow's Image.open fails), core::Error("format") for image data that cannot be decoded (where
// Pillow's load() fails), core::Error("image_too_large") for one that is too big.
Image read_png(std::string_view bytes, const PngLimits& limits = {});

// Image.open(bytes) and load() for PNG, 8-bit JPEG (L/RGB/CMYK, including progressive),
// and BMP (1/4/8/16/24/32-bit, OS2, bitfields, RLE4/8). Caps precede pixel allocation.
// Decoders reject incomplete pixel output; BMP retains complete short RLE packets and optional row padding.
// GIF87a/89a first-frame P/L pixels: global/local palette, transparency, frame extents and interlace.
// Other GIF metadata extensions are skipped as Pillow does; the first frame is decoded completely.
// PSD as one picture (its merged picture, as Pillow's PsdImagePlugin: render/psd_image.hpp), but for one of Lab
// colours (Pillow turns those into RGB through Little CMS, not ported yet).
// That and other formats Pillow opens (TIFF, WebP) throw NotYetPorted("image_format");
// anything else core::Error("unidentified_image").
Image open_image(std::string_view bytes, const PngLimits& limits = {});
// Image.open(bytes).size: a PNG's from its chunks before the image data, read as Pillow reads them when it opens a file
// (its pixels are not decoded); the other pictures open_image reads, decoded. core::Error as read_png and open_image throw
// them: "unidentified_image", "image_too_large" (above kPillowOpenLimits: Pillow's DecompressionBombError), "format"; and
// NotYetPorted("image_format").
Size picture_size(std::string_view bytes);
// Whether open_image refuses these bytes as one of those other formats (NotYetPorted("image_format")): known by their
// first bytes, without reading the rest.
bool unported_image_format(std::string_view bytes);

// The image as PNG bytes ("1", "L", "LA", "I;16", "RGB", "RGBA" and "P"); compress_level as zlib's (0..9).
std::string write_png(const Image& image, int compress_level = 6);

// The image as JPEG bytes (an RGB copy of it), as Pillow's save(…, quality=q) with its other defaults writes it: libjpeg's
// defaults with jpeg_set_quality(q, force_baseline), 4:2:0, no density. core::Error("format") when libjpeg fails.
std::string write_jpeg(const Image& image, int quality = 85);

// write_png to a file (core::Error("io") when it cannot be written).
void save_png(const Image& image, const std::filesystem::path& path, int compress_level = 6);

}  // namespace genko::render
