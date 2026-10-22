#include "render/pat.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>

#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/patterns.hpp"
#include "core/ruby.hpp"
#include "render/patterns.hpp"
#include "render/psd_descriptor.hpp"

// PhotoCraft v0.2.0 crates/psd/src/patterns.rs (read_pattern, write_pattern, the block and the .pat file) and
// crates/io/src/pattern_map.rs (from_psd, to_psd), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::pat {

namespace {

[[noreturn]] void broken(const std::string& why) { throw core::Error("format", "the patterns cannot be read: " + why); }

struct Reader {
    std::string_view b;
    std::size_t p = 0;
    std::string_view take(std::size_t n) {
        if (n > b.size() - p) broken("they end early");
        const std::string_view out = b.substr(p, n);
        p += n;
        return out;
    }
    std::uint32_t u8() { return static_cast<unsigned char>(take(1)[0]); }
    std::uint32_t u16() {
        const auto s = take(2);
        return (std::uint32_t(static_cast<unsigned char>(s[0])) << 8) | static_cast<unsigned char>(s[1]);
    }
    std::uint32_t u32() {
        const auto s = take(4);
        return (std::uint32_t(static_cast<unsigned char>(s[0])) << 24) | (std::uint32_t(static_cast<unsigned char>(s[1])) << 16) |
               (std::uint32_t(static_cast<unsigned char>(s[2])) << 8) | static_cast<unsigned char>(s[3]);
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
};

void put_u8(std::string& out, unsigned v) { out.push_back(static_cast<char>(v & 0xff)); }
void put_u16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}
void put_u32(std::string& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((v >> s) & 0xff));
}

// PackBits (a PSD's RLE): one row.
void unpack_row(std::string_view in, char* out, std::size_t size) {
    std::size_t i = 0, o = 0;
    while (i < in.size() && o < size) {
        const int h = static_cast<signed char>(in[i++]);
        if (h >= 0) {
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(h) + 1, std::min(in.size() - i, size - o));
            std::memcpy(out + o, in.data() + i, n);
            i += static_cast<std::size_t>(h) + 1;
            o += n;
        } else if (h != -128) {
            if (i >= in.size()) break;
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(1 - h), size - o);
            std::memset(out + o, in[i++], n);
            o += n;
        }
    }
    if (o != size) broken("a row of PackBits is short");
}

std::string pack_row(std::string_view row) {
    std::string out;
    std::size_t i = 0;
    const std::size_t n = row.size();
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 2) {
            out.push_back(static_cast<char>(1 - static_cast<int>(run)));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        std::size_t lit = 1;  // (bytes until a run of two starts, at most 128)
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && row[i + lit] == row[i + lit + 1])) ++lit;
        out.push_back(static_cast<char>(lit - 1));
        out.append(row.substr(i, lit));
        i += lit;
    }
    return out;
}

std::size_t row_bytes(std::uint32_t width, unsigned depth) { return depth == 1 ? (width + 7) / 8 : std::size_t(width) * (depth / 8); }

std::string decode_plane(unsigned compression, std::string_view data, std::uint32_t width, std::uint32_t height, unsigned depth,
                         std::uint64_t& budget) {
    const std::size_t rb = row_bytes(width, depth);
    const std::uint64_t size = std::uint64_t(rb) * height;
    if (size > budget) throw core::Error("too_large", "the patterns decode to more than 512 MiB");
    budget -= size;
    std::string out(static_cast<std::size_t>(size), '\0');
    if (compression == 0) {
        if (data.size() < size) broken("a channel is short");
        std::memcpy(out.data(), data.data(), static_cast<std::size_t>(size));
        return out;
    }
    if (compression != 1) broken("a channel's compression is " + std::to_string(compression));
    Reader r{data};
    std::vector<std::uint32_t> counts(height);
    for (auto& c : counts) c = r.u16();
    for (std::uint32_t y = 0; y < height; ++y) unpack_row(r.take(counts[y]), out.data() + std::size_t(y) * rb, rb);
    return out;
}

std::string encode_plane(std::string_view plane, std::uint32_t width, std::uint32_t height, unsigned depth, bool rle) {
    if (!rle) return std::string(plane);
    const std::size_t rb = row_bytes(width, depth);
    std::string counts, rows;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::string packed = pack_row(plane.substr(std::size_t(y) * rb, rb));
        put_u16(counts, static_cast<std::uint32_t>(packed.size()));
        rows += packed;
    }
    return counts + rows;
}

unsigned mode_channels(std::uint32_t mode) { return mode == 3 || mode == 9 ? 3u : mode == 4 ? 4u : 1u; }

PsdPattern read_pattern(Reader& r, std::uint64_t& budget) {
    PsdPattern p;
    if (const std::uint32_t version = r.u32(); version != 1) broken("a pattern of version " + std::to_string(version));
    p.mode = r.u32();
    (void)r.u16();
    (void)r.u16();
    const std::uint32_t units = r.u32();
    if (units > 65536) broken("a pattern's name is too long");
    const std::string_view name = r.take(std::size_t(units) * 2);
    std::u16string text;
    for (std::size_t i = 0; i + 1 < name.size(); i += 2)
        text.push_back(static_cast<char16_t>((static_cast<unsigned char>(name[i]) << 8) | static_cast<unsigned char>(name[i + 1])));
    while (!text.empty() && text.back() == u'\0') text.pop_back();
    p.name = psd::utf8_of(text);
    p.id = std::string(r.take(r.u8()));
    if (p.mode == 2) {
        p.palette = std::string(r.take(768));
        // (some writers follow the palette with 4 bytes more, which a reader skips: as ag-psd (MIT) reads them — its
        // behaviour taken, no code)
        if (r.b.size() - r.p >= 4 && r.b.substr(r.p, 4) != std::string_view("\0\0\0\3", 4)) (void)r.take(4);
    }
    if (const std::uint32_t v = r.u32(); v != 3) broken("a virtual memory array list of version " + std::to_string(v));
    const std::uint32_t length = r.u32();
    Reader v{r.take(length)};
    const std::int32_t top = v.i32(), left = v.i32(), bottom = v.i32(), right = v.i32();
    const auto w = static_cast<std::uint32_t>(std::max<std::int64_t>(0, std::int64_t(right) - left));
    const auto h = static_cast<std::uint32_t>(std::max<std::int64_t>(0, std::int64_t(bottom) - top));
    if (w > kMaxEdge || h > kMaxEdge) broken("a pattern is larger than " + std::to_string(kMaxEdge) + " pixels a side");
    p.width = w;
    p.height = h;
    // (one no book takes — past 8192 a side or 64 M pixels —: its channels never decoded; from_psd says why it is not
    // taken, and the next pattern is read)
    if (w > core::kMaxPatternSide || h > core::kMaxPatternSide || std::uint64_t(w) * h > std::uint64_t(core::kMaxPatternPixels)) return p;
    const std::uint32_t count = v.u32();
    if (count > 64) broken("a pattern has too many channels");
    // (its colours and its transparency are what is kept: the arrays past them are not decoded)
    const unsigned nc = mode_channels(p.mode);
    const std::uint64_t kept_planes = std::min<std::uint64_t>(std::uint64_t(count) + 2, nc + 1);
    std::vector<std::string> planes;
    for (std::uint32_t k = 0; k < count + 2; ++k) {
        if (v.p >= v.b.size()) break;
        if (v.u32() == 0) continue;
        const std::uint32_t alen = v.u32();
        if (alen == 0) continue;
        if (alen < 23) broken("a virtual memory array is short");
        if (planes.size() > nc) {
            (void)v.take(alen);
            continue;
        }
        const std::uint32_t d32 = v.u32();
        const std::int32_t ct = v.i32(), cl = v.i32(), cb = v.i32(), cr = v.i32();
        const std::uint32_t d16 = v.u16();
        const std::uint32_t comp = v.u8();
        const std::string_view data = v.take(alen - 23);
        const unsigned d = (d16 == 1 || d16 == 8 || d16 == 16 || d16 == 32) ? d16 : d32;
        if (d != 1 && d != 8 && d != 16 && d != 32) broken("a pattern of depth " + std::to_string(d));
        p.depth = static_cast<std::uint16_t>(d);
        const auto cw = static_cast<std::uint32_t>(std::max<std::int64_t>(0, std::int64_t(cr) - cl));
        const auto ch = static_cast<std::uint32_t>(std::max<std::int64_t>(0, std::int64_t(cb) - ct));
        if (cw > kMaxEdge || ch > kMaxEdge) broken("a pattern's channel is too large");
        std::string plane = decode_plane(comp, data, cw, ch, d, budget);
        if (d != 1 && (cw != w || ch != h)) {  // (a channel's rect smaller than the pattern's, placed in a whole plane)
            const std::size_t bpp = d / 8;
            // (in the budget before any is made — the planes it will keep at most at once —: a small file never makes GBs)
            const std::uint64_t whole = std::uint64_t(w) * h * bpp;
            if (whole * (kept_planes - planes.size()) > budget) throw core::Error("too_large", "the patterns decode to more than 512 MiB");
            budget -= whole;
            std::string full(std::size_t(w) * h * bpp, '\0');
            for (std::uint32_t y = 0; y < ch; ++y) {
                const std::int64_t ty = std::int64_t(y) + ct - top;
                if (ty < 0 || ty >= h) continue;
                for (std::uint32_t x = 0; x < cw; ++x) {
                    const std::int64_t tx = std::int64_t(x) + cl - left;
                    if (tx < 0 || tx >= w) continue;
                    std::memcpy(full.data() + (std::size_t(ty) * w + std::size_t(tx)) * bpp, plane.data() + (std::size_t(y) * cw + x) * bpp, bpp);
                }
            }
            plane = std::move(full);
        }
        planes.push_back(std::move(plane));
    }
    if (planes.size() < nc) broken("a pattern has fewer channels than its colour mode");
    if (planes.size() > nc) p.alpha = std::move(planes[nc]);
    planes.resize(nc);
    p.channels = std::move(planes);
    return p;
}

void write_pattern(const PsdPattern& p, std::string& out) {
    if (p.width > kMaxEdge || p.height > kMaxEdge || p.width > 32767 || p.height > 32767) throw core::Error("value", "the pattern is too large to write");
    put_u32(out, 1);
    put_u32(out, p.mode);
    put_u16(out, p.height);
    put_u16(out, p.width);
    std::u16string name = psd::utf16(p.name);
    name.push_back(u'\0');
    put_u32(out, static_cast<std::uint32_t>(name.size()));
    for (const char16_t u : name) put_u16(out, u);
    const std::size_t idl = std::min<std::size_t>(p.id.size(), 255);
    put_u8(out, static_cast<unsigned>(idl));
    out.append(p.id, 0, idl);
    if (p.mode == 2) {
        std::string pal = p.palette;
        pal.resize(768, '\0');
        out += pal;
    }
    std::string body;
    const auto rect = [&](std::string& s) {
        put_u32(s, 0);
        put_u32(s, 0);
        put_u32(s, p.height);
        put_u32(s, p.width);
    };
    rect(body);
    constexpr std::uint32_t kSlots = 24;
    put_u32(body, kSlots);
    const auto array = [&](const std::string& plane) {
        const std::string data = encode_plane(plane, p.width, p.height, p.depth, p.depth == 8);
        put_u32(body, 1);
        put_u32(body, static_cast<std::uint32_t>(23 + data.size()));
        put_u32(body, p.depth);
        rect(body);
        put_u16(body, p.depth);
        put_u8(body, p.depth == 8 ? 1 : 0);
        body += data;
    };
    const std::size_t nc = std::min<std::size_t>(p.channels.size(), kSlots);
    for (std::size_t c = 0; c < nc; ++c) array(p.channels[c]);
    for (std::size_t c = nc; c < kSlots; ++c) put_u32(body, 0);
    if (p.alpha) {
        array(*p.alpha);
    } else {
        put_u32(body, 0);
    }
    put_u32(body, 0);  // (the sheet mask: none)
    put_u32(out, 3);
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    out += body;
}

bool is_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

float sample_of(std::string_view plane, std::size_t i, unsigned depth) {
    if (depth == 8) return static_cast<unsigned char>(plane[i]) / 255.0f;
    if (depth == 16) return ((static_cast<unsigned char>(plane[2 * i]) << 8) | static_cast<unsigned char>(plane[2 * i + 1])) / 65535.0f;
    std::uint32_t bits = 0;
    for (int k = 0; k < 4; ++k) bits = (bits << 8) | static_cast<unsigned char>(plane[4 * i + k]);
    const float f = std::bit_cast<float>(bits);
    return std::isfinite(f) ? f : 0.0f;
}

void put_sample(std::string& plane, float v, unsigned depth) {
    if (depth == 8) {
        plane.push_back(static_cast<char>(std::lround(std::clamp(static_cast<double>(v), 0.0, 1.0) * 255.0)));
    } else if (depth == 16) {
        put_u16(plane, static_cast<std::uint32_t>(std::lround(std::clamp(static_cast<double>(v), 0.0, 1.0) * 65535.0)));
    } else {
        put_u32(plane, std::bit_cast<std::uint32_t>(v));
    }
}

}  // namespace

std::vector<PsdPattern> parse_block(std::string_view data) {
    if (data.size() > kMaxFileBytes) throw core::Error("too_large", "the patterns are larger than 512 MiB");
    std::uint64_t budget = kMaxDecodedBytes;
    Reader r{data};
    std::vector<PsdPattern> out;
    while (data.size() - r.p >= 4) {
        const std::uint32_t length = r.u32();
        if (length == 0) break;
        Reader one{r.take(length)};
        out.push_back(read_pattern(one, budget));
        r.p = std::min(data.size(), r.p + (4 - length % 4) % 4);
    }
    return out;
}

std::string write_block(const std::vector<PsdPattern>& patterns) {
    std::string out;
    for (const PsdPattern& p : patterns) {
        std::string one;
        write_pattern(p, one);
        put_u32(out, static_cast<std::uint32_t>(one.size()));
        out += one;
        out.append((4 - one.size() % 4) % 4, '\0');
    }
    return out;
}

std::vector<PsdPattern> parse_file(std::string_view data) {
    if (data.size() > kMaxFileBytes) throw core::Error("too_large", "the pattern file is larger than 512 MiB");
    std::uint64_t budget = kMaxDecodedBytes;
    Reader r{data};
    if (data.size() < 4 || r.take(4) != "8BPT") throw core::Error("format", "not a pattern file (.pat: it begins with 8BPT)");
    (void)r.u16();
    const std::uint32_t count = r.u32();
    std::vector<PsdPattern> out;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 100000); ++i) out.push_back(read_pattern(r, budget));
    return out;
}

std::string write_file(const std::vector<PsdPattern>& patterns) {
    std::string out = "8BPT";
    put_u16(out, 1);
    put_u32(out, static_cast<std::uint32_t>(patterns.size()));
    for (const PsdPattern& p : patterns) write_pattern(p, out);
    return out;
}

std::string_view block_key(int depth) { return depth == 16 ? "Pat2" : depth == 32 ? "Pat3" : "Patt"; }

std::string shown_name(std::string_view name) {
    if (name.starts_with("$$$/")) {
        if (const auto eq = name.rfind('='); eq != std::string_view::npos) return std::string(name.substr(eq + 1));
    }
    return std::string(name);
}

std::optional<Taken> from_psd(const PsdPattern& p, double dpi, std::string* why) {
    const auto refuse = [&](const std::string& reason) -> std::optional<Taken> {
        if (why != nullptr) *why = reason;
        return std::nullopt;
    };
    const std::uint32_t w = p.width, h = p.height;
    if (w == 0 || h == 0) return refuse("it has no pixels");
    if (w > core::kMaxPatternSide || h > core::kMaxPatternSide || std::int64_t(w) * h > core::kMaxPatternPixels) {
        return refuse("it is larger than 8192 pixels a side or 64 million pixels");
    }
    const std::size_t n = std::size_t(w) * h;
    // (1 bit: 8-bit grey — a set bit black; other depths read where they are, never copied)
    std::vector<std::string> ones;
    unsigned depth = p.depth;
    if (depth == 1) {
        const std::size_t rb = (w + 7) / 8;
        for (const std::string& plane : p.channels) {
            std::string grey(n, '\0');
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t at = (i / w) * rb + (i % w) / 8;
                const bool set = at < plane.size() && (static_cast<unsigned char>(plane[at]) & (0x80 >> (i % w % 8))) != 0;
                grey[i] = static_cast<char>(set ? 0 : 255);
            }
            ones.push_back(std::move(grey));
        }
        depth = 8;
    }
    const std::vector<std::string>& planes = p.depth == 1 ? ones : p.channels;
    const std::size_t plane_size = n * (depth / 8);
    for (const std::string& plane : planes)
        if (plane.size() < plane_size) return refuse("a channel is short");
    if (p.alpha && p.depth != 1 && p.alpha->size() < plane_size) return refuse("its transparency is short");
    patterns::Tile t;
    t.width = w;
    t.height = h;
    t.depth = static_cast<int>(depth);
    switch (p.mode) {
        case 2:  // (indexed: RGB through its palette)
            if (planes.empty() || p.palette.size() < 768) return refuse("its palette cannot be read");
            t.mode = "rgb";
            t.depth = 8;
            t.colours = 3;
            break;
        case 3:
        case 9:
            if (planes.size() < 3) return refuse("it has fewer channels than its colour mode");
            t.mode = p.mode == 3 ? "rgb" : "lab";
            t.colours = 3;
            break;
        case 4:
            if (planes.size() < 4) return refuse("it has fewer channels than its colour mode");
            t.mode = "cmyk";
            t.colours = 4;
            break;
        default:  // (grey, duotone, multichannel: the first channel as grey)
            if (planes.empty()) return refuse("it has no channels");
            t.mode = "gray";
            t.colours = 1;
            break;
    }
    // (each value straight into the tile — 32-bit Lab and CMYK hold 0..1; an 8-bit grey of 32 bits stays as it is)
    const unsigned all = t.colours + 1;
    t.values.assign(n * all, 1.0f);
    const bool clamped = t.mode != "rgb" && !(t.mode == "gray" && t.depth == 32);
    for (std::size_t i = 0; i < n; ++i) {
        float* o = t.values.data() + i * all;
        for (unsigned c = 0; c < t.colours; ++c) {
            float v = p.mode == 2 ? static_cast<unsigned char>(p.palette[static_cast<unsigned char>(planes[0][i]) * 3 + c]) / 255.0f
                                  : sample_of(planes[c], i, depth);
            if (p.mode == 4) v = 1.0f - v;  // (a PSD's CMYK: 0 is all ink)
            o[c] = clamped ? std::clamp(v, 0.0f, 1.0f) : v;
        }
        if (p.alpha && p.depth != 1) o[t.colours] = std::clamp(sample_of(*p.alpha, i, depth), 0.0f, 1.0f);
    }
    Taken out;
    std::string bytes = patterns::encode(t);
    out.pattern.mode = t.mode;
    out.pattern.depth = t.depth;
    out.pattern.width = w;
    out.pattern.height = h;
    out.pattern.dpi = std::clamp(dpi, 50.0, 2400.0);
    std::string name = shown_name(p.name);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) name.pop_back();
    const std::u32string points = core::ruby::code_points(name);
    if (points.size() > 255) name = core::ruby::to_utf8(std::u32string_view(points).substr(0, 255));
    out.pattern.name = name.empty() ? std::string("パターン") : name;
    out.pattern.asset = core::bytes_ref(bytes);
    out.bytes = std::make_shared<const std::string>(std::move(bytes));
    out.file_id = p.id;
    return out;
}

PsdPattern to_psd(const core::Pattern& pattern, std::string_view bytes, const std::string& id) {
    const patterns::Tile t = patterns::decode(pattern, bytes);
    PsdPattern p;
    p.mode = t.mode == "gray" ? 1 : t.mode == "cmyk" ? 4 : t.mode == "lab" ? 9 : 3;
    p.width = t.width;
    p.height = t.height;
    p.name = pattern.name;
    p.id = id;
    p.depth = static_cast<std::uint16_t>(t.depth);
    const std::size_t n = std::size_t(t.width) * t.height;
    const unsigned all = t.colours + 1;
    p.channels.assign(t.colours, std::string());
    bool clear = false;
    for (std::size_t i = 0; i < n; ++i) {
        for (unsigned c = 0; c < t.colours; ++c) {
            const float v = t.values[i * all + c];
            put_sample(p.channels[c], t.mode == "cmyk" ? 1.0f - v : v, p.depth);
        }
        if (t.values[i * all + t.colours] < 1.0f) clear = true;
    }
    if (clear) {
        std::string alpha;
        for (std::size_t i = 0; i < n; ++i) put_sample(alpha, t.values[i * all + t.colours], p.depth);
        p.alpha = std::move(alpha);
    }
    return p;
}

std::string global_block(const core::Document& book, const std::set<std::string>& ids, int depth) {
    std::vector<PsdPattern> written;
    for (const core::Pattern& p : book.patterns) {
        if (!ids.contains(p.id)) continue;
        const auto file = book.pattern_files.find(p.asset);
        if (file == book.pattern_files.end() || !file->second) continue;
        written.push_back(to_psd(p, *file->second, file_id(p)));
    }
    if (written.empty()) return {};
    std::string data = write_block(written);
    std::string out = "8BIM";
    out += block_key(depth);
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    out += data;
    return out;
}

std::string file_id(const core::Pattern& pattern) {
    if (is_uuid(pattern.id)) return pattern.id;
    const std::string hash = core::bytes_ref(pattern.asset + "|" + pattern.id).substr(7);  // (64 hex digits)
    return hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-" + hash.substr(12, 4) + "-" + hash.substr(16, 4) + "-" + hash.substr(20, 12);
}

}  // namespace genko::render::pat
