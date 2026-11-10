// GIF (render/codecs/formats.hpp): read through this build's GIF reader (its first frame, a transparent entry as an
// alpha), as RGBA; written as one picture (GIF89a): its colours — all of them when they are 256 or fewer, else 256 by
// median cut (render/quantize, PhotoCraft's adaptive palette, no dither) —, a pixel more transparent than half one
// transparent entry, the indices by GIF's LZW (render/movie's).

#include <cmath>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/movie.hpp"
#include "render/quantize.hpp"

namespace genko::render::codecs::detail {

CodecImage read_gif(std::string_view bytes, const Limits& limits) {
    CodecImage out = this_builds_picture(bytes, limits, false);
    return out.layout() == Layout::Rgba ? out : out.convert(Layout::Rgba, Sample::U8);
}

std::string write_gif(const CodecImage& stored, const EncodeOptions&) {
    if (stored.sample() != Sample::U8 || stored.layout() != Layout::Rgba) throw core::Error("value", "GIF is written from 8-bit RGBA");
    const std::size_t w = stored.width(), n = stored.pixel_count();
    std::vector<quantize::Rgba> px(n);
    const auto* d = reinterpret_cast<const unsigned char*>(stored.data().data());
    bool transparent = false;
    for (std::size_t i = 0; i < n; ++i) {
        px[i] = {d[i * 4] / 255.0f, d[i * 4 + 1] / 255.0f, d[i * 4 + 2] / 255.0f, d[i * 4 + 3] / 255.0f};
        if (px[i][3] < 0.5f) transparent = true;
    }
    std::vector<quantize::Rgb8> palette = quantize::build_palette(px, quantize::PaletteKind::Adaptive, transparent ? 255 : 256, quantize::Forced::None);
    if (palette.empty()) palette.push_back({0, 0, 0});  // (every pixel transparent)
    const std::optional<std::size_t> clear = transparent ? std::optional<std::size_t>(palette.size()) : std::nullopt;
    const std::vector<std::uint8_t> indices = quantize::quantize(px, w, palette, quantize::Dither::None, 0.0f, clear);
    if (clear) palette.push_back({0, 0, 0});
    int bits = 1;
    while ((std::size_t(1) << bits) < palette.size()) ++bits;

    std::string out("GIF89a", 6);
    put_le16(out, stored.width());
    put_le16(out, stored.height());
    out.push_back(static_cast<char>(0x80 | ((bits - 1) << 4) | (bits - 1)));  // (a global colour table of 2^bits)
    out.push_back('\0');  // (the background's entry)
    out.push_back('\0');  // (no aspect ratio)
    for (std::size_t i = 0; i < (std::size_t(1) << bits); ++i) {
        const quantize::Rgb8 c = i < palette.size() ? palette[i] : quantize::Rgb8{0, 0, 0};
        out.push_back(static_cast<char>(c[0]));
        out.push_back(static_cast<char>(c[1]));
        out.push_back(static_cast<char>(c[2]));
    }
    if (clear) {  // (a graphic control extension: its transparent entry)
        out += std::string("\x21\xF9\x04\x01\x00\x00", 6);
        out.push_back(static_cast<char>(*clear));
        out.push_back('\0');
    }
    out.push_back('\x2C');  // (the picture: at 0, 0, of the screen's size, no table of its own, not interlaced)
    put_le16(out, 0);
    put_le16(out, 0);
    put_le16(out, stored.width());
    put_le16(out, stored.height());
    out.push_back('\0');
    const int min_bits = std::max(2, bits);
    out.push_back(static_cast<char>(min_bits));
    out += movie::gif_lzw(indices, min_bits);
    out.push_back('\x3B');
    return out;
}

}  // namespace genko::render::codecs::detail
