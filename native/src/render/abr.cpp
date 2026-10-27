#include "render/abr.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/brushes.hpp"
#include "render/fill.hpp"
#include "render/png.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::abr {

namespace {

using core::Json;

[[noreturn]] void abr_error(const std::string& message) { throw core::PyValueError(message); }

using TipSink = std::function<void(Tip&&)>;

class Reader {
public:
    Reader(std::string_view data, std::int64_t max_pixels) : data_(data), pixels_left_(max_pixels) {}
    std::string_view take(std::size_t n) {
        if (pos_ > data_.size() || n > data_.size() - pos_) abr_error("the file ends too early");
        const std::string_view out = data_.substr(pos_, n);
        pos_ += n;
        return out;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }
    std::uint32_t big(std::size_t n) {
        const std::string_view bytes = take(n);
        std::uint32_t v = 0;
        for (const char c : bytes) v = (v << 8) | static_cast<std::uint8_t>(c);
        return v;
    }
    std::int32_t i16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(big(2))); }
    std::uint32_t u16() { return big(2); }
    std::int32_t i32() { return static_cast<std::int32_t>(big(4)); }
    std::uint32_t u32() { return big(4); }
    std::size_t pos() const { return pos_; }
    void seek(std::size_t pos) { pos_ = pos; }  // (past the end as Python's slicing allows: the next take fails)
    std::size_t size() const { return data_.size(); }
    // The tips' pixels counted against what one file may hold (a file of rows packed to nothing, or of records that
    // point back at the same tip, would otherwise unpack far more than it holds).
    void charge(std::int64_t pixels) {
        pixels_left_ -= pixels;
        if (pixels_left_ < 0) throw core::Error("abr_too_large", "the file's tips are too large to read");
    }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
    std::int64_t pixels_left_;
};

// A record that does not move on (its end at or before its start) is a broken file, not one to read again.
void moves_on(std::int64_t end, std::int64_t start) {
    if (end <= start) abr_error("a brush record points back into the file");
}

// PackBits rows: a count for each row first, then the rows.
std::string unpack_bits(Reader& reader, int width, int height) {
    std::vector<std::uint32_t> lengths;
    lengths.reserve(static_cast<std::size_t>(height));
    for (int r = 0; r < height; ++r) lengths.push_back(reader.u16());
    std::string out;
    out.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (const std::uint32_t length : lengths) {
        const std::string_view row = reader.take(length);
        std::string line;
        std::size_t i = 0;
        while (i < row.size() && line.size() < static_cast<std::size_t>(width)) {
            const int n = static_cast<std::uint8_t>(row[i]) < 128 ? static_cast<std::uint8_t>(row[i]) : static_cast<std::uint8_t>(row[i]) - 256;
            ++i;
            if (n >= 0) {  // (row[i:i + n + 1]: what is there)
                line.append(row.substr(std::min(i, row.size()), static_cast<std::size_t>(n) + 1));
                i += static_cast<std::size_t>(n) + 1;
            } else if (n > -128) {
                if (i >= row.size()) throw core::PyUncaught("IndexError", "index out of range");
                line.append(static_cast<std::size_t>(1 - n), row[i]);
                ++i;
            }
        }
        line.resize(std::min(line.size(), static_cast<std::size_t>(width)));
        line.resize(static_cast<std::size_t>(width), '\0');  // (.ljust(width, b"\0"))
        out += line;
    }
    return out;
}

Image tip_image(Reader& reader, std::int64_t width, std::int64_t height, int depth, bool compressed) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) abr_error("a tip has no size");
    if (depth != 8) abr_error("only 8-bit tips are read (this one is " + std::to_string(depth) + "-bit)");
    const int w = static_cast<int>(width), h = static_cast<int>(height);
    reader.charge(width * height);
    const std::string data = compressed ? unpack_bits(reader, w, h) : std::string(reader.take(static_cast<std::size_t>(w) * static_cast<std::size_t>(h)));
    return Image::frombytes("L", Size{w, h}, data);
}

// bytes.decode("utf-16-be", "replace")
std::string utf16be(std::string_view bytes) {
    std::string out;
    const auto put = [&out](std::uint32_t u) {
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xc0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3f));
        } else if (u < 0x10000) {
            out += static_cast<char>(0xe0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (u & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (u >> 18));
            out += static_cast<char>(0x80 | ((u >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (u & 0x3f));
        }
    };
    constexpr std::uint32_t kReplacement = 0xfffd;
    std::size_t i = 0;
    while (i + 1 < bytes.size()) {
        const std::uint32_t unit = (static_cast<std::uint8_t>(bytes[i]) << 8) | static_cast<std::uint8_t>(bytes[i + 1]);
        i += 2;
        if (unit >= 0xd800 && unit < 0xdc00) {  // a high surrogate: its low one next, or a replacement
            if (i + 1 < bytes.size()) {
                const std::uint32_t low = (static_cast<std::uint8_t>(bytes[i]) << 8) | static_cast<std::uint8_t>(bytes[i + 1]);
                if (low >= 0xdc00 && low < 0xe000) {
                    put(0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00));
                    i += 2;
                    continue;
                }
            }
            put(kReplacement);
        } else if (unit >= 0xdc00 && unit < 0xe000) {
            put(kReplacement);
        } else {
            put(unit);
        }
    }
    if (i < bytes.size()) put(kReplacement);  // (an odd byte at the end)
    return out;
}

// The first `n` characters of UTF-8 text (Python's str[:n]).
std::string first_chars(const std::string& text, std::size_t n) {
    std::size_t at = 0, count = 0;
    while (at < text.size() && count < n) {
        const auto lead = static_cast<std::uint8_t>(text[at]);
        at += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        ++count;
    }
    return text.substr(0, std::min(at, text.size()));
}

void read_v12(Reader& reader, int version, const TipSink& take) {
    const std::uint32_t count = reader.u16();
    for (std::uint32_t n = 0; n < count; ++n) {
        const auto start = static_cast<std::int64_t>(reader.pos());
        const int kind = reader.i16();
        const std::int64_t size = reader.i32();
        const std::int64_t end = static_cast<std::int64_t>(reader.pos()) + size;
        moves_on(end, start);
        const auto go_to_end = [&] { reader.seek(static_cast<std::size_t>(end)); };
        if (kind != 2) {  // (computed round tips carry no picture)
            go_to_end();
            continue;
        }
        (void)reader.i32();  // misc
        const int spacing = reader.i16();
        std::string name;
        if (version == 2) {
            const std::uint32_t length = reader.u32();
            name = utf16be(reader.take(static_cast<std::size_t>(length) * 2));
            while (!name.empty() && name.back() == '\0') name.pop_back();
        }
        (void)reader.u8();  // antialiasing
        for (int k = 0; k < 4; ++k) (void)reader.i16();  // short bounds (an older copy)
        const std::int64_t top = reader.i32(), left = reader.i32(), bottom = reader.i32(), right = reader.i32();
        const int depth = reader.i16();
        const bool compressed = reader.u8() != 0;
        Image image = tip_image(reader, right - left, bottom - top, depth, compressed);
        take(Tip{name.empty() ? "ブラシ " + std::to_string(n + 1) : name, std::move(image), spacing});
        go_to_end();
    }
}

void read_v6(Reader& reader, int subversion, const TipSink& take) {
    std::size_t taken = 0;
    while (reader.pos() + 12 <= reader.size()) {
        if (reader.take(4) != "8BIM") abr_error("a section does not start with 8BIM");
        const std::string_view key = reader.take(4);
        const std::size_t size = reader.u32();
        const std::size_t end = reader.pos() + size;
        if (key != "samp") {
            reader.seek(end);
            continue;
        }
        while (reader.pos() < end) {
            const std::size_t length = reader.u32();
            const std::size_t padded = length + ((4 - length % 4) % 4);
            const std::size_t brush_end = reader.pos() + padded;
            (void)reader.take(subversion == 1 ? 47 : 301);  // the tip's id and Photoshop's own fields
            const std::int64_t top = reader.i32(), left = reader.i32(), bottom = reader.i32(), right = reader.i32();
            const int depth = reader.i16();
            const bool compressed = reader.u8() != 0;
            std::optional<Image> image;
            try {
                image = tip_image(reader, right - left, bottom - top, depth, compressed);
            } catch (const core::PyValueError&) {
                // (a tip this does not read: the next one)
            }
            if (image) take(Tip{"ブラシ " + std::to_string(++taken), std::move(*image), 25});
            reader.seek(brush_end);
        }
        reader.seek(end);
    }
}

// The tips one by one as they are read (each let go before the next is unpacked).
void read_tips(std::string_view data, std::int64_t max_pixels, const TipSink& take) {
    Reader reader(data, max_pixels);
    const int version = reader.i16();
    if (version == 1 || version == 2) {
        read_v12(reader, version, take);
    } else if (version == 6 || version == 7 || version == 10) {
        read_v6(reader, reader.i16(), take);
    } else {
        abr_error("version " + std::to_string(version) + " brush files are not read");
    }
}

}  // namespace

std::vector<Tip> read(std::string_view data, std::int64_t max_pixels) {
    std::vector<Tip> tips;
    read_tips(data, max_pixels, [&tips](Tip&& tip) { tips.push_back(std::move(tip)); });
    if (tips.empty()) abr_error("the file has no picture tips");
    return tips;
}

std::string tip_png(const Image& image, int longest) {
    Image small = image.convert("L");
    if (std::max(small.width(), small.height()) > longest) small.thumbnail(Size{longest, longest});
    return core::b64encode(fills::png_data(small));
}

std::vector<Json> brushes_from(std::string_view data, std::string_view prefix, std::int64_t max_pixels) {
    std::vector<Json> out;
    read_tips(data, max_pixels, [&](Tip&& tip) {
        Json brush = Json::object();
        brush["label"] = first_chars(std::string(prefix) + tip.name, 40);
        brush["base"] = "gpen";
        brush["tip"] = "image";
        brush["tip_png"] = tip_png(tip.image);
        brush["spacing"] = core::py_max(0.02, core::py_min(5.0, tip.spacing / 100.0));
        brush["min_pressure"] = 0.3;
        brush["taper"] = false;
        out.push_back(std::move(brush));
    });
    if (out.empty()) abr_error("the file has no picture tips");
    return out;
}

std::string tip_from_picture(const Image& image) {
    const auto ink = brushes::tip_ink(core::b64encode(fills::png_data(image)));
    if (!ink) abr_error("the picture could not be read");
    return tip_png(*ink);
}

std::string tip_from_picture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: '" + path.string() + "'");
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return tip_from_picture(open_image(bytes, kPillowOpenLimits));
}

// --- the whole file (PC4 E2: upstream's crates/psd/src/abr.rs) ---------------------------------------------------------

namespace {

// What one brush (or section) could not be read for: it is left out with a warning.
struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Rd {
public:
    explicit Rd(std::string_view b) : b_(b) {}
    std::size_t left() const { return b_.size() - p_; }
    std::size_t at() const { return p_; }
    std::string_view take(std::size_t n) {
        if (n > left()) throw Bad("途中で終わっています（" + std::to_string(p_) + " バイト目で " + std::to_string(n - left()) + " バイト足りません）");
        const std::string_view out = b_.substr(p_, n);
        p_ += n;
        return out;
    }
    std::uint32_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }
    std::uint32_t u16() {
        const std::string_view s = take(2);
        return (std::uint32_t(static_cast<std::uint8_t>(s[0])) << 8) | static_cast<std::uint8_t>(s[1]);
    }
    std::uint32_t u32() {
        const std::uint32_t high = u16();
        return (high << 16) | u16();
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    void skip(std::size_t n) { (void)take(n); }
    std::string_view rest() { return take(left()); }
    void move_to(std::size_t p) { p_ = std::min(p, b_.size()); }
    std::string_view peek(std::size_t from, std::size_t n) const {
        return from <= b_.size() && n <= b_.size() - from ? b_.substr(from, n) : std::string_view();
    }

private:
    std::string_view b_;
    std::size_t p_ = 0;
};

// What one file may unpack to — its tips, its patterns and its descriptors together (review P1-1) —, spent before each
// is unpacked.
struct Budget {
    std::size_t left = kMaxSampleBytes;
    std::size_t most = kMaxSampleBytes;
    [[noreturn]] void refuse() const {
        throw core::Error("too_large", "the brush file's tips are too large to read (more than " + std::to_string(most >> 20) + " MB unpacked)");
    }
    void spend(std::size_t n) {
        if (n > left) refuse();
        left -= n;
    }
};

// A warning: kept while there are fewer than kMaxWarnings, counted always (review P1-2: a broken file of tiny items
// makes no more words than that).
template <class Words>
void warn(File& out, Words words) {
    if (++out.warning_count <= kMaxWarnings) out.warnings.push_back(words());
}

// PackBits rows (a u16 count for each row first): exactly `row_bytes` a row, else Bad.
std::string unpack_rows(std::string_view data, std::uint32_t rows, std::size_t row_bytes) {
    Rd r(data);
    if (std::size_t(rows) * 2 > r.left()) throw Bad("行の長さの表が切れています");
    std::vector<std::uint32_t> counts(rows);
    for (std::uint32_t y = 0; y < rows; ++y) counts[y] = r.u16();
    // (PackBits makes at most 64 times its bytes: a tip larger than that of what is left is broken, not unpacked)
    if (std::uint64_t(rows) * row_bytes > std::uint64_t(r.left()) * 64) throw Bad("圧縮した行が大きさに足りません");
    std::string out;
    out.reserve(std::size_t(rows) * row_bytes);
    for (const std::uint32_t count : counts) {
        const std::string_view in = r.take(count);
        const std::size_t start = out.size();
        std::size_t i = 0;
        while (i < in.size() && out.size() - start < row_bytes) {
            const int n = static_cast<std::int8_t>(in[i]);
            ++i;
            if (n >= 0) {
                const std::size_t len = std::size_t(n) + 1;
                if (len > in.size() - i || len > row_bytes - (out.size() - start)) throw Bad("圧縮した行が壊れています");
                out.append(in.substr(i, len));
                i += len;
            } else if (n > -128) {
                const std::size_t len = std::size_t(1 - n);
                if (i >= in.size() || len > row_bytes - (out.size() - start)) throw Bad("圧縮した行が壊れています");
                out.append(len, in[i]);
                ++i;
            }
        }
        if (out.size() - start != row_bytes) throw Bad("圧縮した行が短すぎます");
    }
    return out;
}

std::string pack_row(std::string_view row) {
    std::string out;
    std::size_t i = 0;
    while (i < row.size()) {
        std::size_t run = 1;
        while (i + run < row.size() && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 3) {
            out.push_back(static_cast<char>(1 - static_cast<int>(run)));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        std::size_t n = 0;
        while (i + n < row.size() && n < 128) {
            if (i + n + 2 < row.size() && row[i + n] == row[i + n + 1] && row[i + n] == row[i + n + 2]) break;
            ++n;
        }
        out.push_back(static_cast<char>(n - 1));
        out.append(row.substr(i, n));
        i += n;
    }
    return out;
}

void put_u16(std::string& o, std::uint32_t v) {
    o.push_back(static_cast<char>((v >> 8) & 0xff));
    o.push_back(static_cast<char>(v & 0xff));
}
void put_u32(std::string& o, std::uint32_t v) {
    put_u16(o, v >> 16);
    put_u16(o, v & 0xffff);
}

// bounds, depth, compression and the pixels of a tip (the rest of its entry)
Sample sample_body(Rd& r, std::string id, Budget& budget) {
    const std::int64_t top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    const std::int64_t w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0) throw Bad("先端の大きさが " + std::to_string(w) + " × " + std::to_string(h) + " 画素です");
    if (w > kMaxEdge || h > kMaxEdge) throw Bad("先端が一辺 " + std::to_string(kMaxEdge) + " 画素より大きい");
    const std::uint32_t depth = r.u16();
    if (depth != 8 && depth != 16) throw Bad("先端が " + std::to_string(depth) + " bit です（8・16 bit を読みます）");
    const std::uint32_t compression = r.u8();
    if (compression > 1) throw Bad("先端の圧縮 " + std::to_string(compression) + " は読みません");
    const std::size_t row_bytes = std::size_t(w) * (depth / 8);
    const std::size_t total = row_bytes * std::size_t(h);
    if (compression == 0 && r.left() < total) throw Bad("先端の画素が切れています");  // (review P3-4: before the budget)
    budget.spend(total);  // (before anything is unpacked)
    Sample s;
    s.id = std::move(id);
    s.width = static_cast<std::uint32_t>(w);
    s.height = static_cast<std::uint32_t>(h);
    s.depth = static_cast<std::uint16_t>(depth);
    if (compression == 0) {
        s.data = std::string(r.take(total));
    } else {
        s.data = unpack_rows(r.rest(), s.height, row_bytes);
    }
    return s;
}

std::string ucs2(Rd& r) {
    const std::uint32_t n = r.u32();
    if (n > 4096) throw Bad("名前が長すぎます");
    const std::string_view bytes = r.take(std::size_t(n) * 2);
    std::u16string text;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        text.push_back(static_cast<char16_t>((static_cast<std::uint8_t>(bytes[i]) << 8) | static_cast<std::uint8_t>(bytes[i + 1])));
    }
    while (!text.empty() && text.back() == char16_t(0)) text.pop_back();
    return psd::utf8_of(text);
}

void parse_v12(Rd& r, int version, File& out, Budget& budget) {
    const std::uint32_t count = r.u16();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i >= kMaxBrushes) {  // (review P3-4: as version 6 counts them; the broken ones too)
            warn(out, [&] { return "ブラシが " + std::to_string(kMaxBrushes) + " 本を越えるので、残りは外しました"; });
            break;
        }
        if (r.left() == 0) {
            warn(out, [&] { return "ファイルが " + std::to_string(count) + " 本のうち " + std::to_string(i) + " 本の後で終わっています"; });
            break;
        }
        std::string_view body;
        std::uint32_t type = 0;
        try {
            type = r.u16();
            const std::uint32_t length = r.u32();
            body = r.take(length);
        } catch (const Bad&) {
            warn(out, [&] { return "ブラシ " + std::to_string(i + 1) + ": 途中で切れています"; });
            break;
        }
        Rd b(body);
        try {
            LegacyBrush brush;
            if (type == 1) {
                (void)b.u32();
                brush.spacing = static_cast<std::uint16_t>(b.u16());
                brush.computed = true;
                brush.diameter = static_cast<std::uint16_t>(b.u16());
                brush.roundness = static_cast<std::uint16_t>(b.u16());
                brush.angle = static_cast<std::int16_t>(static_cast<std::uint16_t>(b.u16()));
                brush.hardness = static_cast<std::uint16_t>(b.u16());
            } else if (type == 2) {
                (void)b.u32();
                brush.spacing = static_cast<std::uint16_t>(b.u16());
                if (version == 2) brush.name = ucs2(b);
                brush.anti_alias = b.u8() != 0;
                b.skip(8);  // (the short bounds: the 32-bit ones follow)
                brush.sample = sample_body(b, {}, budget);
            } else {
                warn(out, [&] { return "ブラシ " + std::to_string(i + 1) + ": 種類 " + std::to_string(type) + " のブラシは読みません"; });
                continue;
            }
            out.legacy.push_back(std::move(brush));
        } catch (const Bad& why) {
            warn(out, [&] { return "ブラシ " + std::to_string(i + 1) + ": " + why.what() + "（外しました）"; });
        }
    }
}

void read_samp(std::string_view data, int subversion, File& out, Budget& budget, std::size_t& entries) {
    Rd r(data);
    while (r.left() >= 4) {
        if (entries >= kMaxBrushes) {  // (review P1-2: the entries that cannot be read counted too)
            warn(out, [&] { return "先端が " + std::to_string(kMaxBrushes) + " を越えるので、残りは外しました"; });
            break;
        }
        ++entries;
        const std::uint32_t length = r.u32();
        if (length == 0) break;
        const std::size_t padded = (std::size_t(length) + 3) & ~std::size_t(3);
        const std::string_view body = r.take(std::min(padded, r.left()));
        if (body.size() < length) {
            warn(out, [&] { return "先端 " + std::to_string(entries) + ": 途中で切れています"; });
            break;
        }
        Rd b(body.substr(0, length));
        try {
            const std::uint32_t n = b.u8();
            std::string id(b.take(n));
            b.skip(subversion == 1 ? 10 : 264);
            out.samples.push_back(sample_body(b, std::move(id), budget));
        } catch (const Bad& why) {
            warn(out, [&] { return "先端 " + std::to_string(entries) + ": " + why.what() + "（外しました）"; });
        }
    }
}

void read_desc(std::string_view data, File& out, Budget& budget) {
    try {
        // (in the file's one budget: a descriptor past what is left refuses the file, as a tip would)
        psd::descriptor::Object root = psd::descriptor::read_versioned(data, 0, budget.left);
        for (auto& [key, list] : root.items) {
            if (key != "Brsh") continue;
            if (list.type != "VlLs") break;
            for (psd::descriptor::Value& v : list.list) {
                if (out.presets.size() >= kMaxBrushes) {
                    warn(out, [&] { return "ブラシが " + std::to_string(kMaxBrushes) + " 本を越えるので、残りは外しました"; });
                    break;
                }
                // (moved, not copied: what was read is held once)
                if ((v.type == "Objc" || v.type == "GlbO") && v.object) out.presets.push_back(std::move(*v.object));
            }
            return;
        }
        warn(out, [] { return std::string("ブラシの設定（desc）にブラシの一覧がありません"); });
    } catch (const psd::descriptor::TooLarge&) {
        budget.refuse();
    } catch (const psd::descriptor::Broken& why) {
        warn(out, [&] { return std::string("ブラシの設定（desc）を読めませんでした: ") + why.what(); });
    }
}

void parse_v6(Rd& r, File& out, Budget& budget) {
    out.subversion = static_cast<int>(r.u16());
    if (out.subversion != 1 && out.subversion != 2) {
        throw core::Error("format", "brush files of subversion " + std::to_string(out.subversion) + " are not read");
    }
    std::size_t entries = 0;
    while (r.left() >= 12) {
        if (r.take(4) != "8BIM") {
            warn(out, [&] { return std::to_string(r.at() - 4) + " バイト目からは節でないので読みません"; });
            break;
        }
        const std::string key(r.take(4));
        const std::uint32_t length = r.u32();
        std::string_view data;
        if (length <= r.left()) {
            data = r.take(length);
        } else {
            warn(out, [&] { return "節 " + key + " が途中で切れています"; });
            data = r.rest();
        }
        // (sections are padded to 4 bytes; a writer that does not pad them is read too)
        const std::size_t pad = (4 - length % 4) % 4;
        if (pad > 0 && r.peek(r.at(), 4) != "8BIM" && r.peek(r.at() + pad, 4) == "8BIM") r.move_to(r.at() + pad);
        if (key == "samp") {
            read_samp(data, out.subversion, out, budget, entries);
        } else if (key == "patt") {
            // (in the file's one budget: patterns past what is left refuse the file — review P1-1)
            std::uint64_t left = budget.left;
            try {
                std::vector<pat::PsdPattern> held = pat::parse_block(data, left);
                for (pat::PsdPattern& p : held) out.patterns.push_back(std::move(p));
                budget.left = static_cast<std::size_t>(left);
            } catch (const core::Error& why) {
                if (why.code() == "too_large") budget.refuse();
                warn(out, [&] { return std::string("ファイルの中のパターンを読めませんでした: ") + why.what(); });
            }
        } else if (key == "desc") {
            read_desc(data, out, budget);
        } else if (key == "phry" || key == "lPdc") {
            // (the presets' folders and the tool presets: not the brushes)
        } else {
            warn(out, [&] { return "節 " + key + " は読みません"; });
        }
    }
}

void sample_out(std::string& o, const Sample& s, bool rle) {
    if (s.width == 0 || s.height == 0 || s.width > kMaxEdge || s.height > kMaxEdge || (s.depth != 8 && s.depth != 16) ||
        s.data.size() != std::size_t(s.width) * s.height * (s.depth / 8)) {
        throw core::Error("value", "a tip of " + std::to_string(s.width) + " × " + std::to_string(s.height) + " pixels of " +
                                       std::to_string(s.depth) + " bits cannot be written");
    }
    put_u32(o, 0);
    put_u32(o, 0);
    put_u32(o, s.height);
    put_u32(o, s.width);
    put_u16(o, s.depth);
    o.push_back(static_cast<char>(rle ? 1 : 0));
    if (!rle) {
        o += s.data;
        return;
    }
    const std::size_t row = std::size_t(s.width) * (s.depth / 8);
    std::vector<std::string> rows;
    for (std::uint32_t y = 0; y < s.height; ++y) rows.push_back(pack_row(std::string_view(s.data).substr(std::size_t(y) * row, row)));
    for (const std::string& packed : rows) put_u16(o, static_cast<std::uint32_t>(packed.size()));
    for (const std::string& packed : rows) o += packed;
}

void section(std::string& o, std::string_view key, std::string_view data) {
    o += "8BIM";
    o += key;
    const std::size_t pad = (4 - data.size() % 4) % 4;
    put_u32(o, static_cast<std::uint32_t>(data.size() + pad));
    o += data;
    o.append(pad, char(0));
}

}  // namespace

float Sample::unit(std::size_t i) const {
    if (depth == 16) {
        const auto hi = static_cast<std::uint8_t>(data[2 * i]), lo = static_cast<std::uint8_t>(data[2 * i + 1]);
        return static_cast<float>((hi << 8) | lo) / 65535.0f;
    }
    return static_cast<float>(static_cast<std::uint8_t>(data[i])) / 255.0f;
}

File parse(std::string_view data, std::size_t max_unpacked_bytes) {
    if (data.size() > kMaxFileBytes) {
        throw core::Error("too_large", "the brush file is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB");
    }
    Rd r(data);
    File out;
    Budget budget{max_unpacked_bytes, max_unpacked_bytes};
    try {
        out.version = static_cast<int>(r.u16());
        if (out.version == 1 || out.version == 2) {
            parse_v12(r, out.version, out, budget);
        } else if (out.version >= 6 && out.version <= 10) {
            parse_v6(r, out, budget);
        } else {
            throw core::Error("format", "brush files of version " + std::to_string(out.version) + " are not read");
        }
    } catch (const Bad& why) {
        throw core::Error("format", std::string("not a brush file that can be read: ") + why.what());
    }
    if (out.legacy.empty() && out.samples.empty() && out.presets.empty()) {
        throw core::Error("format", "the brush file holds no brush that can be read" +
                                        (out.warnings.empty() ? std::string() : " (" + out.warnings.front() + ")"));
    }
    return out;
}

std::string read_file(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec && size > kMaxFileBytes) {
        throw core::Error("too_large", "the brush file is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB");
    }
    return storage::read_file_bounded(path, kMaxFileBytes);
}

std::string write_v12(int version, const std::vector<LegacyBrush>& brushes, bool rle) {
    if ((version != 1 && version != 2) || brushes.size() > 0xffff) {
        throw core::Error("value", "brush files of version " + std::to_string(version) + " are not written");
    }
    std::string o;
    put_u16(o, static_cast<std::uint32_t>(version));
    put_u16(o, static_cast<std::uint32_t>(brushes.size()));
    for (const LegacyBrush& b : brushes) {
        std::string body;
        put_u32(body, 0);
        put_u16(body, b.spacing);
        if (b.computed) {
            put_u16(body, b.diameter);
            put_u16(body, b.roundness);
            put_u16(body, static_cast<std::uint16_t>(b.angle));
            put_u16(body, b.hardness);
        } else {
            if (version == 2) {
                std::u16string name = psd::utf16(b.name);
                name.push_back(char16_t(0));
                put_u32(body, static_cast<std::uint32_t>(name.size()));
                for (const char16_t c : name) put_u16(body, c);
            }
            body.push_back(static_cast<char>(b.anti_alias ? 1 : 0));
            put_u16(body, 0);
            put_u16(body, 0);
            put_u16(body, std::min<std::uint32_t>(b.sample.height, 0xffff));
            put_u16(body, std::min<std::uint32_t>(b.sample.width, 0xffff));
            sample_out(body, b.sample, rle);
        }
        put_u16(o, b.computed ? 1 : 2);
        put_u32(o, static_cast<std::uint32_t>(body.size()));
        o += body;
    }
    return o;
}

std::string write_v6(int subversion, const std::vector<Sample>& samples, const std::vector<pat::PsdPattern>& patterns,
                     const std::vector<std::pair<std::string, psd::descriptor::Writer>>& presets, bool rle) {
    if (subversion != 1 && subversion != 2) {
        throw core::Error("value", "brush files of subversion " + std::to_string(subversion) + " are not written");
    }
    std::string o;
    put_u16(o, 6);
    put_u16(o, static_cast<std::uint32_t>(subversion));
    std::string samp;
    for (const Sample& s : samples) {
        std::string e;
        const std::size_t n = std::min<std::size_t>(s.id.size(), 255);
        e.push_back(static_cast<char>(n));
        e += s.id.substr(0, n);
        e.append(subversion == 1 ? 10 : 264, char(0));
        sample_out(e, s, rle);
        put_u32(samp, static_cast<std::uint32_t>(e.size()));
        samp += e;
        samp.append((4 - e.size() % 4) % 4, char(0));
    }
    section(o, "samp", samp);
    if (!patterns.empty()) section(o, "patt", pat::write_block(patterns));
    psd::descriptor::Writer root;
    root.objects("Brsh", presets);
    section(o, "desc", root.versioned("null"));
    return o;
}

}  // namespace genko::render::abr
