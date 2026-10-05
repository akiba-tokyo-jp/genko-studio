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
};

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
// Other formats Pillow opens (GIF, TIFF, WebP, PSD) throw NotYetPorted("image_format");
// anything else core::Error("unidentified_image").
Image open_image(std::string_view bytes, const PngLimits& limits = {});

// The image as PNG bytes ("1", "L", "LA", "I;16", "RGB", "RGBA" and "P"); compress_level as zlib's (0..9).
std::string write_png(const Image& image, int compress_level = 6);

// write_png to a file (core::Error("io") when it cannot be written).
void save_png(const Image& image, const std::filesystem::path& path, int compress_level = 6);

}  // namespace genko::render
