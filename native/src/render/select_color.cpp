#include "render/select_color.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "core/color_model.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/selection_ext.hpp"

// PhotoCraft v0.2.0 crates/engine/src/selection_cmds.rs (lines 30-288), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::select_color {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using selection_ext::Rgba;

namespace {

constexpr int kWorkDpi = 200;  // (the page as it shows: image_ops::page_pixels' working dpi)

// The pixels a selection by colour compares: RGB 0..1 and alpha (clipped to 0..1: a precise page's HDR as its brightest),
// on a grid of width × height stretched over the page.
struct Sampled {
    std::vector<Rgba> px;
    std::size_t width = 0;
    std::size_t height = 0;
};

const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto it = value.find(key);
    return it == value.end() || it->is_null() ? nullptr : &*it;
}

double number(const Json& value, const char* key, double fallback, const std::string& what) {
    const Json* v = field(value, key);
    if (v == nullptr) return fallback;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(what + "." + key + " must be a finite number");
    return v->get<double>();
}

bool flag(const Json& value, const char* key, bool fallback, const std::string& what) {
    const Json* v = field(value, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(what + "." + key + " must be true or false");
    return v->get<bool>();
}

const core::Document& book_of(const area_kinds::Context& c, const std::string& what) {
    if (c.episode == nullptr) throw OpError(what + " needs the book to read its pixels");
    return *c.episode;
}

// The layer "layer" names (none: the page as it shows).
const core::Layer* layer_of(const Json& value, const core::Page& page, const std::string& what) {
    const Json* v = field(value, "layer");
    if (v == nullptr) return nullptr;
    if (v->is_string()) {
        for (const core::Layer& layer : page.layers)
            if (layer.id == v->get_ref<const std::string&>()) return &layer;
    }
    throw OpError(what + ": no layer " + core::py_str(*v));
}

Sampled sample(const core::Document& doc, const core::Page& page, const core::Layer* layer) {
    const Pixels pixels = layer != nullptr ? image_ops::shown_layer_pixels(doc, page, *layer) : image_ops::page_pixels(doc, page);
    const image_ops::Held held(std::uint64_t(pixels.size()) * sizeof(Rgba));
    Sampled out;
    out.px = image_ops::rgba_of(pixels);
    for (Rgba& p : out.px)
        for (float& v : p) v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    out.width = pixels.width();
    out.height = pixels.height();
    return out;
}

// The pixel of the grid under a point of the page (mm); none off the page.
std::optional<std::pair<int, int>> pixel_at(const core::Page& page, std::size_t w, std::size_t h, double x_mm, double y_mm) {
    const double fx = x_mm / page.spec.width_mm.value() * static_cast<double>(w);
    const double fy = y_mm / page.spec.height_mm.value() * static_cast<double>(h);
    if (!(fx >= 0 && fy >= 0 && fx < static_cast<double>(w) && fy < static_cast<double>(h))) return std::nullopt;
    return std::make_pair(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
}

// A mask worked on the sampled grid as an "L" picture of `size` (stretched bilinear when it is grey or made smaller; a
// two-valued one made larger stays hard).
Image to_size(const std::vector<float>& m, std::size_t w, std::size_t h, Size size, bool grey) {
    const Size grid{static_cast<int>(w), static_cast<int>(h)};
    Image mask = selection_ext::to_l(m, grid);
    if (grid == size) return mask;
    const bool smaller = size.width < grid.width || size.height < grid.height;
    return mask.resize(size, grey || smaller ? Resample::Bilinear : Resample::Nearest);
}

// A mask ("L" of the page at some size) on the sampled grid, as coverage 0..1.
std::vector<float> on_grid(const Image& mask, std::size_t w, std::size_t h) {
    const Size grid{static_cast<int>(w), static_cast<int>(h)};
    return selection_ext::from_l(mask.size() == grid ? mask : mask.resize(grid, Resample::Bilinear));
}

std::array<float, 3> rgb_of(const Json& v, const std::string& what) {
    if (!v.is_array() || v.size() != 3) throw OpError(what + " must be [r, g, b] (0..255 each)");
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!v[i].is_number() || v[i].is_boolean() || !std::isfinite(v[i].get<double>()) || v[i].get<double>() < 0 || v[i].get<double>() > 255) {
            throw OpError(what + " must be [r, g, b] (0..255 each)");
        }
        out[i] = static_cast<float>(v[i].get<double>() / 255.0);
    }
    return out;
}

// The eyedropper's samples: the colour under each point (mm) and where (grid pixels).
std::vector<selection_ext::RangeSample> samples_at(const Json& value, const char* key, const core::Page& page, const Sampled& s,
                                                   const std::string& what) {
    std::vector<selection_ext::RangeSample> out;
    const Json* points = field(value, key);
    if (points == nullptr) return out;
    const std::string name = what + "." + key;
    if (!points->is_array()) throw OpError(name + " must be [[x, y], …] (mm)");
    limits::check_count(static_cast<double>(points->size()), static_cast<double>(limits::kPoints), name);
    for (const Json& point : *points) {
        if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number() || !std::isfinite(point[0].get<double>()) ||
            !std::isfinite(point[1].get<double>())) {
            throw OpError(name + " must be [[x, y], …] (mm)");
        }
        const auto at = pixel_at(page, s.width, s.height, point[0].get<double>(), point[1].get<double>());
        if (!at) throw OpError(name + ": the point " + point.dump() + " is off the page");
        const Rgba& p = s.px[static_cast<std::size_t>(at->second) * s.width + static_cast<std::size_t>(at->first)];
        out.push_back(selection_ext::RangeSample{{p[0], p[1], p[2]}, std::make_pair(static_cast<float>(at->first), static_cast<float>(at->second))});
    }
    return out;
}

// out_of_gamut's proof setup from the op: "profile" a built-in id or one of the book's by its sha256 ref (none: the plain
// conversion for print).
proof::Setup setup_of(const Json& value, const core::Document& doc, const std::string& what) {
    proof::Setup setup;
    const Json* given = field(value, "profile");
    if (given == nullptr) return setup;
    if (!given->is_string()) throw OpError(what + ".profile must be a built-in profile's id or a sha256 ref of the book's");
    const std::string name = given->get<std::string>();
    if (name.starts_with("sha256:")) {
        if (const core::BuiltinProfile* builtin = core::builtin_profile_by_ref(name)) {
            setup.icc = builtin->bytes;
            return setup;
        }
        const auto held = [&](const std::optional<core::PageColor>& inks, const std::optional<core::RgbMode>& rgb) {
            if (inks && inks->profile == name && inks->icc) setup.icc = inks->icc;
            if (rgb && rgb->profile == name && rgb->icc) setup.icc = rgb->icc;
        };
        held(doc.color, doc.rgb);
        for (std::size_t i = 0; i < doc.pages.size() && !setup.icc; ++i)
            if (!doc.is_deferred(i)) held(doc.page(i).color, doc.page(i).rgb);
        if (!setup.icc) throw OpError(what + ".profile: the book has no ICC profile " + name);
        return setup;
    }
    const core::BuiltinProfile* builtin = core::builtin_profile(name);
    if (builtin == nullptr || !builtin->bytes) throw OpError(what + ".profile: no built-in profile " + core::py_repr_str(name));
    setup.icc = builtin->bytes;
    return setup;
}

// grow_similar: the area's pixels (half covered or more) and those whose colour lies within its range widened by the
// tolerance — joined to it (Grow) or anywhere (Similar).
Image grow_or_similar(const Json& value, const area_kinds::Context& c, bool contiguous) {
    const std::string what = contiguous ? "grow" : "similar";
    if (!value.is_object()) throw OpError(what + " must be {\"area\", \"tolerance\"?, \"layer\"?}");
    for (const auto& [key, v] : value.items()) {
        if (key != "area" && key != "tolerance" && key != "layer") throw OpError(what + " has no setting " + core::py_repr_str(key));
    }
    const Json* inner = field(value, "area");
    if (inner == nullptr) throw OpError(what + ".area (the selection to " + what + ") is required");
    const float tol = static_cast<float>(std::clamp(number(value, "tolerance", 32.0, what), 0.0, 255.0) / 255.0);
    const core::Document& doc = book_of(c, what);
    const core::Layer* layer = layer_of(value, c.page, what);
    const Image chosen = c.nested(*inner);
    Sampled s = sample(doc, c.page, layer);
    if (s.width < static_cast<std::size_t>(c.size.width) || s.height < static_cast<std::size_t>(c.size.height)) {
        // (a layer coarser than the area's grid: its colours on that grid, each pixel the one under it — a small selection
        // would otherwise be less than half of one of its pixels, and nothing)
        const std::size_t w = static_cast<std::size_t>(c.size.width), h = static_cast<std::size_t>(c.size.height);
        const image_ops::Held held(std::uint64_t(w) * h * sizeof(Rgba));
        std::vector<Rgba> fine(w * h);
        for (std::size_t y = 0; y < h; ++y) {
            const std::size_t sy = std::min(s.height - 1, y * s.height / h);
            for (std::size_t x = 0; x < w; ++x) fine[y * w + x] = s.px[sy * s.width + std::min(s.width - 1, x * s.width / w)];
        }
        s = Sampled{std::move(fine), w, h};
    }
    const std::vector<float> m = on_grid(chosen, s.width, s.height);
    std::array<float, 4> lo{2, 2, 2, 2}, hi{-1, -1, -1, -1};
    bool any = false;
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (m[i] < 0.5f) continue;
        any = true;
        for (std::size_t k = 0; k < 4; ++k) {
            lo[k] = std::min(lo[k], s.px[i][k]);
            hi[k] = std::max(hi[k], s.px[i][k]);
        }
    }
    if (!any) throw OpError(what + ": the area is empty");
    const auto inside = [&](const Rgba& q) {
        for (std::size_t k = 0; k < 4; ++k)
            if (q[k] < lo[k] - tol - 1e-6f || q[k] > hi[k] + tol + 1e-6f) return false;
        return true;
    };
    const std::size_t w = s.width, h = s.height;
    std::vector<float> out(m.size(), 0.0f);
    for (std::size_t i = 0; i < m.size(); ++i) out[i] = m[i] >= 0.5f ? 1.0f : 0.0f;
    if (contiguous) {
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (out[i] > 0.0f) stack.push_back(i);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const std::size_t x = i % w, y = i / w;
            const auto visit = [&](std::size_t j) {
                if (out[j] == 0.0f && inside(s.px[j])) {
                    out[j] = 1.0f;
                    stack.push_back(j);
                }
            };
            if (x > 0) visit(i - 1);
            if (x + 1 < w) visit(i + 1);
            if (y > 0) visit(i - w);
            if (y + 1 < h) visit(i + w);
        }
    } else {
        for (std::size_t i = 0; i < out.size(); ++i)
            if (inside(s.px[i])) out[i] = 1.0f;
    }
    return to_size(out, w, h, c.size, false);
}

}  // namespace

bool own_colour(const Json& spec, const core::Page& page) {
    if (field(spec, "layer") != nullptr || field(spec, "antialias") != nullptr) return true;
    return page.color.has_value() || (page.rgb && (page.rgb->depth != 8 || !page.rgb->profile.empty()));
}

Image colour(const Json& spec, const core::Page& page, const core::Document* episode, Size size) {
    const std::string what = "color";
    if (!spec.is_object()) throw OpError("color must be {\"x_mm\", \"y_mm\", …}");
    for (const char* key : {"x_mm", "y_mm"}) {
        if (field(spec, key) == nullptr) throw OpError(std::string("color.") + key + " is required");
    }
    const double x = number(spec, "x_mm", 0, what), y = number(spec, "y_mm", 0, what);
    const float tolerance = static_cast<float>(std::clamp(number(spec, "tolerance", 24.0, what), 0.0, 255.0));
    const bool contiguous = flag(spec, "contiguous", false, what);
    const bool antialias = flag(spec, "antialias", false, what);
    if (episode == nullptr) throw OpError("color needs the book to read its pixels");
    const core::Layer* layer = layer_of(spec, page, what);
    const Sampled s = sample(*episode, page, layer);
    const auto at = pixel_at(page, s.width, s.height, x, y);
    if (!at) throw OpError("the colour point is off the page");
    const selection_ext::Rect whole{0, 0, static_cast<int>(s.width), static_cast<int>(s.height)};
    const std::vector<float> m = selection_ext::magic_wand(s.px, whole, *at, tolerance, contiguous, antialias);
    return to_size(m, s.width, s.height, size, antialias);
}

Image range(const Json& value, const Json&, const area_kinds::Context& c) {
    const std::string what = "range";
    if (!value.is_object()) throw OpError("range must be {\"select\"?, …}");
    for (const auto& [key, v] : value.items()) {
        static const std::vector<std::string> kKeys{"select",  "colors", "points_mm", "subtract_points_mm", "fuzziness", "localized", "range",
                                                    "tonal_range", "invert", "layer", "profile", "threshold"};
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) throw OpError("range has no setting " + core::py_repr_str(key));
    }
    const Json* given = field(value, "select");
    if (given != nullptr && !given->is_string()) throw OpError("range.select must be a string");
    const std::string preset = given != nullptr ? given->get<std::string>() : std::string("sampled");
    const core::Document& doc = book_of(c, what);
    const core::Layer* layer = layer_of(value, c.page, what);
    const auto fuzz = [&](double fallback) { return static_cast<float>(std::max(0.0, number(value, "fuzziness", fallback, what))); };
    std::vector<float> mask;
    std::size_t w = 0, h = 0;
    if (preset == "sampled") {
        const Sampled s = sample(doc, c.page, layer);
        w = s.width;
        h = s.height;
        std::vector<selection_ext::RangeSample> samples = samples_at(value, "points_mm", c.page, s, what);
        const std::vector<selection_ext::RangeSample> minus = samples_at(value, "subtract_points_mm", c.page, s, what);
        if (const Json* colors = field(value, "colors")) {
            if (!colors->is_array()) throw OpError("range.colors must be a list of [r, g, b]");
            for (const Json& one : *colors) samples.push_back(selection_ext::RangeSample{rgb_of(one, "range.colors"), std::nullopt});
        }
        if (samples.empty()) throw OpError("range: Sampled Colors needs colors or points_mm");
        std::optional<float> localized;
        if (flag(value, "localized", false, what)) {
            const bool placed = std::any_of(samples.begin(), samples.end(), [](const auto& x) { return x.at.has_value(); }) ||
                                std::any_of(minus.begin(), minus.end(), [](const auto& x) { return x.at.has_value(); });
            if (!placed) throw OpError("range: localized clusters need points_mm");
            localized = static_cast<float>(std::clamp(number(value, "range", 100.0, what), 0.0, 100.0) / 100.0 * static_cast<double>(std::max(w, h)));
        }
        const float f = std::min(fuzz(40.0), 200.0f);
        mask = selection_ext::color_range_samples(s.px, w, samples, f, localized);
        if (!minus.empty()) {
            const std::vector<float> cut = selection_ext::color_range_samples(s.px, w, minus, f, localized);
            for (std::size_t i = 0; i < mask.size(); ++i) mask[i] *= 1.0f - cut[i];
        }
    } else if (preset == "reds" || preset == "yellows" || preset == "greens" || preset == "cyans" || preset == "blues" || preset == "magentas") {
        const float center = preset == "reds"      ? 0.0f
                             : preset == "yellows" ? 60.0f
                             : preset == "greens"  ? 120.0f
                             : preset == "cyans"   ? 180.0f
                             : preset == "blues"   ? 240.0f
                                                   : 300.0f;
        const Sampled s = sample(doc, c.page, layer);
        w = s.width;
        h = s.height;
        mask = selection_ext::hue_range(s.px, center);
    } else if (preset == "highlights" || preset == "midtones" || preset == "shadows") {
        // (Range in levels — Highlights and Shadows one, Midtones [low, high] —; Fuzziness a percentage of the scale)
        const auto level = [&](const Json& v) {
            if (!v.is_number() || v.is_boolean() || !std::isfinite(v.get<double>()) || v.get<double>() < 0 || v.get<double>() > 255) {
                throw OpError("range.tonal_range: want " + std::string(preset == "midtones" ? "[low, high] levels 0..255" : "a level 0..255") +
                              " (got " + v.dump() + ")");
            }
            return static_cast<float>(v.get<double>());
        };
        float lo = 105, hi = 150;
        const Json* tonal = field(value, "tonal_range");
        if (preset == "shadows") {
            lo = 0;
            hi = tonal != nullptr ? level(*tonal) : 65.0f;
        } else if (preset == "highlights") {
            lo = tonal != nullptr ? level(*tonal) : 190.0f;
            hi = 255;
        } else if (tonal != nullptr) {
            if (!tonal->is_array() || tonal->size() != 2) throw OpError("range.tonal_range: want [low, high] levels 0..255 (got " + tonal->dump() + ")");
            lo = level((*tonal)[0]);
            hi = level((*tonal)[1]);
            if (lo > hi) throw OpError("range.tonal_range: want [low, high] levels 0..255 (got " + tonal->dump() + ")");
        }
        const Sampled s = sample(doc, c.page, layer);
        w = s.width;
        h = s.height;
        mask = selection_ext::tone_range(s.px, lo, hi, std::min(fuzz(20.0), 100.0f) / 100.0f * 255.0f);
    } else if (preset == "out_of_gamut") {
        const double threshold = number(value, "threshold", proof::kDefaultThreshold, what);
        if (threshold < 0) throw OpError("range.threshold must not be negative");
        Image out = out_of_gamut(c.page, doc, setup_of(value, doc, what), threshold, c.size);
        if (flag(value, "invert", false, what)) out = out.point([](int v) { return 255 - v; });
        return out;
    } else {
        throw OpError("range: unknown select " + core::py_repr_str(preset) +
                      " (sampled, reds, yellows, greens, cyans, blues, magentas, highlights, midtones, shadows, out_of_gamut)");
    }
    if (flag(value, "invert", false, what)) {
        for (float& v : mask) v = 1.0f - v;
    }
    return to_size(mask, w, h, c.size, true);
}

Image grow(const Json& value, const Json&, const area_kinds::Context& c) { return grow_or_similar(value, c, true); }

Image similar(const Json& value, const Json&, const area_kinds::Context& c) { return grow_or_similar(value, c, false); }

Image out_of_gamut(const core::Page& page, const core::Document& doc, const proof::Setup& setup, double threshold, Size size) {
    // (the page as 色域外警告 reads it: its own colours — an RGB page's of its profile, a grey or CMYK page's inks — where
    // render_page gives them, else the picture's sRGB)
    proof::Source source;
    if (page.rgb && page.rgb->icc) source.icc = page.rgb->icc;
    if (page.color && page.color->space == core::InkSpace::Gray) source = proof::Source{page.color->icc, true, false, nullptr};
    const bool cmyk = page.color && page.color->space == core::InkSpace::Cmyk;
    if (cmyk) source = proof::Source{page.color->icc, false, true, nullptr};
    RenderOptions options = proof_options();
    options.onion = false;
    options.inks = source.icc && page.color.has_value();
    options.precise = source.icc && !page.color;
    const RenderResult drawn = render_page(page, kWorkDpi, options, &doc);
    if (options.precise && drawn.precise) source.values = proof::values_of(*drawn.precise);
    if (options.inks && drawn.inks) source.values = proof::values_of(*drawn.inks);
    std::vector<unsigned char> out;
    try {
        const auto check = proof::Gamut::of(setup, source);
        if (check->of_source() && source.values) {
            out = check->mask(*source.values, drawn.image, threshold);
        } else {
            out = proof::Gamut::of(setup)->mask(drawn.image, threshold);
        }
    } catch (const core::OpError&) {
        throw;
    } catch (const core::Error& error) {  // (a profile Little CMS cannot use)
        throw OpError(std::string("range: out_of_gamut: ") + error.what());
    }
    Image mask = Image::frombytes("L", drawn.image.size(), std::string(out.begin(), out.end()));
    return mask.size() == size ? mask : mask.resize(size, Resample::Bilinear);
}

}  // namespace genko::render::select_color
