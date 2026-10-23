// PhotoCraft's .pcraft manifest (render/pcraft_manifest.hpp).

#include "render/pcraft_manifest.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "core/error.hpp"
#include "render/blake3.hpp"

namespace genko::render::pcraft {

using core::Json;

namespace {

[[noreturn]] void unreadable(std::string_view what, std::string_view why) { throw Unreadable(std::string(what) + ": " + std::string(why)); }

constexpr std::array<std::pair<Mode, std::string_view>, 8> kModes{{{Mode::Bitmap, "Bitmap"},
                                                                    {Mode::Grayscale, "Grayscale"},
                                                                    {Mode::Indexed, "Indexed"},
                                                                    {Mode::Rgb, "Rgb"},
                                                                    {Mode::Cmyk, "Cmyk"},
                                                                    {Mode::Lab, "Lab"},
                                                                    {Mode::Multichannel, "Multichannel"},
                                                                    {Mode::Duotone, "Duotone"}}};

// Upstream's blend modes (photocraft_color::BlendMode) and Genko's, where Genko has it.
constexpr std::array<std::pair<std::string_view, std::string_view>, 28> kBlends{{{"PassThrough", ""},
                                                                                 {"Normal", "normal"},
                                                                                 {"Dissolve", ""},
                                                                                 {"Darken", "darken"},
                                                                                 {"Multiply", "multiply"},
                                                                                 {"ColorBurn", "color_burn"},
                                                                                 {"LinearBurn", "linear_burn"},
                                                                                 {"DarkerColor", ""},
                                                                                 {"Lighten", "lighten"},
                                                                                 {"Screen", "screen"},
                                                                                 {"ColorDodge", "color_dodge"},
                                                                                 {"LinearDodge", "add"},
                                                                                 {"LighterColor", ""},
                                                                                 {"Overlay", "overlay"},
                                                                                 {"SoftLight", "soft_light"},
                                                                                 {"HardLight", "hard_light"},
                                                                                 {"VividLight", ""},
                                                                                 {"LinearLight", ""},
                                                                                 {"PinLight", ""},
                                                                                 {"HardMix", ""},
                                                                                 {"Difference", "difference"},
                                                                                 {"Exclusion", "exclusion"},
                                                                                 {"Subtract", "subtract"},
                                                                                 {"Divide", "divide"},
                                                                                 {"Hue", "hue"},
                                                                                 {"Saturation", "saturation"},
                                                                                 {"Color", "color"},
                                                                                 {"Luminosity", "luminosity"}}};

core::vector::XY point_from(const Json& value, double k, std::string_view what) {
    if (!value.is_object()) unreadable(what, "a point is {x, y}");
    const double x = number(field(value, "x", what), what) * k, y = number(field(value, "y", what), what) * k;
    if (!(std::fabs(x) <= core::vector::kCoordinateMax) || !(std::fabs(y) <= core::vector::kCoordinateMax)) unreadable(what, "a point too far off the page");
    return core::vector::XY{x, y};
}

Json point_json(const core::vector::XY& p, double k) { return Json{{"x", p.x * k}, {"y", p.y * k}}; }

}  // namespace

const Json& field(const Json& object, std::string_view key, std::string_view what) {
    if (!object.is_object()) unreadable(what, "not an object");
    const auto it = object.find(key);
    if (it == object.end()) unreadable(what, "no " + std::string(key));
    return *it;
}

const Json* optional_field(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() || it->is_null() ? nullptr : &*it;
}

double number(const Json& value, std::string_view what) {
    if (!value.is_number()) unreadable(what, "not a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) unreadable(what, "not a finite number");
    return v;
}

bool boolean(const Json& value, std::string_view what) {
    if (!value.is_boolean()) unreadable(what, "not true or false");
    return value.get<bool>();
}

std::string text(const Json& value, std::string_view what) {
    if (!value.is_string()) unreadable(what, "not text");
    return value.get<std::string>();
}

std::int64_t whole(const Json& value, std::string_view what) {
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) unreadable(what, "a number too large");
        return static_cast<std::int64_t>(v);
    }
    if (!value.is_number_integer()) unreadable(what, "not a whole number");
    return value.get<std::int64_t>();
}

std::pair<std::string, const Json*> variant(const Json& value, std::string_view what) {
    if (value.is_string()) return {value.get<std::string>(), nullptr};
    if (value.is_object() && value.size() == 1) return {value.begin().key(), &value.begin().value()};
    unreadable(what, "not one of its kinds");
}

std::string_view to_string(Mode mode) {
    for (const auto& [m, name] : kModes) {
        if (m == mode) return name;
    }
    return "Rgb";
}

std::string_view to_string(Sample sample) { return sample == Sample::U8 ? "U8" : sample == Sample::U16 ? "U16" : "F32"; }

Mode mode_from(const Json& value) {
    if (value.is_string()) {
        for (const auto& [m, name] : kModes) {
            if (value.get_ref<const std::string&>() == name) return m;
        }
    }
    unreadable("mode", "not a colour mode this build knows");
}

Sample sample_from(const Json& value) {
    if (value == "U8") return Sample::U8;
    if (value == "U16") return Sample::U16;
    if (value == "F32") return Sample::F32;
    unreadable("sample", "not a sample type this build knows");
}

unsigned colour_channels(Mode mode) {
    switch (mode) {
    case Mode::Bitmap:
    case Mode::Grayscale:
    case Mode::Duotone:
        return 1;
    case Mode::Cmyk:
        return 4;
    default:
        return 3;
    }
}

unsigned sample_bytes(Sample sample) { return sample == Sample::U8 ? 1 : sample == Sample::U16 ? 2 : 4; }

PixelFormat format_from(const Json& value) {
    PixelFormat f;
    f.mode = mode_from(field(value, "mode", "format"));
    f.sample = sample_from(field(value, "sample", "format"));
    f.alpha = boolean(field(value, "alpha", "format"), "format.alpha");
    return f;
}

Json to_json(const PixelFormat& f) { return Json{{"mode", to_string(f.mode)}, {"sample", to_string(f.sample)}, {"alpha", f.alpha}}; }

double read_sample(const unsigned char* b, Sample sample) {
    switch (sample) {
    case Sample::U8:
        return b[0] / 255.0;
    case Sample::U16:
        return static_cast<double>(static_cast<std::uint16_t>(b[0] | (b[1] << 8))) / 65535.0;
    case Sample::F32: {
        const std::uint32_t u = std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
        float f;
        std::memcpy(&f, &u, 4);
        return static_cast<double>(f);
    }
    }
    return 0.0;
}

void write_sample(unsigned char* b, Sample sample, double v) {
    switch (sample) {
    case Sample::U8:
        b[0] = static_cast<unsigned char>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
        return;
    case Sample::U16: {
        const auto q = static_cast<std::uint16_t>(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
        b[0] = static_cast<unsigned char>(q & 0xff);
        b[1] = static_cast<unsigned char>(q >> 8);
        return;
    }
    case Sample::F32: {
        const float f = static_cast<float>(v);
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
        return;
    }
    }
}

Surface surface_from(const Json& value) {
    Surface s;
    s.format = format_from(field(value, "format", "surface"));
    const std::string hex = text(field(value, "default", "surface"), "surface.default");
    const std::size_t bytes = s.format.pixel_bytes();
    if (hex.size() != 2 * bytes) unreadable("surface.default", "the default pixel has the wrong size for its format");
    std::vector<unsigned char> raw(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto digit = [&](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            unreadable("surface.default", "not hex");
        };
        raw[i] = static_cast<unsigned char>(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
    }
    const unsigned sb = sample_bytes(s.format.sample);
    for (unsigned c = 0; c < s.format.channels(); ++c) s.background.push_back(read_sample(raw.data() + c * sb, s.format.sample));
    const Json& tiles = field(value, "tiles", "surface");
    if (!tiles.is_array()) unreadable("surface.tiles", "not a list");
    std::set<std::pair<std::int32_t, std::int32_t>> seen;
    for (const Json& t : tiles) {
        TileRef ref;
        const std::int64_t tx = whole(field(t, "tx", "tile"), "tile.tx"), ty = whole(field(t, "ty", "tile"), "tile.ty");
        if (tx < std::numeric_limits<std::int32_t>::min() || tx > std::numeric_limits<std::int32_t>::max() ||
            ty < std::numeric_limits<std::int32_t>::min() || ty > std::numeric_limits<std::int32_t>::max())
            unreadable("tile", "a place out of range");
        ref.tx = static_cast<std::int32_t>(tx);
        ref.ty = static_cast<std::int32_t>(ty);
        ref.hash = text(field(t, "hash", "tile"), "tile.hash");
        if (!blake3::is_hex_hash(ref.hash)) throw core::Error("format", "the .pcraft is broken (a tile named by something that is not a hash)");
        if (!seen.emplace(ref.tx, ref.ty).second) {
            throw core::Error("format", "the .pcraft is broken (the tile (" + std::to_string(tx) + ", " + std::to_string(ty) + ") twice)");
        }
        s.tiles.push_back(std::move(ref));
    }
    return s;
}

Json to_json(const Surface& s) {
    std::string raw(s.format.pixel_bytes(), '\0');
    const unsigned sb = sample_bytes(s.format.sample);
    for (unsigned c = 0; c < s.format.channels() && c < s.background.size(); ++c) {
        write_sample(reinterpret_cast<unsigned char*>(raw.data()) + c * sb, s.format.sample, s.background[c]);
    }
    Json tiles = Json::array();
    for (const TileRef& t : s.tiles) tiles.push_back(Json{{"tx", t.tx}, {"ty", t.ty}, {"hash", t.hash}});
    return Json{{"format", to_json(s.format)}, {"default", hex_of(raw)}, {"tiles", std::move(tiles)}};
}

Color color_from(const Json& value) {
    Color c;
    c.mode = mode_from(field(value, "mode", "colour"));
    const Json& comps = field(value, "c", "colour");
    if (!comps.is_array() || comps.size() != 4) unreadable("colour", "c is 4 numbers");
    for (std::size_t i = 0; i < 4; ++i) c.c[i] = number(comps[i], "colour.c");
    c.alpha = std::clamp(number(field(value, "alpha", "colour"), "colour.alpha"), 0.0, 1.0);
    return c;
}

Json to_json(const Color& c) {
    return Json{{"mode", to_string(c.mode)}, {"c", Json::array({c.c[0], c.c[1], c.c[2], c.c[3]})}, {"alpha", c.alpha}};
}

Color rgb_color(double r, double g, double b, double alpha) {
    Color c;
    c.mode = Mode::Rgb;
    c.c = {r, g, b, 0.0};
    c.alpha = alpha;
    return c;
}

std::optional<std::string> genko_blend(std::string_view upstream) {
    for (const auto& [name, genko] : kBlends) {
        if (name == upstream) return genko.empty() ? std::nullopt : std::optional<std::string>(std::string(genko));
    }
    return std::nullopt;
}

std::string upstream_blend(std::string_view genko) {
    for (const auto& [name, g] : kBlends) {
        if (!g.empty() && g == genko) return std::string(name);
    }
    return "Normal";
}

std::string blend_from(const Json& value) {
    if (value.is_string()) {
        for (const auto& [name, genko] : kBlends) {
            if (value.get_ref<const std::string&>() == name) return std::string(name);
        }
    }
    unreadable("blend", "not a blend mode this build knows");
}

Affine affine_from(const Json& value) {
    const Json& m = field(value, "m", "transform");
    if (!m.is_array() || m.size() != 6) unreadable("transform", "m is 6 numbers");
    Affine out{};
    for (std::size_t i = 0; i < 6; ++i) out[i] = number(m[i], "transform.m");
    return out;
}

Json to_json(const Affine& m) { return Json{{"m", Json::array({m[0], m[1], m[2], m[3], m[4], m[5]})}}; }

core::vector::Path path_from(const Json& value, double k) {
    core::vector::Path path;
    const Json& subpaths = field(value, "subpaths", "path");
    if (!subpaths.is_array()) unreadable("path", "subpaths is a list");
    std::size_t knots = 0;
    for (const Json& s : subpaths) {
        core::vector::Subpath sub;
        sub.closed = boolean(field(s, "closed", "subpath"), "subpath.closed");
        if (const Json* op = optional_field(s, "op")) {
            const std::string name = text(*op, "subpath.op");
            sub.op = name == "Combine"     ? core::vector::PathOp::Combine
                     : name == "Subtract"  ? core::vector::PathOp::Subtract
                     : name == "Intersect" ? core::vector::PathOp::Intersect
                     : name == "Exclude"   ? core::vector::PathOp::Exclude
                                           : (unreadable("subpath.op", "not an operation this build knows"), core::vector::PathOp::Combine);
        }
        const Json& list = field(s, "knots", "subpath");
        if (!list.is_array()) unreadable("subpath", "knots is a list");
        for (const Json& kn : list) {
            if (++knots > core::vector::kKnotsMax) unreadable("path", "more knots than this build holds");
            core::vector::Knot knot;
            knot.anchor = point_from(field(kn, "anchor", "knot"), k, "knot.anchor");
            knot.in = point_from(field(kn, "in_ctrl", "knot"), k, "knot.in_ctrl");
            knot.out = point_from(field(kn, "out_ctrl", "knot"), k, "knot.out_ctrl");
            if (const Json* smooth = optional_field(kn, "smooth")) knot.smooth = boolean(*smooth, "knot.smooth");
            sub.knots.push_back(knot);
        }
        path.subpaths.push_back(std::move(sub));
    }
    if (const Json* rule = optional_field(value, "fill_rule")) {
        const std::string name = text(*rule, "path.fill_rule");
        if (name == "EvenOdd") {
            path.fill_rule = core::vector::FillRule::EvenOdd;
        } else if (name != "NonZero") {
            unreadable("path.fill_rule", "not a fill rule this build knows");
        }
    }
    if (const Json* inverted = optional_field(value, "inverted")) path.inverted = boolean(*inverted, "path.inverted");
    return path;
}

Json path_json(const core::vector::Path& path, double k) {
    Json subpaths = Json::array();
    for (const core::vector::Subpath& s : path.subpaths) {
        Json knots = Json::array();
        for (const core::vector::Knot& kn : s.knots) {
            knots.push_back(Json{{"anchor", point_json(kn.anchor, k)}, {"in_ctrl", point_json(kn.in, k)}, {"out_ctrl", point_json(kn.out, k)}, {"smooth", kn.smooth}});
        }
        const std::string_view op = s.op == core::vector::PathOp::Subtract    ? "Subtract"
                                    : s.op == core::vector::PathOp::Intersect ? "Intersect"
                                    : s.op == core::vector::PathOp::Exclude   ? "Exclude"
                                                                              : "Combine";
        subpaths.push_back(Json{{"closed", s.closed}, {"knots", std::move(knots)}, {"op", op}});
    }
    return Json{{"subpaths", std::move(subpaths)},
                {"fill_rule", path.fill_rule == core::vector::FillRule::EvenOdd ? "EvenOdd" : "NonZero"},
                {"inverted", path.inverted}};
}

core::vector::VectorMask vector_mask_from(const Json& value, double k) {
    core::vector::VectorMask mask;
    mask.path = path_from(field(value, "path", "vector_mask"), k);
    mask.enabled = boolean(field(value, "enabled", "vector_mask"), "vector_mask.enabled");
    mask.linked = boolean(field(value, "linked", "vector_mask"), "vector_mask.linked");
    mask.density = std::clamp(number(field(value, "density", "vector_mask"), "vector_mask.density"), 0.0, 1.0);
    mask.feather_mm = std::clamp(number(field(value, "feather", "vector_mask"), "vector_mask.feather") * k, 0.0, core::vector::kFeatherMax);
    return mask;
}

Json vector_mask_json(const core::vector::VectorMask& mask, double k) {
    return Json{{"path", path_json(mask.path, k)}, {"enabled", mask.enabled}, {"linked", mask.linked}, {"density", mask.density}, {"feather", mask.feather_mm * k}};
}

std::optional<std::string> key_from_hex(std::string_view hex) {
    if (hex.size() != 8) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < 8; i += 2) {
        int v = 0;
        for (std::size_t j = i; j < i + 2; ++j) {
            const char c = hex[j];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * 16 + d;
        }
        out.push_back(static_cast<char>(v));
    }
    return out;
}

std::string hex_of(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

std::string first_chars(std::string_view text, std::size_t count, std::size_t max_bytes) {
    std::size_t at = 0, n = 0;
    while (at < text.size() && n < count) {
        std::size_t next = at + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        if (next > max_bytes) break;
        at = next;
        ++n;
    }
    return std::string(text.substr(0, at));
}

}  // namespace genko::render::pcraft
