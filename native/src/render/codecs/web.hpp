#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/codecs/limits.hpp"

// Web 用に保存（従来）'s own writers (PC4 F-c, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/web.rs, MIT): a GIF89a
// of a palette's indices (a transparent entry, interlaced or not; GIF's LZW is render/movie's), a PNG of a palette (PNG-8:
// PLTE and tRNS, its bits as few as the palette needs) and WBMP (WAP's wireless bitmap, type 0: one bit a pixel, 1 white)
// written — WBMP read back too, every writer here having its reader (a GIF and a PNG through the codecs' decode). A JPEG's
// progressive scans and optimized tables are the codecs' JPEG writer's (EncodeOptions::jpeg_progressive, jpeg_optimize).
//
// Each writer hands its bytes to `sink` as it makes them (a file written beside its place, or only counted for an
// estimate: never the whole file held), or returns them. core::Error("value") for indices that are not the picture's or a
// palette that does not cover them, ("image_too_large") for a GIF past 65535 pixels a side.

namespace genko::render::codecs::web {

using Rgb8 = std::array<std::uint8_t, 3>;
using Sink = std::function<void(std::string_view)>;

// A GIF89a: one global table of the palette (1..256 entries, padded to a power of two), one index a pixel (rows from the
// top), the transparent entry in a graphic control extension, the rows interlaced (GIF's four passes) or in order.
void write_gif_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                       std::optional<std::uint8_t> transparent, bool interlaced, const Sink& sink);
std::string encode_gif_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                               std::optional<std::uint8_t> transparent, bool interlaced);

// A PNG of a palette (colour type 3) of 1, 2, 4 or 8 bits, the transparent entry alone transparent (tRNS), Adam7 when
// `interlaced`, the resolution (pHYs) when given.
void write_png_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                       std::optional<std::uint8_t> transparent, bool interlaced, std::optional<double> dpi, const Sink& sink);
std::string encode_png_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                               std::optional<std::uint8_t> transparent, bool interlaced, std::optional<double> dpi = std::nullopt);

// WBMP type 0: `white` one a pixel (non-zero: white), rows padded to bytes.
void write_wbmp(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> white, const Sink& sink);
std::string encode_wbmp(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> white);

struct Wbmp {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> white;  // 1 white, 0 black, one a pixel
};
// A WBMP read: core::Error("format") for bytes that are not a type 0 WBMP or are cut short, ("image_too_large") past the
// limits — its size checked before its pixels are made.
Wbmp decode_wbmp(std::string_view bytes, const Limits& limits = {});

}  // namespace genko::render::codecs::web
