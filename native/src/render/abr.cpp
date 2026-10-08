#include "render/abr.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/brushes.hpp"
#include "render/fill.hpp"
#include "render/png.hpp"

namespace genko::render::abr {

namespace {

using core::Json;

[[noreturn]] void abr_error(const std::string& message) { throw core::PyValueError(message); }

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}
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

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

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

std::vector<Tip> read_v12(Reader& reader, int version) {
    const std::uint32_t count = reader.u16();
    std::vector<Tip> out;
    for (std::uint32_t n = 0; n < count; ++n) {
        const int kind = reader.i16();
        const std::int64_t size = reader.i32();
        const std::int64_t end = static_cast<std::int64_t>(reader.pos()) + size;
        const auto go_to_end = [&] { reader.seek(static_cast<std::size_t>(std::max<std::int64_t>(0, end))); };
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
        out.push_back(Tip{name.empty() ? "ブラシ " + std::to_string(n + 1) : name, std::move(image), spacing});
        go_to_end();
    }
    return out;
}

std::vector<Tip> read_v6(Reader& reader, int subversion) {
    std::vector<Tip> out;
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
            try {
                Image image = tip_image(reader, right - left, bottom - top, depth, compressed);
                out.push_back(Tip{"ブラシ " + std::to_string(out.size() + 1), std::move(image), 25});
            } catch (const core::PyValueError&) {
                // (a tip this does not read: the next one)
            }
            reader.seek(brush_end);
        }
        reader.seek(end);
    }
    return out;
}

}  // namespace

std::vector<Tip> read(std::string_view data) {
    Reader reader(data);
    const int version = reader.i16();
    std::vector<Tip> tips;
    if (version == 1 || version == 2) {
        tips = read_v12(reader, version);
    } else if (version == 6 || version == 7 || version == 10) {
        tips = read_v6(reader, reader.i16());
    } else {
        abr_error("version " + std::to_string(version) + " brush files are not read");
    }
    if (tips.empty()) abr_error("the file has no picture tips");
    return tips;
}

std::string tip_png(const Image& image, int longest) {
    Image small = image.convert("L");
    if (std::max(small.width(), small.height()) > longest) small.thumbnail(Size{longest, longest});
    return core::b64encode(fills::png_data(small));
}

std::vector<Json> brushes_from(std::string_view data, std::string_view prefix) {
    std::vector<Json> out;
    for (const Tip& tip : read(data)) {
        Json brush = Json::object();
        brush["label"] = first_chars(std::string(prefix) + tip.name, 40);
        brush["base"] = "gpen";
        brush["tip"] = "image";
        brush["tip_png"] = tip_png(tip.image);
        brush["spacing"] = core::py_max(0.02, core::py_min(5.0, tip.spacing / 100.0));
        brush["min_pressure"] = 0.3;
        brush["taper"] = false;
        out.push_back(std::move(brush));
    }
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

}  // namespace genko::render::abr
