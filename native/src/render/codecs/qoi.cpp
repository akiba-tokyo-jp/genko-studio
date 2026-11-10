// QOI (render/codecs/formats.hpp; the Quite OK Image format, specification 1.0 of 2022): RGB and RGBA of 8 bits, read and
// written by the specification (its index of 64 colours, runs, differences and lumas; the end marker of seven zeros and
// a one).

#include <array>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs::detail {

namespace {

constexpr unsigned char kIndex = 0x00, kDiff = 0x40, kLuma = 0x80, kRun = 0xc0, kRgb = 0xfe, kRgba = 0xff;
constexpr std::string_view kEnd("\0\0\0\0\0\0\0\1", 8);

// (the index starts all zeros, the previous pixel opaque black)
struct Px {
    unsigned char r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Px&) const = default;
};

unsigned slot(const Px& p) { return (p.r * 3u + p.g * 5u + p.b * 7u + p.a * 11u) % 64u; }

}  // namespace

std::pair<std::uint32_t, std::uint32_t> qoi_size(std::string_view b) {
    if (b.size() < 14 || b.substr(0, 4) != "qoif") broken("QOI", "its header");
    return {be32(b, 4), be32(b, 8)};
}

CodecImage read_qoi(std::string_view b, const Limits& limits) {
    const auto [w, h] = qoi_size(b);
    const unsigned ch = u8(b, 12);
    if (ch != 3 && ch != 4) broken("QOI", "its channels");
    if (u8(b, 13) > 1) broken("QOI", "its colour space");
    const Layout layout = ch == 4 ? Layout::Rgba : Layout::Rgb;
    limits.check(w, h, layout, Sample::U8);
    const std::size_t count = std::size_t(w) * h;
    // (an op gives at most 62 pixels, a run: a file too short for its size is cut short, said before its pixels are made)
    const std::size_t ops = b.size() >= 22 ? b.size() - 22 : 0;
    if (ops * 62 < count) broken("QOI", "its pixels are cut short");
    std::string out(count * ch, '\0');
    std::array<Px, 64> seen{};
    Px px{0, 0, 0, 255};
    std::size_t at = 14;
    const std::size_t end = b.size() >= 8 ? b.size() - 8 : 0;  // (the end marker is not pixels)
    std::size_t run = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (run > 0) {
            --run;
        } else {
            if (at >= end) broken("QOI", "its pixels are cut short");
            const unsigned op = u8(b, at++);
            if (op == kRgb) {
                if (at + 3 > end) broken("QOI", "its pixels are cut short");
                px.r = static_cast<unsigned char>(b[at]);
                px.g = static_cast<unsigned char>(b[at + 1]);
                px.b = static_cast<unsigned char>(b[at + 2]);
                at += 3;
            } else if (op == kRgba) {
                if (at + 4 > end) broken("QOI", "its pixels are cut short");
                px = {static_cast<unsigned char>(b[at]), static_cast<unsigned char>(b[at + 1]), static_cast<unsigned char>(b[at + 2]),
                      static_cast<unsigned char>(b[at + 3])};
                at += 4;
            } else if ((op & 0xc0) == kIndex) {
                px = seen[op & 0x3f];
            } else if ((op & 0xc0) == kDiff) {
                px.r = static_cast<unsigned char>(px.r + ((op >> 4) & 3) - 2);
                px.g = static_cast<unsigned char>(px.g + ((op >> 2) & 3) - 2);
                px.b = static_cast<unsigned char>(px.b + (op & 3) - 2);
            } else if ((op & 0xc0) == kLuma) {
                if (at >= end) broken("QOI", "its pixels are cut short");
                const unsigned next = u8(b, at++);
                const int dg = static_cast<int>(op & 0x3f) - 32;
                px.r = static_cast<unsigned char>(px.r + dg - 8 + static_cast<int>((next >> 4) & 0x0f));
                px.g = static_cast<unsigned char>(px.g + dg);
                px.b = static_cast<unsigned char>(px.b + dg - 8 + static_cast<int>(next & 0x0f));
            } else {
                run = op & 0x3f;
            }
            seen[slot(px)] = px;
        }
        char* to = out.data() + i * ch;
        to[0] = static_cast<char>(px.r);
        to[1] = static_cast<char>(px.g);
        to[2] = static_cast<char>(px.b);
        if (ch == 4) to[3] = static_cast<char>(px.a);
    }
    return from_eight_bit(w, h, layout, std::move(out));
}

std::string write_qoi(const CodecImage& stored, const EncodeOptions&) {
    if (stored.sample() != Sample::U8 || !is_rgb(stored.layout())) throw core::Error("value", "QOI holds 8-bit RGB, with or without an alpha");
    const unsigned ch = channels(stored.layout());
    std::string out("qoif", 4);
    put_be32(out, stored.width());
    put_be32(out, stored.height());
    out.push_back(static_cast<char>(ch));
    out.push_back('\0');  // (sRGB with a linear alpha)
    std::array<Px, 64> seen{};
    Px prev{0, 0, 0, 255};
    std::size_t run = 0;
    const auto* d = reinterpret_cast<const unsigned char*>(stored.data().data());
    const std::size_t count = stored.pixel_count();
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* p = d + i * ch;
        const Px px{p[0], p[1], p[2], ch == 4 ? p[3] : static_cast<unsigned char>(255)};
        if (px == prev) {
            ++run;
            if (run == 62 || i + 1 == count) {
                out.push_back(static_cast<char>(kRun | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run > 0) {
            out.push_back(static_cast<char>(kRun | (run - 1)));
            run = 0;
        }
        const unsigned at = slot(px);
        if (seen[at] == px) {
            out.push_back(static_cast<char>(kIndex | at));
        } else {
            seen[at] = px;
            if (px.a == prev.a) {
                const int dr = static_cast<signed char>(px.r - prev.r), dg = static_cast<signed char>(px.g - prev.g),
                          db = static_cast<signed char>(px.b - prev.b);
                const int dr_dg = dr - dg, db_dg = db - dg;
                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                    out.push_back(static_cast<char>(kDiff | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
                } else if (dr_dg >= -8 && dr_dg <= 7 && dg >= -32 && dg <= 31 && db_dg >= -8 && db_dg <= 7) {
                    out.push_back(static_cast<char>(kLuma | (dg + 32)));
                    out.push_back(static_cast<char>(((dr_dg + 8) << 4) | (db_dg + 8)));
                } else {
                    out.push_back(static_cast<char>(kRgb));
                    out.push_back(static_cast<char>(px.r));
                    out.push_back(static_cast<char>(px.g));
                    out.push_back(static_cast<char>(px.b));
                }
            } else {
                out.push_back(static_cast<char>(kRgba));
                out.push_back(static_cast<char>(px.r));
                out.push_back(static_cast<char>(px.g));
                out.push_back(static_cast<char>(px.b));
                out.push_back(static_cast<char>(px.a));
            }
        }
        prev = px;
    }
    out.append(kEnd);
    return out;
}

}  // namespace genko::render::codecs::detail
