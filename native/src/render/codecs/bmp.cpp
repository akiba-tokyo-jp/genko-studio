// BMP (render/codecs/formats.hpp): read through this build's BMP reader (as the drawing reads it) with its resolution
// (pixels a metre); written as its reader reads it back: RGB as 24 bits (Pillow's BMP), RGBA as 32 bits with its alpha
// named by bitfields (a BITMAPV4HEADER: Pillow's own BMP of RGBA leaves the alpha unnamed, and readers drop it).
// pillow_bmp_bytes is Pillow's own BMP (`genko render --out x.bmp` and .dib, M5).

#include <algorithm>
#include <array>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs {

namespace {

std::uint32_t pixels_a_metre(double dpi) { return static_cast<std::uint32_t>(std::max(0.0, std::min(dpi * 39.3701 + 0.5, 2147483647.0))); }

void le(std::string& out, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

}  // namespace

std::string pillow_bmp_bytes(const Image& image, bool file_header) {
    const std::string mode(image.mode());
    int bits = 0;
    std::vector<std::array<std::uint8_t, 3>> palette;
    if (mode == "RGB") {
        bits = 24;
    } else if (mode == "RGBA") {
        bits = 32;
    } else if (mode == "L") {
        bits = 8;
        for (int i = 0; i < 256; ++i) palette.push_back({static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i)});
    } else {
        throw core::Error("io", "cannot write mode " + mode + " as BMP");
    }
    const std::int64_t w = image.width(), h = image.height();
    const std::int64_t stride = ((w * bits + 7) / 8 + 3) & ~std::int64_t{3};
    const std::int64_t offset = 14 + 40 + static_cast<std::int64_t>(palette.size()) * 4;
    const std::int64_t ppm = static_cast<std::int64_t>(96 * 39.3701 + 0.5);
    std::string out;
    if (file_header) {
        if (offset + stride * h > 0xFFFFFFFFLL) throw core::PyValueError("File size is too large for the BMP format");
        out += "BM";
        le(out, static_cast<std::uint64_t>(offset + stride * h), 4);
        le(out, 0, 4);
        le(out, static_cast<std::uint64_t>(offset), 4);
    }
    le(out, 40, 4);
    le(out, static_cast<std::uint64_t>(w), 4);
    le(out, static_cast<std::uint64_t>(h), 4);
    le(out, 1, 2);
    le(out, static_cast<std::uint64_t>(bits), 2);
    le(out, 0, 4);
    le(out, static_cast<std::uint64_t>(stride * h), 4);
    le(out, static_cast<std::uint64_t>(ppm), 4);
    le(out, static_cast<std::uint64_t>(ppm), 4);
    le(out, palette.size(), 4);
    le(out, palette.size(), 4);
    for (const auto& c : palette) {
        out.push_back(static_cast<char>(c[2]));
        out.push_back(static_cast<char>(c[1]));
        out.push_back(static_cast<char>(c[0]));
        out.push_back(0);
    }
    const std::string raw = image.tobytes();
    const int bands = image.bands();
    for (std::int64_t y = h - 1; y >= 0; --y) {
        std::string row;
        row.reserve(static_cast<std::size_t>(stride));
        for (std::int64_t x = 0; x < w; ++x) {
            const char* px = raw.data() + (y * w + x) * bands;
            if (bands == 1) {
                row.push_back(px[0]);
            } else {
                row.push_back(px[2]);
                row.push_back(px[1]);
                row.push_back(px[0]);
                if (bands == 4) row.push_back(px[3]);
            }
        }
        row.resize(static_cast<std::size_t>(stride), '\0');
        out += row;
    }
    return out;
}

namespace detail {

CodecImage read_bmp(std::string_view bytes, const Limits& limits) {
    CodecImage out = this_builds_picture(bytes, limits, false);
    if (bytes.size() >= 46 && le32(bytes, 14) >= 40) {
        const std::uint32_t x = le32(bytes, 38), y = le32(bytes, 42);
        if (x > 0 && y > 0 && x < 0x80000000u && y < 0x80000000u) out.meta.dpi = std::make_pair(x * 0.0254, y * 0.0254);
    }
    return out;
}

std::string write_bmp(const CodecImage& stored, const EncodeOptions&) {
    if (stored.sample() != Sample::U8 || !is_rgb(stored.layout())) throw core::Error("value", "BMP is written from 8-bit RGB, with or without an alpha");
    const bool alpha = stored.layout() == Layout::Rgba;
    const int bits = alpha ? 32 : 24;
    const std::uint64_t w = stored.width(), h = stored.height();
    const std::uint64_t stride = ((w * bits + 7) / 8 + 3) & ~std::uint64_t{3};
    const std::uint64_t header = alpha ? 108 : 40;
    const std::uint64_t offset = 14 + header;
    if (offset + stride * h > 0xFFFFFFFFULL) throw core::Error("image_too_large", "the picture is too large for a BMP file (4 GiB)");
    const double dx = stored.meta.dpi ? stored.meta.dpi->first : 96.0, dy = stored.meta.dpi ? stored.meta.dpi->second : 96.0;
    std::string out;
    out.reserve(static_cast<std::size_t>(offset + stride * h));
    out += "BM";
    le(out, offset + stride * h, 4);
    le(out, 0, 4);
    le(out, offset, 4);
    le(out, header, 4);
    le(out, w, 4);
    le(out, h, 4);  // (rows from the bottom)
    le(out, 1, 2);
    le(out, static_cast<std::uint64_t>(bits), 2);
    le(out, alpha ? 3 : 0, 4);  // (BI_BITFIELDS: the masks below; BI_RGB)
    le(out, stride * h, 4);
    le(out, pixels_a_metre(dx), 4);
    le(out, pixels_a_metre(dy), 4);
    le(out, 0, 4);
    le(out, 0, 4);
    if (alpha) {
        le(out, 0x00ff0000, 4);
        le(out, 0x0000ff00, 4);
        le(out, 0x000000ff, 4);
        le(out, 0xff000000, 4);
        le(out, 0x73524742, 4);  // (LCS_sRGB)
        out.append(36 + 12, '\0');  // (the end points and the gammas: none)
    }
    const std::string& raw = stored.data();
    const unsigned bands = alpha ? 4 : 3;
    std::string row;
    for (std::uint64_t y = h; y-- > 0;) {
        row.assign(static_cast<std::size_t>(stride), '\0');
        const char* from = raw.data() + y * w * bands;
        for (std::uint64_t x = 0; x < w; ++x) {
            row[x * bands] = from[x * bands + 2];
            row[x * bands + 1] = from[x * bands + 1];
            row[x * bands + 2] = from[x * bands];
            if (alpha) row[x * bands + 3] = from[x * bands + 3];
        }
        out += row;
    }
    return out;
}

}  // namespace detail

}  // namespace genko::render::codecs
