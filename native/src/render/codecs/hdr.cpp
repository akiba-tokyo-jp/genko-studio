// Radiance HDR (render/codecs/formats.hpp; Greg Ward's RGBE of Graphics Gems II and Radiance's color.c): its header
// ("#?RADIANCE" or "#?RGBE", FORMAT=32-bit_rle_rgbe, lines up to an empty one), its size line ("-Y h +X w", rows from the
// top; "+Y h +X w" from the bottom) and its scanlines — flat, old run-length or new run-length encoded — read as RGB of
// floats ((byte + ½) × 2^(exponent − 136): light, linear); written with the new run-length encoding where the width
// allows (8 to 32767), a colour's largest channel setting its exponent (negatives as 0).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs::detail {

namespace {

struct Head {
    std::uint32_t width = 0, height = 0;
    bool bottom_up = false;
    std::size_t pixels_at = 0;
};

Head head_of(std::string_view b) {
    if (!b.starts_with("#?RADIANCE") && !b.starts_with("#?RGBE")) broken("Radiance HDR", "its header");
    std::size_t at = 0;
    bool rgbe = true;
    // (header lines up to an empty one)
    for (;;) {
        const std::size_t end = b.find('\n', at);
        if (end == std::string_view::npos || end - at > 4096) broken("Radiance HDR", "its header is cut short");
        const std::string_view line = b.substr(at, end - at);
        at = end + 1;
        if (line.empty()) break;
        if (line.starts_with("FORMAT=")) rgbe = line == "FORMAT=32-bit_rle_rgbe";
    }
    if (!rgbe) broken("Radiance HDR", "its colours are not RGBE (XYZE is not read)");
    const std::size_t end = b.find('\n', at);
    if (end == std::string_view::npos || end - at > 64) broken("Radiance HDR", "its size line");
    const std::string line(b.substr(at, end - at));
    char ys = 0, xs = 0, yc = 0, xc = 0;
    long long h = 0, w = 0;
    if (std::sscanf(line.c_str(), "%c%c %lld %c%c %lld", &ys, &yc, &h, &xs, &xc, &w) != 6 || yc != 'Y' || xc != 'X' || xs != '+' ||
        (ys != '-' && ys != '+') || h <= 0 || w <= 0 || h > 0xffffffffLL || w > 0xffffffffLL)
        broken("Radiance HDR", "a size line it does not read (" + line + ")");
    Head out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.bottom_up = ys == '+';
    out.pixels_at = end + 1;
    return out;
}

// One scanline's RGBE (4 bytes a pixel) from `at`.
void scanline(std::string_view b, std::size_t& at, std::uint32_t width, unsigned char* out) {
    const auto need = [&](std::size_t n) {
        if (at > b.size() || n > b.size() - at) broken("Radiance HDR", "its pixels are cut short");
    };
    need(4);
    const bool fresh = width >= 8 && width <= 0x7fff && u8(b, at) == 2 && u8(b, at + 1) == 2 && (u8(b, at + 2) & 0x80) == 0;
    if (fresh) {
        if (((u8(b, at + 2) << 8) | u8(b, at + 3)) != width) broken("Radiance HDR", "a scanline of another width");
        at += 4;
        for (unsigned c = 0; c < 4; ++c) {  // (each channel run-length encoded in turn)
            std::uint32_t x = 0;
            while (x < width) {
                need(1);
                unsigned n = u8(b, at++);
                if (n > 128) {
                    n -= 128;
                    if (n > width - x) broken("Radiance HDR", "a run past its scanline");
                    need(1);
                    const auto v = static_cast<unsigned char>(b[at++]);
                    for (unsigned k = 0; k < n; ++k) out[(x + k) * 4 + c] = v;
                } else {
                    if (n == 0 || n > width - x) broken("Radiance HDR", "a run past its scanline");
                    need(n);
                    for (unsigned k = 0; k < n; ++k) out[(x + k) * 4 + c] = static_cast<unsigned char>(b[at + k]);
                    at += n;
                }
                x += n;
            }
        }
        return;
    }
    // (flat, with the old runs: 1, 1, 1, n repeats the last pixel n << shift times)
    std::uint32_t x = 0;
    unsigned shift = 0;
    while (x < width) {
        need(4);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data() + at);
        at += 4;
        if (p[0] == 1 && p[1] == 1 && p[2] == 1) {
            if (x == 0 || shift > 24) broken("Radiance HDR", "a run with nothing before it");
            const std::uint64_t n = std::uint64_t(p[3]) << shift;
            if (n > width - x) broken("Radiance HDR", "a run past its scanline");
            for (std::uint64_t k = 0; k < n; ++k) std::memcpy(out + (x + k) * 4, out + (x - 1) * 4, 4);
            x += static_cast<std::uint32_t>(n);
            shift += 8;
            continue;
        }
        std::memcpy(out + x * 4, p, 4);
        ++x;
        shift = 0;
    }
}

std::array<unsigned char, 4> rgbe_of(float r, float g, float bl) {
    r = std::max(0.0f, std::isnan(r) ? 0.0f : r);
    g = std::max(0.0f, std::isnan(g) ? 0.0f : g);
    bl = std::max(0.0f, std::isnan(bl) ? 0.0f : bl);
    const float v = std::max({r, g, bl});
    if (!(v >= 1e-32f)) return {0, 0, 0, 0};
    if (std::isinf(v)) return {255, 255, 255, 255};
    int e = 0;
    const float m = std::frexp(v, &e) * 256.0f / v;
    if (e + 128 > 255) return {255, 255, 255, 255};
    if (e + 128 < 1) return {0, 0, 0, 0};
    return {static_cast<unsigned char>(std::min(255.0f, r * m)), static_cast<unsigned char>(std::min(255.0f, g * m)),
            static_cast<unsigned char>(std::min(255.0f, bl * m)), static_cast<unsigned char>(e + 128)};
}

void put_channel(std::string& out, const unsigned char* line, std::uint32_t width, unsigned c) {
    std::uint32_t x = 0;
    const auto at = [&](std::uint32_t i) { return line[i * 4 + c]; };
    while (x < width) {
        // (a run of 3 or more alike is worth its two bytes)
        std::uint32_t run = 1;
        while (x + run < width && run < 127 && at(x + run) == at(x)) ++run;
        if (run >= 3) {
            out.push_back(static_cast<char>(128 + run));
            out.push_back(static_cast<char>(at(x)));
            x += run;
            continue;
        }
        std::uint32_t raw = 1;
        while (x + raw < width && raw < 128) {
            if (x + raw + 2 < width && at(x + raw) == at(x + raw + 1) && at(x + raw) == at(x + raw + 2)) break;
            ++raw;
        }
        out.push_back(static_cast<char>(raw));
        for (std::uint32_t k = 0; k < raw; ++k) out.push_back(static_cast<char>(at(x + k)));
        x += raw;
    }
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> hdr_size(std::string_view bytes) {
    const Head h = head_of(bytes);
    return {h.width, h.height};
}

CodecImage read_hdr(std::string_view b, const Limits& limits) {
    const Head h = head_of(b);
    limits.check(h.width, h.height, Layout::Rgb, Sample::F32);
    // (a scanline takes 4 bytes at the least: a file too short for its height is cut short, said before its pixels are made)
    if (h.pixels_at > b.size() || (b.size() - h.pixels_at) / 4 < h.height) broken("Radiance HDR", "its pixels are cut short");
    std::string out(std::size_t(h.width) * h.height * 3 * 4, '\0');
    std::vector<unsigned char> line(std::size_t(h.width) * 4);
    std::size_t at = h.pixels_at;
    for (std::uint32_t y = 0; y < h.height; ++y) {
        scanline(b, at, h.width, line.data());
        const std::uint32_t row = h.bottom_up ? h.height - 1 - y : y;
        char* to = out.data() + std::size_t(row) * h.width * 12;
        for (std::uint32_t x = 0; x < h.width; ++x) {
            const unsigned char* p = line.data() + std::size_t(x) * 4;
            const float scale = p[3] == 0 ? 0.0f : std::ldexp(1.0f, static_cast<int>(p[3]) - 136);
            for (unsigned c = 0; c < 3; ++c) {
                const float v = p[3] == 0 ? 0.0f : (static_cast<float>(p[c]) + 0.5f) * scale;
                std::memcpy(to + (std::size_t(x) * 3 + c) * 4, &v, 4);
            }
        }
    }
    return CodecImage::from_bytes(h.width, h.height, Layout::Rgb, Sample::F32, std::move(out));
}

std::string write_hdr(const CodecImage& stored, const EncodeOptions&) {
    if (stored.sample() != Sample::F32 || stored.layout() != Layout::Rgb) throw core::Error("value", "Radiance HDR is written from RGB floats");
    const std::uint32_t w = stored.width(), h = stored.height();
    std::string out = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(h) + " +X " + std::to_string(w) + "\n";
    std::vector<unsigned char> line(std::size_t(w) * 4);
    const bool runs = w >= 8 && w <= 0x7fff;
    for (std::uint32_t y = 0; y < h; ++y) {
        const char* from = stored.data().data() + std::size_t(y) * w * 12;
        for (std::uint32_t x = 0; x < w; ++x) {
            float rgb[3];
            std::memcpy(rgb, from + std::size_t(x) * 12, 12);
            const auto e = rgbe_of(rgb[0], rgb[1], rgb[2]);
            std::memcpy(line.data() + std::size_t(x) * 4, e.data(), 4);
        }
        if (!runs) {
            out.append(reinterpret_cast<const char*>(line.data()), line.size());
            continue;
        }
        out.push_back(2);
        out.push_back(2);
        put_be16(out, w);
        for (unsigned c = 0; c < 4; ++c) put_channel(out, line.data(), w, c);
    }
    return out;
}

}  // namespace genko::render::codecs::detail
