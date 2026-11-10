#pragma once
#include "render/png.hpp"

namespace genko::render {

// A Photoshop file opened as one picture, as Pillow 12.3's PsdImagePlugin opens it (Image.open, then load()): its merged
// picture — the image data at the file's end, raw or PackBits — in the mode Pillow gives it: "1" (bitmap), "L" (grey,
// duotone, multichannel), "P" (indexed, with the colour mode data's 768-byte palette), "RGB", "RGBA" (RGB of four
// channels), "CMYK", "LAB"; 8 bits (1 for a bitmap). Pillow's own decisions are kept, its odd ones too (PackBits row
// counts read for the mode's channels only, so the extra channels of a file shift the picture; an extra channel of an
// RGB file of four channels is its alpha).
//
// What Pillow refuses, refused alike: core::Error "unidentified_image" where Image.open finds no picture (16 or 32 bits,
// a PSB, a mode Pillow does not read, a size of 0, a header or section cut short), "format" with Pillow's words for its
// OSError ("not enough channels" when it opens; "cannot load this image" for a merged picture of zip, or none, "image
// file is truncated (N bytes not processed)" when it loads), "image_too_large" for a picture past limits.max_pixels
// (Pillow's DecompressionBombError, its words) or limits.max_side. The picture is made only when its pixels fit in
// 1 GiB ("memory" otherwise).
Image read_psd_image(std::string_view bytes, const PngLimits& limits = {});

// Image.open(bytes).size of a PSD: its header and sections read as Image.open reads them (with its refusals), its
// pixels not decoded.
Size psd_image_size(std::string_view bytes, const PngLimits& limits = {});

// Whether these bytes are a PSD whose picture this build cannot use yet: Pillow opens a Lab file as "LAB" and turns it
// into RGB through Little CMS's sRGB and Lab profiles, which is not ported (open_image refuses it as
// NotYetPorted("image_format")). Known by the header alone.
bool psd_lab(std::string_view bytes);

}  // namespace genko::render
