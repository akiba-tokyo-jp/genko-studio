#include "render/patterns.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <mutex>
#include <numbers>
#include <unordered_map>

#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/patterns.hpp"
#include "render/color_canvas.hpp"
#include "render/image_ops.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/png.hpp"

// PhotoCraft v0.2.0 crates/compose/src/pattern.rs (Tile::sample, Placement::map, render; MIT:
// native/third_party/photocraft/NOTICE.md), placed in mm and on the three ways a page holds its colours.

namespace genko::render::patterns {

using core::Json;

namespace {

double number_at(const Json& spec, const char* key, double fallback) {
    const auto v = spec.find(key);
    return v != spec.end() && v->is_number() && !v->is_boolean() && std::isfinite(v->get<double>()) ? v->get<double>() : fallback;
}

unsigned colours_of(std::string_view mode) { return mode == "gray" ? 1u : mode == "cmyk" ? 4u : 3u; }

core::InkSpace space_of(std::string_view mode) {
    return mode == "cmyk" ? core::InkSpace::Cmyk : mode == "lab" ? core::InkSpace::Lab : core::InkSpace::Gray;
}

// The colour mode a CMYK or Lab pattern's values mean colours by when it is not on a page of its own mode: Lab's, or the
// built-in coated CMYK profile's.
core::PageColor own_color(const Tile& tile) {
    core::PageColor color;
    color.space = space_of(tile.mode);
    color.depth = tile.depth == 32 ? 16 : tile.depth;
    if (color.space == core::InkSpace::Cmyk) {
        if (const core::BuiltinProfile* b = core::builtin_profile("coated-cmyk")) {
            color.profile = b->ref();
            color.icc = b->bytes;
        }
    }
    return color;
}

bool same_mode(const Tile& tile, const core::PageColor& color) {
    if (tile.mode == "rgb") return false;
    return space_of(tile.mode) == color.space && color.space != core::InkSpace::Multi;
}

// The colours a tile means, straight sRGB (or an RGB page's values for an RGB tile: as they are), 3 floats a pixel.
std::vector<float> meant(const Tile& tile) {
    const std::size_t n = std::size_t(tile.width) * tile.height;
    const unsigned all = tile.colours + 1;
    std::vector<float> out(n * 3);
    if (tile.mode == "rgb") {
        for (std::size_t i = 0; i < n; ++i)
            for (unsigned c = 0; c < 3; ++c) out[i * 3 + c] = tile.values[i * all + c];
        return out;
    }
    if (tile.mode == "gray") {
        for (std::size_t i = 0; i < n; ++i) out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = tile.values[i * all];
        return out;
    }
    const core::PageColor color = own_color(tile);
    const ink::Rules rules(color);
    constexpr std::size_t kBand = 65536;
    std::vector<double> inks, rgb;
    for (std::size_t start = 0; start < n; start += kBand) {
        const std::size_t count = std::min(kBand, n - start);
        inks.assign(count * tile.colours, 0.0);
        rgb.assign(count * 3, 0.0);
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < tile.colours; ++c)
                inks[i * tile.colours + c] = from_canvas(color.space, c, tile.values[(start + i) * all + c]);
        rules.meaning(inks.data(), rgb.data(), count);
        for (std::size_t i = 0; i < count * 3; ++i) out[start * 3 + i] = static_cast<float>(rgb[i]);
    }
    return out;
}

// The tile in the target's values, straight (colours then alpha).
std::vector<float> made_for(const Tile& tile, const Target& target) {
    const std::size_t n = std::size_t(tile.width) * tile.height;
    const unsigned all = tile.colours + 1;
    const unsigned out_all = target.colours() + 1;
    std::vector<float> out(n * out_all);
    if (target.kind == Target::Kind::Inks && same_mode(tile, *target.color)) {
        return tile.values;  // (a pattern of the page's mode: its values as they are)
    }
    const std::vector<float> rgb = meant(tile);
    if (target.kind == Target::Kind::Rgb) {
        std::vector<double> page(3);
        const bool convert = tile.mode != "rgb" && tile.mode != "gray" && target.space && !target.space->is_srgb();
        for (std::size_t i = 0; i < n; ++i) {
            for (unsigned c = 0; c < 3; ++c) page[c] = rgb[i * 3 + c];
            if (convert) target.space->from_srgb(page.data(), 1);  // (the colour inks mean, as the page's values)
            for (unsigned c = 0; c < 3; ++c) out[i * out_all + c] = static_cast<float>(page[c]);
            out[i * out_all + 3] = tile.values[i * all + tile.colours];
        }
        return out;
    }
    // (another mode's colours as the page's inks, by its rules: once)
    const core::PageColor& color = *target.color;
    const ink::Rules rules(color);
    const unsigned planes = target.colours();
    constexpr std::size_t kBand = 65536;
    std::vector<double> colours, inks;
    for (std::size_t start = 0; start < n; start += kBand) {
        const std::size_t count = std::min(kBand, n - start);
        colours.assign(count * 3, 0.0);
        inks.assign(count * planes, 0.0);
        for (std::size_t i = 0; i < count * 3; ++i) colours[i] = std::clamp(static_cast<double>(rgb[start * 3 + i]), 0.0, 1.0);
        rules.inks(colours.data(), inks.data(), count);
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < planes; ++c)
                out[(start + i) * out_all + c] = static_cast<float>(to_canvas(color.space, c, inks[i * planes + c]));
            out[(start + i) * out_all + planes] = tile.values[(start + i) * all + tile.colours];
        }
    }
    return out;
}

// --- the made tiles kept --------------------------------------------------------------------------------------------

constexpr std::size_t kKeptBytes = 256u << 20;

struct Kept {
    std::string key;
    std::shared_ptr<const std::vector<float>> premultiplied;
    std::uint32_t width = 0, height = 0;
    unsigned colours = 3;
};

struct Store {
    std::mutex mutex;
    std::list<Kept> items;  // (most recent first)
    std::size_t bytes = 0;
};

Store& store() {
    static Store s;
    return s;
}

std::size_t bytes_of(const Kept& k) { return k.premultiplied ? k.premultiplied->size() * sizeof(float) : 0; }

std::optional<Kept> kept_one(const std::string& key) {
    Store& s = store();
    const std::lock_guard lock(s.mutex);
    for (auto it = s.items.begin(); it != s.items.end(); ++it) {
        if (it->key != key) continue;
        s.items.splice(s.items.begin(), s.items, it);
        return s.items.front();
    }
    return std::nullopt;
}

void keep(const Kept& made) {
    const std::size_t size = bytes_of(made);
    if (size > kKeptBytes / 2) return;  // (a very large tile is made each time)
    Store& s = store();
    const std::lock_guard lock(s.mutex);
    s.items.push_front(made);
    s.bytes += size;
    while (s.bytes > kKeptBytes && !s.items.empty()) {
        s.bytes -= bytes_of(s.items.back());
        s.items.pop_back();
    }
}

Kept made(const core::Pattern& pattern, const core::Bytes& bytes, const Target& target) {
    const std::string key = pattern.asset + "|" + pattern.mode + "|" + std::to_string(pattern.depth) + "|" + target.key();
    if (auto found = kept_one(key)) return *found;
    if (!bytes) throw core::Error("value", "the picture of the pattern " + pattern.id + " is not held");
    const std::uint64_t pixels = std::uint64_t(pattern.width) * std::uint64_t(pattern.height);
    // (the tile decoded, made and premultiplied: in the imaging budget while they are made)
    const image_ops::Held held(pixels * (colours_of(pattern.mode) + 1 + target.colours() + 1) * 2 * sizeof(float));
    const Tile tile = decode(pattern, *bytes);
    std::vector<float> values = made_for(tile, target);
    const unsigned all = target.colours() + 1;
    for (std::size_t i = 0; i < pixels; ++i) {
        float* p = values.data() + i * all;
        for (unsigned c = 0; c < all - 1; ++c) p[c] *= p[all - 1];
    }
    Kept out{key, std::make_shared<const std::vector<float>>(std::move(values)), tile.width, tile.height, target.colours()};
    keep(out);
    return out;
}

}  // namespace

Spot spot_of(const Json& spec) {
    Spot s;
    if (!spec.is_object()) return s;
    s.scale = std::clamp(number_at(spec, "scale", 100.0), core::kPatternScaleMin, core::kPatternScaleMax) / 100.0;
    s.angle = number_at(spec, "angle", 0.0);
    if (const auto p = spec.find("phase_mm"); p != spec.end() && p->is_array() && p->size() == 2) {
        if ((*p)[0].is_number()) s.phase_x_mm = (*p)[0].get<double>();
        if ((*p)[1].is_number()) s.phase_y_mm = (*p)[1].get<double>();
        if (!std::isfinite(s.phase_x_mm)) s.phase_x_mm = 0;
        if (!std::isfinite(s.phase_y_mm)) s.phase_y_mm = 0;
    }
    if (const auto l = spec.find("link"); l != spec.end() && l->is_boolean()) s.link = l->get<bool>();
    return s;
}

Tile decode(const core::Pattern& pattern, std::string_view bytes) {
    if (const auto why = core::check_pattern_file(pattern, bytes)) throw core::Error("value", "the pattern " + pattern.id + ": " + *why);
    Tile t;
    t.mode = pattern.mode;
    t.depth = pattern.depth;
    t.width = static_cast<std::uint32_t>(pattern.width);
    t.height = static_cast<std::uint32_t>(pattern.height);
    t.colours = colours_of(pattern.mode);
    const std::size_t n = std::size_t(t.width) * t.height;
    const unsigned all = t.colours + 1;
    t.values.assign(n * all, 0.0f);
    const std::string_view suffix = core::pattern_suffix(pattern);
    if (suffix == ".png") {
        const Image picture = read_png(bytes);
        const std::string raw = picture.tobytes();
        if (raw.size() != n * all) throw core::Error("value", "the pattern " + pattern.id + ": its picture cannot be read");
        for (std::size_t i = 0; i < raw.size(); ++i) t.values[i] = static_cast<unsigned char>(raw[i]) / 255.0f;
    } else if (suffix == core::kColorRasterSuffix) {
        const core::ColorRasterView view(bytes);
        for (std::size_t i = 0; i < n; ++i) {
            const std::array<double, 4> p = view.pixel(i);
            for (unsigned c = 0; c < 4; ++c) t.values[i * 4 + c] = static_cast<float>(p[c]);
        }
    } else {
        const core::InkPixels inks = core::decode_ink_raster(bytes);
        for (std::size_t i = 0; i < n; ++i)
            for (unsigned c = 0; c < all; ++c) t.values[i * all + c] = static_cast<float>(inks.value(i, c));
    }
    return t;
}

std::string encode(const Tile& tile) {
    const std::size_t n = std::size_t(tile.width) * tile.height;
    const unsigned all = tile.colours + 1;
    const auto byte = [](float v) { return static_cast<char>(std::lround(std::clamp(static_cast<double>(v), 0.0, 1.0) * 255.0)); };
    if (tile.depth == 8 && (tile.mode == "rgb" || tile.mode == "gray")) {
        std::string raw(n * all, '\0');
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = byte(tile.values[i]);
        return write_png(Image::frombytes(tile.mode == "rgb" ? "RGBA" : "LA", Size{static_cast<int>(tile.width), static_cast<int>(tile.height)}, raw));
    }
    if (tile.mode == "rgb") {
        return core::encode_color_pixels(tile.width, tile.height, tile.depth == 32 ? "f32" : "u16", [&](std::size_t i) {
            const float* p = tile.values.data() + i * 4;
            return std::array<double, 4>{p[0], p[1], p[2], std::clamp(static_cast<double>(p[3]), 0.0, 1.0)};
        });
    }
    core::InkPixels pixels = core::blank_ink_pixels(space_of(tile.mode), tile.depth, tile.width, tile.height);
    for (std::size_t i = 0; i < n; ++i)
        for (unsigned c = 0; c < all; ++c) pixels.set(i, c, tile.values[i * all + c]);
    return core::encode_ink_raster(pixels);
}

Target Target::rgb(const core::Page& page) {
    Target t;
    t.kind = Kind::Rgb;
    if (page.rgb && !page.rgb->profile.empty()) t.space = std::make_shared<const RgbSpace>(RgbSpace::of(page));
    if (page.rgb) {
        t.space_key = page.rgb->profile + "|" + page.rgb->intent + (page.rgb->bpc ? "|bpc" : "|");
    }
    return t;
}

Target Target::srgb() { return Target{}; }

Target Target::inks(const core::PageColor& color) {
    Target t;
    t.kind = Kind::Inks;
    t.color = color;
    t.space_key = core::to_json(color).dump();
    return t;
}

unsigned Target::colours() const { return kind == Kind::Rgb ? 3u : color->planes(); }

std::string Target::key() const {
    if (kind == Kind::Rgb) return space && !space->is_srgb() ? "rgb:" + space_key : std::string("srgb");
    return "inks:" + std::to_string(std::hash<std::string>{}(space_key));
}

Laid::Laid(std::shared_ptr<const std::vector<float>> premultiplied, std::uint32_t width, std::uint32_t height, unsigned colours, const Spot& spot,
           double pattern_dpi, double dpi)
    : px_(std::move(premultiplied)), w_(std::max(1u, width)), h_(std::max(1u, height)), colours_(colours) {
    // (a grid point (x, y) → its place from the phase in grid pixels, turned back by the angle, in the tile's pixels:
    // pattern.rs Placement::map with the phase in mm and the tile's pixel 1/pattern_dpi inch × the scale)
    ox_ = spot.phase_x_mm / 25.4 * dpi;
    oy_ = spot.phase_y_mm / 25.4 * dpi;
    const double a = spot.angle * std::numbers::pi / 180.0;
    c_ = std::cos(a);
    s_ = std::sin(a);
    if (std::fabs(c_) < 1e-15) c_ = 0;
    if (std::fabs(s_) < 1e-15) s_ = 0;
    const double scale = spot.scale > 1e-3 ? spot.scale : 1.0;
    k_ = pattern_dpi / (dpi * scale);
}

std::pair<double, double> Laid::map(double x, double y) const {
    const double dx = x - ox_, dy = y - oy_;
    return {(dx * c_ - dy * s_) * k_, (dx * s_ + dy * c_) * k_};
}

void Laid::at(int x, int y, float* out) const {
    const auto [u0, v0] = map(x + 0.5, y + 0.5);
    double u = u0 - 0.5, v = v0 - 0.5;
    double x0 = std::floor(u), y0 = std::floor(v);
    double fx = u - x0, fy = v - y0;
    // (a placement on whole pixels gives the tile exactly: what the arithmetic leaves of a step is none)
    constexpr double kSnap = 1e-6;
    if (fx < kSnap) fx = 0;
    if (fx > 1 - kSnap) { x0 += 1; fx = 0; }
    if (fy < kSnap) fy = 0;
    if (fy > 1 - kSnap) { y0 += 1; fy = 0; }
    const unsigned all = colours_ + 1;
    float acc[core::kPlatesMax + 1] = {};
    const auto wrap = [](double i, std::uint32_t n) {
        const auto m = static_cast<std::int64_t>(std::fmod(i, static_cast<double>(n)));
        return static_cast<std::size_t>(m < 0 ? m + n : m);
    };
    const std::size_t xs[2] = {wrap(x0, w_), wrap(x0 + 1, w_)};
    const std::size_t ys[2] = {wrap(y0, h_), wrap(y0 + 1, h_)};
    const double wx[2] = {1 - fx, fx}, wy[2] = {1 - fy, fy};
    for (int j = 0; j < 2; ++j) {
        if (wy[j] == 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (wx[i] == 0) continue;
            const float k = static_cast<float>(wx[i] * wy[j]);
            const float* p = px_->data() + (ys[j] * w_ + xs[i]) * all;
            for (unsigned c = 0; c < all; ++c) acc[c] += p[c] * k;
        }
    }
    const float a = acc[colours_];
    if (!(a > 0.0f)) {
        for (unsigned c = 0; c < all; ++c) out[c] = 0.0f;
        return;
    }
    for (unsigned c = 0; c < colours_; ++c) out[c] = acc[c] / a;
    out[colours_] = std::min(a, 1.0f);
}

Laid laid(const core::Pattern& pattern, const core::Bytes& bytes, const Json& fill_pattern, const Target& target, double dpi) {
    const Kept k = made(pattern, bytes, target);
    return Laid(k.premultiplied, k.width, k.height, k.colours, spot_of(fill_pattern), pattern.dpi, dpi);
}

std::optional<Laid> laid(const core::Document* book, const Json& fill_pattern, const Target& target, double dpi) {
    if (book == nullptr || !fill_pattern.is_object()) return std::nullopt;
    const auto id = fill_pattern.find("id");
    if (id == fill_pattern.end() || !id->is_string()) return std::nullopt;
    const core::Pattern* p = core::find_pattern(*book, id->get<std::string>());
    if (p == nullptr) return std::nullopt;
    const auto file = book->pattern_files.find(p->asset);
    if (file == book->pattern_files.end() || !file->second) return std::nullopt;
    return laid(*p, file->second, fill_pattern, target, dpi);
}

Image rgba8(const Laid& laid, const Box& area) {
    const int w = std::max(0, area.width()), h = std::max(0, area.height());
    std::string bytes(std::size_t(w) * h * 4, '\0');
    float v[core::kPlatesMax + 1];
    const auto byte = [](float x) { return static_cast<char>(std::lround(std::clamp(static_cast<double>(x), 0.0, 1.0) * 255.0)); };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            laid.at(area.x0 + x, area.y0 + y, v);
            char* o = bytes.data() + (std::size_t(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) o[c] = byte(v[c]);
        }
    }
    return Image::frombytes("RGBA", Size{w, h}, bytes);
}

ColorPicture linear(const Laid& laid, const Box& area, const RgbSpace& space) {
    ColorPicture out;
    out.size = Size{std::max(0, area.width()), std::max(0, area.height())};
    out.pixels.resize(std::size_t(out.size.width) * out.size.height);
    float v[core::kPlatesMax + 1];
    for (int y = 0; y < out.size.height; ++y) {
        for (int x = 0; x < out.size.width; ++x) {
            laid.at(area.x0 + x, area.y0 + y, v);
            auto& p = out.pixels[std::size_t(y) * out.size.width + x];
            for (unsigned c = 0; c < 3; ++c) p[c] = space.decode(c, v[c]);
            p[3] = v[3];
        }
    }
    return out;
}

InkPicture inks(const Laid& laid, const Box& area) {
    InkPicture out;
    out.size = Size{std::max(0, area.width()), std::max(0, area.height())};
    out.channels = laid.colours();
    const unsigned all = out.channels + 1;
    out.values.resize(std::size_t(out.size.width) * out.size.height * all);
    float v[core::kPlatesMax + 1];
    for (int y = 0; y < out.size.height; ++y) {
        for (int x = 0; x < out.size.width; ++x) {
            laid.at(area.x0 + x, area.y0 + y, v);
            double* o = out.values.data() + (std::size_t(y) * out.size.width + x) * all;
            for (unsigned c = 0; c < all; ++c) o[c] = v[c];
        }
    }
    return out;
}

std::size_t kept() {
    Store& s = store();
    const std::lock_guard lock(s.mutex);
    return s.items.size();
}

void forget() {
    Store& s = store();
    const std::lock_guard lock(s.mutex);
    s.items.clear();
    s.bytes = 0;
}

FromPage from_page(const core::Document& doc, const core::Page& page, const Json* area, const std::string& name) {
    const image_ops::Pixels p = image_ops::page_pixels(doc, page);
    const std::uint32_t w = p.width(), h = p.height();
    std::vector<float> cover;
    if (area != nullptr) cover = image_ops::coverage(page, *area, w, h, p.kind() == image_ops::Pixels::Kind::Eight);
    // (the box of what the area covers)
    std::uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (!cover.empty() && !(cover[std::size_t(y) * w + x] > 0.0f)) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) throw core::Error("value", "the area covers none of the page");
    const std::uint32_t tw = x1 - x0, th = y1 - y0;
    if (tw > core::kMaxPatternSide || th > core::kMaxPatternSide || std::int64_t(tw) * th > core::kMaxPatternPixels) {
        throw core::Error("value", "a pattern is at most 8192 pixels a side (" + std::to_string(tw) + " × " + std::to_string(th) + ")");
    }
    Tile t;
    t.width = tw;
    t.height = th;
    std::vector<image_ops::Rgba> meant_rgb;
    const bool inks = p.kind() == image_ops::Pixels::Kind::Inks;
    const bool multi = inks && p.ink && p.ink->space == core::InkSpace::Multi;
    if (!inks || multi) {
        t.mode = "rgb";
        t.depth = p.kind() == image_ops::Pixels::Kind::Precise ? (p.floating ? 32 : 16) : 8;
        t.colours = 3;
        if (multi) meant_rgb = image_ops::rgba_of(p);
    } else {
        t.mode = p.ink->space == core::InkSpace::Cmyk ? "cmyk" : p.ink->space == core::InkSpace::Lab ? "lab" : "gray";
        t.depth = p.ink->depth;
        t.colours = p.colours();
    }
    const unsigned all = t.colours + 1;
    t.values.assign(std::size_t(tw) * th * all, 0.0f);
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::size_t from = std::size_t(y0 + y) * w + (x0 + x);
            float* o = t.values.data() + (std::size_t(y) * tw + x) * all;
            const float k = cover.empty() ? 1.0f : std::clamp(cover[from], 0.0f, 1.0f);
            if (multi) {
                for (unsigned c = 0; c < 3; ++c) o[c] = meant_rgb[from][c];
                o[3] = meant_rgb[from][3] * k;
                continue;
            }
            const float* v = p.at(from);
            for (unsigned c = 0; c < t.colours; ++c) o[c] = v[c];
            o[t.colours] = std::clamp(v[p.colours()], 0.0f, 1.0f) * k;
        }
    }
    FromPage out;
    std::string bytes = encode(t);
    out.pattern.name = name.empty() ? std::string("パターン") : name;
    out.pattern.asset = core::bytes_ref(bytes);
    out.pattern.mode = t.mode;
    out.pattern.depth = t.depth;
    out.pattern.width = tw;
    out.pattern.height = th;
    out.pattern.dpi = std::clamp(w / (page.spec.width_mm.value() / 25.4), 50.0, 2400.0);
    out.bytes = std::make_shared<const std::string>(std::move(bytes));
    return out;
}

std::string grey(const core::Pattern& pattern, std::string_view bytes) {
    const Tile tile = decode(pattern, bytes);
    const std::vector<float> rgb = meant(tile);
    const std::size_t n = std::size_t(tile.width) * tile.height;
    const unsigned all = tile.colours + 1;
    std::string out(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        const double a = std::clamp(static_cast<double>(tile.values[i * all + tile.colours]), 0.0, 1.0);
        const double l = std::clamp(0.299 * rgb[i * 3] + 0.587 * rgb[i * 3 + 1] + 0.114 * rgb[i * 3 + 2], 0.0, 1.0);
        out[i] = static_cast<char>(std::lround((l * a + (1 - a)) * 255.0));
    }
    return out;
}

Image swatch(const core::Pattern& pattern, std::string_view bytes, int side) {
    side = std::max(1, side);
    const Tile tile = decode(pattern, bytes);
    Target target = Target::srgb();
    std::vector<float> values = made_for(tile, target);
    for (std::size_t i = 0; i < values.size() / 4; ++i)
        for (unsigned c = 0; c < 3; ++c) values[i * 4 + c] *= values[i * 4 + 3];
    // (the tile at a size that shows a few repeats: a large one shrunk to the side, a small one repeated)
    const double longest = std::max(tile.width, tile.height);
    const double per_pixel = longest > side ? longest / side : 1.0 / std::max(1.0, std::floor(side / 2.0 / longest));  // (tile pixels a pixel)
    const Spot spot;
    const Laid one(std::make_shared<const std::vector<float>>(std::move(values)), tile.width, tile.height, 3, spot, per_pixel, 1.0);
    return rgba8(one, Box{0, 0, side, side});
}

}  // namespace genko::render::patterns
