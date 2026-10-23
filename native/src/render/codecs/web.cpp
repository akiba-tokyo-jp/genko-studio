// Web 用に保存（従来）'s own writers (render/codecs/web.hpp; PhotoCraft v0.2.0 crates/codecs/src/web.rs, MIT).

#include "render/codecs/web.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <exception>

#include "core/error.hpp"
#include "render/movie.hpp"

namespace genko::render::codecs::web {

namespace {

void put_le16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void check_indices(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                   std::optional<std::uint8_t> transparent) {
    if (width == 0 || height == 0 || indices.size() != std::size_t(width) * height) throw core::Error("value", "the indices are not the picture's");
    if (palette.empty() || palette.size() > 256) throw core::Error("value", "a palette holds 1 to 256 colours");
    if (transparent && *transparent >= palette.size()) throw core::Error("value", "the transparent entry is not in the palette");
    for (const std::uint8_t i : indices) {
        if (i >= palette.size()) throw core::Error("value", "an index is past the palette");
    }
}

// The bits a table of `count` entries needs (1, 2, 4 or 8: PNG's depths; GIF's tables are 2^1..2^8).
int bits_for(std::size_t count, bool png) {
    int bits = 1;
    while ((std::size_t(1) << bits) < count) ++bits;
    if (png && bits == 3) bits = 4;
    if (png && bits > 4) bits = 8;
    return bits;
}

// --- PNG of a palette (libpng; its bytes to the sink as they are made) ---

struct PngOut {
    const Sink* sink = nullptr;
    std::exception_ptr error;
    char message[200] = {0};
};

void on_error(png_structp png, png_const_charp message) {
    auto* out = static_cast<PngOut*>(png_get_error_ptr(png));
    std::snprintf(out->message, sizeof out->message, "%s", message != nullptr ? message : "broken");
    png_longjmp(png, 1);
}
void on_warning(png_structp, png_const_charp) {}
void on_write(png_structp png, png_bytep data, png_size_t n) {
    auto* out = static_cast<PngOut*>(png_get_io_ptr(png));
    try {
        (*out->sink)(std::string_view(reinterpret_cast<const char*>(data), n));
    } catch (...) {  // (the sink failed — a full disk —: out of libpng first, then said)
        out->error = std::current_exception();
        png_error(png, "the file cannot be written");
    }
}
void on_flush(png_structp) {}

struct PngPlan {
    png_uint_32 width = 0, height = 0;
    int bits = 8;
    bool interlaced = false;
    std::optional<png_uint_32> ppm;
    std::vector<png_color> palette;
    std::vector<png_byte> alpha;  // (tRNS: the entries up to the transparent one)
    png_bytepp rows = nullptr;
};

bool write_png(png_structp png, png_infop info, const PngPlan* p) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_set_compression_level(png, 9);
    png_set_IHDR(png, info, p->width, p->height, p->bits, PNG_COLOR_TYPE_PALETTE, p->interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, p->palette.data(), static_cast<int>(p->palette.size()));
    if (!p->alpha.empty()) png_set_tRNS(png, info, p->alpha.data(), static_cast<int>(p->alpha.size()), nullptr);
    if (p->ppm) png_set_pHYs(png, info, *p->ppm, *p->ppm, PNG_RESOLUTION_METER);
    png_write_info(png, info);
    png_write_image(png, p->rows);
    png_write_end(png, info);
    return true;
}

struct PngGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    ~PngGuard() { png_destroy_write_struct(&png, &info); }
};

}  // namespace

void write_gif_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                       std::optional<std::uint8_t> transparent, bool interlaced, const Sink& sink) {
    if (width > 65535 || height > 65535) throw core::Error("image_too_large", "GIF holds at most 65535×65535 pixels");
    check_indices(width, height, indices, palette, transparent);
    const int bits = bits_for(palette.size(), false);
    std::string head("GIF89a", 6);
    put_le16(head, width);
    put_le16(head, height);
    head.push_back(static_cast<char>(0x80 | ((bits - 1) << 4) | (bits - 1)));  // (a global table of 2^bits)
    head.push_back(static_cast<char>(transparent.value_or(0)));                // (the background's entry)
    head.push_back('\0');
    for (std::size_t i = 0; i < (std::size_t(1) << bits); ++i) {
        const Rgb8 c = i < palette.size() ? palette[i] : Rgb8{0, 0, 0};
        for (const std::uint8_t v : c) head.push_back(static_cast<char>(v));
    }
    if (transparent) {  // (a graphic control extension: its transparent entry)
        head += std::string("\x21\xF9\x04\x01\x00\x00", 6);
        head.push_back(static_cast<char>(*transparent));
        head.push_back('\0');
    }
    head.push_back('\x2C');  // (the picture: at 0, 0, of the screen's size, no table of its own)
    put_le16(head, 0);
    put_le16(head, 0);
    put_le16(head, width);
    put_le16(head, height);
    head.push_back(interlaced ? '\x40' : '\0');
    const int min_bits = std::max(2, bits);
    head.push_back(static_cast<char>(min_bits));
    sink(head);
    std::vector<std::uint8_t> order;
    if (interlaced) {  // (every 8th row from 0, every 8th from 4, every 4th from 2, every 2nd from 1)
        order.reserve(indices.size());
        for (const auto [start, step] : {std::pair<std::uint32_t, std::uint32_t>{0, 8}, {4, 8}, {2, 4}, {1, 2}}) {
            for (std::uint32_t y = start; y < height; y += step) {
                const auto row = indices.subspan(std::size_t(y) * width, width);
                order.insert(order.end(), row.begin(), row.end());
            }
        }
    } else {
        order.assign(indices.begin(), indices.end());
    }
    sink(movie::gif_lzw(order, min_bits));  // (its sub-blocks and their end)
    sink(std::string_view("\x3B", 1));
}

std::string encode_gif_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                               std::optional<std::uint8_t> transparent, bool interlaced) {
    std::string out;
    write_gif_indexed(width, height, indices, palette, transparent, interlaced, [&out](std::string_view bytes) { out += bytes; });
    return out;
}

void write_png_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                       std::optional<std::uint8_t> transparent, bool interlaced, std::optional<double> dpi, const Sink& sink) {
    check_indices(width, height, indices, palette, transparent);
    PngPlan plan;
    plan.width = width;
    plan.height = height;
    plan.bits = bits_for(palette.size(), true);
    plan.interlaced = interlaced;
    if (dpi && *dpi > 0) plan.ppm = static_cast<png_uint_32>(std::clamp(std::round(*dpi / 0.0254), 1.0, 2147483647.0));
    for (const Rgb8& c : palette) plan.palette.push_back(png_color{c[0], c[1], c[2]});
    if (transparent) {
        plan.alpha.assign(std::size_t(*transparent) + 1, 255);
        plan.alpha.back() = 0;
    }
    // (the rows packed to the depth: at most one byte a pixel, as the indices are)
    const std::size_t stride = (std::size_t(width) * static_cast<std::size_t>(plan.bits) + 7) / 8;
    std::vector<png_byte> packed(stride * height, 0);
    std::vector<png_bytep> rows(height);
    for (std::uint32_t y = 0; y < height; ++y) {
        png_bytep row = packed.data() + std::size_t(y) * stride;
        rows[y] = row;
        const std::uint8_t* from = indices.data() + std::size_t(y) * width;
        if (plan.bits == 8) {
            std::copy(from, from + width, row);
            continue;
        }
        const int per = 8 / plan.bits;
        for (std::uint32_t x = 0; x < width; ++x) {
            const int shift = 8 - plan.bits * (static_cast<int>(x % static_cast<std::uint32_t>(per)) + 1);
            row[x / static_cast<std::uint32_t>(per)] = static_cast<png_byte>(row[x / static_cast<std::uint32_t>(per)] | (from[x] << shift));
        }
    }
    plan.rows = rows.data();
    PngOut out;
    out.sink = &sink;
    PngGuard guard;
    guard.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &out, on_error, on_warning);
    if (guard.png == nullptr) throw core::Error("memory", "the PNG writer cannot be made");
    guard.info = png_create_info_struct(guard.png);
    if (guard.info == nullptr) throw core::Error("memory", "the PNG writer cannot be made");
    png_set_write_fn(guard.png, &out, on_write, on_flush);
    if (!write_png(guard.png, guard.info, &plan)) {
        if (out.error) std::rethrow_exception(out.error);
        throw core::Error("io", std::string("cannot write PNG: ") + out.message);
    }
}

std::string encode_png_indexed(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> indices, std::span<const Rgb8> palette,
                               std::optional<std::uint8_t> transparent, bool interlaced, std::optional<double> dpi) {
    std::string out;
    write_png_indexed(width, height, indices, palette, transparent, interlaced, dpi, [&out](std::string_view bytes) { out += bytes; });
    return out;
}

namespace {

void put_multibyte(std::string& out, std::uint32_t v) {
    std::string groups(1, static_cast<char>(v & 0x7f));
    for (v >>= 7; v > 0; v >>= 7) groups.push_back(static_cast<char>((v & 0x7f) | 0x80));
    out.append(groups.rbegin(), groups.rend());
}

}  // namespace

void write_wbmp(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> white, const Sink& sink) {
    if (width == 0 || height == 0 || white.size() != std::size_t(width) * height) throw core::Error("value", "the pixels are not the picture's");
    std::string head(2, '\0');  // (type 0, no extension headers)
    put_multibyte(head, width);
    put_multibyte(head, height);
    sink(head);
    const std::size_t stride = (std::size_t(width) + 7) / 8;
    std::string band;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::string row(stride, '\0');
        const std::uint8_t* from = white.data() + std::size_t(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            if (from[x] != 0) row[x / 8] = static_cast<char>(static_cast<unsigned char>(row[x / 8]) | (0x80u >> (x % 8)));
        }
        band += row;
        if (band.size() >= (1u << 16)) {
            sink(band);
            band.clear();
        }
    }
    if (!band.empty()) sink(band);
}

std::string encode_wbmp(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> white) {
    std::string out;
    write_wbmp(width, height, white, [&out](std::string_view bytes) { out += bytes; });
    return out;
}

Wbmp decode_wbmp(std::string_view bytes, const Limits& limits) {
    std::size_t at = 0;
    const auto next = [&]() -> unsigned {
        if (at >= bytes.size()) throw core::Error("format", "the file is not a WBMP file");
        return static_cast<unsigned char>(bytes[at++]);
    };
    if (next() != 0 || (next() & 0x9f) != 0) throw core::Error("format", "the file is not a WBMP file");
    const auto multibyte = [&]() -> std::uint32_t {
        std::uint64_t v = 0;
        for (int i = 0; i < 5; ++i) {
            const unsigned b = next();
            v = (v << 7) | (b & 0x7f);
            if (v > 0xffffffffULL) break;
            if ((b & 0x80) == 0) return static_cast<std::uint32_t>(v);
        }
        throw core::Error("format", "the file is not a WBMP file");
    };
    Wbmp out;
    out.width = multibyte();
    out.height = multibyte();
    if (out.width == 0 || out.height == 0) throw core::Error("format", "the file is not a WBMP file");
    limits.check(out.width, out.height, Layout::Gray, Sample::U8);  // (before its pixels are made)
    const std::size_t stride = (std::size_t(out.width) + 7) / 8;
    if (bytes.size() - at < stride * out.height) throw core::Error("format", "the WBMP file cannot be read (its pixels are cut short)");
    out.white.resize(std::size_t(out.width) * out.height);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const unsigned char* row = reinterpret_cast<const unsigned char*>(bytes.data()) + at + std::size_t(y) * stride;
        for (std::uint32_t x = 0; x < out.width; ++x) out.white[std::size_t(y) * out.width + x] = (row[x / 8] & (0x80u >> (x % 8))) != 0 ? 1 : 0;
    }
    return out;
}

}  // namespace genko::render::codecs::web
