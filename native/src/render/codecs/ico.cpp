// ICO (render/codecs/formats.hpp): an icon's largest picture read — a PNG (this build's PNG reader) or a device-independent
// bitmap of 1, 4, 8, 24 or 32 bits with its AND mask (a 32-bit one's own alpha, its mask where its alpha is all clear) —,
// as RGBA; written as PNG pictures of at most 256 × 256 (one, or one for each size of encode_icon).

#include <algorithm>
#include <array>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs::detail {

namespace {

struct Entry {
    std::uint32_t width = 0, height = 0;  // (as the directory says: 0 is 256)
    unsigned bits = 0;
    std::size_t offset = 0, size = 0;
};

std::vector<Entry> directory(std::string_view b) {
    if (b.size() < 6 || le16(b, 0) != 0 || (le16(b, 2) != 1 && le16(b, 2) != 2)) broken("ICO", "its header");
    const std::size_t count = le16(b, 4);
    if (count == 0 || 6 + count * 16 > b.size()) broken("ICO", "its directory is cut short");
    std::vector<Entry> out;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = 6 + i * 16;
        Entry e;
        e.width = u8(b, at) == 0 ? 256 : u8(b, at);
        e.height = u8(b, at + 1) == 0 ? 256 : u8(b, at + 1);
        e.bits = le16(b, at + 6);
        e.size = le32(b, at + 8);
        e.offset = le32(b, at + 12);
        if (e.offset > b.size() || e.size > b.size() - e.offset || e.size < 8) continue;  // (an entry past the file: not taken)
        out.push_back(e);
    }
    if (out.empty()) broken("ICO", "no picture in it can be read");
    return out;
}

// The entry read: the largest, then the deepest (as Pillow's IcoFile picks it).
Entry largest(const std::vector<Entry>& entries) {
    return *std::max_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const std::uint64_t pa = std::uint64_t(a.width) * a.height, pb = std::uint64_t(b.width) * b.height;
        return pa != pb ? pa < pb : a.bits < b.bits;
    });
}

CodecImage dib(std::string_view d, const Limits& limits) {
    if (d.size() < 40 || le32(d, 0) < 40 || le32(d, 0) > d.size()) broken("ICO", "a bitmap's header");
    const std::size_t header = le32(d, 0);
    const auto width = static_cast<std::int32_t>(le32(d, 4)), doubled = static_cast<std::int32_t>(le32(d, 8));
    const unsigned bits = le16(d, 14);
    const std::uint32_t compression = le32(d, 16);
    if (width <= 0 || width > 1024 || doubled <= 0 || doubled > 2048 || doubled % 2 != 0) broken("ICO", "a bitmap's size");
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) broken("ICO", "a bitmap's depth");
    if (compression != 0 && !(compression == 3 && bits == 32)) broken("ICO", "a bitmap's compression");
    const auto w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(doubled / 2);
    limits.check(w, h, Layout::Rgba, Sample::U8);
    std::size_t at = header + (compression == 3 && header == 40 ? 12 : 0);
    std::vector<std::array<unsigned char, 3>> palette;
    if (bits <= 8) {
        std::size_t colours = le32(d, 32);
        if (colours == 0 || colours > (std::size_t(1) << bits)) colours = std::size_t(1) << bits;
        if (at > d.size() || colours * 4 > d.size() - at) broken("ICO", "a bitmap's colour table is cut short");
        for (std::size_t i = 0; i < colours; ++i) palette.push_back({static_cast<unsigned char>(d[at + i * 4 + 2]), static_cast<unsigned char>(d[at + i * 4 + 1]),
                                                                     static_cast<unsigned char>(d[at + i * 4])});
        at += colours * 4;
    }
    const std::size_t stride = (std::size_t(w) * bits + 31) / 32 * 4, mask_stride = (std::size_t(w) + 31) / 32 * 4;
    if (at > d.size() || stride * h > d.size() - at) broken("ICO", "a bitmap's pixels are cut short");
    const std::size_t mask_at = at + stride * h;
    const bool masked = mask_at <= d.size() && mask_stride * h <= d.size() - mask_at;
    std::string out(std::size_t(w) * h * 4, '\0');
    bool any_alpha = false;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::size_t row = at + std::size_t(h - 1 - y) * stride;  // (rows from the bottom)
        for (std::uint32_t x = 0; x < w; ++x) {
            unsigned char* to = reinterpret_cast<unsigned char*>(out.data()) + (std::size_t(y) * w + x) * 4;
            if (bits <= 8) {
                const unsigned byte = u8(d, row + x * bits / 8);
                const unsigned index = (byte >> (8 - bits - (x * bits) % 8)) & ((1u << bits) - 1);
                const auto c = index < palette.size() ? palette[index] : std::array<unsigned char, 3>{0, 0, 0};
                to[0] = c[0];
                to[1] = c[1];
                to[2] = c[2];
                to[3] = 255;
            } else {
                const std::size_t p = row + std::size_t(x) * (bits / 8);
                to[0] = static_cast<unsigned char>(d[p + 2]);
                to[1] = static_cast<unsigned char>(d[p + 1]);
                to[2] = static_cast<unsigned char>(d[p]);
                to[3] = bits == 32 ? static_cast<unsigned char>(d[p + 3]) : 255;
                if (bits == 32 && to[3] != 0) any_alpha = true;
            }
        }
    }
    // (the AND mask: a set bit is a transparent pixel — for a 32-bit bitmap only where its own alpha is all clear)
    if (masked && (bits != 32 || !any_alpha)) {
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::size_t row = mask_at + std::size_t(h - 1 - y) * mask_stride;
            for (std::uint32_t x = 0; x < w; ++x) {
                const bool clear = ((u8(d, row + x / 8) >> (7 - x % 8)) & 1) != 0;
                out[(std::size_t(y) * w + x) * 4 + 3] = clear ? '\0' : static_cast<char>(0xff);
            }
        }
    }
    return from_eight_bit(w, h, Layout::Rgba, std::move(out));
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> ico_size(std::string_view bytes) {
    // (the size its picture says — a PNG's header, a bitmap's —: the directory's, at most 256, may not be it)
    const Entry e = largest(directory(bytes));
    const std::string_view data = bytes.substr(e.offset, e.size);
    if (data.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) && data.size() >= 24 && data.substr(12, 4) == "IHDR") return {be32(data, 16), be32(data, 20)};
    if (!data.starts_with(std::string_view("\x89PNG", 4)) && data.size() >= 40 && le32(data, 0) >= 40) {
        const auto width = static_cast<std::int32_t>(le32(data, 4)), doubled = static_cast<std::int32_t>(le32(data, 8));
        if (width > 0 && doubled > 1) return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(doubled / 2)};
    }
    return {e.width, e.height};
}

CodecImage read_ico(std::string_view b, const Limits& limits) {
    const Entry e = largest(directory(b));
    const std::string_view data = b.substr(e.offset, e.size);
    CodecImage out = data.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) ? read_png(data, limits) : dib(data, limits);
    out.icc.reset();
    out.meta = Metadata{};
    return out.layout() == Layout::Rgba && out.sample() == Sample::U8 ? out : out.convert(Layout::Rgba, Sample::U8);
}

std::string write_ico(std::span<const CodecImage> stored, const EncodeOptions& options) {
    std::vector<std::string> pictures;
    EncodeOptions png = options;
    png.png_interlaced = false;
    for (const CodecImage& image : stored) {
        if (image.width() > 256 || image.height() > 256) throw core::Error("image_too_large", "ICO holds at most 256×256 pixels");
        CodecImage bare = image.convert(Layout::Rgba, Sample::U8);
        bare.icc.reset();
        bare.meta = Metadata{};
        pictures.push_back(write_png(bare, png));
    }
    std::string out;
    put_le16(out, 0);
    put_le16(out, 1);  // (an icon)
    put_le16(out, static_cast<std::uint32_t>(pictures.size()));
    std::size_t offset = 6 + 16 * pictures.size();
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        out.push_back(static_cast<char>(stored[i].width() >= 256 ? 0 : stored[i].width()));
        out.push_back(static_cast<char>(stored[i].height() >= 256 ? 0 : stored[i].height()));
        out.push_back('\0');  // (no colour table)
        out.push_back('\0');
        put_le16(out, 1);   // (planes)
        put_le16(out, 32);  // (bits)
        put_le32(out, static_cast<std::uint32_t>(pictures[i].size()));
        put_le32(out, static_cast<std::uint32_t>(offset));
        offset += pictures[i].size();
    }
    for (const std::string& p : pictures) out += p;
    return out;
}

}  // namespace genko::render::codecs::detail
