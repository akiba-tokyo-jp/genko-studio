#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "render/codecs/image.hpp"

// The formats the codecs know (PC4 F-0, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/format.rs, MIT): their
// names and file extensions, what each holds and what this build reads and writes of it (caps), and how a file's first
// bytes say which it is (detect). A PSD or PSB, a camera RAW file and a .pcraft are not codecs' formats: render/psd,
// render/raw (F-e) and render/pcraft_read (F-f) read them (formats/open_file).
//
// caps() is this build's table: `read` and `write` say what it does now, and the depths, layouts and metadata of a
// format it writes are what its writer keeps — the plan of a write and the losses it says (render/codecs/fidelity.hpp)
// are made from them, so a loss is said exactly when the file changes. PC4 F-0 reads PNG, JPEG, TIFF, WebP, GIF and BMP
// through this build's readers (render/png.hpp, render/smart_decode.hpp) and writes PNG and JPEG through its writers
// (render::write_png, render::write_jpeg: the pixels only); F-a widens each row as it writes the format's own codec.

namespace genko::render::codecs {

enum class Format { Png, Jpeg, Tiff, WebP, Gif, Bmp, Tga, Ico, Pnm, Qoi, OpenExr, Hdr, Avif };

inline constexpr std::array<Format, 13> kFormats{Format::Png, Format::Jpeg, Format::Tiff, Format::WebP, Format::Gif,
                                                 Format::Bmp, Format::Tga,  Format::Ico,  Format::Pnm,  Format::Qoi,
                                                 Format::OpenExr, Format::Hdr, Format::Avif};

struct FormatCaps {
    bool read = false;   // this build decodes it
    bool write = false;  // this build encodes it
    std::span<const Sample> depths;   // the samples written without a change
    std::span<const Layout> layouts;  // the layouts written (and read back) without a change
    bool alpha = false;
    bool icc = false;
    bool exif = false;
    bool xmp = false;
    bool dpi = false;        // the resolution
    bool text = false;       // free key and value text
    bool animation = false;  // the container can hold frames (only the first is read, one is written)
    bool lossy = false;      // its writer loses what is not seen (lossy compression, quantization)
};

FormatCaps caps(Format format);
// Readable or writable in this build.
bool enabled(Format format);
std::string_view name(Format format);       // "PNG", "JPEG", "Netpbm", "OpenEXR", "Radiance HDR", …
std::string_view id_of(Format format);      // "png", "jpeg", "tiff", "webp", "gif", "bmp", "tga", "ico", "pnm", "qoi", "exr", "hdr", "avif"
std::optional<Format> from_id(std::string_view id);
// Its file extensions (lower case, no dot), the one written first.
std::span<const std::string_view> extensions(Format format);
std::string_view mime_type(Format format);
// The largest picture the format holds (width, height), when it has a limit.
std::optional<std::pair<std::uint32_t, std::uint32_t>> max_dimensions(Format format);

// The format the first bytes say (magic numbers; TGA, which has none, last by its header's sense), or none.
std::optional<Format> detect(std::string_view bytes);
// The format of an extension (any case, with or without its dot, or a whole file name or path), or none.
std::optional<Format> from_extension(std::string_view extension);

// The formats this build reads and does not write, or writes and does not read — each with why (the others it has are
// both: every format written is read back).
struct Asymmetric {
    Format format;
    std::string_view reason;
};
std::span<const Asymmetric> asymmetric_formats();

}  // namespace genko::render::codecs
