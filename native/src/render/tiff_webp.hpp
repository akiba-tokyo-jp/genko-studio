#pragma once

#include <string_view>

#include "render/png.hpp"

// TIFF and WebP pictures read as Pillow 12 reads them (TiffImagePlugin through libtiff, WebPImagePlugin through
// libwebp's animation decoder): the first picture (frame) of the file, in Pillow's mode for it.

namespace genko::render {

// The first bytes of a TIFF (Pillow's PREFIXES, BigTIFF too) and of a WebP (RIFF … WEBP with a VP8, VP8L or VP8X chunk).
bool tiff_signature(std::string_view bytes);
bool webp_signature(std::string_view bytes);

// Image.open(bytes) and load() of a TIFF: its first directory, mode and raw mode by Pillow's OPEN_INFO, the samples as
// libtiff decodes them (any compression libtiff reads; old-style YCbCr through its RGBA reader, JPEG's YCbCr as RGB), the
// palette, and the EXIF orientation applied (ImageOps.exif_transpose). core::Error("unidentified_image") where Pillow
// cannot open it, ("format") where its pixels cannot be decoded, ("image_too_large") past the limits; and
// NotYetPorted("image_format") for the ones this build does not read yet (several samples in separate planes of fewer
// than 8 bits; planes of 8, 16 or 32 bits are read and put side by side, YCbCr ones through libtiff's RGBA reader).
Image read_tiff(std::string_view bytes, const PngLimits& limits = {});
// Image.open(bytes).size of a TIFF (its directory read, the pixels not decoded).
Size tiff_size(std::string_view bytes, const PngLimits& limits = {});
// A TIFF this build does not read yet (read_tiff's NotYetPorted), known from its directory without its pixels; false
// for any other bytes (a TIFF that cannot be read at all included).
bool tiff_unported(std::string_view bytes);

// The same for a WebP: RGB, or RGBA when it has alpha (the first frame of an animation, on its canvas).
Image read_webp(std::string_view bytes, const PngLimits& limits = {});
Size webp_size(std::string_view bytes, const PngLimits& limits = {});

}  // namespace genko::render
