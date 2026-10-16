#include "render/ink_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "core/color_adjust.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "render/color_filters.hpp"
#include "render/filters.hpp"
#include "render/grid.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ink_edit.hpp"
#include "render/op_limits.hpp"

namespace genko::render::ink_filters {

namespace {

using core::InkSpace;
using core::Json;

// --- a raster's samples, in the imaging budget ------------------------------------------------------------------------

struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};

// Samples as canvas values (render/ink_canvas: CMYK inks 0..1, Lab L/100 and (a, b + 128)/255), each pixel's colours
// then its alpha (straight).
class Plane {
public:
    Plane(int w, int h, unsigned n) : w_(w), h_(h), n_(n), work_(std::make_unique<Work>()) {
        const auto count = std::uint64_t(w) * std::uint64_t(h) * (n + 1);
        if (!genko_imaging_budget_reserve(&work_->reservation, count * sizeof(float))) detail::throw_imaging_error();
        v_.assign(static_cast<std::size_t>(count), 0.0f);
    }
    Plane(const Plane& other) : Plane(other.w_, other.h_, other.n_) { v_ = other.v_; }
    Plane& operator=(const Plane&) = delete;
    Plane(Plane&&) noexcept = default;
    Plane& operator=(Plane&&) noexcept = default;
    int width() const { return w_; }
    int height() const { return h_; }
    unsigned n() const { return n_; }  // (colours)
    unsigned all() const { return n_ + 1; }
    std::size_t size() const { return std::size_t(w_) * h_; }
    float* at(std::size_t i) { return v_.data() + i * all(); }
    const float* at(std::size_t i) const { return v_.data() + i * all(); }
    float* at(int x, int y) { return at(std::size_t(y) * w_ + x); }
    const float* at(int x, int y) const { return at(std::size_t(y) * w_ + x); }

private:
    int w_, h_;
    unsigned n_;
    std::unique_ptr<Work> work_;
    std::vector<float> v_;
};

// No ink (Lab: L 100, a = b = 0; Gray: white), as a clear pixel holds it (n colour channels).
void clear_colours(InkSpace space, float* p, unsigned n) {
    if (space == InkSpace::Multi) {
        std::fill_n(p, n, 0.0f);
        return;
    }
    if (space == InkSpace::Gray) {
        p[0] = 1.0f;
        return;
    }
    p[0] = space == InkSpace::Lab ? 1.0f : 0.0f;
    p[1] = p[2] = space == InkSpace::Lab ? static_cast<float>(128.0 / 255.0) : 0.0f;
    if (space == InkSpace::Cmyk) p[3] = 0.0f;
}

// Genko's black: K 100% alone (Lab: L 0, a = b = 0; Gray: lightness 0; a multichannel page: every plate solid, as RGB's
// black is through 1 − its values).
void black_colours(InkSpace space, float* p, unsigned n) {
    clear_colours(space, p, n);
    if (space == InkSpace::Multi) {
        std::fill_n(p, n, 1.0f);
        return;
    }
    p[space == InkSpace::Cmyk ? 3 : 0] = space == InkSpace::Cmyk ? 1.0f : 0.0f;
}

// Is the space one of inks on every channel (0 none, 1 solid: CMYK, a multichannel page's plates)?
bool inky(InkSpace space) { return space == InkSpace::Cmyk || space == InkSpace::Multi; }

Plane read(const core::InkPixels& r) {
    const unsigned n = r.colours();
    Plane p(static_cast<int>(r.width), static_cast<int>(r.height), n);
    if (r.is_float()) {  // (32 bits: the canvas's values themselves)
        for (std::size_t i = 0; i < p.size(); ++i) {
            float* v = p.at(i);
            for (unsigned c = 0; c <= n; ++c) v[c] = static_cast<float>(r.value(i, c));
        }
        return p;
    }
    const double most = r.max();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const std::uint16_t* s = r.samples.data() + i * (n + 1);
        float* v = p.at(i);
        for (unsigned c = 0; c < n; ++c) v[c] = static_cast<float>(to_canvas(r.space, c, ink::decode_sample(r.space, c, s[c], r.depth)));
        v[n] = static_cast<float>(s[n] / most);
    }
    return p;
}

core::InkPixels write(const Plane& p, InkSpace space, int depth) {
    core::InkPixels out = core::blank_ink_pixels(space, depth, static_cast<std::uint32_t>(p.width()), static_cast<std::uint32_t>(p.height()),
                                                 space == InkSpace::Multi ? p.n() : 0);
    const unsigned n = p.n();
    const double most = out.max();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float* v = p.at(i);
        for (unsigned c = 0; c <= n; ++c) {
            if (!std::isfinite(v[c])) throw core::Error("value", "the filter exceeds the finite working range");
        }
        if (depth == 32) {  // (floats: a grey's HDR kept, the rest 0..1; clear as a blank raster holds it)
            if (!(v[n] > 0)) continue;
            for (unsigned c = 0; c < n; ++c) out.set(i, c, v[c]);
            out.set(i, n, v[n]);
            continue;
        }
        const auto alpha = static_cast<std::uint16_t>(std::lround(std::clamp(static_cast<double>(v[n]), 0.0, 1.0) * most));
        if (alpha == 0) continue;  // (clear: as a blank raster holds it)
        std::uint16_t* s = out.samples.data() + i * (n + 1);
        for (unsigned c = 0; c < n; ++c) s[c] = ink::encode_sample(space, c, from_canvas(space, c, std::clamp(static_cast<double>(v[c]), 0.0, 1.0)), depth);
        s[n] = alpha;
    }
    return out;
}

// The pixels' inks (channels each, as render/ink takes them) and back into the plane's colours.
std::vector<double> inks_of(const Plane& p, InkSpace space) {
    std::vector<double> out(p.size() * p.n());
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < p.n(); ++c) out[i * p.n() + c] = from_canvas(space, c, p.at(i)[c]);
    return out;
}
void set_inks(Plane& p, InkSpace space, const std::vector<double>& inks) {
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < p.n(); ++c) p.at(i)[c] = static_cast<float>(to_canvas(space, c, inks[i * p.n() + c]));
}

// The colours the pixels mean (render/ink meaning_of), sRGB 0..1, 3 each.
std::vector<double> meant(const Plane& p, const core::PageColor& color) {
    const std::vector<double> inks = inks_of(p, color.space);
    std::vector<double> rgb(p.size() * 3);
    ink::meaning_of(color, inks.data(), rgb.data(), p.size());
    return rgb;
}

// --- settings (the 8-bit filter has read and refused them already) ---------------------------------------------------

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : core::to_float(*v);
}
double whole(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : static_cast<double>(core::py_trunc(core::to_float(*v)));
}

float to_sample(double v) {
    if (v >= 3.4028234663852886e38) return INFINITY;
    if (v <= -3.4028234663852886e38) return -INFINITY;
    return static_cast<float>(v);
}

// What the 8-bit filter clips to 0..255, held to 0..1.
float held(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// --- weighted by alpha -------------------------------------------------------------------------------------------------

void premultiply(Plane& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        for (unsigned c = 0; c < p.n(); ++c) v[c] *= v[p.n()];
    }
}
void unpremultiply(Plane& p, InkSpace space) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        const float a = v[p.n()];
        if (a > 1e-7f) {
            for (unsigned c = 0; c < p.n(); ++c) v[c] = std::clamp(v[c] / a, 0.0f, 1.0f);
            v[p.n()] = std::min(a, 1.0f);
        } else {
            clear_colours(space, v, p.n());
            v[p.n()] = 0;
        }
    }
}

// --- Pillow's box and Gaussian blur (render/color_filters), on any number of channels ---------------------------------

float gaussian_box(float radius, int passes) {
    const float sigma2 = radius * radius / static_cast<float>(passes);
    const float L = std::sqrt(12.0f * sigma2 + 1.0f);
    const float l = std::floor((L - 1.0f) / 2.0f);
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

// One box pass along a line of `count` pixels `stride` pixels apart (the edges repeated), its first `used` channels.
void box_line(float* line, std::size_t stride, int count, float radius, unsigned all, unsigned used, std::vector<float>& scratch) {
    const int r = static_cast<int>(radius);
    const double w = 1.0 / (radius * 2.0 + 1.0);
    const double fw = (1.0 - (r * 2 + 1) * w) / 2.0;
    scratch.resize(std::size_t(count) * used);
    const auto in = [&](int x) -> const float* { return line + std::size_t(std::clamp(x, 0, count - 1)) * stride * all; };
    std::array<double, 5> acc{};
    for (int k = -r; k <= r; ++k)
        for (unsigned c = 0; c < used; ++c) acc[c] += in(k)[c];
    for (int x = 0; x < count; ++x) {
        const float* far_left = in(x - r - 1);
        const float* far_right = in(x + r + 1);
        const float* leaving = in(x - r);
        for (unsigned c = 0; c < used; ++c) {
            scratch[std::size_t(x) * used + c] = to_sample(acc[c] * w + (static_cast<double>(far_left[c]) + far_right[c]) * fw);
            acc[c] += static_cast<double>(far_right[c]) - leaving[c];
        }
    }
    for (int x = 0; x < count; ++x)
        for (unsigned c = 0; c < used; ++c) line[std::size_t(x) * stride * all + c] = scratch[std::size_t(x) * used + c];
}

// ImageFilter.GaussianBlur(radius): three box passes across, three down; the colours, and the alpha too when asked.
void gaussian(Plane& p, double radius, bool alpha) {
    const float box = gaussian_box(static_cast<float>(radius), 3);
    if (box == 0) return;
    const unsigned used = alpha ? p.all() : p.n();
    std::vector<float> scratch;
    for (int pass = 0; pass < 3; ++pass)
        for (int y = 0; y < p.height(); ++y) box_line(p.at(0, y), 1, p.width(), box, p.all(), used, scratch);
    for (int pass = 0; pass < 3; ++pass)
        for (int x = 0; x < p.width(); ++x) box_line(p.at(x, 0), std::size_t(p.width()), p.height(), box, p.all(), used, scratch);
}

// The blur of the colours weighted by alpha (a line over nothing keeps its inks; its alpha fades).
void blur(Plane& p, double radius, InkSpace space) {
    premultiply(p);
    gaussian(p, radius, true);
    unpremultiply(p, space);
}

// ImageFilter.UnsharpMask on each ink: where the blur differs by more than the threshold (8-bit levels), the
// difference × percent added.
void unsharp(Plane& p, double radius, double percent, double threshold) {
    Plane blurred = p;
    gaussian(blurred, radius, false);
    for (std::size_t i = 0; i < p.size(); ++i) {
        for (unsigned c = 0; c < p.n(); ++c) {
            const float in = p.at(i)[c];
            const float diff = in - blurred.at(i)[c];
            if (std::abs(diff) * 255 > threshold) p.at(i)[c] = held(in + static_cast<double>(diff) * percent / 100);
        }
    }
}

// Darker by a factor (ImageEnhance.Brightness below 1): more K (Lab: less L), the other inks as they are.
void darken(float* v, InkSpace space, double factor, unsigned n) {
    if (space == InkSpace::Multi) for (unsigned c = 0; c < n; ++c) v[c] = held(1.0 - factor * (1.0 - v[c]));
    else if (space == InkSpace::Cmyk) v[3] = held(1.0 - factor * (1.0 - v[3]));
    else v[0] = held(v[0] * factor);
}

// --- resampling, weighted by alpha ------------------------------------------------------------------------------------

// The pixels at (x, y) as Pillow's bilinear transform takes them (a pixel spans [k, k + 1)), nothing outside: the
// colours weighted by alpha (out: weighted, then the alpha).
bool bilinear(const Plane& p, double xin, double yin, float* out) {
    if (xin < 0.0 || xin >= p.width() || yin < 0.0 || yin >= p.height()) return false;
    xin -= 0.5;
    yin -= 0.5;
    const int x = static_cast<int>(std::floor(xin)), y = static_cast<int>(std::floor(yin));
    const double dx = xin - x, dy = yin - y;
    const int x0 = std::clamp(x, 0, p.width() - 1), x1 = std::clamp(x + 1, 0, p.width() - 1);
    const int y0 = std::clamp(y, 0, p.height() - 1);
    const bool below = y + 1 >= 0 && y + 1 < p.height();
    const unsigned n = p.n();
    const auto pre = [&](int xx, int yy, unsigned c) { const float* v = p.at(xx, yy); return c == n ? double(v[n]) : double(v[c]) * v[n]; };
    for (unsigned c = 0; c <= n; ++c) {
        const double v1 = pre(x0, y0, c) + (pre(x1, y0, c) - pre(x0, y0, c)) * dx;
        const double v2 = below ? pre(x0, y + 1, c) + (pre(x1, y + 1, c) - pre(x0, y + 1, c)) * dx : v1;
        out[c] = static_cast<float>(v1 + (v2 - v1) * dy);
    }
    return true;
}

// Image.transform(size, AFFINE, a, BILINEAR) of the weighted samples (nothing where it falls outside).
Plane affine(const Plane& p, const std::array<double, 6>& a) {
    Plane out(p.width(), p.height(), p.n());
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const double xx = x + 0.5, yy = y + 0.5;
            (void)bilinear(p, a[0] * xx + a[1] * yy + a[2], a[3] * xx + a[4] * yy + a[5], out.at(x, y));
        }
    }
    return out;
}

Plane rotated(const Plane& p, double degrees, double cx, double cy) {
    const double rad = -(core::py_fmod(degrees, 360.0) * (core::kPi / 180.0));
    std::array<double, 6> m{core::py_round(core::py_cos(rad), 15), core::py_round(core::py_sin(rad), 15), 0.0,
                            core::py_round(-core::py_sin(rad), 15), core::py_round(core::py_cos(rad), 15), 0.0};
    m[2] = m[0] * -cx + m[1] * -cy + m[2] + cx;
    m[5] = m[3] * -cx + m[4] * -cy + m[5] + cy;
    return affine(p, m);
}

// --- the filters ---------------------------------------------------------------------------------------------------------

void sharpen(Plane& p, const Json& params, double scale, InkSpace space) {
    if (const Json* amount = core::get(params, "amount"); amount != nullptr && !amount->is_null()) {
        const double strength = core::py_max(0.1, core::py_min(5.0, core::to_float(*amount)));
        unsharp(p, core::py_max(0.5, number(params, "radius", 2)) * scale, static_cast<double>(core::py_round_int(120 * strength)), 2);
        return;
    }
    // lighter by 12 levels: less of each ink (Lab: more L)
    const unsigned n = p.n();
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        if (inky(space)) {
            for (unsigned c = 0; c < n; ++c) v[c] = held(v[c] - 12.0 / 255);
        } else {
            v[0] = held(v[0] + 12.0 / 255);
        }
    }
    // ImageEnhance.Contrast(1.3) from the mean (a whole level): each ink from its own mean (Lab: L from its mean, a and
    // b from no colour)
    std::array<double, core::kPlatesMax> mean{};
    for (unsigned c = 0; c < n; ++c) {
        if (space == InkSpace::Lab && c > 0) {
            mean[c] = 128.0 / 255.0;
            continue;
        }
        double sum = 0;
        for (std::size_t i = 0; i < p.size(); ++i) sum += p.at(i)[c] * 255.0;
        mean[c] = std::floor(sum / static_cast<double>(std::max<std::size_t>(1, p.size())) + 0.5) / 255;
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < n; ++c) p.at(i)[c] = held(mean[c] + 1.3 * (p.at(i)[c] - mean[c]));
    unsharp(p, 2 * scale, 150, 3);
}

void glow(Plane& p, const Json& params, double scale, const core::PageColor& color) {
    const double radius = core::py_max(0.5, number(params, "radius", 12)) * scale;
    const double amount = core::py_max(0.0, core::py_min(3.0, number(params, "amount", 0.8)));
    const double cut = whole(params, "threshold", 170);
    const bool cmyk = inky(color.space);
    const unsigned n = p.n();
    // the light: the bright pixels' lightness (CMYK: each ink's 1 − ink; Lab: L), nothing elsewhere
    const std::vector<double> rgb = meant(p, color);
    Plane light(p.width(), p.height(), n);
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (!(p.at(i)[n] > 0)) continue;  // (nothing there: no light — the 8-bit picture's clear pixels are black)
        const double luma = (rgb[i * 3] * 19595.0 + rgb[i * 3 + 1] * 38470.0 + rgb[i * 3 + 2] * 7471.0) / 65536.0;
        if (std::floor(luma * 255 + 0.5) < cut) continue;
        if (cmyk) {
            for (unsigned c = 0; c < n; ++c) light.at(i)[c] = 1.0f - p.at(i)[c];
        } else {
            light.at(i)[0] = p.at(i)[0];
        }
    }
    gaussian(light, radius, false);
    // ImageEnhance.Brightness(amount) on it, then ImageChops.screen
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        for (unsigned c = 0; c < (cmyk ? n : 1u); ++c) {
            const double l = held(light.at(i)[c] * amount);
            const double bright = cmyk ? 1.0 - v[c] : v[c];
            const double screened = bright + l * std::max(0.0, 1.0 - bright);
            v[c] = held(cmyk ? 1.0 - screened : screened);
        }
    }
}

void mosaic(Plane& p, const Json& params, double scale, InkSpace space) {
    const double block = core::py_max(2.0, std::round(core::py_max(2.0, whole(params, "block", 8)) * scale));
    const int w = p.width(), h = p.height();
    const int sw = static_cast<int>(std::max(1.0, std::floor(w / block))), sh = static_cast<int>(std::max(1.0, std::floor(h / block)));
    const auto nearest = [](int to, int size) {  // (as render/color_filters: Pillow's affine scale, NEAREST)
        std::vector<int> out(static_cast<std::size_t>(to), -1);
        const double step = static_cast<double>(size) / to;
        double at = step * 0.5;
        for (int x = 0; x < to; ++x) {
            const int in = at < 0.0 ? -1 : static_cast<int>(at);
            if (in >= 0 && in < size) out[static_cast<std::size_t>(x)] = in;
            at += step;
        }
        return out;
    };
    const auto down_x = nearest(sw, w), down_y = nearest(sh, h), up_x = nearest(w, sw), up_y = nearest(h, sh);
    const Plane source = p;
    const unsigned n = p.n();
    for (int y = 0; y < h; ++y) {
        const int my = up_y[static_cast<std::size_t>(y)];
        const int sy = my < 0 ? -1 : down_y[static_cast<std::size_t>(my)];
        for (int x = 0; x < w; ++x) {
            const int mx = up_x[static_cast<std::size_t>(x)];
            const int sx = mx < 0 ? -1 : down_x[static_cast<std::size_t>(mx)];
            float* v = p.at(x, y);
            if (sx < 0 || sy < 0) {  // (nothing there: black, as the resized picture leaves it)
                black_colours(space, v, n);
                continue;
            }
            // (the sample's colour, the pixel's alpha kept; a clear sample black, as the 8-bit picture's clear pixels are)
            if (source.at(sx, sy)[n] > 0) std::copy_n(source.at(sx, sy), n, v);
            else black_colours(space, v, n);
            darken(v, space, 0.88, n);  // (ImageEnhance.Brightness(0.88))
        }
    }
}

void motion_blur(Plane& p, const Json& params, double scale, InkSpace space) {
    const double distance = core::py_max(1.0, std::round(core::py_max(1.0, whole(params, "distance", 12)) * scale));
    const double angle = number(params, "angle", 0) * (core::kPi / 180.0);
    const int steps = static_cast<int>(core::py_max(2.0, core::py_min(64.0, distance)));
    const int w = p.width(), h = p.height();
    const unsigned all = p.all();
    premultiply(p);
    std::vector<double> sum(p.size() * all, 0.0);
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * distance;
        const auto ox = static_cast<long long>(core::py_round_whole(t * core::py_cos(angle)));
        const auto oy = static_cast<long long>(core::py_round_whole(t * core::py_sin(angle)));
        for (int y = 0; y < h; ++y) {
            const int sy = static_cast<int>(((y - oy) % h + h) % h);
            for (int x = 0; x < w; ++x) {
                const int sx = static_cast<int>(((x - ox) % w + w) % w);
                const float* v = p.at(sx, sy);
                double* s = sum.data() + (std::size_t(y) * w + x) * all;
                for (unsigned c = 0; c < all; ++c) s[c] += v[c];
            }
        }
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < all; ++c) p.at(i)[c] = static_cast<float>(sum[i * all + c] / steps);
    unpremultiply(p, space);
}

void radial_zoom(Plane& p, std::string_view kind, const Json& params, InkSpace space) {
    const double cx = number(params, "cx", 0.5) * p.width(), cy = number(params, "cy", 0.5) * p.height();
    const double amount = core::py_max(0.0, number(params, "amount", 0.08));
    constexpr int steps = 10;
    const unsigned all = p.all();
    std::vector<double> sum(p.size() * all, 0.0);
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * amount;
        const Plane moved = kind == "radial_blur" ? rotated(p, t * (180.0 / core::kPi), cx, cy)
                                                  : affine(p, {1 / (1 + t), 0, cx - cx / (1 + t), 0, 1 / (1 + t), cy - cy / (1 + t)});
        for (std::size_t i = 0; i < p.size(); ++i)
            for (unsigned c = 0; c < all; ++c) sum[i * all + c] += moved.at(i)[c];
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < all; ++c) p.at(i)[c] = static_cast<float>(sum[i * all + c] / steps);
    unpremultiply(p, space);
}

void wave_twirl(Plane& p, std::string_view kind, const Json& params, double scale, InkSpace space) {
    // the maps as render/color_filters makes them (float32, numpy's sine and cosine), then bilinear (outside: clear)
    const int w = p.width(), h = p.height();
    std::vector<float> map_x(p.size()), map_y(p.size());
    if (kind == "wave") {
        const auto amplitude = to_sample(number(params, "amplitude", 6) * scale);
        const auto wavelength = to_sample(core::py_max(2.0, number(params, "wavelength", 60)) * scale);
        const auto pi = static_cast<float>(core::kPi);
        const auto along = [&](float v) { return filters::numpy_sinf(((v * 2.0f) * pi) / wavelength); };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                map_x[i] = static_cast<float>(x) + amplitude * along(static_cast<float>(y));
                map_y[i] = static_cast<float>(y) + amplitude * along(static_cast<float>(x));
            }
        }
    } else {
        const auto cx = static_cast<float>(w / 2.0), cy = static_cast<float>(h / 2.0);
        const auto radius = static_cast<float>(core::py_max(1.0, number(params, "radius", 0.45) * std::min(w, h)));
        const auto twist = static_cast<float>(number(params, "angle", 90) * (core::kPi / 180.0));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
                const float share = std::min(std::max(1.0f - std::sqrt(dx * dx + dy * dy) / radius, 0.0f), 1.0f);
                const float turn = twist * (share * share);
                const float cos_t = filters::numpy_cosf(turn), sin_t = filters::numpy_sinf(turn);
                const std::size_t i = std::size_t(y) * w + x;
                map_x[i] = (cx + dx * cos_t) - dy * sin_t;
                map_y[i] = (cy + dx * sin_t) + dy * cos_t;
            }
        }
    }
    premultiply(p);
    const Plane source = p;
    const unsigned all = p.all();
    const auto at = [&](long long yy, long long xx, unsigned c) -> double {
        if (xx < 0 || xx >= w || yy < 0 || yy >= h) return 0.0;
        return source.at(static_cast<int>(xx), static_cast<int>(yy))[c];
    };
    for (std::size_t i = 0; i < p.size(); ++i) {
        const double mx = map_x[i], my = map_y[i];
        float* v = p.at(i);
        if (!(mx > -2 && mx < w + 1 && my > -2 && my < h + 1)) {  // (all four taps outside)
            std::fill_n(v, all, 0.0f);
            continue;
        }
        const auto x0 = static_cast<long long>(std::floor(mx)), y0 = static_cast<long long>(std::floor(my));
        const double fx = mx - static_cast<double>(x0), fy = my - static_cast<double>(y0);
        for (unsigned c = 0; c < all; ++c) {
            const double top = at(y0, x0, c) * (1 - fx) + at(y0, x0 + 1, c) * fx;
            const double bottom = at(y0 + 1, x0, c) * (1 - fx) + at(y0 + 1, x0 + 1, c) * fx;
            v[c] = static_cast<float>(top * (1 - fy) + bottom * fy);
        }
    }
    unpremultiply(p, space);
}

// Image.blend with a grain of random levels (one grey, or a colour: red first): each pixel's inks towards the inks the
// grain's colour takes by the page's rules (a grey: K alone).
void noise(Plane& p, const Json& params, const core::PageColor& color) {
    const double amount = core::py_max(0.0, core::py_min(1.0, number(params, "amount", 0.15)));
    core::PyRandom rng = core::PyRandom::from_str(core::py_str(core::get_or(params, "seed", Json("genko"))));
    const bool mono = core::py_truthy(core::get_or(params, "mono", Json(true)));
    std::vector<double> grain(p.size() * 3);
    for (unsigned c = 0; c < (mono ? 1u : 3u); ++c) {
        for (std::size_t i = 0; i < p.size(); ++i) {
            const double g = static_cast<double>(rng.randrange(256)) / 255;
            if (mono) grain[i * 3] = grain[i * 3 + 1] = grain[i * 3 + 2] = g;
            else grain[i * 3 + c] = g;
        }
    }
    const unsigned n = p.n();
    std::vector<double> target(p.size() * n);
    ink::inks_by_rules(color, grain.data(), target.data(), p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        for (unsigned c = 0; c < n; ++c) {
            const double t = to_canvas(color.space, c, target[i * n + c]);
            v[c] = static_cast<float>(v[c] + amount * (t - v[c]));
        }
    }
}

// The 8-bit streaks (filters.rain_layer) in the page's inks, laid over (straight), the alpha the larger of the two.
void rain(Plane& p, const Json& params, double scale, const core::PageColor& color) {
    Json scaled = params;
    scaled["length"] = std::min(limits::kStreak, core::py_max(2.0, number(params, "length", 40)) * scale);
    scaled["width"] = std::min(limits::kReach, std::max(1.0, core::py_round_whole(number(params, "width", 1) * scale)));
    const std::string streaks = filters::rain_layer(Size{p.width(), p.height()}, scaled).tobytes();
    const unsigned n = p.n();
    std::vector<unsigned char> rgb(p.size() * 3), alpha(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<unsigned char>(streaks[i * 4 + c]);
        alpha[i] = static_cast<unsigned char>(streaks[i * 4 + 3]);
    }
    std::vector<double> inks(p.size() * n, 0.0);
    ink::inks_of(color, rgb.data(), alpha.data(), inks.data(), p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        const double sa = alpha[i] / 255.0;
        if (sa == 0) continue;
        float* d = p.at(i);
        const double da = d[n], a = sa + da * (1 - sa);
        for (unsigned c = 0; c < n; ++c) {
            d[c] = static_cast<float>((to_canvas(color.space, c, inks[i * n + c]) * sa + d[c] * da * (1 - sa)) / a);
        }
        d[n] = static_cast<float>(std::max(da, sa));
    }
}

void despeckle(Plane& p, const Json& params, double scale, const core::PageColor& color) {
    const double dpi = number(params, "dpi", 200);
    const Json* given = core::get(params, "size_px");
    const double size = (given != nullptr && !given->is_null() ? core::to_float(*given) : number(params, "size_mm", 0.3) / 25.4 * dpi) * scale;
    const Json* what_value = core::get(params, "what");
    const std::string what = what_value != nullptr && core::py_truthy(*what_value) ? core::py_str(*what_value) : std::string("ink");
    const int w = p.width(), h = p.height();
    const unsigned n = p.n();
    const auto ink_of = [&] {  // (the pixels that show dark, by the colours they mean)
        const std::vector<double> rgb = meant(p, color);
        BoolGrid ink(w, h);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const float lum = static_cast<float>(rgb[i * 3] * 255.0) * 0.299f + static_cast<float>(rgb[i * 3 + 1] * 255.0) * 0.587f +
                              static_cast<float>(rgb[i * 3 + 2] * 255.0) * 0.114f;
            ink.cells[i] = p.at(i)[n] * 255 >= 128 && lum < 128.0f ? 1 : 0;
        }
        return ink;
    };
    BoolGrid ink = ink_of();
    const double limit = std::pow(std::max(1.0, size), 2.0);
    std::size_t solid = 0;
    for (std::size_t i = 0; i < p.size(); ++i) solid += p.at(i)[n] * 255 > 250 ? 1 : 0;
    const bool opaque = p.size() != 0 && static_cast<double>(solid) / static_cast<double>(p.size()) > 0.9;
    for (const std::string& part : what == "both" ? std::vector<std::string>{"ink", "holes"} : std::vector<std::string>{what}) {
        BoolGrid target = ink;
        if (part != "ink") for (unsigned char& c : target.cells) c = c != 0 ? 0 : 1;
        const auto sizes = filters::component_sizes(target);
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (target.cells[i] == 0 || static_cast<double>(sizes[i]) >= limit) continue;
            float* v = p.at(i);
            if (part == "ink") {  // (a speck gone: the paper, or nothing)
                clear_colours(color.space, v, n);
                v[n] = opaque ? 1.0f : 0.0f;
            } else {  // (a hole filled: Genko's black)
                black_colours(color.space, v, n);
                v[n] = 1.0f;
            }
        }
        ink = ink_of();
    }
}

void lineart(Plane& p, const Json& params_in, double scale, const core::PageColor& color) {
    // a line drawing of the layer as the screen shows it (render/color_filters' sizes), its colours by the rules
    Json params = params_in;
    if (scale != 1) {
        const double radius = std::round(number(params_in, "radius", 7) * scale);
        if (radius > static_cast<double>(limits::kRankSize))
            throw core::OpError("radius is too large for a raster of this resolution (at most " +
                                std::to_string(static_cast<int>(std::floor(limits::kRankSize / scale))) + ")");
        params["radius"] = radius;
        params["min_px"] = std::round(number(params_in, "min_px", 12) * scale * scale);
    }
    const Image shown = ink::to_rgba8(write(p, color.space, color.depth), color);
    const std::string drawn = filters::apply_filter(shown, "lineart", params).convert("RGBA").tobytes();
    const unsigned n = p.n();
    std::vector<unsigned char> rgb(p.size() * 3), alpha(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<unsigned char>(drawn[i * 4 + c]);
        alpha[i] = static_cast<unsigned char>(drawn[i * 4 + 3]);
    }
    std::vector<double> inks(p.size() * n, 0.0);
    ink::inks_of(color, rgb.data(), nullptr, inks.data(), p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        for (unsigned c = 0; c < n; ++c) v[c] = static_cast<float>(to_canvas(color.space, c, inks[i * n + c]));
        v[n] = alpha[i] / 255.0f;
        if (alpha[i] == 0) clear_colours(color.space, v, n);
    }
}

// A correction (levels, curve, hue …): the colours the inks mean, back by the rules (what it leaves keeps its inks).
void corrected(Plane& p, const core::PreciseAdjustment& adjust, const core::PageColor& color) {
    const std::vector<double> inks = inks_of(p, color.space);
    std::vector<double> out(inks.size());
    ink::correct(color, inks.data(), out.data(), p.size(), [&adjust](const std::array<double, 3>& rgb) { return adjust(rgb); });
    set_inks(p, color.space, out);
}

}  // namespace

std::string apply(const core::Page& page, const core::InkPixels& raster, std::string_view kind, const Json& params, const Json* area) {
    if (!page.color) throw core::OpError("an RGB page holds no ink pixels");
    const core::PageColor& color = *page.color;
    if (raster.space != color.space || raster.depth != color.depth) throw core::OpError("the ink pixels are not in the page's colour mode");
    color_filters::check(kind, params);  // (the settings read and refused as the 8-bit filter reads them)
    const double scale = ink_edit::raster_scale(page, raster.width, raster.height);
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    Plane p = read(raster);
    const Plane original = area != nullptr ? p : Plane(1, 1, p.n());
    const InkSpace space = color.space;
    if (kind == "blur") {
        blur(p, number(params, "radius", 2) * scale, space);
    } else if (kind == "sharpen") {
        sharpen(p, params, scale, space);
    } else if (kind == "glow") {
        glow(p, params, scale, color);
    } else if (kind == "rain") {
        rain(p, params, scale, color);
    } else if (kind == "despeckle") {
        despeckle(p, params, scale, color);
    } else if (kind == "mosaic") {
        mosaic(p, params, scale, space);
    } else if (kind == "motion_blur") {
        motion_blur(p, params, scale, space);
    } else if (kind == "radial_blur" || kind == "zoom_blur") {
        radial_zoom(p, kind, params, space);
    } else if (kind == "noise") {
        noise(p, params, color);
    } else if (kind == "wave" || kind == "twirl") {
        wave_twirl(p, kind, params, scale, space);
    } else if (kind == "lineart") {
        lineart(p, params, scale, color);
    } else {
        corrected(p, core::PreciseAdjustment(kind, params), color);
    }
    if (area != nullptr) {  // filters.within: the filter only inside the selection
        const std::string inside = ink_edit::area_coverage(page, raster.width, raster.height, *area);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const double m = static_cast<unsigned char>(inside[i]) / 255.0;
            if (m == 1) continue;
            for (unsigned c = 0; c < p.all(); ++c) p.at(i)[c] = static_cast<float>(p.at(i)[c] * m + original.at(i)[c] * (1 - m));
        }
    }
    return core::encode_ink_raster(write(p, color.space, color.depth));
}

}  // namespace genko::render::ink_filters
