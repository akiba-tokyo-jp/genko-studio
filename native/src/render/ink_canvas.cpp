#include "render/ink_canvas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#include "core/error.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"

namespace genko::render {

namespace {

using core::InkSpace;

// --- the blend formulas (as ColorCanvas: PhotoCraft's color/blend.rs) ----------------------------------------------
using RGB = std::array<double, 3>;
double luminosity(const RGB& c) { return .3 * c[0] + .59 * c[1] + .11 * c[2]; }
RGB set_luminosity(RGB c, double target) {
    const double delta = target - luminosity(c);
    for (auto& v : c) v += delta;
    const double l = luminosity(c), lo = *std::min_element(c.begin(), c.end()), hi = *std::max_element(c.begin(), c.end());
    if (lo < 0) for (auto& v : c) v = std::abs(l - lo) < 1e-12 ? l : l + (v - l) * l / (l - lo);
    if (hi > 1) for (auto& v : c) v = std::abs(hi - l) < 1e-12 ? l : l + (v - l) * (1 - l) / (hi - l);
    return c;
}
double saturation(const RGB& c) { return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end()); }
RGB set_saturation(RGB c, double target) {
    const double lo = *std::min_element(c.begin(), c.end()), span = saturation(c);
    for (auto& v : c) v = span > 0 ? (v - lo) * target / span : 0;
    return c;
}
double color_burn(double b, double s) { return b >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.0, (1 - b) / s); }
double color_dodge(double b, double s) { return b <= 0 ? 0 : s >= 1 ? 1 : std::min(1.0, b / (1 - s)); }

double separable(std::string_view mode, double cb, double cs) {
    if (mode == "multiply") return cb * cs;
    if (mode == "screen") return cb + cs - cb * cs;
    if (mode == "overlay") return cb <= .5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (mode == "add") return std::min(1.0, cb + cs);
    if (mode == "darken") return std::min(cb, cs);
    if (mode == "lighten") return std::max(cb, cs);
    if (mode == "color_burn") return color_burn(cb, cs);
    if (mode == "color_dodge") return color_dodge(cb, cs);
    if (mode == "linear_burn") return std::max(0.0, cb + cs - 1);
    if (mode == "soft_light") return cs <= .5 ? 2 * cb * cs + cb * cb * (1 - 2 * cs) : 2 * cb * (1 - cs) + std::sqrt(std::max(0.0, cb)) * (2 * cs - 1);
    if (mode == "hard_light") return cs <= .5 ? 2 * cb * cs : cb + (2 * cs - 1) - cb * (2 * cs - 1);
    if (mode == "difference") return std::abs(cb - cs);
    if (mode == "exclusion") return cb + cs - 2 * cb * cs;
    if (mode == "subtract") return std::max(0.0, cb - cs);
    if (mode == "divide") return cs <= 0 ? (cb <= 0 ? 0 : 1) : std::min(1.0, cb / cs);
    return cs;
}

bool non_separable(std::string_view mode) { return mode == "hue" || mode == "saturation" || mode == "color" || mode == "luminosity"; }

constexpr std::array<std::string_view, 20> kModes{"normal",      "multiply",    "screen",     "add",        "overlay",
                                                  "darken",      "lighten",     "color_burn", "color_dodge", "linear_burn",
                                                  "soft_light",  "hard_light",  "difference", "exclusion",  "subtract",
                                                  "divide",      "hue",         "saturation", "color",      "luminosity"};

// The mix of a pixel below (b) and above (s) in the canvas's values (n colour channels), as the space composites it.
void mix(InkSpace space, unsigned n, std::string_view mode, const double* b, const double* s, double* out) {
    if (space == InkSpace::Multi) {  // (each plate as a CMYK plate: on 1 − ink; the separable modes only)
        for (unsigned c = 0; c < n; ++c) out[c] = 1.0 - separable(mode, 1.0 - b[c], 1.0 - s[c]);
        return;
    }
    if (space == InkSpace::Lab) {
        for (unsigned c = 0; c < 3; ++c) out[c] = separable(mode, b[c], s[c]);
        return;
    }
    if (space == InkSpace::Gray) {
        // (its lightness as an RGB grey (v, v, v): hue, saturation and color keep the grey below, luminosity takes the
        // one above, as the RGB formulas give a grey)
        if (non_separable(mode)) out[0] = mode == "luminosity" ? s[0] : b[0];
        else out[0] = separable(mode, b[0], s[0]);
        return;
    }
    if (non_separable(mode)) {  // (the inverted C, M, Y as RGB; K from below, or from above for luminosity)
        const RGB cb{1.0 - b[0], 1.0 - b[1], 1.0 - b[2]}, cs{1.0 - s[0], 1.0 - s[1], 1.0 - s[2]};
        RGB r;
        if (mode == "hue") r = set_luminosity(set_saturation(cs, saturation(cb)), luminosity(cb));
        else if (mode == "saturation") r = set_luminosity(set_saturation(cb, saturation(cs)), luminosity(cb));
        else if (mode == "color") r = set_luminosity(cs, luminosity(cb));
        else r = set_luminosity(cb, luminosity(cs));
        for (unsigned c = 0; c < 3; ++c) out[c] = 1.0 - r[c];
        out[3] = mode == "luminosity" ? s[3] : b[3];
        return;
    }
    for (unsigned c = 0; c < 4; ++c) out[c] = 1.0 - separable(mode, 1.0 - b[c], 1.0 - s[c]);
}

// Pillow's bicubic filter (support 2, a = -0.5) and its coefficients (Resample.c precompute_coeffs), for the outputs
// [first, last) of an axis of `out` from `in`.
double bicubic(double x) {
    constexpr double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}

// Pillow's lanczos (support 3): a sinc windowed by a sinc.
double sinc(double x) {
    if (x == 0.0) return 1.0;
    x = x * 3.14159265358979323846;
    return std::sin(x) / x;
}
double lanczos(double x) { return -3.0 <= x && x < 3.0 ? sinc(x) * sinc(x / 3) : 0.0; }

struct Coeffs {
    std::vector<int> start, count;
    std::vector<double> weights;
    int ksize = 0;
};

Coeffs coeffs(int in, int out, int first, int last, bool use_lanczos = false) {
    Coeffs k;
    const double scale = static_cast<double>(in) / out;
    const double filterscale = std::max(scale, 1.0);
    const double support = (use_lanczos ? 3.0 : 2.0) * filterscale;
    k.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    const int n = last - first;
    k.start.resize(n);
    k.count.resize(n);
    k.weights.assign(static_cast<std::size_t>(n) * k.ksize, 0.0);
    for (int i = 0; i < n; ++i) {
        const int xx = first + i;
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in) xmax = in;
        xmax -= xmin;
        double ww = 0.0;
        double* w = k.weights.data() + static_cast<std::size_t>(i) * k.ksize;
        for (int x = 0; x < xmax; ++x) {
            w[x] = use_lanczos ? lanczos((x + xmin - center + 0.5) * ss) : bicubic((x + xmin - center + 0.5) * ss);
            ww += w[x];
        }
        for (int x = 0; x < xmax; ++x) {
            if (ww != 0.0) w[x] /= ww;
        }
        k.start[i] = xmin;
        k.count[i] = xmax;
    }
    return k;
}

struct DecodedInk {
    std::weak_ptr<const std::string> bytes;
    const std::string* address = nullptr;
    std::shared_ptr<const core::InkPixels> pixels;
};
std::mutex g_ink_mutex;
std::vector<DecodedInk> g_ink;
constexpr std::size_t kInkCount = 16;
constexpr std::size_t kInkBytes = 768ULL * 1024 * 1024;

// Snap a colour drawn in 8 bits and taken back from its two pictures (0..255 floats, its cover `alpha`) to one the page
// knows exactly (its table, Genko's blacks, white) when it lies within the rounding of the two pictures; else its
// nearest 8-bit colour.
std::array<unsigned char, 3> snapped(const core::PageColor& color, const float* c, double alpha) {
    // (an opaque pixel is taken back exactly: only its own colour; a partly covered one within the rounding of the two
    // pictures, about 1/alpha)
    const double tolerance = alpha >= 1.0 - 1e-9 ? 0.0 : 1.0 / std::max(alpha, 1e-6);
    const auto near = [&](int r, int g, int b) {
        return std::abs(c[0] - r) <= tolerance && std::abs(c[1] - g) <= tolerance && std::abs(c[2] - b) <= tolerance;
    };
    for (const core::InkEntry& entry : color.inks) {
        if (near(entry.rgb[0], entry.rgb[1], entry.rgb[2])) {
            return {static_cast<unsigned char>(entry.rgb[0]), static_cast<unsigned char>(entry.rgb[1]), static_cast<unsigned char>(entry.rgb[2])};
        }
    }
    for (const int v : {0, 10, 15, 20, 255}) {
        if (near(v, v, v)) return {static_cast<unsigned char>(v), static_cast<unsigned char>(v), static_cast<unsigned char>(v)};
    }
    std::array<unsigned char, 3> out{};
    for (unsigned k = 0; k < 3; ++k) out[k] = static_cast<unsigned char>(std::lround(std::clamp(c[k], 0.0f, 255.0f)));
    return out;
}

}  // namespace

// (sample_ink_raster with Pillow's Lanczos or Bicubic; of a 32-bit raster's floats)
InkPicture sample_with(const core::InkPixels& raster, Size full, Box area, bool use_lanczos);
InkPicture sample_floats(const core::InkPixels& raster, Size full, Box area, bool use_lanczos);

double to_canvas(InkSpace space, unsigned channel, double ink) {
    if (space == InkSpace::Gray) return channel == 0 ? 1.0 - ink : ink;
    if (space != InkSpace::Lab || channel >= 3) return ink;
    if (channel == 0) return ink / 100.0;
    return (ink + 128.0) / 255.0;
}

double from_canvas(InkSpace space, unsigned channel, double value) {
    if (space == InkSpace::Gray) return channel == 0 ? 1.0 - value : value;
    if (space != InkSpace::Lab || channel >= 3) return value;
    if (channel == 0) return value * 100.0;
    return value * 255.0 - 128.0;
}

struct InkCanvas::Impl {
    // (the budget outlives the reservation and what it pays for)
    ImageAllocationBudget budget{512ULL * 1024 * 1024};
    ImagingMemoryInstance reservation{};
    core::PageColor color;
    Size size;
    unsigned n = 4;       // colour channels
    unsigned stride = 6;  // colour, alpha, the alpha of the layer before
    std::vector<double> px;
    bool previous = false;
    bool hdr = false;  // (a grey page of 32 bits: lightness past 0..1 is kept)

    Impl(const core::PageColor& c, Size s)
        : color(c), size(s), n(c.planes()), stride(n + 2), hdr(c.space == InkSpace::Gray && c.depth == 32) {
        if (s.width <= 0 || s.height <= 0) throw core::Error("value", "invalid ink canvas dimensions");
        const auto count = std::uint64_t(s.width) * std::uint64_t(s.height);
        if (count > (512ULL * 1024 * 1024) / (stride * sizeof(double))) throw core::Error("image_too_large", "ink canvas budget exceeded");
        if (!genko_imaging_budget_reserve(&reservation, count * stride * sizeof(double))) detail::throw_imaging_error();
        px.assign(static_cast<std::size_t>(count) * stride, 0.0);
    }
    ~Impl() { genko_imaging_budget_release(&reservation); }

    // A pixel laid over (canvas values `src`, its alpha): straight alpha, the blend's mix where both are there.
    void over(std::size_t i, const double* src, double src_alpha, double opacity, bool clip, std::string_view mode, bool layer) {
        double* dst = px.data() + i * stride;
        const double a = src_alpha * std::clamp(opacity, 0.0, 1.0) * (clip && previous ? dst[n + 1] : 1.0);
        if (a == 0) {
            if (layer) dst[n + 1] = src_alpha;
            return;
        }
        const bool normal = mode.empty() || mode == "normal";
        const double da = dst[n];
        if ((normal && a == 1) || da == 0) {
            for (unsigned c = 0; c < n; ++c) dst[c] = src[c];
            dst[n] = a;
            if (layer) dst[n + 1] = src_alpha;
            return;
        }
        const double alpha = a + da * (1 - a);
        std::array<double, core::kPlatesMax> mixed{};
        if (!normal) mix(color.space, n, mode, dst, src, mixed.data());
        for (unsigned c = 0; c < n; ++c) {
            const double next = normal ? (src[c] * a + dst[c] * da * (1 - a)) / alpha
                                       : (dst[c] * da * (1 - a) + src[c] * a * (1 - da) + mixed[c] * a * da) / alpha;
            // (a grey page of 32 bits keeps an HDR lightness, as a precise RGB page does)
            dst[c] = hdr ? next : std::clamp(next, 0.0, 1.0);
            if (!std::isfinite(dst[c])) throw core::Error("value", "the ink composite exceeds the finite working range");
        }
        dst[n] = alpha;
        if (layer) dst[n + 1] = src_alpha;
    }
};

InkCanvas::InkCanvas(const core::PageColor& color, const Image& paper) : impl_(std::make_unique<Impl>(color, paper.size())) {
    const InkPicture inks = ink_picture(paper.mode() == "RGBA" ? paper : paper.convert("RGBA"), color);
    const unsigned n = impl_->n;
    for (std::size_t i = 0; i < impl_->px.size() / impl_->stride; ++i) {
        double* p = impl_->px.data() + i * impl_->stride;
        for (unsigned c = 0; c < n; ++c) p[c] = inks.values[i * (n + 1) + c];
        p[n] = 1.0;  // (the paper is opaque)
        p[n + 1] = 0.0;
    }
}

InkCanvas::InkCanvas(const core::PageColor& color, Size size) : impl_(std::make_unique<Impl>(color, size)) {
    if (color.space != InkSpace::Lab && color.space != InkSpace::Gray) return;  // (CMYK: no ink is 0, the alpha 0)
    for (std::size_t i = 0; i < impl_->px.size() / impl_->stride; ++i) {
        double* p = impl_->px.data() + i * impl_->stride;
        if (color.space == InkSpace::Gray) {
            p[0] = 1.0;  // (Gray: white, the paper's)
            continue;
        }
        p[0] = to_canvas(InkSpace::Lab, 0, 100.0);  // (Lab: L 100, a and b 0 — the paper's)
        p[1] = p[2] = to_canvas(InkSpace::Lab, 1, 0.0);
    }
}

InkCanvas::~InkCanvas() = default;

const core::PageColor& InkCanvas::color() const { return impl_->color; }
Size InkCanvas::size() const { return impl_->size; }
unsigned InkCanvas::channels() const { return impl_->n; }

void InkCanvas::styled(const Box& part, bool clip, const std::vector<float>& own,
                       const std::function<void(std::vector<float>&, const std::vector<float>&)>& run) {
    const Size s = impl_->size;
    if (part.x0 < 0 || part.y0 < 0 || part.x1 > s.width || part.y1 > s.height || part.width() <= 0 || part.height() <= 0)
        throw core::Error("value", "the ink canvas part does not match");
    const std::size_t count = std::size_t(part.width()) * part.height();
    if (own.size() != count) throw core::Error("value", "the styled ink layer does not match the canvas");
    const unsigned n = impl_->n, st = impl_->stride;
    std::vector<float> values(count * (n + 1)), weight(count, 1.0f);
    for (int y = 0; y < part.height(); ++y) {
        for (int x = 0; x < part.width(); ++x) {
            const std::size_t i = std::size_t(y) * part.width() + x;
            const double* p = impl_->px.data() + (std::size_t(y + part.y0) * s.width + x + part.x0) * st;
            for (unsigned c = 0; c <= n; ++c) values[i * (n + 1) + c] = static_cast<float>(p[c]);
            if (clip && impl_->previous) weight[i] = static_cast<float>(p[n + 1]);
        }
    }
    run(values, weight);
    for (int y = 0; y < part.height(); ++y) {
        for (int x = 0; x < part.width(); ++x) {
            const std::size_t i = std::size_t(y) * part.width() + x;
            double* p = impl_->px.data() + (std::size_t(y + part.y0) * s.width + x + part.x0) * st;
            for (unsigned c = 0; c <= n; ++c) {
                const double v = values[i * (n + 1) + c];
                if (!std::isfinite(v)) throw core::Error("value", "the ink composite exceeds the finite working range");
                p[c] = c < n && !impl_->hdr ? std::clamp(v, 0.0, 1.0) : v;
            }
            p[n + 1] = own[i];
        }
    }
}

void InkCanvas::styled_done() { impl_->previous = true; }

void ink_blend(core::InkSpace space, unsigned n, std::string_view mode, const double* below, const double* above, double* out) {
    if (mode.empty() || mode == "normal") {
        for (unsigned c = 0; c < n; ++c) out[c] = above[c];
        return;
    }
    mix(space, n, mode, below, above, out);
}

bool InkCanvas::supports_blend(std::string_view mode) const {
    if (impl_->color.space == InkSpace::Lab) return core::lab_blend(mode);
    if (impl_->color.space == InkSpace::Multi && !core::multi_blend(mode)) return false;
    return mode.empty() || std::find(kModes.begin(), kModes.end(), mode) != kModes.end();
}

void InkCanvas::blend(const InkPicture& picture, double opacity, bool clip, std::string_view mode, const Image* mask) {
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "the blend " + std::string(mode) + " on a " + std::string(core::to_string(impl_->color.space)) + " page");
    if (picture.size != impl_->size || picture.channels != impl_->n) throw core::Error("value", "the ink picture does not match the canvas");
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "the layer mask does not match the canvas");
    const unsigned n = impl_->n;
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < impl_->size.width; ++x) {
            const std::size_t i = std::size_t(y) * impl_->size.width + x;
            const double* p = picture.values.data() + i * (n + 1);
            double a = p[n];
            if (shown) a *= shown[x] / 255.0;
            impl_->over(i, p, a, opacity, clip, mode, true);
        }
    }
    impl_->previous = true;
}

void InkCanvas::blend_rgba(const Image& rgba, double opacity, bool clip, std::string_view mode) {
    if (rgba.size() != impl_->size) throw core::Error("value", "the picture does not match the ink canvas");
    blend(ink_picture(rgba, impl_->color), opacity, clip, mode);
}

void InkCanvas::draw_8bit(const std::function<void(Image&)>& draw) {
    Image black = Image::create("RGB", impl_->size, Ink{0, 0, 0});
    Image white = Image::create("RGB", impl_->size, Ink{255, 255, 255});
    draw(black);
    draw(white);
    for (const Image* out : {&black, &white}) {
        if (out->size() != impl_->size) throw core::Error("value", "8-bit drawing does not match the ink canvas");
    }
    const Image b = black.mode() == "RGB" ? black : black.convert("RGB");
    const Image w = white.mode() == "RGB" ? white : white.convert("RGB");
    const unsigned n = impl_->n;
    std::vector<unsigned char> colours;
    std::vector<std::size_t> where;
    std::vector<double> alphas;
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* rb = reinterpret_cast<const unsigned char*>(b.raw()->image[y]);
        const auto* rw = reinterpret_cast<const unsigned char*>(w.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) {
            const unsigned char* pb = rb + x * 4;
            const unsigned char* pw = rw + x * 4;
            if (pb[0] == 0 && pb[1] == 0 && pb[2] == 0 && pw[0] == 255 && pw[1] == 255 && pw[2] == 255) continue;  // (not drawn)
            double alpha = 0;
            for (unsigned c = 0; c < 3; ++c) alpha += 1.0 - (static_cast<double>(pw[c]) - pb[c]) / 255.0;
            alpha = std::clamp(alpha / 3.0, 0.0, 1.0);
            if (alpha <= 0) continue;
            float c3[3];
            for (unsigned c = 0; c < 3; ++c) c3[c] = static_cast<float>(pb[c] / alpha);
            const auto rgb = snapped(impl_->color, c3, alpha);
            colours.insert(colours.end(), rgb.begin(), rgb.end());
            where.push_back(std::size_t(y) * impl_->size.width + x);
            alphas.push_back(alpha);
        }
    }
    if (where.empty()) return;
    std::vector<double> inks(where.size() * n);
    ink::inks_of(impl_->color, colours.data(), nullptr, inks.data(), where.size());
    std::vector<double> canvas(n);
    for (std::size_t j = 0; j < where.size(); ++j) {
        for (unsigned c = 0; c < n; ++c) canvas[c] = to_canvas(impl_->color.space, c, inks[j * n + c]);
        impl_->over(where[j], canvas.data(), alphas[j], 1.0, false, "normal", false);
    }
}

void InkCanvas::recolour(const core::AdjustFunction& change, double opacity, bool clip, const Image* mask, std::string_view mode, Point at) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "the correction's mask does not match the canvas");
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "the blend " + std::string(mode) + " on a " + std::string(core::to_string(impl_->color.space)) + " page");
    const bool normal = mode.empty() || mode == "normal";
    const unsigned n = impl_->n;
    const core::InkSpace space = impl_->color.space;
    const ink::Rules rules(impl_->color);  // (the table looked up once for the whole canvas)
    std::vector<double> inks, fixed;
    std::vector<std::size_t> where;
    std::vector<int> xs;
    std::vector<double> amounts;
    std::array<double, core::kPlatesMax> now{}, mixed{};
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        inks.clear();
        where.clear();
        xs.clear();
        amounts.clear();
        for (int x = 0; x < impl_->size.width; ++x) {
            const std::size_t i = std::size_t(y) * impl_->size.width + x;
            const double* p = impl_->px.data() + i * impl_->stride;
            double amount = std::clamp(opacity, 0.0, 1.0) * (clip && impl_->previous ? p[n + 1] : 1.0);
            if (shown) amount *= shown[x] / 255.0;
            if (!(amount > 0)) continue;
            for (unsigned c = 0; c < n; ++c) inks.push_back(from_canvas(space, c, p[c]));
            where.push_back(i);
            xs.push_back(x);
            amounts.push_back(amount);
        }
        if (where.empty()) continue;
        fixed.resize(inks.size());
        std::size_t k = 0;  // (ink::Rules::correct asks for each pixel once, in their order)
        rules.correct(inks.data(), fixed.data(), where.size(), [&](const std::array<double, 3>& rgb) {
            const int x = xs[std::min(k++, xs.size() - 1)];
            return change.apply(rgb, at.x + x, at.y + y);
        });
        for (std::size_t j = 0; j < where.size(); ++j) {
            double* p = impl_->px.data() + where[j] * impl_->stride;
            for (unsigned c = 0; c < n; ++c) now[c] = to_canvas(space, c, fixed[j * n + c]);
            // (PC2: its native.blend — the changed inks over those below by the page's formula)
            if (!normal) mix(space, n, mode, p, now.data(), mixed.data());
            for (unsigned c = 0; c < n; ++c) {
                const double next = p[c] + ((normal ? now[c] : mixed[c]) - p[c]) * amounts[j];
                p[c] = impl_->hdr ? next : std::clamp(next, 0.0, 1.0);
            }
        }
    }
}

void InkCanvas::adjust_planes(const core::InkRecords& records, double opacity, bool clip, const Image* mask, std::string_view mode) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "the correction's mask does not match the canvas");
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "the blend " + std::string(mode) + " on a " + std::string(core::to_string(impl_->color.space)) + " page");
    const core::InkSpace space = impl_->color.space;
    if (records.space != space || space == core::InkSpace::Multi) throw core::Error("value", "the correction's records are not the page's plates");
    const bool normal = mode.empty() || mode == "normal";
    const unsigned n = impl_->n;
    // (each plate's tone: CMYK its brightness 1 − ink; Lab its normalized value; grey its lightness — the canvas's values)
    const auto tone = [space](double value) { return space == core::InkSpace::Cmyk ? 1.0 - value : value; };
    std::array<double, core::kPlatesMax> now{}, mixed{};
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < impl_->size.width; ++x) {
            double* p = impl_->px.data() + (std::size_t(y) * impl_->size.width + x) * impl_->stride;
            double amount = std::clamp(opacity, 0.0, 1.0) * (clip && impl_->previous ? p[n + 1] : 1.0);
            if (shown) amount *= shown[x] / 255.0;
            if (!(amount > 0)) continue;
            for (unsigned c = 0; c < n; ++c) {
                const bool kept = c >= records.plates.size() || !records.plates[c];
                now[c] = kept ? p[c] : tone(records.plates[c](tone(p[c])));
            }
            if (!normal) mix(space, n, mode, p, now.data(), mixed.data());
            for (unsigned c = 0; c < n; ++c) {
                const double next = p[c] + ((normal ? now[c] : mixed[c]) - p[c]) * amount;
                p[c] = impl_->hdr ? next : std::clamp(next, 0.0, 1.0);
                if (!std::isfinite(p[c])) throw core::Error("value", "the ink composite exceeds the finite working range");
            }
        }
    }
}

void InkCanvas::correct(const InkPicture& changed, double opacity, bool clip, const Image* mask, std::string_view mode) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "the correction's mask does not match the canvas");
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "the blend " + std::string(mode) + " on a " + std::string(core::to_string(impl_->color.space)) + " page");
    if (changed.size != impl_->size || changed.channels != impl_->n) throw core::Error("value", "the corrected inks do not match the canvas");
    const bool normal = mode.empty() || mode == "normal";
    const unsigned n = impl_->n;
    std::array<double, core::kPlatesMax> mixed{};
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < impl_->size.width; ++x) {
            const std::size_t i = std::size_t(y) * impl_->size.width + x;
            double* p = impl_->px.data() + i * impl_->stride;
            double amount = std::clamp(opacity, 0.0, 1.0) * (clip && impl_->previous ? p[n + 1] : 1.0);
            if (shown) amount *= shown[x] / 255.0;
            if (!(amount > 0)) continue;
            const double* now = changed.values.data() + i * (n + 1);
            if (!normal) mix(impl_->color.space, n, mode, p, now, mixed.data());
            for (unsigned c = 0; c < n; ++c) {
                const double next = p[c] + ((normal ? now[c] : mixed[c]) - p[c]) * amount;
                p[c] = impl_->hdr ? next : std::clamp(next, 0.0, 1.0);
                if (!std::isfinite(p[c])) throw core::Error("value", "the ink composite exceeds the finite working range");
            }
            p[n] = std::clamp(p[n] + (now[n] - p[n]) * amount, 0.0, 1.0);  // (the 8-bit page's composite mixes the alpha too)
        }
    }
}

void InkCanvas::forget_clip() { impl_->previous = false; }

InkPicture InkCanvas::picture() const {
    const unsigned n = impl_->n;
    const std::size_t count = impl_->px.size() / impl_->stride;
    InkPicture out{impl_->size, n, {}};
    out.values.resize(count * (n + 1));
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned c = 0; c <= n; ++c) out.values[i * (n + 1) + c] = impl_->px[i * impl_->stride + c];
    return out;
}

void InkCanvas::mix_back(const InkPicture& before, double opacity, const Image* mask) {
    if (before.size != impl_->size || before.channels != impl_->n) throw core::Error("value", "the ink picture does not match the canvas");
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "the layer mask does not match the canvas");
    const unsigned n = impl_->n;
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < impl_->size.width; ++x) {
            const std::size_t i = std::size_t(y) * impl_->size.width + x;
            const double k = std::clamp(opacity, 0.0, 1.0) * (shown ? shown[x] / 255.0 : 1.0);
            if (k >= 1) continue;
            double* p = impl_->px.data() + i * impl_->stride;
            const double* b = before.values.data() + i * (n + 1);
            const double alpha = b[n] + (p[n] - b[n]) * k;
            for (unsigned c = 0; c < n; ++c) {
                const double premultiplied = b[c] * b[n] + (p[c] * p[n] - b[c] * b[n]) * k;
                p[c] = alpha > 0 ? premultiplied / alpha : 0.0;
            }
            p[n] = std::clamp(alpha, 0.0, 1.0);
        }
    }
}

void InkCanvas::clip_to(const Image& alpha) {
    if (alpha.mode() != "L" || alpha.size() != impl_->size) throw core::Error("value", "the clipping alpha does not match the canvas");
    const unsigned n = impl_->n;
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* a = reinterpret_cast<const unsigned char*>(alpha.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) impl_->px[(std::size_t(y) * impl_->size.width + x) * impl_->stride + n + 1] = a[x] / 255.0;
    }
    impl_->previous = true;
}

// color-model@1 (PC1d): a bitmap page's lightness as it prints, 0 or 1 at 50% (per pixel: a part of the page drawn the
// same as the whole).
double bitmap_value(double lightness) { return lightness >= 0.5 ? 1.0 : 0.0; }

Image InkCanvas::display() const {
    const unsigned n = impl_->n;
    const std::size_t count = impl_->px.size() / impl_->stride;
    const core::PageColor& color = impl_->color;
    std::vector<double> inks(count * n);
    std::vector<float> srgb(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < n; ++c) {
            const double v = impl_->px[i * impl_->stride + c];
            inks[i * n + c] = from_canvas(color.space, c, color.bitmap ? bitmap_value(v) : v);
        }
    }
    if (!color.duotone.empty()) {  // (a duotone page: its grey printed in its inks, PhotoCraft's Duotone::render)
        for (std::size_t i = 0; i < count; ++i) {
            const std::array<double, 3> shown = core::duotone_print(color.duotone, impl_->px[i * impl_->stride]);
            for (unsigned c = 0; c < 3; ++c) srgb[i * 3 + c] = static_cast<float>(shown[c]);
        }
    } else {
        ink::to_srgb(color, inks.data(), srgb.data(), count);
    }
    Image out = Image::create("RGB", impl_->size, Ink{255, 255, 255});
    for (int y = 0; y < impl_->size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) {
            const std::size_t i = std::size_t(y) * impl_->size.width + x;
            const double a = impl_->px[i * impl_->stride + n];
            for (unsigned c = 0; c < 3; ++c) {
                const double v = srgb[i * 3 + c] * a + (1 - a);  // (over white where the page is not opaque)
                row[x * 4 + c] = static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
            }
        }
    }
    return out;
}

core::InkPixels InkCanvas::ink_pixels(int depth, bool alpha) const {
    core::InkPixels out;
    out.space = impl_->color.space;
    out.depth = depth;
    out.width = static_cast<std::uint32_t>(impl_->size.width);
    out.height = static_cast<std::uint32_t>(impl_->size.height);
    const unsigned n = impl_->n;
    out.planes = out.space == InkSpace::Multi ? n : 0;
    // (a bitmap page's lightness as it prints: 0 or 1, as display() shows it)
    const auto value = [&](std::size_t i, unsigned c) {
        const double v = impl_->px[i * impl_->stride + c];
        return impl_->color.bitmap && c < n ? bitmap_value(v) : v;
    };
    if (depth == 32) {  // (floats: the canvas's values as they are)
        out.floats.resize(out.pixel_count() * (n + 1));
        for (std::size_t i = 0; i < out.pixel_count(); ++i) {
            for (unsigned c = 0; c < n; ++c) out.set(i, c, value(i, c));
            out.set(i, n, alpha ? impl_->px[i * impl_->stride + n] : 1.0);
        }
        return out;
    }
    const double most = depth == 16 ? 65535.0 : 255.0;
    out.samples.resize(out.pixel_count() * (n + 1));
    for (std::size_t i = 0; i < out.pixel_count(); ++i) {
        for (unsigned c = 0; c < n; ++c) {
            out.samples[i * (n + 1) + c] = static_cast<std::uint16_t>(std::lround(std::clamp(value(i, c), 0.0, 1.0) * most));
        }
        out.samples[i * (n + 1) + n] =
            alpha ? static_cast<std::uint16_t>(std::lround(std::clamp(impl_->px[i * impl_->stride + n], 0.0, 1.0) * most)) : static_cast<std::uint16_t>(most);
    }
    return out;
}

InkPicture ink_picture(const Image& rgba_in, const core::PageColor& color) {
    const Image rgba = rgba_in.mode() == "RGBA" ? rgba_in : rgba_in.convert("RGBA");
    InkPicture out;
    out.size = rgba.size();
    out.channels = color.planes();
    const unsigned n = out.channels;
    const std::size_t count = static_cast<std::size_t>(rgba.width()) * rgba.height();
    std::vector<unsigned char> rgb(count * 3), alpha(count);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        for (int x = 0; x < rgba.width(); ++x) {
            const std::size_t i = std::size_t(y) * rgba.width() + x;
            rgb[i * 3] = row[x * 4];
            rgb[i * 3 + 1] = row[x * 4 + 1];
            rgb[i * 3 + 2] = row[x * 4 + 2];
            alpha[i] = row[x * 4 + 3];
        }
    }
    std::vector<double> inks(count * n, 0.0);
    ink::inks_of(color, rgb.data(), alpha.data(), inks.data(), count);
    out.values.assign(count * (n + 1), 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        if (alpha[i] == 0) continue;
        for (unsigned c = 0; c < n; ++c) out.values[i * (n + 1) + c] = to_canvas(color.space, c, inks[i * n + c]);
        out.values[i * (n + 1) + n] = alpha[i] / 255.0;
    }
    return out;
}

void ink_over(InkPicture& below, const InkPicture& above) {
    if (below.size != above.size || below.channels != above.channels) throw core::Error("value", "the ink pictures do not match");
    const unsigned n = below.channels, all = n + 1;
    for (std::size_t i = 0; i < below.values.size() / all; ++i) {
        double* d = below.values.data() + i * all;
        const double* s = above.values.data() + i * all;
        const double a = s[n];
        if (a <= 0) continue;
        const double da = d[n];
        const double alpha = a + da * (1 - a);
        for (unsigned c = 0; c < n; ++c) d[c] = alpha > 0 ? (s[c] * a + d[c] * da * (1 - a)) / alpha : 0.0;
        d[n] = alpha;
    }
}

Image ink_picture_rgba(const InkPicture& picture, const core::PageColor& color) {
    const unsigned n = picture.channels, all = n + 1;
    const std::size_t count = static_cast<std::size_t>(picture.size.width) * picture.size.height;
    std::vector<double> inks(count * n);
    std::vector<float> srgb(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(color.space, c, picture.values[i * all + c]);
    }
    ink::to_srgb(color, inks.data(), srgb.data(), count);
    Image out = Image::create("RGBA", picture.size, Ink{0, 0, 0, 0});
    for (int y = 0; y < picture.size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < picture.size.width; ++x) {
            const std::size_t i = std::size_t(y) * picture.size.width + x;
            for (unsigned c = 0; c < 3; ++c) row[x * 4 + c] = static_cast<unsigned char>(std::lround(srgb[i * 3 + c] * 255.0f));
            row[x * 4 + 3] = static_cast<unsigned char>(std::lround(std::clamp(picture.values[i * all + n], 0.0, 1.0) * 255.0));
        }
    }
    return out;
}

Image ink_alpha_rgba(const InkPicture& picture) {
    const unsigned all = picture.channels + 1;
    Image out = Image::create("RGBA", picture.size, Ink{0, 0, 0, 0});
    for (int y = 0; y < picture.size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < picture.size.width; ++x) {
            const std::size_t i = std::size_t(y) * picture.size.width + x;
            row[x * 4 + 3] = static_cast<unsigned char>(std::lround(std::clamp(picture.values[i * all + picture.channels], 0.0, 1.0) * 255.0));
        }
    }
    return out;
}

core::InkPixels crop_inks(const core::InkPixels& pixels, Box box) {
    core::InkPixels out;
    out.space = pixels.space;
    out.depth = pixels.depth;
    out.planes = pixels.planes;
    out.width = static_cast<std::uint32_t>(box.width());
    out.height = static_cast<std::uint32_t>(box.height());
    const unsigned all = pixels.channels();
    if (pixels.is_float()) out.floats.resize(out.pixel_count() * all);
    else out.samples.resize(out.pixel_count() * all);
    for (int y = 0; y < box.height(); ++y) {
        const std::size_t from = (std::size_t(box.y0 + y) * pixels.width + box.x0) * all, to = std::size_t(y) * out.width * all;
        if (pixels.is_float()) std::copy_n(pixels.floats.data() + from, std::size_t(box.width()) * all, out.floats.data() + to);
        else std::copy_n(pixels.samples.data() + from, std::size_t(box.width()) * all, out.samples.data() + to);
    }
    return out;
}

void paste_inks(core::InkPixels& into, const core::InkPixels& part, Point at) {
    const unsigned all = into.channels();
    const int x0 = std::max(0, at.x), y0 = std::max(0, at.y);
    const int x1 = std::min<int>(static_cast<int>(into.width), at.x + static_cast<int>(part.width));
    const int y1 = std::min<int>(static_cast<int>(into.height), at.y + static_cast<int>(part.height));
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) {
        const std::size_t from = (std::size_t(y - at.y) * part.width + (x0 - at.x)) * all, to = (std::size_t(y) * into.width + x0) * all;
        if (into.is_float()) std::copy_n(part.floats.data() + from, std::size_t(x1 - x0) * all, into.floats.data() + to);
        else std::copy_n(part.samples.data() + from, std::size_t(x1 - x0) * all, into.samples.data() + to);
    }
}

core::InkPixels resize_inks(const core::InkPixels& pixels, Size size, Resample resample) {
    if (static_cast<int>(pixels.width) == size.width && static_cast<int>(pixels.height) == size.height) return pixels;
    const bool lanczos = resample == Resample::Lanczos;
    const InkPicture picture = sample_with(pixels, size, Box{0, 0, size.width, size.height}, lanczos);
    core::InkPixels out;
    out.space = pixels.space;
    out.depth = pixels.depth;
    out.planes = pixels.planes;
    out.width = static_cast<std::uint32_t>(size.width);
    out.height = static_cast<std::uint32_t>(size.height);
    const unsigned all = pixels.channels();
    if (pixels.is_float()) {
        out.floats.resize(out.pixel_count() * all);
        for (std::size_t i = 0; i < out.pixel_count(); ++i)
            for (unsigned c = 0; c < all; ++c) out.set(i, c, picture.values[i * all + c]);
        return out;
    }
    const double most = pixels.max();
    out.samples.resize(out.pixel_count() * all);
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        out.samples[i] = static_cast<std::uint16_t>(std::lround(std::clamp(picture.values[i], 0.0, 1.0) * most));
    }
    return out;
}

std::shared_ptr<const core::InkPixels> decoded_ink(const core::Bytes& bytes) {
    {
        std::lock_guard lock(g_ink_mutex);
        for (const auto& d : g_ink) {
            if (d.address == bytes.get() && !d.bytes.expired()) return d.pixels;
        }
    }
    auto pixels = std::make_shared<const core::InkPixels>(core::decode_ink_raster(*bytes));
    std::lock_guard lock(g_ink_mutex);
    std::erase_if(g_ink, [](const DecodedInk& d) { return d.bytes.expired(); });
    g_ink.push_back(DecodedInk{bytes, bytes.get(), pixels});
    const auto bytes_of = [](const core::InkPixels& p) { return p.samples.size() * sizeof(std::uint16_t) + p.floats.size() * sizeof(float); };
    std::size_t held = 0;
    for (const auto& d : g_ink) held += bytes_of(*d.pixels);
    while (g_ink.size() > 1 && (g_ink.size() > kInkCount || held > kInkBytes)) {
        held -= bytes_of(*g_ink.front().pixels);
        g_ink.erase(g_ink.begin());
    }
    return pixels;
}

InkPicture sample_ink_raster(const core::InkPixels& raster, Size full, Box area) { return sample_with(raster, full, area, false); }

namespace {

// sample_with over a raster's values read by `read` (the flat index of a sample: its normalized value); `hdr`: a grey
// page's 32-bit lightness is not clamped to 0..1.
template <class Read>
InkPicture sample_values(const core::InkPixels& raster, Size full, Box area, bool use_lanczos, bool hdr, Read read) {
    InkPicture out;
    out.size = Size{area.width(), area.height()};
    out.channels = raster.colours();
    const unsigned n = out.channels, all = n + 1;
    out.values.assign(static_cast<std::size_t>(out.size.width) * out.size.height * all, 0.0);
    const int rw = static_cast<int>(raster.width), rh = static_cast<int>(raster.height);
    if (rw == full.width && rh == full.height) {  // (its own size: no resampling)
        for (int y = area.y0; y < area.y1; ++y) {
            for (int x = area.x0; x < area.x1; ++x) {
                const std::size_t s = (std::size_t(y) * rw + x) * all;
                double* d = out.values.data() + (std::size_t(y - area.y0) * out.size.width + (x - area.x0)) * all;
                for (unsigned c = 0; c < all; ++c) d[c] = read(s + c);
            }
        }
        return out;
    }
    // premultiplied, horizontally into the rows the vertical pass needs, then vertically (Pillow's two passes)
    const Coeffs ky = coeffs(rh, full.height, area.y0, area.y1, use_lanczos);
    const Coeffs kx = coeffs(rw, full.width, area.x0, area.x1, use_lanczos);
    int row0 = rh, row1 = 0;
    for (std::size_t i = 0; i < ky.start.size(); ++i) {
        row0 = std::min(row0, ky.start[i]);
        row1 = std::max(row1, ky.start[i] + ky.count[i]);
    }
    if (row1 <= row0) return out;
    const int ow = out.size.width;
    std::vector<double> horizontal(static_cast<std::size_t>(row1 - row0) * ow * all, 0.0);
    std::vector<double> premultiplied(static_cast<std::size_t>(rw) * all);
    for (int y = row0; y < row1; ++y) {
        const std::size_t s = std::size_t(y) * rw * all;
        for (int x = 0; x < rw; ++x) {
            const double a = read(s + x * all + n);
            for (unsigned c = 0; c < n; ++c) premultiplied[x * all + c] = read(s + x * all + c) * a;
            premultiplied[x * all + n] = a;
        }
        double* h = horizontal.data() + std::size_t(y - row0) * ow * all;
        for (int i = 0; i < ow; ++i) {
            const double* w = kx.weights.data() + static_cast<std::size_t>(i) * kx.ksize;
            for (int k = 0; k < kx.count[i]; ++k) {
                const double* p = premultiplied.data() + std::size_t(kx.start[i] + k) * all;
                for (unsigned c = 0; c < all; ++c) h[i * all + c] += p[c] * w[k];
            }
        }
    }
    for (int j = 0; j < out.size.height; ++j) {
        const double* w = ky.weights.data() + static_cast<std::size_t>(j) * ky.ksize;
        for (int i = 0; i < ow; ++i) {
            std::array<double, core::kPlatesMax + 1> v{};
            for (int k = 0; k < ky.count[j]; ++k) {
                const double* h = horizontal.data() + (std::size_t(ky.start[j] + k - row0) * ow + i) * all;
                for (unsigned c = 0; c < all; ++c) v[c] += h[c] * w[k];
            }
            const double a = std::clamp(v[n], 0.0, 1.0);
            double* d = out.values.data() + (std::size_t(j) * ow + i) * all;
            d[n] = a;
            for (unsigned c = 0; c < n; ++c) d[c] = a > 0 ? (hdr ? v[c] / a : std::clamp(v[c] / a, 0.0, 1.0)) : 0.0;
        }
    }
    return out;
}

}  // namespace

InkPicture sample_with(const core::InkPixels& raster, Size full, Box area, bool use_lanczos) {
    if (raster.is_float()) return sample_floats(raster, full, area, use_lanczos);
    const double most = raster.max();
    const std::uint16_t* s = raster.samples.data();
    return sample_values(raster, full, area, use_lanczos, false, [s, most](std::size_t at) { return s[at] / most; });
}

InkPicture sample_floats(const core::InkPixels& raster, Size full, Box area, bool use_lanczos) {
    const float* f = raster.floats.data();
    return sample_values(raster, full, area, use_lanczos, core::traits(raster.space).hdr,
                         [f](std::size_t at) { return static_cast<double>(f[at]); });
}

}  // namespace genko::render