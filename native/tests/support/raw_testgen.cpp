// Synthetic camera RAW files for the tests (tests/support/raw_testgen.hpp): PhotoCraft v0.2.0's raw/src/testgen.rs (MIT).

#include "raw_testgen.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace genko::test::rawgen {

namespace {

// --- lossless JPEG --------------------------------------------------------------------------------------------------------

// Huffman code lengths given to the categories in decreasing frequency (Kraft sum < 1, no code all ones).
constexpr std::array<int, 17> kLengths{2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13};

int category(int d) {
    if (d == 0) return 0;
    if (d == 32768) return 16;
    unsigned v = static_cast<unsigned>(d < 0 ? -d : d);
    int bits = 0;
    while (v != 0) {
        ++bits;
        v >>= 1;
    }
    return bits;
}

struct BitWriter {
    std::string out;
    unsigned acc = 0, n = 0;
    void put(unsigned v, unsigned bits) {
        for (int i = static_cast<int>(bits) - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1u);
            if (++n == 8) {
                const auto b = static_cast<char>(acc & 0xFF);
                out.push_back(b);
                if (static_cast<unsigned char>(b) == 0xFF) out.push_back('\0');
                acc = 0;
                n = 0;
            }
        }
    }
    void flush() {
        if (n > 0) {
            const unsigned pad = 8 - n;
            put((1u << pad) - 1, pad);
        }
    }
};

void be16(std::string& out, unsigned v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

// --- TIFF -----------------------------------------------------------------------------------------------------------------

std::size_t val_size(const TiffBuilder& t, const Val& v);
std::size_t ifd_size(const TiffBuilder& t, std::size_t i) { return 2 + 12 * t.ifds[i].size() + 4; }
std::size_t val_size(const TiffBuilder& t, const Val& v) {
    struct Size {
        const TiffBuilder& t;
        std::size_t operator()(const Byte& b) const { return b.v.size(); }
        std::size_t operator()(const Undefined& b) const { return b.v.size(); }
        std::size_t operator()(const Ascii& s) const { return s.v.size() + 1; }
        std::size_t operator()(const Short& s) const { return 2 * s.v.size(); }
        std::size_t operator()(const Long& l) const { return 4 * l.v.size(); }
        std::size_t operator()(const Rational& r) const { return 8 * r.v.size(); }
        std::size_t operator()(const SRational& r) const { return 8 * r.v.size(); }
        std::size_t operator()(const Blobs& b) const { return 4 * b.v.size(); }
        std::size_t operator()(const Ifds& i) const { return 4 * i.v.size(); }
        std::size_t operator()(const IfdBytes& i) const { return ifd_size(t, i.ifd); }
    };
    return std::visit(Size{t}, v);
}

Entries sorted(const TiffBuilder& t, std::size_t i) {
    Entries e = t.ifds[i];
    std::stable_sort(e.begin(), e.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return e;
}

struct Encoded {
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::string bytes;
};

Encoded encode(const TiffBuilder& t, const Val& v, const std::vector<std::size_t>& blob_pos, const std::vector<std::size_t>& ifd_pos) {
    const bool be = t.big_endian;
    const auto b16 = [be](std::string& o, std::uint16_t x) {
        if (be) {
            o.push_back(static_cast<char>(x >> 8));
            o.push_back(static_cast<char>(x & 0xFF));
        } else {
            o.push_back(static_cast<char>(x & 0xFF));
            o.push_back(static_cast<char>(x >> 8));
        }
    };
    const auto b32 = [&](std::string& o, std::uint32_t x) {
        if (be) {
            b16(o, static_cast<std::uint16_t>(x >> 16));
            b16(o, static_cast<std::uint16_t>(x & 0xFFFF));
        } else {
            b16(o, static_cast<std::uint16_t>(x & 0xFFFF));
            b16(o, static_cast<std::uint16_t>(x >> 16));
        }
    };
    Encoded out;
    if (const auto* v1 = std::get_if<Byte>(&v)) {
        out = {1, static_cast<std::uint32_t>(v1->v.size()), std::string(v1->v.begin(), v1->v.end())};
    } else if (const auto* v2 = std::get_if<Undefined>(&v)) {
        out = {7, static_cast<std::uint32_t>(v2->v.size()), std::string(v2->v.begin(), v2->v.end())};
    } else if (const auto* v3 = std::get_if<Ascii>(&v)) {
        out = {2, static_cast<std::uint32_t>(v3->v.size() + 1), v3->v + std::string(1, '\0')};
    } else if (const auto* v4 = std::get_if<Short>(&v)) {
        out = {3, static_cast<std::uint32_t>(v4->v.size()), {}};
        for (const auto s : v4->v) b16(out.bytes, s);
    } else if (const auto* v5 = std::get_if<Long>(&v)) {
        out = {4, static_cast<std::uint32_t>(v5->v.size()), {}};
        for (const auto l : v5->v) b32(out.bytes, l);
    } else if (const auto* v6 = std::get_if<Rational>(&v)) {
        out = {5, static_cast<std::uint32_t>(v6->v.size()), {}};
        for (const auto& [n, d] : v6->v) {
            b32(out.bytes, n);
            b32(out.bytes, d);
        }
    } else if (const auto* v7 = std::get_if<SRational>(&v)) {
        out = {10, static_cast<std::uint32_t>(v7->v.size()), {}};
        for (const auto& [n, d] : v7->v) {
            b32(out.bytes, static_cast<std::uint32_t>(n));
            b32(out.bytes, static_cast<std::uint32_t>(d));
        }
    } else if (const auto* v8 = std::get_if<Blobs>(&v)) {
        out = {4, static_cast<std::uint32_t>(v8->v.size()), {}};
        for (const auto i : v8->v) b32(out.bytes, static_cast<std::uint32_t>(blob_pos[i]));
    } else if (const auto* v9 = std::get_if<Ifds>(&v)) {
        out = {4, static_cast<std::uint32_t>(v9->v.size()), {}};
        for (const auto i : v9->v) b32(out.bytes, static_cast<std::uint32_t>(ifd_pos[i]));
    } else if (const auto* v10 = std::get_if<IfdBytes>(&v)) {
        out = {7, static_cast<std::uint32_t>(ifd_size(t, v10->ifd)), std::string(8, '\0')};
    }
    return out;
}

std::pair<std::int32_t, std::int32_t> srat(double v) { return {static_cast<std::int32_t>(std::lround(v * 10000.0)), 10000}; }
std::pair<std::uint32_t, std::uint32_t> urat(double v) { return {static_cast<std::uint32_t>(std::lround(v * 10000.0)), 10000}; }

}  // namespace

std::string lj92_encode(const std::vector<std::uint16_t>& samples, std::size_t width, std::size_t height, std::size_t components, int precision,
                        int predictor, std::optional<std::size_t> restart_rows) {
    if (samples.size() != width * height * components) throw std::invalid_argument("lj92_encode: samples");
    const std::size_t row = width * components;
    const std::size_t interval = restart_rows.value_or(std::size_t(-1));
    std::vector<int> diffs;
    diffs.reserve(samples.size());
    const int initial = 1 << (precision - 1);
    for (std::size_t y = 0; y < height; ++y) {
        const bool first_line = y % interval == 0;
        for (std::size_t x = 0; x < width; ++x)
            for (std::size_t c = 0; c < components; ++c) {
                const std::size_t i = y * row + x * components + c;
                int px = 0;
                if (x == 0) {
                    px = first_line ? initial : samples[i - row];
                } else if (first_line) {
                    px = samples[i - components];
                } else {
                    const int ra = samples[i - components], rb = samples[i - row], rc = samples[i - row - components];
                    switch (predictor) {
                        case 1: px = ra; break;
                        case 2: px = rb; break;
                        case 3: px = rc; break;
                        case 4: px = ra + rb - rc; break;
                        case 5: px = ra + ((rb - rc) >> 1); break;
                        case 6: px = rb + ((ra - rc) >> 1); break;
                        default: px = (ra + rb) >> 1; break;
                    }
                }
                int d = (static_cast<int>(samples[i]) - px) & 0xFFFF;
                if (d > 32768) d -= 65536;
                diffs.push_back(d);
            }
    }
    std::array<std::size_t, 17> freq{};
    for (const int d : diffs) ++freq[std::size_t(category(d))];
    std::array<int, 17> order{};
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return freq[std::size_t(a)] > freq[std::size_t(b)]; });
    std::array<int, 17> len_of{};
    for (std::size_t rank = 0; rank < order.size(); ++rank) len_of[std::size_t(order[rank])] = kLengths[rank];
    std::array<std::uint8_t, 16> counts{};
    std::string vals;
    std::array<std::pair<unsigned, unsigned>, 17> code_of{};
    unsigned code = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int c = 0; c < 17; ++c) {
            if (len_of[std::size_t(c)] == len) {
                ++counts[std::size_t(len - 1)];
                vals.push_back(static_cast<char>(c));
                code_of[std::size_t(c)] = {code, static_cast<unsigned>(len)};
                ++code;
            }
        }
        code <<= 1;
    }
    std::string out{'\xFF', '\xD8'};
    out += std::string{'\xFF', '\xC3'};  // (SOF3)
    be16(out, static_cast<unsigned>(8 + 3 * components));
    out.push_back(static_cast<char>(precision));
    be16(out, static_cast<unsigned>(height));
    be16(out, static_cast<unsigned>(width));
    out.push_back(static_cast<char>(components));
    for (std::size_t c = 0; c < components; ++c) out += std::string{static_cast<char>(c + 1), '\x11', '\0'};
    out += std::string{'\xFF', '\xC4'};  // (DHT: one table, id 0)
    be16(out, static_cast<unsigned>(2 + 17 + vals.size()));
    out.push_back('\0');
    for (const auto n : counts) out.push_back(static_cast<char>(n));
    out += vals;
    if (restart_rows) {
        out += std::string{'\xFF', '\xDD', '\0', '\x04'};
        be16(out, static_cast<unsigned>(*restart_rows * width));
    }
    out += std::string{'\xFF', '\xDA'};  // (SOS)
    be16(out, static_cast<unsigned>(6 + 2 * components));
    out.push_back(static_cast<char>(components));
    for (std::size_t c = 0; c < components; ++c) out += std::string{static_cast<char>(c + 1), '\0'};
    out += std::string{static_cast<char>(predictor), '\0', '\0'};
    BitWriter w;
    w.out = std::move(out);
    unsigned rst = 0;
    const std::size_t every = interval == std::size_t(-1) ? std::size_t(-1) : interval * row;
    for (std::size_t i = 0; i < diffs.size(); ++i) {
        if (i > 0 && every != std::size_t(-1) && i % every == 0) {
            w.flush();
            w.out += std::string{'\xFF', static_cast<char>(0xD0 + rst)};
            rst = (rst + 1) % 8;
        }
        const int d = diffs[i];
        const int s = category(d);
        const auto [c, l] = code_of[std::size_t(s)];
        w.put(c, l);
        if (s >= 1 && s < 16) {
            const unsigned extra = d < 0 ? static_cast<unsigned>(d - 1) & ((1u << s) - 1) : static_cast<unsigned>(d);
            w.put(extra, static_cast<unsigned>(s));
        }
    }
    w.flush();
    w.out += std::string{'\xFF', '\xD9'};
    return w.out;
}

std::size_t TiffBuilder::ifd(Entries entries) {
    ifds.push_back(std::move(entries));
    return ifds.size() - 1;
}

std::size_t TiffBuilder::blob(std::string data) {
    blobs.push_back(std::move(data));
    return blobs.size() - 1;
}

std::string TiffBuilder::build() const {
    const std::size_t header = cr2_raw_ifd ? 16 : 8;
    std::size_t pos = header;
    std::vector<std::size_t> ifd_pos(ifds.size(), 0);
    std::vector<std::vector<std::size_t>> val_pos(ifds.size());
    std::vector<std::size_t> embedded;
    for (const Entries& entries : ifds)
        for (const auto& [tag, v] : entries)
            if (const auto* inner = std::get_if<IfdBytes>(&v)) embedded.push_back(inner->ifd);
    for (std::size_t i = 0; i < ifds.size(); ++i) {
        if (std::find(embedded.begin(), embedded.end(), i) != embedded.end()) continue;
        ifd_pos[i] = pos;
        pos += ifd_size(*this, i);
        std::vector<std::size_t> vp;
        for (const auto& [tag, v] : sorted(*this, i)) {
            const std::size_t size = val_size(*this, v);
            if (size > 4) {
                vp.push_back(pos);
                if (const auto* inner = std::get_if<IfdBytes>(&v)) {
                    const std::size_t e = inner->ifd;
                    ifd_pos[e] = pos;
                    std::size_t q = pos + ifd_size(*this, e);
                    std::vector<std::size_t> ip;
                    for (const auto& [itag, iv] : sorted(*this, e)) {
                        const std::size_t s = val_size(*this, iv);
                        if (s > 4) {
                            ip.push_back(q);
                            q += s + (s & 1);
                        } else {
                            ip.push_back(0);
                        }
                    }
                    val_pos[e] = ip;
                    pos = q;
                } else {
                    pos += size + (size & 1);
                }
            } else {
                vp.push_back(0);
            }
        }
        val_pos[i] = vp;
    }
    std::vector<std::size_t> blob_pos;
    for (const std::string& b : blobs) {
        blob_pos.push_back(pos);
        pos += b.size() + (b.size() & 1);
    }
    std::string out(pos, '\0');
    const auto w16 = [&](std::size_t at, std::uint16_t v) {
        out[at] = static_cast<char>(big_endian ? v >> 8 : v & 0xFF);
        out[at + 1] = static_cast<char>(big_endian ? v & 0xFF : v >> 8);
    };
    const auto w32 = [&](std::size_t at, std::uint32_t v) {
        if (big_endian) {
            w16(at, static_cast<std::uint16_t>(v >> 16));
            w16(at + 2, static_cast<std::uint16_t>(v & 0xFFFF));
        } else {
            w16(at, static_cast<std::uint16_t>(v & 0xFFFF));
            w16(at + 2, static_cast<std::uint16_t>(v >> 16));
        }
    };
    out.replace(0, 4, big_endian ? std::string("MM\0*", 4) : std::string("II*\0", 4));
    w32(4, static_cast<std::uint32_t>(ifd_pos[chain.at(0)]));
    if (cr2_raw_ifd) {
        out.replace(8, 4, std::string("CR\x02\0", 4));
        w32(12, static_cast<std::uint32_t>(ifd_pos[*cr2_raw_ifd]));
    }
    for (std::size_t i = 0; i < ifds.size(); ++i) {
        const std::size_t at = ifd_pos[i];
        const Entries entries = sorted(*this, i);
        w16(at, static_cast<std::uint16_t>(entries.size()));
        for (std::size_t k = 0; k < entries.size(); ++k) {
            const std::size_t e = at + 2 + 12 * k;
            const auto& [tag, v] = entries[k];
            const Encoded enc = encode(*this, v, blob_pos, ifd_pos);
            w16(e, tag);
            w16(e + 2, enc.type);
            w32(e + 4, enc.count);
            if (enc.bytes.size() <= 4) {
                out.replace(e + 8, enc.bytes.size(), enc.bytes);
            } else {
                const std::size_t vp = val_pos[i][k];
                w32(e + 8, static_cast<std::uint32_t>(vp));
                if (!std::holds_alternative<IfdBytes>(v)) out.replace(vp, enc.bytes.size(), enc.bytes);
            }
        }
        std::size_t next = 0;
        if (const auto p = std::find(chain.begin(), chain.end(), i); p != chain.end() && p + 1 != chain.end()) next = ifd_pos[*(p + 1)];
        w32(at + 2 + 12 * entries.size(), static_cast<std::uint32_t>(next));
    }
    for (std::size_t i = 0; i < blobs.size(); ++i) out.replace(blob_pos[i], blobs[i].size(), blobs[i]);
    return out;
}

DngSpec DngSpec::cfa_of(std::size_t width, std::size_t height, std::vector<std::uint16_t> data) {
    DngSpec d;
    d.width = width;
    d.height = height;
    d.data = std::move(data);
    d.rows = height;
    return d;
}

std::string DngSpec::build() const {
    TiffBuilder t;
    t.big_endian = big_endian;
    const auto pack = [&](std::size_t from, std::size_t to) {
        const std::size_t n = width * samples;
        std::string out;
        if (bits == 8) {
            for (std::size_t i = from * n; i < to * n; ++i) out.push_back(static_cast<char>(data[i] & 0xFF));
        } else if (bits == 16) {
            for (std::size_t i = from * n; i < to * n; ++i) {
                const std::uint16_t v = data[i];
                if (big_endian) {
                    out.push_back(static_cast<char>(v >> 8));
                    out.push_back(static_cast<char>(v & 0xFF));
                } else {
                    out.push_back(static_cast<char>(v & 0xFF));
                    out.push_back(static_cast<char>(v >> 8));
                }
            }
        } else {  // (MSB-first bit packing, each row padded to a byte)
            for (std::size_t y = from; y < to; ++y) {
                std::uint64_t acc = 0;
                int have = 0;
                for (std::size_t i = y * n; i < (y + 1) * n; ++i) {
                    acc = (acc << bits) | data[i];
                    have += bits;
                    while (have >= 8) {
                        have -= 8;
                        out.push_back(static_cast<char>((acc >> have) & 0xFF));
                    }
                }
                if (have > 0) out.push_back(static_cast<char>((acc << (8 - have)) & 0xFF));
            }
        }
        return out;
    };
    Entries raw{{254, Long{{0}}},
                {256, Long{{static_cast<std::uint32_t>(width)}}},
                {257, Long{{static_cast<std::uint32_t>(height)}}},
                {258, Short{std::vector<std::uint16_t>(samples, static_cast<std::uint16_t>(bits))}},
                {262, Short{{static_cast<std::uint16_t>(samples == 1 ? 32803 : 34892)}}},
                {277, Short{{static_cast<std::uint16_t>(samples)}}},
                {284, Short{{1}}},
                {50717, Long{std::vector<std::uint32_t>(samples, white)}},
                {50713, Short{{black_repeat.first, black_repeat.second}}},
                {50714, Long{black}}};
    if (samples == 1) {
        raw.push_back({33421, Short{{2, 2}}});
        raw.push_back({33422, Byte{std::vector<std::uint8_t>(cfa.begin(), cfa.end())}});
        raw.push_back({50710, Byte{{0, 1, 2}}});
        raw.push_back({50711, Short{{1}}});
    }
    const std::size_t strip_rows = rows == 0 ? height : rows;
    if (storage == DngStorage::Strips) {
        std::vector<std::size_t> offs;
        std::vector<std::uint32_t> lens;
        for (std::size_t y = 0; y < height; y += strip_rows) {
            std::string b = pack(y, std::min(y + strip_rows, height));
            lens.push_back(static_cast<std::uint32_t>(b.size()));
            offs.push_back(t.blob(std::move(b)));
        }
        raw.push_back({259, Short{{compression.value_or(1)}}});
        raw.push_back({278, Long{{static_cast<std::uint32_t>(strip_rows)}}});
        raw.push_back({273, Blobs{offs}});
        raw.push_back({279, Long{lens}});
    } else if (storage == DngStorage::Lj92Strips) {
        std::vector<std::size_t> offs;
        std::vector<std::uint32_t> lens;
        for (std::size_t y = 0; y < height; y += strip_rows) {
            const std::size_t r = std::min(y + strip_rows, height) - y;
            const std::size_t n = width * samples;
            const std::vector<std::uint16_t> part(data.begin() + static_cast<std::ptrdiff_t>(y * n), data.begin() + static_cast<std::ptrdiff_t>((y + r) * n));
            std::string j = lj92_encode(part, width, r, samples, bits, 1);
            lens.push_back(static_cast<std::uint32_t>(j.size()));
            offs.push_back(t.blob(std::move(j)));
        }
        raw.push_back({259, Short{{compression.value_or(7)}}});
        raw.push_back({278, Long{{static_cast<std::uint32_t>(strip_rows)}}});
        raw.push_back({273, Blobs{offs}});
        raw.push_back({279, Long{lens}});
    } else {
        std::vector<std::size_t> offs;
        std::vector<std::uint32_t> lens;
        const std::size_t tw = tile_width, th = tile_height, s = samples;
        for (std::size_t ty = 0; ty < height; ty += th)
            for (std::size_t tx = 0; tx < width; tx += tw) {
                std::vector<std::uint16_t> tile(tw * th * s, 0);
                for (std::size_t y = 0; y < th; ++y)
                    for (std::size_t x = 0; x < tw; ++x) {
                        const std::size_t sy = std::min(ty + y, height - 1), sx = std::min(tx + x, width - 1);
                        for (std::size_t c = 0; c < s; ++c) tile[(y * tw + x) * s + c] = data[(sy * width + sx) * s + c];
                    }
                std::string j = s == 1 && tw % 2 == 0 ? lj92_encode(tile, tw / 2, th, 2, bits, 1) : lj92_encode(tile, tw, th, s, bits, 1);
                lens.push_back(static_cast<std::uint32_t>(j.size()));
                offs.push_back(t.blob(std::move(j)));
            }
        raw.push_back({259, Short{{compression.value_or(7)}}});
        raw.push_back({322, Long{{static_cast<std::uint32_t>(tw)}}});
        raw.push_back({323, Long{{static_cast<std::uint32_t>(th)}}});
        raw.push_back({324, Blobs{offs}});
        raw.push_back({325, Long{lens}});
    }
    if (active_area) raw.push_back({50829, Long{std::vector<std::uint32_t>(active_area->begin(), active_area->end())}});
    if (default_crop) {
        raw.push_back({50719, Long{{default_crop->first[0], default_crop->first[1]}}});
        raw.push_back({50720, Long{{default_crop->second[0], default_crop->second[1]}}});
    }
    if (linearization) raw.push_back({50712, Short{*linearization}});
    const std::size_t raw_ifd = t.ifd(raw);
    std::vector<std::size_t> subs{raw_ifd};
    if (preview_jpeg) {  // (the camera's preview: a JPEG of its own SubIFD, as cameras write it)
        const std::size_t jpeg = t.blob(*preview_jpeg);
        const std::size_t ifd = t.ifd(Entries{{254, Long{{1}}},
                                              {259, Short{{7}}},
                                              {262, Short{{6}}},
                                              {273, Blobs{{jpeg}}},
                                              {277, Short{{3}}},
                                              {279, Long{{static_cast<std::uint32_t>(preview_jpeg->size())}}}});
        subs.push_back(ifd);
    }
    const std::size_t thumb = t.blob(std::string(12, '\x80'));
    Entries ifd0{{254, Long{{1}}},
                 {256, Long{{2}}},
                 {257, Long{{2}}},
                 {258, Short{{8, 8, 8}}},
                 {259, Short{{1}}},
                 {262, Short{{2}}},
                 {271, Ascii{make}},
                 {272, Ascii{model}},
                 {273, Blobs{{thumb}}},
                 {274, Short{{orientation}}},
                 {277, Short{{3}}},
                 {278, Long{{2}}},
                 {279, Long{{12}}},
                 {330, Ifds{subs}},
                 {50706, Byte{{1, 4, 0, 0}}},
                 {50708, Ascii{make + " " + model}}};
    const auto m = [](const std::array<double, 9>& v) {
        SRational r;
        for (const double x : v) r.v.push_back(srat(x));
        return r;
    };
    if (color_matrix1) {
        ifd0.push_back({50721, m(color_matrix1->second)});
        ifd0.push_back({50778, Short{{color_matrix1->first}}});
    }
    if (color_matrix2) {
        ifd0.push_back({50722, m(color_matrix2->second)});
        ifd0.push_back({50779, Short{{color_matrix2->first}}});
    }
    if (forward_matrix1) ifd0.push_back({50964, m(*forward_matrix1)});
    if (as_shot_neutral) {
        Rational r;
        for (const double x : *as_shot_neutral) r.v.push_back(urat(x));
        ifd0.push_back({50728, r});
    }
    if (baseline_exposure) ifd0.push_back({50730, SRational{{srat(*baseline_exposure)}}});
    const std::size_t i0 = t.ifd(ifd0);
    t.chain = {i0};
    return t.build();
}

std::string Cr2Spec::build() const {
    const std::size_t w = width, h = height;
    std::vector<std::size_t> widths = slices.empty() ? std::vector<std::size_t>{w} : slices;
    std::vector<std::uint16_t> seq;
    seq.reserve(w * h);
    std::size_t x0 = 0;
    for (const std::size_t sw : widths) {
        for (std::size_t y = 0; y < h; ++y) seq.insert(seq.end(), data.begin() + static_cast<std::ptrdiff_t>(y * w + x0), data.begin() + static_cast<std::ptrdiff_t>(y * w + x0 + sw));
        x0 += sw;
    }
    const std::size_t c = components;
    const std::string jpeg = lj92_encode(seq, w / c, h, c, precision, 1);
    TiffBuilder t;
    const std::size_t jb = t.blob(jpeg);
    Entries raw{{259, Short{{6}}}, {273, Blobs{{jb}}}, {279, Long{{static_cast<std::uint32_t>(jpeg.size())}}}};
    if (!slices.empty()) {
        const std::size_t n = widths.size() - 1;
        raw.push_back({50752, Short{{static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(widths[0]), static_cast<std::uint16_t>(widths[n])}}});
    }
    const std::size_t raw_ifd = t.ifd(raw);
    Entries mn;
    if (borders) {
        const auto& [l, tp, r, b] = *borders;
        mn.push_back({0x00E0, Short{{34, static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(h), 0, 0, l, tp, r, b, 0, 0, 0, 0, 0, 0, 0, 0}}});
    }
    if (wb_rggb) {
        std::vector<std::uint16_t> cd(1273, 0);
        std::copy(wb_rggb->begin(), wb_rggb->end(), cd.begin() + 0x3F);
        mn.push_back({0x4001, Short{cd}});
    }
    Entries exif{{33434, Rational{{{1, 100}}}}};
    if (!mn.empty()) {
        const std::size_t mi = t.ifd(mn);
        exif.push_back({37500, IfdBytes{mi}});
    }
    const std::size_t exif_ifd = t.ifd(exif);
    const std::size_t thumb = t.blob(std::string(12, '\x80'));
    const std::size_t ifd0 = t.ifd(Entries{{256, Long{{2}}},
                                           {257, Long{{2}}},
                                           {259, Short{{1}}},
                                           {262, Short{{2}}},
                                           {271, Ascii{"Canon"}},
                                           {272, Ascii{model}},
                                           {273, Blobs{{thumb}}},
                                           {274, Short{{orientation}}},
                                           {277, Short{{3}}},
                                           {279, Long{{12}}},
                                           {34665, Ifds{{exif_ifd}}}});
    const std::size_t ifd1 = t.ifd(Entries{{256, Long{{1}}}});
    const std::size_t ifd2 = t.ifd(Entries{{256, Long{{1}}}});
    t.chain = {ifd0, ifd1, ifd2, raw_ifd};
    t.cr2_raw_ifd = raw_ifd;
    return t.build();
}

std::string tiff_ep(const std::string& make, const std::string& model, std::size_t width, std::size_t height, const std::vector<std::uint16_t>& data,
                    std::array<std::uint8_t, 4> cfa, int bits, Entries extra_raw_tags) {
    TiffBuilder t;
    std::string strip;
    for (const auto v : data) {
        strip.push_back(static_cast<char>(v & 0xFF));
        strip.push_back(static_cast<char>(v >> 8));
    }
    const std::size_t s = t.blob(std::move(strip));
    Entries raw{{254, Long{{0}}},
                {256, Long{{static_cast<std::uint32_t>(width)}}},
                {257, Long{{static_cast<std::uint32_t>(height)}}},
                {258, Short{{static_cast<std::uint16_t>(bits)}}},
                {259, Short{{1}}},
                {262, Short{{32803}}},
                {273, Blobs{{s}}},
                {277, Short{{1}}},
                {278, Long{{static_cast<std::uint32_t>(height)}}},
                {279, Long{{static_cast<std::uint32_t>(data.size() * 2)}}},
                {33421, Short{{2, 2}}},
                {33422, Byte{std::vector<std::uint8_t>(cfa.begin(), cfa.end())}}};
    for (auto& tag : extra_raw_tags) raw.push_back(std::move(tag));
    const std::size_t raw_ifd = t.ifd(raw);
    const std::size_t thumb = t.blob(std::string(12, '\x80'));
    Entries ifd0{{254, Long{{1}}},       {256, Long{{2}}},         {257, Long{{2}}},     {259, Short{{1}}},
                 {262, Short{{2}}},      {271, Ascii{make}},       {273, Blobs{{thumb}}}, {277, Short{{3}}},
                 {279, Long{{12}}},      {330, Ifds{{raw_ifd}}}};
    if (!model.empty()) ifd0.push_back({272, Ascii{model}});
    t.chain = {t.ifd(ifd0)};
    return t.build();
}

std::vector<std::array<float, 3>> scene(std::size_t width, std::size_t height) {
    std::vector<std::array<float, 3>> out(width * height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float x = static_cast<float>(i % width) / static_cast<float>(std::max<std::size_t>(width, 1));
        const float y = static_cast<float>(i / width) / static_cast<float>(std::max<std::size_t>(height, 1));
        out[i] = {0.1f + 0.7f * x, 0.15f + 0.5f * (1.0f - y) * x + 0.2f * y, 0.6f - 0.4f * y + 0.2f * x};
    }
    return out;
}

std::vector<std::uint16_t> mosaic(const std::vector<std::array<float, 3>>& rgb, std::size_t width, std::array<std::uint8_t, 4> cfa, std::uint16_t black,
                                  std::uint16_t white) {
    std::vector<std::uint16_t> out(rgb.size());
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        const std::size_t x = i % width, y = i / width;
        const std::size_t c = cfa[(y & 1) * 2 + (x & 1)];
        const float v = std::clamp(rgb[i][c], 0.0f, 1.0f);
        out[i] = static_cast<std::uint16_t>(std::lround(static_cast<float>(black) + v * static_cast<float>(white - black)));
    }
    return out;
}

}  // namespace genko::test::rawgen
