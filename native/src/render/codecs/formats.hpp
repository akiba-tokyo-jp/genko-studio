#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "render/codecs/image.hpp"
#include "render/codecs/limits.hpp"
#include "render/codecs/options.hpp"

// The codecs' formats, one reader and one writer each (PC4 F-a, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/codecs,
// MIT): what render/codecs/codecs.cpp dispatches to. A reader checks the picture its header says against the limits before
// any pixel is allocated and fills the picture's profile and metadata as its file holds them; a writer writes the picture
// already converted to the plan (render/codecs/fidelity.hpp: a layout and a sample the format holds) with what of its
// profile and metadata the options embed. PNG, JPEG, TIFF, WebP, GIF and BMP read their pixels through this build's own
// readers (render/png.hpp's open_image: Pillow's; a PNG of 16 bits and a TIFF of 16 or 32 bits through render/smart_decode's
// readers with all their bits), so that a picture is read the same everywhere; the others are read here.
// Internal to render/codecs.

namespace genko::render::codecs::detail {

// --- reading -------------------------------------------------------------------------------------------------------------

CodecImage read_png(std::string_view bytes, const Limits& limits);
CodecImage read_jpeg(std::string_view bytes, const Limits& limits);
CodecImage read_tiff(std::string_view bytes, const Limits& limits);
CodecImage read_webp(std::string_view bytes, const Limits& limits);
CodecImage read_gif(std::string_view bytes, const Limits& limits);
CodecImage read_bmp(std::string_view bytes, const Limits& limits);
CodecImage read_tga(std::string_view bytes, const Limits& limits);
CodecImage read_ico(std::string_view bytes, const Limits& limits);
CodecImage read_pnm(std::string_view bytes, const Limits& limits);
CodecImage read_qoi(std::string_view bytes, const Limits& limits);
CodecImage read_exr(std::string_view bytes, const Limits& limits);
CodecImage read_hdr(std::string_view bytes, const Limits& limits);
CodecImage read_avif(std::string_view bytes, const Limits& limits);

// The size a file of the formats read here says in its header (no pixel decoded): core::Error("format") when the header
// cannot be read.
std::pair<std::uint32_t, std::uint32_t> tga_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> ico_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> pnm_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> qoi_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> exr_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> hdr_size(std::string_view bytes);
std::pair<std::uint32_t, std::uint32_t> avif_size(std::string_view bytes);

// --- writing (the picture as the plan stores it) ------------------------------------------------------------------------

std::string write_png(const CodecImage& stored, const EncodeOptions& options);
std::string write_jpeg(const CodecImage& stored, const EncodeOptions& options);
std::string write_tiff(const CodecImage& stored, const EncodeOptions& options);
std::string write_webp(const CodecImage& stored, const EncodeOptions& options);
std::string write_gif(const CodecImage& stored, const EncodeOptions& options);
std::string write_bmp(const CodecImage& stored, const EncodeOptions& options);
std::string write_tga(const CodecImage& stored, const EncodeOptions& options);
std::string write_ico(std::span<const CodecImage> stored, const EncodeOptions& options);
std::string write_pnm(const CodecImage& stored, const EncodeOptions& options);
std::string write_qoi(const CodecImage& stored, const EncodeOptions& options);
std::string write_exr(const CodecImage& stored, const EncodeOptions& options);
std::string write_hdr(const CodecImage& stored, const EncodeOptions& options);
std::string write_avif(const CodecImage& stored, const EncodeOptions& options);

// --- what the writers share ---------------------------------------------------------------------------------------------

// The most bytes an EXIF block and an XMP packet of a JPEG take (one APP1 segment each: 65533 bytes less its name).
inline constexpr std::size_t kJpegExifMax = 65533 - 6;
inline constexpr std::size_t kJpegXmpMax = 65533 - 29;
// TIFF's text tags (ImageDescription, Make, Model, Software, DateTime, Artist, Copyright) by the keys the picture's text
// has them under (PhotoCraft's), and the tag of a key (none for a key TIFF has no tag for).
std::optional<std::uint16_t> tiff_text_tag(std::string_view key);
std::span<const std::pair<std::uint16_t, std::string_view>> tiff_text_tags();
// A PNG text chunk's keyword: 1 to 79 Latin-1 characters, no NUL, as Latin-1 bytes (none for a key that cannot be one).
std::optional<std::string> png_keyword(std::string_view key);
// The most bytes an ICC profile of a JPEG takes (255 APP2 segments of 65519 bytes).
inline constexpr std::size_t kJpegIccMax = std::size_t(255) * 65519;

// The picture made from straight interleaved 8-bit samples.
CodecImage from_eight_bit(std::uint32_t width, std::uint32_t height, Layout layout, std::string data);
// The picture this build's own readers read (PNG, JPEG, TIFF, WebP, GIF, BMP): its size and the readers' work checked
// against the limits on its header first; then open_image's picture in Pillow's mode as the codecs' (8 bits; a colour
// table's colours, a transparent colour or entry as an alpha), or — `deep` — render/smart_decode's deep_picture with all its
// bits (a PNG of 16 bits, a TIFF of 8, 16 or 32 bits of grey, RGB or CMYK with or without an alpha). `size`: its size as
// the caller has read it from its header (a deep TIFF's, whose samples Pillow's table of modes does not know — a grey and
// alpha of 16 bits, an RGB of floats —); without it, the size render/png's picture_size reads.
CodecImage this_builds_picture(std::string_view bytes, const Limits& limits, bool deep,
                               std::optional<std::pair<std::uint32_t, std::uint32_t>> size = std::nullopt);

// Byte order helpers.
inline unsigned u8(std::string_view b, std::size_t at) { return static_cast<unsigned char>(b[at]); }
inline std::uint32_t le16(std::string_view b, std::size_t at) { return u8(b, at) | (u8(b, at + 1) << 8); }
inline std::uint32_t le32(std::string_view b, std::size_t at) { return le16(b, at) | (le16(b, at + 2) << 16); }
inline std::uint32_t be16(std::string_view b, std::size_t at) { return (u8(b, at) << 8) | u8(b, at + 1); }
inline std::uint32_t be32(std::string_view b, std::size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }
inline void put_le16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
inline void put_le32(std::string& out, std::uint32_t v) {
    put_le16(out, v & 0xffff);
    put_le16(out, v >> 16);
}
inline void put_be16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}
inline void put_be32(std::string& out, std::uint32_t v) {
    put_be16(out, v >> 16);
    put_be16(out, v & 0xffff);
}

// "format": a file of `what` cut short or broken.
[[noreturn]] void broken(std::string_view what, std::string_view why);

}  // namespace genko::render::codecs::detail
