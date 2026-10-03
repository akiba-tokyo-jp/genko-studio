// How layers come together (Python's genko/render.py _blend_over, _blend_math, _nonseparable, fill_layer_image,
// gradient_*, layer_effects, _adjusted, _has_colour; genko/filters.py's colour adjustments laid on the picture, their
// tables made by core/filters.hpp). The parts Python does with
// numpy are loops here, with numpy's types (float32 for the blend modes, float64 for the gradients) and its order of
// operations, so the pixels are the same.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <tuple>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/filters.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"

namespace genko::render::detail {

namespace {

using core::get;
using core::Json;
using core::py_max;
using core::py_min;

// libm's hypot (numpy's np.hypot), called through a pointer the compiler cannot replace
double (*volatile g_hypot)(double, double) = [](double a, double b) { return std::hypot(a, b); };

// round(x) and int(x) for a value of a pixel or a table
int py_round_i(double x) { return static_cast<int>(core::py_round_int(x)); }
int py_int_trunc(double x) { return static_cast<int>(core::py_trunc_int(x)); }

// spec.get(key, fallback) as float(…)
double number(const Json& spec, std::string_view key, double fallback) {
    const Json* v = get(spec, key);
    return v != nullptr ? core::py_float(*v) : fallback;
}

// a numpy uint8 from a float in 0..255 (astype("uint8") truncates)
// (NaN, an infinity or a value past an int: 0, what numpy gives there on x86, without the undefined cast)
unsigned char to_u8(double v) { return v > -2147483649.0 && v < 2147483648.0 ? static_cast<unsigned char>(static_cast<int>(v)) : 0; }

// --- gradients ------------------------------------------------------------------------------------------------------

struct Stop {
    double pos;
    std::vector<std::int64_t> rgb;
    double opacity;
};

// render.gradient_stops
std::vector<Stop> gradient_stops(const Json& spec) {
    std::vector<Stop> out;
    const Json* raw = get(spec, "stops");
    if (raw != nullptr && core::py_truthy(*raw)) {
        for (const Json& stop : core::py_list(*raw)) {
            const double pos = core::py_float(stop.at(0));
            std::vector<std::int64_t> rgb = core::int_tuple(stop.at(1));
            if (rgb.size() > 3) rgb.resize(3);
            const double opacity = stop.size() > 2 && !stop[2].is_null() ? core::py_float(stop[2]) : 1.0;
            out.push_back(Stop{py_max(0.0, py_min(1.0, pos)), rgb, py_max(0.0, py_min(1.0, opacity))});
        }
        std::stable_sort(out.begin(), out.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
        return out;
    }
    const auto ends = [&](std::string_view key, std::vector<std::int64_t> fallback) {
        const Json* v = get(spec, key);
        std::vector<std::int64_t> rgb = (v != nullptr && core::py_truthy(*v)) ? core::int_tuple(*v) : std::move(fallback);
        if (rgb.size() > 3) rgb.resize(3);
        return rgb;
    };
    out.push_back(Stop{0.0, ends("rgb_from", {20, 20, 20}), py_max(0.0, py_min(1.0, number(spec, "opacity_from", 1.0)))});
    out.push_back(Stop{1.0, ends("rgb_to", {255, 255, 255}), py_max(0.0, py_min(1.0, number(spec, "opacity_to", 1.0)))});
    return out;
}

struct GradientT {
    enum class Shape { Linear, Radial, Ellipse };
    enum class Repeat { None, Repeat, Mirror };
    double fx = 0, fy = 0, tx = 0, ty = 100;
    double length = 1.0;
    Shape shape_id = Shape::Linear;
    Repeat repeat_id = Repeat::None;
    double ux = 0, uy = 0, ratio = 0.5;

    explicit GradientT(const Json& spec) {
        const auto pair = [&](std::string_view key, double a, double b) {
            const Json* v = get(spec, key);
            if (v == nullptr) return std::pair<double, double>{a, b};
            const Json list = core::py_list(*v);
            if (list.size() < 2) throw core::Error("value", "not enough values to unpack (expected 2)");
            return std::pair<double, double>{core::py_float(list[0]), core::py_float(list[1])};
        };
        std::tie(fx, fy) = pair("from", 0, 0);
        std::tie(tx, ty) = pair("to", 0, 100);
        length = core::py_hypot(tx - fx, ty - fy);
        if (length == 0.0) length = 1.0;
        std::string shape = "linear";
        std::string repeat = "none";
        const Json* s = get(spec, "shape");
        if (s != nullptr && core::py_truthy(*s)) shape = core::py_str(*s);
        const Json* r = get(spec, "repeat");
        if (r != nullptr && core::py_truthy(*r)) repeat = core::py_str(*r);
        shape_id = shape == "radial" ? Shape::Radial : shape == "ellipse" ? Shape::Ellipse : Shape::Linear;
        repeat_id = repeat == "repeat" ? Repeat::Repeat : repeat == "mirror" ? Repeat::Mirror : Repeat::None;
        if (shape_id == Shape::Ellipse) {
            ux = (tx - fx) / length;
            uy = (ty - fy) / length;
            ratio = py_max(0.05, number(spec, "ratio", 0.5));
        }
    }

    double operator()(double gx, double gy) const {
        double t = 0.0;
        if (shape_id == Shape::Radial) {
            t = g_hypot(gx - fx, gy - fy) / length;
        } else if (shape_id == Shape::Ellipse) {
            const double along = (gx - fx) * ux + (gy - fy) * uy;
            const double across = (-(gx - fx) * uy + (gy - fy) * ux) / ratio;
            t = g_hypot(along, across) / length;
        } else {
            t = ((gx - fx) * (tx - fx) + (gy - fy) * (ty - fy)) / (length * length);
        }
        if (repeat_id == Repeat::Repeat) return t - std::floor(t);
        if (repeat_id == Repeat::Mirror) return 1 - std::fabs(core::py_fmod(t, 2.0) - 1);
        // np.clip(t, 0.0, 1.0)
        return std::isnan(t) ? t : std::min(std::max(t, 0.0), 1.0);
    }
};

// render.gradient_image over `area`
Image gradient_image(Size size, int dpi, const Json& spec, const Box& area) {
    (void)size;
    const double scale = dpi / 25.4;
    const GradientT t_of(spec);
    const std::vector<Stop> stops = gradient_stops(spec);
    std::vector<double> pos;
    for (const Stop& s : stops) pos.push_back(s.pos);
    std::vector<core::NumpyInterp> colour;
    for (std::size_t c = 0; c < 3; ++c) {
        std::vector<double> fp;
        for (const Stop& s : stops) {
            if (c >= s.rgb.size()) throw core::Error("value", "tuple index out of range");
            fp.push_back(static_cast<double>(s.rgb[c]));
        }
        colour.emplace_back(pos, fp);
    }
    std::vector<double> alpha_fp;
    for (const Stop& s : stops) alpha_fp.push_back(s.opacity);
    const core::NumpyInterp alpha(pos, alpha_fp);
    const int w = area.width();
    const int h = area.height();
    std::string data(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, '\0');
    std::vector<double> gxs(static_cast<std::size_t>(w));
    for (int i = 0; i < w; ++i) gxs[static_cast<std::size_t>(i)] = (static_cast<double>(area.x0 + i) + 0.5) / scale;
    std::size_t at = 0;
    for (int j = 0; j < h; ++j) {
        const double gy = (static_cast<double>(area.y0 + j) + 0.5) / scale;
        for (int i = 0; i < w; ++i) {
            const double t = t_of(gxs[static_cast<std::size_t>(i)], gy);
            for (std::size_t c = 0; c < 3; ++c) data[at++] = static_cast<char>(to_u8(std::nearbyint(colour[c](t))));
            data[at++] = static_cast<char>(to_u8(std::nearbyint(alpha(t) * 255)));
        }
    }
    return Image::frombytes("RGBA", Size{w, h}, data);
}

}  // namespace

struct GradientColours::Impl {
    explicit Impl(const Json& spec) : t_of(spec), stops(gradient_stops(spec)) {
        std::vector<double> pos;
        for (const Stop& s : stops) pos.push_back(s.pos);
        for (std::size_t c = 0; c < 3; ++c) {
            std::vector<double> fp;
            for (const Stop& s : stops) {
                if (c >= s.rgb.size()) throw core::PyUncaught("IndexError", "tuple index out of range");
                fp.push_back(static_cast<double>(s.rgb[c]));
            }
            colour.emplace_back(pos, fp);
        }
        std::vector<double> alpha_fp;
        for (const Stop& s : stops) alpha_fp.push_back(s.opacity);
        alpha.emplace_back(pos, alpha_fp);
    }
    GradientT t_of;
    std::vector<Stop> stops;
    std::vector<core::NumpyInterp> colour;
    std::vector<core::NumpyInterp> alpha;
};

GradientColours::GradientColours(const Json& spec) : impl_(std::make_unique<Impl>(spec)) {}
GradientColours::~GradientColours() = default;

void GradientColours::at(double gx, double gy, unsigned char rgb[3], double& opacity) const {
    const double t = impl_->t_of(gx, gy);
    // (a place along the gradient that is not a number: from and to so far apart or so far off the page that it cannot
    // be worked out; a colour past an int: refused, not cast)
    if (!std::isfinite(t)) throw core::OpError("from and to of the gradient are too far apart or off the page");
    for (std::size_t c = 0; c < 3; ++c) {
        const double v = std::nearbyint(impl_->colour[c](t));
        if (!(v > -2147483649.0 && v < 2147483648.0)) throw core::OpError("the colours of the gradient are too large");
        rgb[c] = to_u8(v);
    }
    opacity = impl_->alpha[0](t);
    if (!std::isfinite(opacity)) throw core::OpError("from and to of the gradient are too far apart or off the page");
}

namespace {

// --- blend modes (numpy float32) --------------------------------------------------------------------------------

using F = float;

F lum3(const F c[3]) { return c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f; }

F min3(const F c[3]) { return std::min(std::min(c[0], c[1]), c[2]); }
F max3(const F c[3]) { return std::max(std::max(c[0], c[1]), c[2]); }

// np.minimum / np.maximum (no NaNs here)
F fmin_(F a, F b) { return b < a ? b : a; }
F fmax_(F a, F b) { return b > a ? b : a; }

void clip_colour(F c[3]) {
    const F el = lum3(c);
    const F n = min3(c);
    const F x = max3(c);
    if (n < 0) {
        for (int k = 0; k < 3; ++k) c[k] = el + (c[k] - el) * el / fmax_(el - n, 1e-6f);
    }
    if (x > 1) {
        for (int k = 0; k < 3; ++k) c[k] = el + (c[k] - el) * (1 - el) / fmax_(x - el, 1e-6f);
    }
}

void set_lum(F c[3], F el) {
    const F d = el - lum3(c);
    for (int k = 0; k < 3; ++k) c[k] = c[k] + d;
    clip_colour(c);
}

F sat3(const F c[3]) { return max3(c) - min3(c); }

void set_sat(F c[3], F s) {
    const F lo = min3(c);
    const F span = sat3(c);
    for (int k = 0; k < 3; ++k) c[k] = span > 1e-6f ? (c[k] - lo) * s / fmax_(span, 1e-6f) : 0.0f;
}

enum class Blend {
    Darken, Lighten, ColorBurn, ColorDodge, LinearBurn, SoftLight, HardLight, Difference, Exclusion, Subtract, Divide,
    Hue, Saturation, Color, Luminosity
};

Blend blend_of(std::string_view mode) {
    static constexpr std::pair<std::string_view, Blend> kModes[] = {
        {"darken", Blend::Darken},         {"lighten", Blend::Lighten},       {"color_burn", Blend::ColorBurn},
        {"color_dodge", Blend::ColorDodge}, {"linear_burn", Blend::LinearBurn}, {"soft_light", Blend::SoftLight},
        {"hard_light", Blend::HardLight},   {"difference", Blend::Difference}, {"exclusion", Blend::Exclusion},
        {"subtract", Blend::Subtract},     {"divide", Blend::Divide},         {"hue", Blend::Hue},
        {"saturation", Blend::Saturation}, {"color", Blend::Color},           {"luminosity", Blend::Luminosity},
    };
    for (const auto& [name, mode_id] : kModes) {
        if (name == mode) return mode_id;
    }
    return Blend::Divide;
}

F separable(Blend mode, F b, F o) {
    switch (mode) {
        case Blend::Darken: return fmin_(b, o);
        case Blend::Lighten: return fmax_(b, o);
        case Blend::ColorBurn:
            if (b >= 1) return 1.0f;
            return o <= 0 ? 0.0f : 1 - fmin_(1.0f, (1 - b) / fmax_(o, 1e-6f));
        case Blend::ColorDodge:
            if (b <= 0) return 0.0f;
            return o >= 1 ? 1.0f : fmin_(1.0f, b / fmax_(1 - o, 1e-6f));
        case Blend::LinearBurn: return std::min(std::max(b + o - 1, 0.0f), 1.0f);
        case Blend::SoftLight: {
            const F d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
            return o <= 0.5f ? b - (1 - 2 * o) * b * (1 - b) : b + (2 * o - 1) * (d - b);
        }
        case Blend::HardLight: return o <= 0.5f ? 2 * b * o : 1 - 2 * (1 - b) * (1 - o);
        case Blend::Difference: return std::fabs(b - o);
        case Blend::Exclusion: return b + o - 2 * b * o;
        case Blend::Subtract: return std::min(std::max(b - o, 0.0f), 1.0f);
        default: return o <= 0 ? 1.0f : fmin_(1.0f, b / fmax_(o, 1e-6f));  // divide
    }
}

// render._blend_math on RGB pictures of one size
Image blend_math(const Image& base, const Image& over, std::string_view mode_name) {
    const std::string b = base.tobytes();
    const std::string o = over.tobytes();
    std::string out(b.size(), '\0');
    const Blend mode = blend_of(mode_name);
    // (each band value as numpy's float32(v) / 255)
    F unit[256];
    for (int v = 0; v < 256; ++v) unit[v] = static_cast<F>(v) / 255.0f;
    const auto* bp = reinterpret_cast<const unsigned char*>(b.data());
    const auto* op = reinterpret_cast<const unsigned char*>(o.data());
    for (std::size_t p = 0; p + 2 < b.size(); p += 3) {
        const F bc[3] = {unit[bp[p]], unit[bp[p + 1]], unit[bp[p + 2]]};
        const F oc[3] = {unit[op[p]], unit[op[p + 1]], unit[op[p + 2]]};
        F m[3];
        switch (mode) {
            case Blend::Hue:
                std::memcpy(m, oc, sizeof m);
                set_sat(m, sat3(bc));
                set_lum(m, lum3(bc));
                break;
            case Blend::Saturation:
                std::memcpy(m, bc, sizeof m);
                set_sat(m, sat3(oc));
                set_lum(m, lum3(bc));
                break;
            case Blend::Color:
                std::memcpy(m, oc, sizeof m);
                set_lum(m, lum3(bc));
                break;
            case Blend::Luminosity:
                std::memcpy(m, bc, sizeof m);
                set_lum(m, lum3(oc));
                break;
            default:
                for (int k = 0; k < 3; ++k) m[k] = separable(mode, bc[k], oc[k]);
        }
        for (int k = 0; k < 3; ++k) {
            // np.clip(m * 255 + 0.5, 0, 255).astype(np.uint8)
            const F v = std::min(std::max(m[k] * 255.0f + 0.5f, 0.0f), 255.0f);
            out[p + static_cast<std::size_t>(k)] = static_cast<char>(static_cast<unsigned char>(static_cast<int>(v)));
        }
    }
    return Image::frombytes("RGB", base.size(), out);
}

// --- filters.py: the colour adjustments (their settings and tables: core/filters.hpp) -------------------------------

constexpr const char* kOtherFilters[] = {"blur", "sharpen", "mosaic", "motion_blur", "radial_blur", "zoom_blur", "noise",
                                         "wave", "twirl", "lineart", "despeckle", "glow", "rain"};

Image keep_alpha(const Image& rgb, const Image& source) {
    Image out = rgb.convert("RGBA");
    out.putalpha(source.getchannel(3));  // (source.split()[3])
    return out;
}

// filters.apply_filter for the colour adjustments; other filters are not drawn yet. Python's exceptions as they are
// (core::PyValueError: the ValueError _adjusted catches).
Image apply_filter(const Ctx& ctx, const Image& image, const std::string& kind, const Json& params, bool* skipped) {
    *skipped = false;
    const Image rgba = image.convert("RGBA");
    const bool known = std::find(core::kAdjustments.begin(), core::kAdjustments.end(), kind) != core::kAdjustments.end();
    if (!known) {
        // (a person's filter plugin runs in the external runner, not here)
        if (kind.starts_with("plugin:") && skip_unported(ctx, "adjust:plugin")) {
            *skipped = true;
            return rgba;
        }
        // the filters that change shapes too (an old book's correction layer may hold one): render/filters.cpp
        if (std::find(std::begin(kOtherFilters), std::end(kOtherFilters), kind) != std::end(kOtherFilters)) {
            try {
                return filters::apply_filter(rgba, kind, params);
            } catch (const core::OpError& error) {
                throw core::PyValueError(error.what());  // (a setting this build refuses: the layer does nothing)
            }
        }
        throw core::PyValueError("unknown filter " + kind);
    }
    const core::Adjustment adjustment = core::adjustment(kind, params);
    const auto& [first, second, third] = adjustment.tables;
    switch (adjustment.way) {
        case core::Adjustment::Way::Hsv: {
            const std::vector<Image> hsv = rgba.convert("RGB").convert("HSV").split();
            return keep_alpha(Image::merge("HSV", {hsv[0].point(first), hsv[1].point(second), hsv[2].point(third)}).convert("RGB"),
                              rgba);
        }
        case core::Adjustment::Way::Grey: {
            const Image grey = rgba.convert("L");  // (ImageOps.grayscale)
            if (first == second && second == third) return keep_alpha(grey.point(first).convert("RGB"), rgba);
            return keep_alpha(Image::merge("RGB", {grey.point(first), grey.point(second), grey.point(third)}), rgba);
        }
        case core::Adjustment::Way::Rgb: break;
    }
    std::vector<int> lut(first);
    lut.insert(lut.end(), second.begin(), second.end());
    lut.insert(lut.end(), third.begin(), third.end());
    return keep_alpha(rgba.convert("RGB").point(lut), rgba);
}

// ImageFilter.GaussianBlur(radius): the box radius each of its three passes uses (BoxBlur.c _gaussian_blur_radius)
int gaussian_reach(double radius) {
    const auto r = static_cast<float>(radius);
    const float sigma2 = r * r / 3;
    const float big_l = std::sqrt(12.0f * sigma2 + 1.0f);
    const float l = std::floor((big_l - 1.0f) / 2.0f);
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    const float box = l + a;
    return 3 * (static_cast<int>(std::floor(std::fabs(box))) + 2) + 2;
}

}  // namespace

// --- render._blend_over ----------------------------------------------------------------------------------------------

Image blend_over(const Image& base, const Image& over_in, std::string_view mode, double opacity, const Image* clip) {
    // (Python's convert("RGBA") copies, and split/merge to change the alpha: the same pixels as changing the alpha of
    // one copy, which is what is done here)
    const Image* over_ptr = &over_in;
    Image owned;
    if (over_in.mode() != "RGBA") {
        owned = over_in.convert("RGBA");
        over_ptr = &owned;
    }
    if (clip != nullptr || opacity < 1) {
        if (owned.empty()) owned = over_in.copy();
        if (clip != nullptr) owned.putalpha(chops::multiply(owned.getchannel(3), clip->mode() == "L" ? *clip : clip->convert("L")));
        if (opacity < 1) owned.putalpha(owned.getchannel(3).point([&](int p) { return py_int_trunc(p * opacity); }));
        over_ptr = &owned;
    }
    const Image& over = *over_ptr;
    Image base_converted;
    if (base.mode() != "RGBA") base_converted = base.convert("RGBA");
    const Image& base_rgba = base_converted.empty() ? base : base_converted;
    if (mode.empty() || mode == "normal") return alpha_composite(base_rgba, over);
    const std::vector<Image> b = base_rgba.split();
    const std::vector<Image> o = over.split();
    const Image base_rgb = Image::merge("RGB", {b[0], b[1], b[2]});
    const Image over_rgb = Image::merge("RGB", {o[0], o[1], o[2]});
    Image mixed;
    if (mode == "multiply") {
        mixed = chops::multiply(base_rgb, over_rgb);
    } else if (mode == "screen") {
        mixed = chops::screen(base_rgb, over_rgb);
    } else if (mode == "add") {
        mixed = chops::add(base_rgb, over_rgb);
    } else if (mode == "overlay") {
        mixed = chops::overlay(base_rgb, over_rgb);
    } else if (mode == "darken" || mode == "lighten" || mode == "color_burn" || mode == "color_dodge" ||
               mode == "linear_burn" || mode == "soft_light" || mode == "hard_light" || mode == "difference" ||
               mode == "exclusion" || mode == "subtract" || mode == "divide" || mode == "hue" || mode == "saturation" ||
               mode == "color" || mode == "luminosity") {
        mixed = blend_math(base_rgb, over_rgb, mode);
    } else {
        mixed = over_rgb;
    }
    Image mixed_rgba = mixed.convert("RGBA");
    mixed_rgba.putalpha(o[3]);
    return alpha_composite(base_rgba, mixed_rgba);
}

// --- fill layers ------------------------------------------------------------------------------------------------------

Image fill_layer_image(const core::Layer& layer, Size size, int dpi, bool mono, const Box& area) {
    const Json spec = layer.fill ? *layer.fill : Json::object();
    Image image;
    const Json* gradient = get(spec, "gradient");
    if (gradient != nullptr && core::py_truthy(*gradient)) {
        image = gradient_image(size, dpi, *gradient, area);
    } else {
        std::vector<std::int64_t> rgb{255, 255, 255};
        const Json* own = get(spec, "rgb");
        if (own != nullptr && core::py_truthy(*own)) {
            rgb = core::int_tuple(*own);
        } else if (layer.fill_rgb && !layer.fill_rgb->empty()) {
            rgb.clear();
            for (const core::Num& n : *layer.fill_rgb) rgb.push_back(core::py_int(n));
        }
        if (rgb.size() > 3) rgb.resize(3);
        image = Image::create("RGBA", Size{area.width(), area.height()}, Ink::with_alpha(rgb, 255));
    }
    if (mono) {
        const Image alpha = image.split()[3];
        image = image.convert("L").convert("RGBA");
        image.putalpha(alpha);
    }
    return image;
}

// --- layer_effects ---------------------------------------------------------------------------------------------------

int effect_margin(const core::Layer& layer, int dpi) {
    if (!layer.effect || !core::py_truthy(*layer.effect)) return 0;
    const Json& effect = *layer.effect;
    int margin = 0;
    const Json* water = get(effect, "water_edge");
    if (water != nullptr && core::py_truthy(*water)) {
        const int width = std::max(1, py_round_i(number(*water, "width_mm", 0.6) / 25.4 * dpi));
        margin = std::max(margin, width < 12 ? width + 2 : gaussian_reach(width));
    }
    const Json* border = get(effect, "border");
    if (border != nullptr && core::py_truthy(*border)) {
        const int width = std::max(1, py_round_i(number(*border, "width_mm", 0.5) / 25.4 * dpi));
        margin = std::max(margin, gaussian_reach(width / 1.5));
    }
    return margin;
}

Image layer_effects(const core::Layer& layer, Image raster, int dpi) {
    if (!layer.effect || !core::py_truthy(*layer.effect)) return raster;
    const Json& effect = *layer.effect;
    Image out = raster.mode() == "RGBA" ? std::move(raster) : raster.convert("RGBA");
    const Image alpha = out.getchannel(3);
    const Json* water = get(effect, "water_edge");
    if (water != nullptr && core::py_truthy(*water)) {
        if (!water->is_object()) throw core::Error("value", "'" + core::py_type_name(*water) + "' object has no attribute 'get'");
        const int width = std::max(1, py_round_i(number(*water, "width_mm", 0.6) / 25.4 * dpi));
        const double strength = py_max(0.0, py_min(1.0, number(*water, "strength", 0.6)));
        const Image inner = width < 12 ? alpha.filter(Filter::min_filter(width * 2 + 1))
                                       : alpha.filter(Filter::gaussian_blur(width)).point([](int v) { return v > 245 ? 255 : 0; });
        const Image rim = chops::subtract(alpha, inner);
        Image dark = Image::create("RGBA", out.size(), Ink{0, 0, 0, 0});
        dark.putalpha(rim.point([&](int v) { return py_int_trunc(v * strength * 0.6); }));
        Image shaded = out.copy();
        shaded.alpha_composite(dark);
        shaded.putalpha(alpha);
        out = std::move(shaded);
    }
    const Json* border_spec = get(effect, "border");
    if (border_spec != nullptr && core::py_truthy(*border_spec)) {
        if (!border_spec->is_object()) {
            throw core::Error("value", "'" + core::py_type_name(*border_spec) + "' object has no attribute 'get'");
        }
        const int width = std::max(1, py_round_i(number(*border_spec, "width_mm", 0.5) / 25.4 * dpi));
        std::vector<std::int64_t> rgb{255, 255, 255};
        const Json* own = get(*border_spec, "rgb");
        if (own != nullptr && core::py_truthy(*own)) rgb = core::int_tuple(*own);
        if (rgb.size() > 3) rgb.resize(3);
        const Image grown = alpha.filter(Filter::gaussian_blur(width / 1.5)).point([](int v) { return v > 12 ? 255 : 0; });
        Image border = Image::create("RGBA", out.size(), Ink::with_alpha(rgb, 0));
        border.putalpha(grown);
        border.alpha_composite(out);
        out = std::move(border);
    }
    return out;
}

bool needs_whole_page(const core::Page& page) {
    for (const core::Layer& layer : page.layers) {
        if (layer.kind != core::LayerKind::Adjust || !layer.visible || !layer.adjust || !layer.adjust->is_object()) continue;
        const auto it = layer.adjust->find("kind");
        if (it == layer.adjust->end() || !it->is_string()) continue;
        if (std::find(std::begin(kOtherFilters), std::end(kOtherFilters), it->get<std::string>()) != std::end(kOtherFilters)) return true;
    }
    return false;
}

// --- render._adjusted --------------------------------------------------------------------------------------------------

Image adjusted(const Ctx& ctx, Image rgba, const core::Layer& layer, const Image* clip, const Box& area) {
    Json spec = layer.adjust ? *layer.adjust : Json::object();
    if (!spec.is_object()) spec = Json::object();
    std::string kind;
    const auto it = spec.find("kind");
    if (it != spec.end()) {
        if (core::py_truthy(*it)) kind = it->is_string() ? it->get<std::string>() : core::py_str(*it);
        spec.erase(it);
    }
    if (kind.empty()) return rgba;
    Image changed;
    try {
        bool skipped = false;
        changed = apply_filter(ctx, rgba, kind, spec, &skipped);
        if (skipped) return rgba;
    } catch (const core::PyValueError&) {
        return rgba;  // (a ValueError: the layer does nothing)
    }
    const double opacity = layer.opacity;
    Image strength = Image::create("L", rgba.size(), Ink(py_round_i(255 * py_max(0.0, py_min(1.0, opacity)))));
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty()) {
        const Image shown =
            resized_part(open_image(*layer.mask->png, kPillowOpenLimits).convert("L"), ctx.size, area, Resample::Bicubic);
        strength = chops::multiply(strength, shown);
    }
    if (clip != nullptr) strength = chops::multiply(strength, clip->convert("L"));
    return composite(changed.convert("RGBA"), rgba, strength);
}

// --- render._has_colour --------------------------------------------------------------------------------------------

bool has_colour(const Image& image) {
    Image small = image.convert("RGBA");
    small.thumbnail(Size{96, 96});
    const Image alpha = small.getchannel("A");
    const Image sat = small.convert("RGB").convert("HSV").getchannel("S");
    const Image seen = chops::multiply(sat, alpha.point([](int v) { return v > 32 ? 255 : 0; }));
    return seen.getextrema()[0].second > 40;
}

Image resized_part(const Image& source, Size size, const Box& area, Resample resample) {
    return source.resize_region(size, area, resample);
}

}  // namespace genko::render::detail
