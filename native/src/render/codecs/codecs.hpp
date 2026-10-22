#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "render/codecs/fidelity.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/limits.hpp"
#include "render/codecs/options.hpp"
#include "render/image.hpp"

// The codecs (PC4 F-0 and F-a, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/lib.rs, MIT): a file's bytes read into
// a picture of its own depth with its profile and metadata, and a picture written in a format by the plan fidelity.hpp
// says. Bytes in, bytes out (the callers read and write the files).
//
// Each format has one reader and one writer (render/codecs/formats.hpp). PNG, JPEG, TIFF, WebP, GIF and BMP are read as
// the drawing reads them (render/png.hpp's open_image: Pillow's modes, 8 bits), a PNG of 16 bits and a TIFF of 16 or 32
// bits — and a TIFF of CMYK with an alpha, which Pillow does not read — with all their bits (render/smart_decode.hpp's
// deep_picture); their profiles, EXIF, XMP, resolution and text from their containers. TGA, ICO, Netpbm, QOI, Radiance
// HDR, OpenEXR (OpenEXR's library) and AVIF (libavif with libaom) are read and written here. The exports' files
// (formats/pillow_save.hpp: Pillow's bytes) stay as they are: only the new ways in (コピーを保存…, 書き出し形式…, the
// command line's new extensions) write through the codecs.
//
// OpenEXR and Radiance HDR hold linear light (linear_light): an RGB picture of theirs is read with the built-in linear-srgb
// profile (sRGB's primaries, a linear curve) as its own — a grey one with none (no built-in grey is linear: its readers give
// it sRGB's curve, srgb_encoded) —, and a picture of that profile is written to them without a loss said.
//
// Errors are core::Error: "unidentified_image" for bytes of no format this build knows, "format" for a file cut short or
// broken, "image_too_large" past the limits (before the pixels are allocated where the header says the size), "value"
// for a picture of no pixels or of the wrong bytes; render::NotYetPorted("image_format") for a format this build does
// not read, ("image_write") for one it does not write.

namespace genko::render::codecs {

// The format the bytes are (detect), read.
CodecImage decode(std::string_view bytes, const Limits& limits = {});
// The bytes read as `format` (Photoshop's 指定形式で開く: the bytes are still checked to be that format's — a TGA, which
// has no magic number, by its header as its reader reads it).
CodecImage decode_as(Format format, std::string_view bytes, const Limits& limits = {});
// The picture written as `format`, converted as plan() says (losses() says what that loses). A fatal loss throws:
// NotYetPorted("image_write") for a format this build does not write, core::Error("image_too_large") past the format's
// size, core::Error("value") for a picture of no pixels.
std::string encode(const CodecImage& image, Format format, const EncodeOptions& options = {});
// An icon of several sizes (Pillow's ICO of a picture: one entry for each, each at most 256 × 256), each as encode writes
// an ICO's one entry.
std::string encode_icon(std::span<const CodecImage> sizes, const EncodeOptions& options = {});

// The size its header says, without decoding its pixels (none when the bytes are not a picture this build reads, or its
// header cannot be read).
std::optional<std::pair<std::uint32_t, std::uint32_t>> header_size(std::string_view bytes);

// The format holds linear light: OpenEXR, Radiance HDR.
bool linear_light(Format format);
// The format is read by the codecs themselves (TGA, ICO, Netpbm, QOI, OpenEXR, Radiance HDR, AVIF), not by the drawing's
// 8-bit readers: render::open_image reads these through decode and eight_bit.
bool read_here(Format format);

// A picture of linear light (linear_light's formats) with sRGB's curve on its colours (past 1 too: the curve's power part
// goes on; its alpha as it is), as floats of no profile — what the drawing takes as an sRGB picture's numbers.
CodecImage srgb_encoded(const CodecImage& linear);

// The picture as the drawing's 8-bit picture (Pillow's modes): grey "L", grey and alpha "LA", "RGB", "RGBA", "CMYK"
// (CMYK and an alpha: "RGBA" by Pillow's plain CMYK → RGB). Deeper samples rounded to 8 bits; `linear` (linear_light's
// formats) their colours given sRGB's curve first (srgb_encoded), past 1 cut.
Image eight_bit(const CodecImage& picture, bool linear);

// Pillow's own BMP of a picture ("RGB" of 24 bits, "RGBA" of 32 bits — its alpha not named —, "L" of 8 bits with a grey
// table; 96 dpi; without the file header: a DIB), as `genko render --out x.bmp` (and .dib) writes it.
std::string pillow_bmp_bytes(const Image& image, bool file_header = true);

// The versions of the libraries the codecs use, for `genko --version` (api/buildinfo): OpenEXR's, libavif's and the AV1
// codec libavif reads and writes with (libaom).
std::string openexr_version();
std::string libavif_version();
std::string aom_version();

}  // namespace genko::render::codecs
