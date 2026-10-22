// Netpbm (render/codecs/formats.hpp; PhotoCraft v0.2.0 crates/codecs/src/codecs/pnm.rs, MIT): P1–P6 (PBM, PGM, PPM, in
// text and in bytes), P7 (PAM, with the CMYK and CMYK_ALPHA tuple types) and PFM (PF colour, Pf grey; floats, rows from the
// bottom, the scale's sign the byte order). Written by what the picture is: floats as PFM (grey, RGB), grey as P5, RGB as
// P6, anything with an alpha or CMYK as P7; 8 bits with a maxval of 255, 16 bits with 65535.

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs::detail {

namespace {

[[noreturn]] void bad(std::string_view why) { broken("Netpbm", why); }

bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

struct Reader {
    std::string_view b;
    std::size_t pos = 2;

    void skip() {
        while (pos < b.size()) {
            const auto c = static_cast<unsigned char>(b[pos]);
            if (c == '#') {
                while (pos < b.size() && b[pos] != '\n' && b[pos] != '\r') ++pos;
            } else if (space(c)) {
                ++pos;
            } else {
                break;
            }
        }
    }
    std::string_view token() {
        skip();
        const std::size_t start = pos;
        while (pos < b.size() && !space(static_cast<unsigned char>(b[pos])) && b[pos] != '#') ++pos;
        if (start == pos) bad("its header ends early");
        return b.substr(start, pos - start);
    }
    std::uint32_t number() {
        const std::string_view t = token();
        std::uint32_t v = 0;
        const auto [end, error] = std::from_chars(t.data(), t.data() + t.size(), v);
        if (error != std::errc() || end != t.data() + t.size()) bad("a number in its header");
        return v;
    }
    // The one whitespace byte between the header and the pixels.
    void one_space() {
        if (pos >= b.size() || !space(static_cast<unsigned char>(b[pos]))) bad("no space after its header");
        ++pos;
    }
    std::string_view rest() const { return b.substr(std::min(pos, b.size())); }
};

Sample sample_for(std::uint32_t maxval) {
    if (maxval >= 1 && maxval <= 255) return Sample::U8;
    if (maxval >= 256 && maxval <= 65535) return Sample::U16;
    bad("its maxval");
}

// Integer values in 0..maxval as the picture's samples (scaled to 255 or 65535).
CodecImage from_values(std::uint32_t w, std::uint32_t h, Layout layout, std::uint32_t maxval, const std::vector<std::uint32_t>& values) {
    const Sample sample = sample_for(maxval);
    std::string data(values.size() * sample_bytes(sample), '\0');
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint32_t v = values[i];
        if (v > maxval) bad("a sample past its maxval");
        if (sample == Sample::U8) {
            data[i] = static_cast<char>(maxval == 255 ? v : (v * 255 + maxval / 2) / maxval);
        } else {
            const auto u = static_cast<std::uint16_t>(maxval == 65535 ? v : (std::uint64_t(v) * 65535 + maxval / 2) / maxval);
            std::memcpy(data.data() + i * 2, &u, 2);
        }
    }
    return CodecImage::from_bytes(w, h, layout, sample, std::move(data));
}

// The pixels in bytes (one a sample below a maxval of 256, two — the high first — from it) as the picture's samples.
CodecImage from_binary(std::uint32_t w, std::uint32_t h, Layout layout, std::uint32_t maxval, std::string_view data) {
    const Sample sample = sample_for(maxval);
    const std::size_t n = std::size_t(w) * h * channels(layout);
    if (data.size() / (maxval < 256 ? 1 : 2) < n) bad("its pixels are cut short");
    std::string out(n * sample_bytes(sample), '\0');
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t v = maxval < 256 ? u8(data, i) : be16(data, i * 2);
        if (v > maxval) bad("a sample past its maxval");
        if (sample == Sample::U8) {
            out[i] = static_cast<char>(maxval == 255 ? v : (v * 255 + maxval / 2) / maxval);
        } else {
            const auto u = static_cast<std::uint16_t>(maxval == 65535 ? v : (std::uint64_t(v) * 65535 + maxval / 2) / maxval);
            std::memcpy(out.data() + i * 2, &u, 2);
        }
    }
    return CodecImage::from_bytes(w, h, layout, sample, std::move(out));
}

struct Pam {
    std::uint32_t w = 0, h = 0, maxval = 0;
    Layout layout = Layout::Gray;
};

Pam pam_header(Reader& r) {
    std::optional<std::uint32_t> w, h, depth, maxval;
    std::string tupltype;
    for (;;) {
        const std::string_view t = r.token();
        if (t == "WIDTH") w = r.number();
        else if (t == "HEIGHT") h = r.number();
        else if (t == "DEPTH") depth = r.number();
        else if (t == "MAXVAL") maxval = r.number();
        else if (t == "TUPLTYPE") {
            if (!tupltype.empty()) tupltype += ' ';
            tupltype += r.token();
        } else if (t == "ENDHDR") break;
        else bad("an unknown field in its PAM header");
    }
    r.one_space();
    if (!w || !h || !depth || !maxval) bad("its PAM header is incomplete");
    Pam p;
    p.w = *w;
    p.h = *h;
    p.maxval = *maxval;
    const std::uint32_t d = *depth;
    if ((tupltype == "BLACKANDWHITE" || tupltype == "GRAYSCALE" || tupltype.empty()) && d == 1) p.layout = Layout::Gray;
    else if ((tupltype == "BLACKANDWHITE_ALPHA" || tupltype == "GRAYSCALE_ALPHA" || tupltype.empty()) && d == 2) p.layout = Layout::GrayA;
    else if ((tupltype == "RGB" || tupltype.empty()) && d == 3) p.layout = Layout::Rgb;
    else if ((tupltype == "RGB_ALPHA" || tupltype.empty()) && d == 4) p.layout = Layout::Rgba;
    else if (tupltype == "CMYK" && d == 4) p.layout = Layout::Cmyk;
    else if (tupltype == "CMYK_ALPHA" && d == 5) p.layout = Layout::CmykA;
    else bad("a PAM tuple type it cannot read (" + tupltype + ", depth " + std::to_string(d) + ")");
    return p;
}

std::pair<std::uint32_t, std::uint32_t> size_of(std::string_view b) {
    if (b.size() < 2 || b[0] != 'P') bad("not a Netpbm file");
    Reader r{b};
    if (b[1] == '7') {
        const Pam p = pam_header(r);
        return {p.w, p.h};
    }
    if (std::string_view("123456Ff").find(b[1]) == std::string_view::npos) bad("an unknown Netpbm kind");
    const std::uint32_t w = r.number(), h = r.number();
    return {w, h};
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> pnm_size(std::string_view bytes) { return size_of(bytes); }

CodecImage read_pnm(std::string_view b, const Limits& limits) {
    if (b.size() < 2 || b[0] != 'P') bad("not a Netpbm file");
    const char kind = b[1];
    Reader r{b};
    if (kind == '1' || kind == '4') {
        const std::uint32_t w = r.number(), h = r.number();
        limits.check(w, h, Layout::Gray, Sample::U8);
        const std::size_t n = std::size_t(w) * h;
        // (a P1 digit a pixel, a P4 bit: a file too short for its size is cut short, said before its pixels are made)
        if (kind == '1' && (r.pos > b.size() || b.size() - r.pos < n)) bad("its pixels are cut short");
        if (kind == '4') r.one_space();
        const std::size_t row = (std::size_t(w) + 7) / 8;
        if (kind == '4' && r.rest().size() / row < h) bad("its pixels are cut short");
        std::string px(n, '\0');
        if (kind == '4') {
            const std::string_view data = r.rest();
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    const bool black = ((u8(data, y * row + x / 8) >> (7 - x % 8)) & 1) != 0;
                    px[y * w + x] = black ? '\0' : static_cast<char>(255);
                }
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                r.skip();
                if (r.pos >= b.size()) bad("its pixels are cut short");
                const char c = b[r.pos++];
                if (c != '0' && c != '1') bad("a PBM digit it cannot have");
                px[i] = c == '1' ? '\0' : static_cast<char>(255);
            }
        }
        return from_eight_bit(w, h, Layout::Gray, std::move(px));
    }
    if (kind == '2' || kind == '3' || kind == '5' || kind == '6') {
        const std::uint32_t w = r.number(), h = r.number(), maxval = r.number();
        const Layout layout = kind == '2' || kind == '5' ? Layout::Gray : Layout::Rgb;
        limits.check(w, h, layout, sample_for(maxval));
        if (kind == '5' || kind == '6') {
            r.one_space();
            return from_binary(w, h, layout, maxval, r.rest());
        }
        const std::size_t n = std::size_t(w) * h * channels(layout);
        std::vector<std::uint32_t> values;  // (as many as the file has: never more than it holds)
        for (std::size_t i = 0; i < n; ++i) values.push_back(r.number());
        return from_values(w, h, layout, maxval, values);
    }
    if (kind == '7') {
        const Pam p = pam_header(r);
        limits.check(p.w, p.h, p.layout, sample_for(p.maxval));
        return from_binary(p.w, p.h, p.layout, p.maxval, r.rest());
    }
    if (kind == 'F' || kind == 'f') {
        const Layout layout = kind == 'F' ? Layout::Rgb : Layout::Gray;
        const std::uint32_t w = r.number(), h = r.number();
        const std::string_view scale_text = r.token();
        double scale = 0;  // (from_chars: a point whatever the locale — Qt sets the person's, a comma in some)
        const char* first = scale_text.data() + (scale_text.starts_with('+') ? 1 : 0);
        const auto [end, error] = std::from_chars(first, scale_text.data() + scale_text.size(), scale);
        if (error != std::errc() || end != scale_text.data() + scale_text.size() || !std::isfinite(scale) || scale == 0) bad("its scale");
        r.one_space();
        limits.check(w, h, layout, Sample::F32);
        const std::size_t row = std::size_t(w) * channels(layout);
        const std::string_view data = r.rest();
        if (data.size() / 4 / row < h) bad("its pixels are cut short");
        const bool little = scale < 0;
        std::string out(row * h * 4, '\0');
        for (std::size_t y = 0; y < h; ++y) {
            const std::size_t to = (h - 1 - y) * row;  // (rows from the bottom)
            for (std::size_t i = 0; i < row; ++i) {
                const std::size_t at = (y * row + i) * 4;
                const std::uint32_t bits = little ? le32(data, at) : be32(data, at);
                std::memcpy(out.data() + (to + i) * 4, &bits, 4);
            }
        }
        return CodecImage::from_bytes(w, h, layout, Sample::F32, std::move(out));
    }
    bad("an unknown Netpbm kind");
}

std::string write_pnm(const CodecImage& stored, const EncodeOptions& options) {
    const std::uint32_t w = stored.width(), h = stored.height();
    const Layout layout = stored.layout();
    std::string out;
    if (options.pnm_kind == PnmKind::Bitmap) {  // (P4: a bit a pixel, 1 black — a grey below half —, rows of whole bytes)
        if (layout != Layout::Gray || stored.sample() != Sample::U8) throw core::Error("value", "PBM is written from 8-bit grey");
        out = "P4\n" + std::to_string(w) + " " + std::to_string(h) + "\n";
        const std::size_t row = (std::size_t(w) + 7) / 8;
        std::string bits(row * h, '\0');
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                if (static_cast<unsigned char>(stored.data()[y * w + x]) < 128) bits[y * row + x / 8] = static_cast<char>(bits[y * row + x / 8] | (0x80 >> (x % 8)));
            }
        }
        return out + bits;
    }
    if (stored.sample() == Sample::F32) {
        if (layout != Layout::Gray && layout != Layout::Rgb) throw core::Error("value", "PFM holds grey and RGB");
        out = std::string(layout == Layout::Gray ? "Pf" : "PF") + "\n" + std::to_string(w) + " " + std::to_string(h) + "\n-1.0\n";
        const std::size_t row = std::size_t(w) * channels(layout) * 4;
        for (std::size_t y = h; y-- > 0;) {
            const char* from = stored.data().data() + y * row;
            for (std::size_t i = 0; i < row; i += 4) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, from + i, 4);
                put_le32(out, bits);
            }
        }
        return out;
    }
    if (stored.sample() != Sample::U8 && stored.sample() != Sample::U16) throw core::Error("value", "Netpbm holds 8 or 16 bits, or 32-bit floats");
    const std::string maxval = stored.sample() == Sample::U8 ? "255" : "65535";
    const std::string size = std::to_string(w) + " " + std::to_string(h);
    const bool pam = options.pnm_kind == PnmKind::Arbitrary;  // (.pam: P7 whatever its layout)
    if (layout == Layout::Gray && !pam) {
        out = "P5\n" + size + "\n" + maxval + "\n";
    } else if (layout == Layout::Rgb && !pam) {
        out = "P6\n" + size + "\n" + maxval + "\n";
    } else {
        const char* tuple = layout == Layout::Gray    ? "GRAYSCALE"
                            : layout == Layout::Rgb   ? "RGB"
                            : layout == Layout::GrayA ? "GRAYSCALE_ALPHA"
                            : layout == Layout::Rgba  ? "RGB_ALPHA"
                            : layout == Layout::Cmyk  ? "CMYK"
                                                      : "CMYK_ALPHA";
        out = "P7\nWIDTH " + std::to_string(w) + "\nHEIGHT " + std::to_string(h) + "\nDEPTH " + std::to_string(channels(layout)) + "\nMAXVAL " + maxval +
              "\nTUPLTYPE " + tuple + "\nENDHDR\n";
    }
    if (stored.sample() == Sample::U8) {
        out += stored.data();
    } else {
        out.reserve(out.size() + stored.data().size());
        for (std::size_t i = 0; i < stored.sample_count(); ++i) {
            std::uint16_t v = 0;
            std::memcpy(&v, stored.data().data() + i * 2, 2);
            put_be16(out, v);
        }
    }
    return out;
}

}  // namespace genko::render::codecs::detail
