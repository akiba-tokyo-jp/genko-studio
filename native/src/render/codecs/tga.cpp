// TGA (render/codecs/formats.hpp; Truevision's TGA 2.0 specification): read — colour-mapped, true-colour and grey
// pictures (types 1, 2, 3), run-length encoded or not (9, 10, 11); 15 and 16-bit (5 bits a colour, one of alpha when the
// descriptor says so), 24 and 32-bit (BGR, BGRA) colours; grey of 8 bits and grey with an alpha of 16; rows from the
// bottom or the top, columns from the left or the right — and written: grey (8 bits), grey and alpha (16), RGB (24) and
// RGBA (32), run-length encoded row by row, from the top, with TGA 2.0's footer.

#include <algorithm>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs::detail {

namespace {

struct Header {
    std::size_t id = 0;
    unsigned cmap_type = 0, type = 0, cmap_first = 0, cmap_length = 0, cmap_depth = 0;
    std::uint32_t width = 0, height = 0;
    unsigned depth = 0, descriptor = 0;
};

Header header_of(std::string_view b) {
    if (b.size() < 18) broken("TGA", "its header is cut short");
    Header h;
    h.id = u8(b, 0);
    h.cmap_type = u8(b, 1);
    h.type = u8(b, 2);
    h.cmap_first = le16(b, 3);
    h.cmap_length = le16(b, 5);
    h.cmap_depth = u8(b, 7);
    h.width = le16(b, 12);
    h.height = le16(b, 14);
    h.depth = u8(b, 16);
    h.descriptor = u8(b, 17);
    const unsigned base = h.type & 7;
    const bool known = h.type == 1 || h.type == 2 || h.type == 3 || h.type == 9 || h.type == 10 || h.type == 11;
    if (!known || h.width == 0 || h.height == 0) broken("TGA", "an image type or size it cannot have");
    if (base == 1 && (h.cmap_type != 1 || h.depth != 8 || h.cmap_length == 0)) broken("TGA", "a colour map it cannot have");
    if (base == 2 && h.depth != 15 && h.depth != 16 && h.depth != 24 && h.depth != 32) broken("TGA", "a depth it cannot have");
    if (base == 3 && h.depth != 8 && h.depth != 16) broken("TGA", "a depth it cannot have");
    if (h.cmap_type == 1 && h.cmap_depth != 15 && h.cmap_depth != 16 && h.cmap_depth != 24 && h.cmap_depth != 32) broken("TGA", "a colour map it cannot have");
    if (h.cmap_type > 1) broken("TGA", "a colour map it cannot have");
    return h;
}

Layout layout_of(const Header& h) {
    const unsigned base = h.type & 7;
    const unsigned alpha_bits = h.descriptor & 0x0f;
    if (base == 3) return h.depth == 16 ? Layout::GrayA : Layout::Gray;
    const unsigned colour_depth = base == 1 ? h.cmap_depth : h.depth;
    if (colour_depth == 32) return Layout::Rgba;
    if (colour_depth == 16 && alpha_bits == 1) return Layout::Rgba;
    return Layout::Rgb;
}

// One stored pixel (of `bytes` bytes) as the picture's samples.
void colour_of(const unsigned char* p, unsigned depth, Layout layout, unsigned char* out) {
    if (depth == 15 || depth == 16) {
        const unsigned v = p[0] | (p[1] << 8);
        const auto five = [](unsigned c) { return static_cast<unsigned char>((c << 3) | (c >> 2)); };
        out[0] = five((v >> 10) & 31);
        out[1] = five((v >> 5) & 31);
        out[2] = five(v & 31);
        if (layout == Layout::Rgba) out[3] = (v & 0x8000) != 0 ? 255 : 0;
    } else {
        out[0] = p[2];
        out[1] = p[1];
        out[2] = p[0];
        if (layout == Layout::Rgba) out[3] = depth == 32 ? p[3] : 255;
    }
}

void put_rle(std::string& out, const char* row, std::size_t width, std::size_t bpp) {
    std::size_t x = 0;
    const auto same = [&](std::size_t a, std::size_t b) { return std::equal(row + a * bpp, row + a * bpp + bpp, row + b * bpp); };
    while (x < width) {
        std::size_t run = 1;
        while (x + run < width && run < 128 && same(x, x + run)) ++run;
        if (run >= 2) {
            out.push_back(static_cast<char>(0x80 | (run - 1)));
            for (std::size_t i = 0; i < bpp; ++i) out.push_back(row[x * bpp + (bpp >= 3 && i < 3 ? 2 - i : i)]);
            x += run;
            continue;
        }
        std::size_t raw = 1;
        while (x + raw < width && raw < 128 && !(x + raw + 1 < width && same(x + raw, x + raw + 1))) ++raw;
        out.push_back(static_cast<char>(raw - 1));
        for (std::size_t p = x; p < x + raw; ++p) {
            for (std::size_t i = 0; i < bpp; ++i) out.push_back(row[p * bpp + (bpp >= 3 && i < 3 ? 2 - i : i)]);
        }
        x += raw;
    }
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> tga_size(std::string_view bytes) {
    const Header h = header_of(bytes);
    return {h.width, h.height};
}

CodecImage read_tga(std::string_view b, const Limits& limits) {
    const Header h = header_of(b);
    const Layout layout = layout_of(h);
    limits.check(h.width, h.height, layout, Sample::U8);
    const unsigned base = h.type & 7;
    const bool rle = (h.type & 8) != 0;
    std::size_t at = 18 + h.id;
    // (its colour map, as the picture's samples)
    std::vector<unsigned char> map;
    const unsigned out_channels = channels(layout);
    if (h.cmap_type == 1) {
        const std::size_t entry = (h.cmap_depth + 7) / 8;
        const std::size_t length = std::size_t(h.cmap_length) * entry;
        if (at > b.size() || length > b.size() - at) broken("TGA", "its colour map is cut short");
        if (base == 1) {
            map.resize(std::size_t(h.cmap_length) * out_channels);
            for (std::size_t i = 0; i < h.cmap_length; ++i)
                colour_of(reinterpret_cast<const unsigned char*>(b.data() + at + i * entry), h.cmap_depth, layout, map.data() + i * out_channels);
        }
        at += length;
    }
    const std::size_t bpp = (h.depth + 7) / 8;
    const std::size_t count = std::size_t(h.width) * h.height;
    // (the bytes its pixels take at the least — every one, or a packet of 128 a header and one pixel —: a file too short
    // for its size is cut short, said before its pixels are made)
    const std::size_t least = rle ? (count + 127) / 128 * (1 + bpp) : count * bpp;
    if (at > b.size() || least > b.size() - at) broken("TGA", "its pixels are cut short");
    std::string pixels(count * out_channels, '\0');
    auto* out = reinterpret_cast<unsigned char*>(pixels.data());
    const auto put = [&](std::size_t i, const unsigned char* p) {
        unsigned char* to = out + i * out_channels;
        if (base == 1) {
            const unsigned index = p[0];
            if (index < h.cmap_first || index - h.cmap_first >= h.cmap_length) broken("TGA", "a pixel past its colour map");
            std::copy_n(map.data() + std::size_t(index - h.cmap_first) * out_channels, out_channels, to);
        } else if (base == 3) {
            to[0] = p[0];
            if (layout == Layout::GrayA) to[1] = p[1];
        } else {
            colour_of(p, h.depth, layout, to);
        }
    };
    if (!rle) {
        if (at > b.size() || count * bpp > b.size() - at) broken("TGA", "its pixels are cut short");
        for (std::size_t i = 0; i < count; ++i) put(i, reinterpret_cast<const unsigned char*>(b.data() + at + i * bpp));
    } else {
        std::size_t i = 0;
        while (i < count) {
            if (at >= b.size()) broken("TGA", "its pixels are cut short");
            const unsigned packet = u8(b, at++);
            const std::size_t n = std::min<std::size_t>((packet & 0x7f) + 1, count - i);
            if ((packet & 0x80) != 0) {
                if (bpp > b.size() - at) broken("TGA", "its pixels are cut short");
                for (std::size_t k = 0; k < n; ++k) put(i + k, reinterpret_cast<const unsigned char*>(b.data() + at));
                at += bpp;
            } else {
                if (n * bpp > b.size() - at) broken("TGA", "its pixels are cut short");
                for (std::size_t k = 0; k < n; ++k) put(i + k, reinterpret_cast<const unsigned char*>(b.data() + at + k * bpp));
                at += n * bpp;
            }
            i += n;
        }
    }
    // (its rows from the bottom unless the descriptor says the top; its columns from the left unless it says the right)
    const std::size_t row = std::size_t(h.width) * out_channels;
    if ((h.descriptor & 0x20) == 0) {
        for (std::size_t y = 0; y < h.height / 2; ++y)
            std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row), pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                             pixels.begin() + static_cast<std::ptrdiff_t>((h.height - 1 - y) * row));
    }
    if ((h.descriptor & 0x10) != 0) {
        for (std::size_t y = 0; y < h.height; ++y) {
            for (std::size_t x = 0; x < h.width / 2; ++x)
                std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row + x * out_channels),
                                 pixels.begin() + static_cast<std::ptrdiff_t>(y * row + (x + 1) * out_channels),
                                 pixels.begin() + static_cast<std::ptrdiff_t>(y * row + (h.width - 1 - x) * out_channels));
        }
    }
    return from_eight_bit(h.width, h.height, layout, std::move(pixels));
}

std::string write_tga(const CodecImage& stored, const EncodeOptions&) {
    if (stored.sample() != Sample::U8 || is_cmyk(stored.layout())) throw core::Error("value", "TGA holds 8-bit grey and RGB, with or without an alpha");
    const Layout layout = stored.layout();
    const std::size_t bpp = channels(layout);
    std::string out;
    out.push_back('\0');                                       // (no id)
    out.push_back('\0');                                       // (no colour map)
    out.push_back(static_cast<char>(is_gray(layout) ? 11 : 10));  // (run-length encoded grey, true colour)
    out.append(5, '\0');                                       // (the colour map's numbers)
    put_le16(out, 0);
    put_le16(out, 0);
    put_le16(out, stored.width());
    put_le16(out, stored.height());
    out.push_back(static_cast<char>(bpp * 8));
    out.push_back(static_cast<char>(0x20 | (has_alpha(layout) ? 8 : 0)));  // (rows from the top; the alpha's bits)
    const std::size_t row = std::size_t(stored.width()) * bpp;
    for (std::uint32_t y = 0; y < stored.height(); ++y) put_rle(out, stored.data().data() + y * row, stored.width(), bpp);
    out.append(8, '\0');  // (no extension area, no developer area)
    out.append("TRUEVISION-XFILE.\0", 18);
    return out;
}

}  // namespace genko::render::codecs::detail
