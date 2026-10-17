// The 3D lookup tables of color_lookup (core/lut3d.hpp). The file formats, the generated looks and the lookup are
// PhotoCraft v0.2.0's (cms/lutfile.rs 1-268, compose/adjust.rs 312-355); attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md.

#include "core/lut3d.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <type_traits>
#include <utility>

#include "core/command_bus.hpp"
#include "core/error.hpp"

namespace genko::core::lut3d {

namespace {

[[noreturn]] void refused(const std::string& why) { throw Error("value", why); }

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

std::string_view trim_quotes(std::string_view s) {
    while (!s.empty() && s.front() == '"') s.remove_prefix(1);
    while (!s.empty() && s.back() == '"') s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && space(line[i])) ++i;
        const std::size_t start = i;
        while (i < line.size() && !space(line[i])) ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

// The lines of a text (Rust's str::lines: "\n" between them, a "\r" before it dropped), each before its "#", trimmed.
template <typename Each>
void each_line(std::string_view text, Each&& each) {
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        each(trimmed(line));
        if (end == text.size()) break;
        start = end + 1;
    }
}

// A number as Rust's str::parse reads it: the whole word a decimal number (an infinity and NaN too; no hexadecimal).
template <typename T>
std::optional<T> number(std::string_view word) {
    if (word.empty() || word.size() > 128) return std::nullopt;
    for (const char c : word) {
        if (c == 'x' || c == 'X' || space(c)) return std::nullopt;
    }
    const std::string text(word);
    char* end = nullptr;
    errno = 0;
    T v{};
    if constexpr (std::is_same_v<T, float>) {
        v = std::strtof(text.c_str(), &end);
    } else {
        v = std::strtod(text.c_str(), &end);
    }
    if (end != text.c_str() + text.size()) return std::nullopt;
    return v;  // (out of range: an infinity, or 0 for one too small — as Rust reads it)
}

std::optional<std::size_t> whole(std::string_view word) {
    if (!word.empty() && word.front() == '+') word.remove_prefix(1);
    if (word.empty()) return std::nullopt;
    std::size_t v = 0;
    for (const char c : word) {
        if (c < '0' || c > '9') return std::nullopt;
        if (v > (static_cast<std::size_t>(-1) - 9) / 10) return std::nullopt;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    return v;
}

std::size_t cube_of(std::size_t n) { return n * n * n; }

Table checked(Table table) {
    if (table.size < 2 || table.size > kMaxSize) refused("unsupported LUT size " + std::to_string(table.size));
    if (table.data.size() != cube_of(table.size) * 3) {
        refused("expected " + std::to_string(cube_of(table.size)) + " entries, found " + std::to_string(table.data.size() / 3));
    }
    if (std::any_of(table.data.begin(), table.data.end(), [](float v) { return !std::isfinite(v); })) refused("non-finite LUT value");
    return table;
}

float luma(const std::array<float, 3>& c) { return 0.299F * c[0] + 0.587F * c[1] + 0.114F * c[2]; }
float smooth(float x) {
    x = std::clamp(x, 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);
}

std::array<float, 3> look_of(std::string_view id, const std::array<float, 3>& c) {
    if (id == "warm") return {c[0] * 0.92F + 0.08F, c[1] * 0.97F + 0.02F, c[2] * 0.85F};
    if (id == "cool") return {c[0] * 0.86F, c[1] * 0.97F + 0.01F, c[2] * 0.9F + 0.1F};
    if (id == "tealOrange") {  // (shadows toward teal, highlights toward orange, skin-ish midtones kept)
        const float l = luma(c);
        const float t = smooth(l);
        constexpr std::array<float, 3> teal{0.0F, 0.5F, 0.55F}, orange{1.0F, 0.62F, 0.3F};
        std::array<float, 3> out{};
        for (std::size_t k = 0; k < 3; ++k) {
            const float tint = teal[k] * (1.0F - t) + orange[k] * t;
            out[k] = c[k] * 0.75F + std::min(tint * l * 1.1F, 1.0F) * 0.25F + (smooth(c[k]) - c[k]) * 0.3F;
        }
        return out;
    }
    if (id == "bleachBypass") {
        const float l = luma(c);
        const float s = smooth(l);
        std::array<float, 3> out{};
        for (std::size_t k = 0; k < 3; ++k) out[k] = (c[k] * 0.45F + l * 0.55F) * 0.5F + s * 0.5F;
        return out;
    }
    if (id == "fadedFilm") {
        const float l = luma(c);
        constexpr std::array<float, 3> tint{1.0F, 0.96F, 0.88F};
        std::array<float, 3> out{};
        for (std::size_t k = 0; k < 3; ++k) out[k] = (0.08F + (c[k] * 0.8F + l * 0.2F) * 0.84F) * tint[k] + 0.02F;
        return out;
    }
    if (id == "dayForNight") {
        const float l = luma(c);
        return {l * 0.35F, l * 0.45F + c[1] * 0.05F, l * 0.65F + c[2] * 0.15F};
    }
    if (id == "monoContrast") {
        const float v = smooth(smooth(luma(c)));
        return {v, v, v};
    }
    return {smooth(c[0]) * 1.05F, c[1] * 1.1F - 0.03F, c[2] * 0.7F + 0.15F};  // crossProcess
}

void put_float(std::string& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((bits >> shift) & 0xffU);
}

float get_float(const unsigned char* p) {
    const std::uint32_t bits = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    float v = 0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

}  // namespace

Table identity(std::size_t size) {
    return from_fn("Identity", size, [](std::array<float, 3> c) { return c; });
}

Table from_fn(std::string title, std::size_t size, const std::function<std::array<float, 3>(std::array<float, 3>)>& f) {
    Table out;
    out.title = std::move(title);
    out.size = size;
    const float m = static_cast<float>(std::max<std::size_t>(size, 2) - 1);
    out.data.reserve(cube_of(size) * 3);
    for (std::size_t b = 0; b < size; ++b)
        for (std::size_t g = 0; g < size; ++g)
            for (std::size_t r = 0; r < size; ++r) {
                const std::array<float, 3> o = f({static_cast<float>(r) / m, static_cast<float>(g) / m, static_cast<float>(b) / m});
                for (const float v : o) out.data.push_back(std::clamp(v, 0.0F, 1.0F));
            }
    return out;
}

Table parse(std::string_view file_name, std::string_view text) {
    std::string lower(file_name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.ends_with(".3dl")) return parse_3dl(text);
    if (lower.ends_with(".look") || trimmed(text).starts_with('<')) return parse_look(text);
    return parse_cube(text);
}

Table parse_cube(std::string_view text) {
    std::string title;
    std::size_t size3 = 0, size1 = 0;
    std::vector<float> rows;
    each_line(text, [&](std::string_view line) {
        if (line.empty()) return;
        const std::vector<std::string_view> w = words(line);
        const std::string_view key = w.front();
        const auto three = [&]() {
            std::size_t n = 0;
            for (std::size_t i = 1; i < w.size(); ++i) n += number<float>(w[i]).has_value() ? 1 : 0;
            if (n != 3) refused("bad line `" + std::string(line) + "`");
        };
        if (key == "TITLE") {
            title = std::string(trim_quotes(trimmed(line.substr(5))));
        } else if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
            const auto n = w.size() > 1 ? whole(w[1]) : std::nullopt;
            if (!n) refused("bad " + std::string(key));
            (key == "LUT_3D_SIZE" ? size3 : size1) = *n;
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            three();  // (a domain other than 0..1 only shifts the input's sampling: 0..1 is taken)
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {
            // (as the domain)
        } else if (const char c = key.front(); (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
            const auto first = number<float>(key);
            if (!first) refused("bad number in `" + std::string(line) + "`");
            std::vector<float> v{*first};
            for (std::size_t i = 1; i < w.size(); ++i)
                if (const auto x = number<float>(w[i])) v.push_back(*x);
            if (v.size() != 3) refused("bad data line `" + std::string(line) + "`");
            rows.insert(rows.end(), v.begin(), v.end());
            // (no table holds more: one that says so is refused below without holding all of it)
            if (rows.size() > cube_of(kMaxSize) * 3 + 3) refused("expected at most " + std::to_string(cube_of(kMaxSize)) + " entries");
        }
        // (keywords it does not know — Resolve's LUT_IN_VIDEO_RANGE — are passed over, as the format allows)
    });
    if (size3 > 0) {
        Table out;
        out.title = std::move(title);
        out.size = size3;
        out.data = std::move(rows);
        return checked(std::move(out));
    }
    if (size1 >= 2 && rows.size() == size1 * 3) {
        const auto curve = [&rows, size1](std::size_t ch, float v) {
            const float x = std::clamp(v, 0.0F, 1.0F) * static_cast<float>(size1 - 1);
            const std::size_t i = std::min(static_cast<std::size_t>(std::floor(x)), size1 - 2);
            const float f = x - static_cast<float>(i);
            return rows[i * 3 + ch] * (1.0F - f) + rows[(i + 1) * 3 + ch] * f;
        };
        return checked(from_fn(title, 33, [&curve](std::array<float, 3> c) { return std::array<float, 3>{curve(0, c[0]), curve(1, c[1]), curve(2, c[2])}; }));
    }
    refused("no LUT_3D_SIZE in .cube file");
}

Table parse_3dl(std::string_view text) {
    std::vector<std::array<double, 3>> rows;
    constexpr std::size_t kMost = kMaxSize * kMaxSize * kMaxSize;
    each_line(text, [&](std::string_view line) {
        if (line.empty() || std::isalpha(static_cast<unsigned char>(line.front()))) return;
        std::vector<double> v;
        for (const std::string_view w : words(line))
            if (const auto x = number<double>(w)) v.push_back(*x);
        if (v.size() == 3) rows.push_back({v[0], v[1], v[2]});
        // (a line of more numbers is the input shaper: taken as linear)
        if (rows.size() > kMost) refused("more than " + std::to_string(kMost) + " rows is not a cube of at most " + std::to_string(kMaxSize));
    });
    const auto n = static_cast<std::size_t>(std::llround(std::cbrt(static_cast<double>(rows.size()))));
    if (n < 2 || cube_of(n) != rows.size()) refused(std::to_string(rows.size()) + " rows is not a cube");
    double most = 0.0;
    for (const auto& row : rows)
        for (const double v : row) most = std::max(most, v);
    double scale = 1.0;
    if (most > 1.0) {  // (the output's bit depth: the first that holds the largest value)
        scale = most;
        for (const double s : {1023.0, 4095.0, 16383.0, 65535.0}) {
            if (most <= s) {
                scale = s;
                break;
            }
        }
    }
    Table out;
    out.size = n;
    out.data.assign(cube_of(n) * 3, 0.0F);
    for (std::size_t i = 0; i < rows.size(); ++i) {  // (blue varies fastest in the file)
        const std::size_t r = i / (n * n), g = (i / n) % n, b = i % n;
        const std::size_t at = ((b * n + g) * n + r) * 3;
        for (std::size_t k = 0; k < 3; ++k) out.data[at + k] = static_cast<float>(rows[i][k] / scale);
    }
    return checked(std::move(out));
}

Table parse_look(std::string_view text) {
    const auto tag = [text](std::string_view name) -> std::optional<std::string_view> {
        const std::string open = "<" + std::string(name) + ">", close = "</" + std::string(name) + ">";
        const std::size_t at = text.find(open);
        if (at == std::string_view::npos) return std::nullopt;
        const std::size_t start = at + open.size();
        const std::size_t end = text.find(close, start);
        if (end == std::string_view::npos) return std::nullopt;
        return trimmed(trim_quotes(trimmed(text.substr(start, end - start))));
    };
    const auto size_text = tag("size");
    const auto size = size_text ? whole(*size_text) : std::nullopt;
    if (!size) refused("no <size> in .look");
    const auto hex_text = tag("data");
    if (!hex_text) refused("no <data> in .look");
    std::string bytes;
    bytes.reserve(hex_text->size() / 2);
    int high = -1;
    for (const char c : *hex_text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) continue;
        const int nib = c <= '9' ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
        if (high < 0) {
            high = nib;
        } else {
            bytes += static_cast<char>((high << 4) | nib);
            high = -1;
        }
    }
    std::vector<float> floats;
    floats.reserve(bytes.size() / 4);
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) floats.push_back(get_float(reinterpret_cast<const unsigned char*>(bytes.data() + i)));
    Table out;
    out.size = *size;
    if (*size >= 2 && *size <= kMaxSize && floats.size() == cube_of(*size) * 4) {  // (RGBA entries: the RGB of each)
        for (std::size_t i = 0; i < floats.size(); i += 4) out.data.insert(out.data.end(), floats.begin() + static_cast<std::ptrdiff_t>(i), floats.begin() + static_cast<std::ptrdiff_t>(i + 3));
    } else {
        out.data = std::move(floats);
    }
    if (const auto title = tag("title")) out.title = std::string(*title);
    return checked(std::move(out));
}

std::string write_cube(const Table& table) {
    std::string out;
    if (!table.title.empty()) {
        std::string title = table.title;
        std::replace(title.begin(), title.end(), '"', '\'');
        out += "TITLE \"" + title + "\"\n";
    }
    out += "LUT_3D_SIZE " + std::to_string(table.size) + "\n";
    out.reserve(out.size() + table.data.size() / 3 * 28);
    char line[96];
    for (std::size_t i = 0; i + 3 <= table.data.size(); i += 3) {
        const int n = std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", static_cast<double>(table.data[i]), static_cast<double>(table.data[i + 1]),
                                    static_cast<double>(table.data[i + 2]));
        out.append(line, static_cast<std::size_t>(std::max(0, n)));
    }
    return out;
}

const std::array<Look, 8>& looks() {
    static const std::array<Look, 8> all{Look{"warm", "Warm Filter"},           Look{"cool", "Cool Filter"},
                                         Look{"tealOrange", "Teal & Orange"},  Look{"bleachBypass", "Bleach Bypass"},
                                         Look{"fadedFilm", "Faded Film"},      Look{"dayForNight", "Day for Night"},
                                         Look{"monoContrast", "Mono High Contrast"}, Look{"crossProcess", "Cross Process"}};
    return all;
}

const Table* builtin(std::string_view id) {
    static const std::vector<Table> tables = [] {
        std::vector<Table> out;
        for (const Look& look : looks()) {
            const std::string_view which = look.id;
            out.push_back(from_fn(std::string(look.label), 33, [which](std::array<float, 3> c) { return look_of(which, c); }));
        }
        return out;
    }();
    for (std::size_t i = 0; i < looks().size(); ++i) {
        if (looks()[i].id == id) return &tables[i];
    }
    return nullptr;
}

std::array<double, 3> sample(const Table& table, const std::array<double, 3>& rgb, bool tetrahedral) {
    const std::size_t n = table.size;
    const double m = static_cast<double>(n - 1);
    std::array<double, 3> pos{}, f{};
    std::array<std::size_t, 3> i0{};
    for (std::size_t k = 0; k < 3; ++k) {
        const double v = std::isnan(rgb[k]) ? 0.0 : std::clamp(rgb[k], 0.0, 1.0);
        pos[k] = v * m;
        i0[k] = std::min(static_cast<std::size_t>(std::floor(pos[k])), n - 2);
        f[k] = pos[k] - static_cast<double>(i0[k]);
    }
    const float* d = table.data.data();
    const auto at = [&](std::size_t dr, std::size_t dg, std::size_t db) {
        const std::size_t idx = (((i0[2] + db) * n + i0[1] + dg) * n + i0[0] + dr) * 3;
        return std::array<double, 3>{d[idx], d[idx + 1], d[idx + 2]};
    };
    const double fr = f[0], fg = f[1], fb = f[2];
    std::array<double, 3> out{};
    if (tetrahedral) {
        const auto mix = [&out](std::initializer_list<std::pair<double, std::array<double, 3>>> w) {
            out = {0, 0, 0};
            for (const auto& [a, v] : w)
                for (std::size_t k = 0; k < 3; ++k) out[k] += a * v[k];
        };
        const auto c000 = at(0, 0, 0), c111 = at(1, 1, 1);
        if (fr > fg) {
            if (fg > fb) {
                mix({{1 - fr, c000}, {fr - fg, at(1, 0, 0)}, {fg - fb, at(1, 1, 0)}, {fb, c111}});
            } else if (fr > fb) {
                mix({{1 - fr, c000}, {fr - fb, at(1, 0, 0)}, {fb - fg, at(1, 0, 1)}, {fg, c111}});
            } else {
                mix({{1 - fb, c000}, {fb - fr, at(0, 0, 1)}, {fr - fg, at(1, 0, 1)}, {fg, c111}});
            }
        } else if (fb > fg) {
            mix({{1 - fb, c000}, {fb - fg, at(0, 0, 1)}, {fg - fr, at(0, 1, 1)}, {fr, c111}});
        } else if (fb > fr) {
            mix({{1 - fg, c000}, {fg - fb, at(0, 1, 0)}, {fb - fr, at(0, 1, 1)}, {fr, c111}});
        } else {
            mix({{1 - fg, c000}, {fg - fr, at(0, 1, 0)}, {fr - fb, at(1, 1, 0)}, {fb, c111}});
        }
        return out;
    }
    const auto lerp = [](const std::array<double, 3>& a, const std::array<double, 3>& b, double t) {
        return std::array<double, 3>{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
    };
    const auto c00 = lerp(at(0, 0, 0), at(1, 0, 0), fr), c10 = lerp(at(0, 1, 0), at(1, 1, 0), fr);
    const auto c01 = lerp(at(0, 0, 1), at(1, 0, 1), fr), c11 = lerp(at(0, 1, 1), at(1, 1, 1), fr);
    return lerp(lerp(c00, c10, fg), lerp(c01, c11, fg), fb);
}

double bayer4(int x, int y) {
    static constexpr std::array<double, 16> kM{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (kM[static_cast<std::size_t>((y & 3) * 4 + (x & 3))] + 0.5) / 16.0 - 0.5;
}

std::string encode(const Table& table) {
    const Table& t = table;
    if (t.size < 2 || t.size > kMaxSize || t.data.size() != cube_of(t.size) * 3) refused("unsupported LUT size " + std::to_string(t.size));
    std::string raw;
    raw.reserve(t.data.size() * 4);
    for (const float v : t.data) put_float(raw, v);
    uLongf room = compressBound(static_cast<uLong>(raw.size()));
    std::string packed(room, '\0');
    if (compress2(reinterpret_cast<Bytef*>(packed.data()), &room, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()),
                  Z_DEFAULT_COMPRESSION) != Z_OK)
        refused("the lookup table cannot be compressed");
    packed.resize(room);
    std::string out = "GKLU";
    out += static_cast<char>(1);
    out += static_cast<char>(t.size);
    return out + packed;
}

Table decode(std::string_view bytes) {
    if (bytes.size() < 6 || bytes.substr(0, 4) != "GKLU") refused("not a lookup table of this build (GKLU)");
    if (static_cast<unsigned char>(bytes[4]) != 1) refused("a lookup table of version " + std::to_string(static_cast<unsigned char>(bytes[4])) + " (this build reads 1)");
    Table out;
    out.size = static_cast<unsigned char>(bytes[5]);
    if (out.size < 2 || out.size > kMaxSize) refused("unsupported LUT size " + std::to_string(out.size));
    const std::size_t want = cube_of(out.size) * 3 * 4;
    std::string raw(want, '\0');
    uLongf got = static_cast<uLongf>(want);
    uLong took = static_cast<uLong>(bytes.size() - 6);
    const int status = uncompress2(reinterpret_cast<Bytef*>(raw.data()), &got, reinterpret_cast<const Bytef*>(bytes.data() + 6), &took);
    if (status != Z_OK || got != want || took != bytes.size() - 6) refused("the lookup table's entries are cut short, broken or more than its size");
    out.data.resize(cube_of(out.size) * 3);
    for (std::size_t i = 0; i < out.data.size(); ++i) out.data[i] = get_float(reinterpret_cast<const unsigned char*>(raw.data() + i * 4));
    return checked(std::move(out));
}

std::shared_ptr<const Table> decoded(const std::string& ref, const Bytes& bytes) {
    if (!bytes) return nullptr;  // (a table not held: not one decoded for another book)
    static std::mutex lock;
    static std::deque<std::pair<std::string, std::shared_ptr<const Table>>> kept;  // (the last used first)
    {
        const std::lock_guard<std::mutex> hold(lock);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (it->first != ref) continue;
            auto found = std::move(*it);
            kept.erase(it);
            kept.push_front(found);
            return found.second;
        }
    }
    auto table = std::make_shared<const Table>(decode(*bytes));
    const std::lock_guard<std::mutex> hold(lock);
    kept.emplace_front(ref, table);
    std::size_t held = 0;  // (a few, within about 128 MB)
    for (std::size_t i = 0; i < kept.size(); ++i) {
        held += kept[i].second->data.size() * sizeof(float);
        if (i >= 1 && (i >= 6 || held > 128u * 1024 * 1024)) {
            kept.resize(i);
            break;
        }
    }
    return table;
}

void keep_luts(std::map<std::string, Bytes>& luts, const std::map<std::string, Bytes>& made, const Json& adjust) {
    if (!adjust.is_object() || adjust.value("kind", Json()) != Json("color_lookup")) return;
    const auto it = adjust.find("lut");
    if (it == adjust.end() || !it->is_string()) return;
    const std::string& ref = it->get_ref<const std::string&>();
    if (luts.contains(ref)) return;
    if (const auto found = made.find(ref); found != made.end() && found->second) {
        luts.emplace(ref, found->second);
        return;
    }
    throw OpError("adjust color_lookup.lut: the book holds no lookup table " + ref + " (give the file's text as data)");
}

namespace {
void add_refs(const Page& page, std::vector<std::string>& out) {
    for (const Layer& layer : page.layers) {
        if (layer.kind != LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object()) continue;
        if (layer.adjust->value("kind", Json()) != Json("color_lookup")) continue;
        const auto lut = layer.adjust->find("lut");
        if (lut == layer.adjust->end() || !lut->is_string()) continue;
        if (std::find(out.begin(), out.end(), lut->get<std::string>()) == out.end()) out.push_back(lut->get<std::string>());
    }
}
}  // namespace

std::vector<std::string> refs_of(const Document& doc) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i)) add_refs(doc.page(i), out);
    }
    return out;
}

std::vector<std::string> refs_of(const Page& page) {
    std::vector<std::string> out;
    add_refs(page, out);
    return out;
}

}  // namespace genko::core::lut3d
