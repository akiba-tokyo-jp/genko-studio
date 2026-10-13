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

// The mix of a pixel below (b) and above (s) in the canvas's values, as the space composites it.
void mix(InkSpace space, std::string_view mode, const double* b, const double* s, double* out) {
    if (space == InkSpace::Lab) {
        for (unsigned c = 0; c < 3; ++c) out[c] = separable(mode, b[c], s[c]);
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

struct Coeffs {
    std::vector<int> start, count;
    std::vector<double> weights;
    int ksize = 0;
};

Coeffs coeffs(int in, int out, int first, int last) {
    Coeffs k;
    const double scale = static_cast<double>(in) / out;
    const double filterscale = std::max(scale, 1.0);
    const double support = 2.0 * filterscale;
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
            w[x] = bicubic((x + xmin - center + 0.5) * ss);
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
    const double tolerance = 1.0 / std::max(alpha, 1e-6) + 0.5;
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

double to_canvas(InkSpace space, unsigned channel, double ink) {
    if (space == InkSpace::Cmyk || channel >= 3) return ink;
    if (channel == 0) return ink / 100.0;
    return (ink + 128.0) / 255.0;
}

double from_canvas(InkSpace space, unsigned channel, double value) {
    if (space == InkSpace::Cmyk || channel >= 3) return value;
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

    Impl(const core::PageColor& c, Size s) : color(c), size(s), n(core::colour_channels(c.space)), stride(n + 2) {
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
        double mixed[4] = {};
        if (!normal) mix(color.space, mode, dst, src, mixed);
        for (unsigned c = 0; c < n; ++c) {
            const double next = normal ? (src[c] * a + dst[c] * da * (1 - a)) / alpha
                                       : (dst[c] * da * (1 - a) + src[c] * a * (1 - da) + mixed[c] * a * da) / alpha;
            dst[c] = std::clamp(next, 0.0, 1.0);
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

InkCanvas::~InkCanvas() = default;

const core::PageColor& InkCanvas::color() const { return impl_->color; }
Size InkCanvas::size() const { return impl_->size; }
unsigned InkCanvas::channels() const { return impl_->n; }

bool InkCanvas::supports_blend(std::string_view mode) const {
    if (impl_->color.space == InkSpace::Lab) return core::lab_blend(mode);
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

void InkCanvas::forget_clip() { impl_->previous = false; }

Image InkCanvas::display() const {
    const unsigned n = impl_->n;
    const std::size_t count = impl_->px.size() / impl_->stride;
    std::vector<double> inks(count * n);
    std::vector<float> srgb(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(impl_->color.space, c, impl_->px[i * impl_->stride + c]);
    }
    ink::to_srgb(impl_->color, inks.data(), srgb.data(), count);
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

core::InkPixels InkCanvas::ink_pixels(int depth) const {
    core::InkPixels out;
    out.space = impl_->color.space;
    out.depth = depth;
    out.width = static_cast<std::uint32_t>(impl_->size.width);
    out.height = static_cast<std::uint32_t>(impl_->size.height);
    const unsigned n = impl_->n;
    const double most = depth == 16 ? 65535.0 : 255.0;
    out.samples.resize(out.pixel_count() * (n + 1));
    for (std::size_t i = 0; i < out.pixel_count(); ++i) {
        for (unsigned c = 0; c < n; ++c) {
            out.samples[i * (n + 1) + c] = static_cast<std::uint16_t>(std::lround(std::clamp(impl_->px[i * impl_->stride + c], 0.0, 1.0) * most));
        }
        out.samples[i * (n + 1) + n] = static_cast<std::uint16_t>(most);
    }
    return out;
}

InkPicture ink_picture(const Image& rgba_in, const core::PageColor& color) {
    const Image rgba = rgba_in.mode() == "RGBA" ? rgba_in : rgba_in.convert("RGBA");
    InkPicture out;
    out.size = rgba.size();
    out.channels = core::colour_channels(color.space);
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
    out.width = static_cast<std::uint32_t>(box.width());
    out.height = static_cast<std::uint32_t>(box.height());
    const unsigned all = pixels.channels();
    out.samples.resize(out.pixel_count() * all);
    for (int y = 0; y < box.height(); ++y) {
        const std::uint16_t* src = pixels.samples.data() + (std::size_t(box.y0 + y) * pixels.width + box.x0) * all;
        std::copy_n(src, std::size_t(box.width()) * all, out.samples.data() + std::size_t(y) * out.width * all);
    }
    return out;
}

void paste_inks(core::InkPixels& into, const core::InkPixels& part, Point at) {
    const unsigned all = into.channels();
    for (std::uint32_t y = 0; y < part.height; ++y) {
        const std::uint16_t* src = part.samples.data() + std::size_t(y) * part.width * all;
        std::copy_n(src, std::size_t(part.width) * all, into.samples.data() + (std::size_t(at.y + y) * into.width + at.x) * all);
    }
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
    std::size_t held = 0;
    for (const auto& d : g_ink) held += d.pixels->samples.size() * sizeof(std::uint16_t);
    while (g_ink.size() > 1 && (g_ink.size() > kInkCount || held > kInkBytes)) {
        held -= g_ink.front().pixels->samples.size() * sizeof(std::uint16_t);
        g_ink.erase(g_ink.begin());
    }
    return pixels;
}

InkPicture sample_ink_raster(const core::InkPixels& raster, Size full, Box area) {
    InkPicture out;
    out.size = Size{area.width(), area.height()};
    out.channels = core::colour_channels(raster.space);
    const unsigned n = out.channels, all = n + 1;
    const double most = raster.max();
    out.values.assign(static_cast<std::size_t>(out.size.width) * out.size.height * all, 0.0);
    const int rw = static_cast<int>(raster.width), rh = static_cast<int>(raster.height);
    if (rw == full.width && rh == full.height) {  // (its own size: no resampling)
        for (int y = area.y0; y < area.y1; ++y) {
            for (int x = area.x0; x < area.x1; ++x) {
                const std::uint16_t* s = raster.samples.data() + (std::size_t(y) * rw + x) * all;
                double* d = out.values.data() + (std::size_t(y - area.y0) * out.size.width + (x - area.x0)) * all;
                for (unsigned c = 0; c < all; ++c) d[c] = s[c] / most;
            }
        }
        return out;
    }
    // premultiplied, horizontally into the rows the vertical pass needs, then vertically (Pillow's two passes)
    const Coeffs ky = coeffs(rh, full.height, area.y0, area.y1);
    const Coeffs kx = coeffs(rw, full.width, area.x0, area.x1);
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
        const std::uint16_t* s = raster.samples.data() + std::size_t(y) * rw * all;
        for (int x = 0; x < rw; ++x) {
            const double a = s[x * all + n] / most;
            for (unsigned c = 0; c < n; ++c) premultiplied[x * all + c] = s[x * all + c] / most * a;
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
            double v[5] = {};
            for (int k = 0; k < ky.count[j]; ++k) {
                const double* h = horizontal.data() + (std::size_t(ky.start[j] + k - row0) * ow + i) * all;
                for (unsigned c = 0; c < all; ++c) v[c] += h[c] * w[k];
            }
            const double a = std::clamp(v[n], 0.0, 1.0);
            double* d = out.values.data() + (std::size_t(j) * ow + i) * all;
            d[n] = a;
            for (unsigned c = 0; c < n; ++c) d[c] = a > 0 ? std::clamp(v[c] / a, 0.0, 1.0) : 0.0;
        }
    }
    return out;
}

}  // namespace genko::render
