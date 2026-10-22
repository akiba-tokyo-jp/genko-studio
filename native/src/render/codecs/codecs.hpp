#pragma once

#include <string>
#include <string_view>

#include "render/codecs/fidelity.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/limits.hpp"
#include "render/codecs/options.hpp"

// The codecs (PC4 F-0, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/lib.rs, MIT): a file's bytes read into a
// picture of its own depth, and a picture written in a format by the plan fidelity.hpp says. Bytes in, bytes out (the
// callers read and write the files).
//
// PC4 F-0 holds this build's readers and writers behind it (one reader for a format, never two): PNG, JPEG, TIFF, WebP,
// GIF and BMP are read as the drawing reads them (render/png.hpp's open_image: Pillow's modes, 8 bits), a PNG of 16 bits
// and a TIFF of 16 or 32 bits with all their bits (render/smart_decode.hpp's deep_picture); PNG and JPEG are written by
// render::write_png and render::write_jpeg (their pixels only). F-a puts each format's own codec here (TGA, ICO, Netpbm,
// QOI, OpenEXR, Radiance HDR, AVIF; the metadata of PNG, JPEG, TIFF and WebP), F-c the Web's (GIF, PNG-8, WBMP,
// progressive JPEG). The exports' files (formats/pillow_save.hpp: Pillow's bytes) stay as they are.
//
// Errors are core::Error: "unidentified_image" for bytes of no format this build knows, "format" for a file cut short or
// broken, "image_too_large" past the limits (before the pixels are allocated where the header says the size), "value"
// for a picture of no pixels or of the wrong bytes; render::NotYetPorted("image_format") for a format this build does
// not read yet, ("image_write") for one it does not write yet.

namespace genko::render::codecs {

// The format the bytes are (detect), read.
CodecImage decode(std::string_view bytes, const Limits& limits = {});
// The bytes read as `format` (Photoshop's 指定形式で開く: the bytes are still checked to be that format's).
CodecImage decode_as(Format format, std::string_view bytes, const Limits& limits = {});
// The picture written as `format`, converted as plan() says (losses() says what that loses). A fatal loss throws:
// NotYetPorted("image_write") for a format this build does not write, core::Error("image_too_large") past the format's
// size, core::Error("value") for a picture of no pixels.
std::string encode(const CodecImage& image, Format format, const EncodeOptions& options = {});

}  // namespace genko::render::codecs
