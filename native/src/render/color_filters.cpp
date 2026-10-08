#include "render/color_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "core/color_adjust.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "render/color_edit.hpp"
#include "render/filters.hpp"
#include "render/imaging.hpp"

namespace genko::render::color_filters {

namespace {

using core::Json;
using Px = std::array<float, 4>;  // straight sRGB samples, as the raster holds them (f32 keeps them all)

// --- a picture of samples, in the imaging budget --------------------------------------------------------------------

struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};

class Picture {
public:
    Picture(int w, int h) : w_(w), h_(h), work_(std::make_unique<Work>()) {
        const auto n = std::uint64_t(w) * std::uint64_t(h);
        if (!genko_imaging_budget_reserve(&work_->reservation, n * sizeof(Px))) detail::throw_imaging_error();
        px_.assign(static_cast<std::size_t>(n), Px{0, 0, 0, 0});
    }
    Picture(const Picture& other) : Picture(other.w_, other.h_) { px_ = other.px_; }
    Picture& operator=(const Picture&) = delete;
    Picture(Picture&&) noexcept = default;
    Picture& operator=(Picture&&) noexcept = default;
    int width() const { return w_; }
    int height() const { return h_; }
    std::size_t size() const { return px_.size(); }
    Px& operator[](std::size_t i) { return px_[i]; }
    const Px& operator[](std::size_t i) const { return px_[i]; }
    Px& at(int x, int y) { return px_[std::size_t(y) * w_ + x]; }
    const Px& at(int x, int y) const { return px_[std::size_t(y) * w_ + x]; }

private:
    int w_, h_;
    std::unique_ptr<Work> work_;
    std::vector<Px> px_;
};

Picture read(const core::ColorRasterView& raster) {
    Picture out(static_cast<int>(raster.width()), static_cast<int>(raster.height()));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto p = raster.pixel(i);
        out[i] = Px{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3])};
    }
    return out;
}

std::string write(const Picture& picture, bool floating) {
    return core::encode_color_pixels(static_cast<std::uint32_t>(picture.width()), static_cast<std::uint32_t>(picture.height()),
                                     floating ? "f32" : "u16", [&](std::size_t i) {
        std::array<double, 4> out{};
        for (unsigned c = 0; c < 4; ++c) {
            const double v = picture[i][c];
            if (!std::isfinite(v)) throw core::Error("value", "high-precision filter exceeds finite working range");
            out[c] = floating && c < 3 ? v : std::clamp(v, 0.0, 1.0);
        }
        return out;
    });
}

// What an 8-bit filter clips to 0..255: an in-range sample held to 0..1, an HDR one (from beyond it) left as it is.
float held(float out, float in) { return in >= 0 && in <= 1 ? std::clamp(out, 0.0f, 1.0f) : out; }

// ImageOps.grayscale's luma (Pillow's 16-bit fixed-point weights), 0..1
double luma(const Px& p) { return (p[0] * 19595.0 + p[1] * 38470.0 + p[2] * 7471.0) / 65536.0; }

// --- settings (the 8-bit filter has read and refused them already) -------------------------------------------------

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : core::to_float(*v);
}
double whole(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : static_cast<double>(core::py_trunc(core::to_float(*v)));
}

// --- Pillow's box and Gaussian blur, without its 8-bit steps --------------------------------------------------------

// ImagingBoxBlur's radius for a Gaussian of `radius` in `passes` boxes (computed in float, as Pillow does)
float gaussian_box(float radius, int passes) {
    const float sigma2 = radius * radius / static_cast<float>(passes);
    const float L = std::sqrt(12.0f * sigma2 + 1.0f);
    const float l = std::floor((L - 1.0f) / 2.0f);
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

// One box pass along a line of n samples `stride` apart (the edges repeated), for the chosen channels.
void box_line(Px* line, std::size_t stride, int n, float radius, const std::array<bool, 4>& channels, std::vector<Px>& scratch) {
    const int r = static_cast<int>(radius);
    const double w = 1.0 / (radius * 2.0 + 1.0);
    const double fw = (1.0 - (r * 2 + 1) * w) / 2.0;
    scratch.resize(static_cast<std::size_t>(n));
    const auto in = [&](int x) -> const Px& { return line[std::size_t(std::clamp(x, 0, n - 1)) * stride]; };
    for (unsigned c = 0; c < 4; ++c) {
        if (!channels[c]) continue;
        double acc = 0;
        for (int k = -r; k <= r; ++k) acc += in(k)[c];
        for (int x = 0; x < n; ++x) {
            scratch[std::size_t(x)][c] = static_cast<float>(acc * w + (static_cast<double>(in(x - r - 1)[c]) + in(x + r + 1)[c]) * fw);
            acc += static_cast<double>(in(x + r + 1)[c]) - in(x - r)[c];
        }
    }
    for (int x = 0; x < n; ++x) {
        Px& p = line[std::size_t(x) * stride];
        for (unsigned c = 0; c < 4; ++c) if (channels[c]) p[c] = scratch[std::size_t(x)][c];
    }
}

// ImageFilter.GaussianBlur(radius): three box passes across, three down
void gaussian(Picture& p, double radius, const std::array<bool, 4>& channels) {
    const float box = gaussian_box(static_cast<float>(radius), 3);
    if (box == 0) return;
    std::vector<Px> scratch;
    for (int pass = 0; pass < 3; ++pass)
        for (int y = 0; y < p.height(); ++y) box_line(&p.at(0, y), 1, p.width(), box, channels, scratch);
    for (int pass = 0; pass < 3; ++pass)
        for (int x = 0; x < p.width(); ++x) box_line(&p.at(x, 0), std::size_t(p.width()), p.height(), box, channels, scratch);
}

constexpr std::array<bool, 4> kAll{true, true, true, true};
constexpr std::array<bool, 4> kColour{true, true, true, false};

// ImageFilter.UnsharpMask(radius, percent, threshold) on the colours: where the blur differs by more than the
// threshold (8-bit levels), the difference × percent is added
void unsharp(Picture& p, double radius, double percent, double threshold) {
    Picture blurred = p;
    gaussian(blurred, radius, kColour);
    for (std::size_t i = 0; i < p.size(); ++i) {
        for (unsigned c = 0; c < 3; ++c) {
            const float in = p[i][c];
            const float diff = in - blurred[i][c];
            if (std::abs(diff) * 255 > threshold) p[i][c] = held(static_cast<float>(in + diff * percent / 100), in);
        }
    }
}

// ImageEnhance.Brightness(f): towards black (the colours times f)
void brightness(Picture& p, double factor) {
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) p[i][c] = held(static_cast<float>(p[i][c] * factor), p[i][c]);
}

// --- Pillow's resampling, without its 8-bit steps ------------------------------------------------------------------

// The sample at (x, y) (a pixel spans [k, k + 1)) as Pillow's bilinear transform takes it from an RGBA picture: its
// colours premultiplied by alpha on the way (Image.transform converts RGBA to RGBa), nothing outside the picture, the
// edges repeated within it
bool bilinear(const Picture& p, double xin, double yin, Px& out) {
    if (xin < 0.0 || xin >= p.width() || yin < 0.0 || yin >= p.height()) return false;
    xin -= 0.5;
    yin -= 0.5;
    const int x = static_cast<int>(std::floor(xin)), y = static_cast<int>(std::floor(yin));
    const double dx = xin - x, dy = yin - y;
    const int x0 = std::clamp(x, 0, p.width() - 1), x1 = std::clamp(x + 1, 0, p.width() - 1);
    const int y0 = std::clamp(y, 0, p.height() - 1);
    const bool below = y + 1 >= 0 && y + 1 < p.height();
    const auto pre = [&](int xx, int yy, unsigned c) { const Px& v = p.at(xx, yy); return c == 3 ? double(v[3]) : double(v[c]) * v[3]; };
    std::array<double, 4> mixed{};
    for (unsigned c = 0; c < 4; ++c) {
        const double v1 = pre(x0, y0, c) + (pre(x1, y0, c) - pre(x0, y0, c)) * dx;
        const double v2 = below ? pre(x0, y + 1, c) + (pre(x1, y + 1, c) - pre(x0, y + 1, c)) * dx : v1;
        mixed[c] = v1 + (v2 - v1) * dy;
    }
    for (unsigned c = 0; c < 3; ++c) out[c] = mixed[3] > 0 ? static_cast<float>(mixed[c] / mixed[3]) : 0.0f;
    out[3] = static_cast<float>(mixed[3]);
    return true;
}

// Image.transform(size, AFFINE, a, BILINEAR): each pixel's centre through a; nothing (clear) where it falls outside
Picture affine(const Picture& p, const std::array<double, 6>& a) {
    Picture out(p.width(), p.height());
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const double xx = x + 0.5, yy = y + 0.5;
            Px v{};
            if (bilinear(p, a[0] * xx + a[1] * yy + a[2], a[3] * xx + a[4] * yy + a[5], v)) out.at(x, y) = v;
        }
    }
    return out;
}

// Image.rotate(degrees, BILINEAR, center=(cx, cy)) as Pillow makes its matrix
Picture rotated(const Picture& p, double degrees, double cx, double cy) {
    const double rad = -(core::py_fmod(degrees, 360.0) * (core::kPi / 180.0));
    std::array<double, 6> m{core::py_round(core::py_cos(rad), 15), core::py_round(core::py_sin(rad), 15), 0.0,
                            core::py_round(-core::py_sin(rad), 15), core::py_round(core::py_cos(rad), 15), 0.0};
    m[2] = m[0] * -cx + m[1] * -cy + m[2] + cx;
    m[5] = m[3] * -cx + m[4] * -cy + m[5] + cy;
    return affine(p, m);
}

// --- the filters -------------------------------------------------------------------------------------------------------

void point(Picture& p, const core::PreciseAdjustment& adjust) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const auto out = adjust({p[i][0], p[i][1], p[i][2]});
        for (unsigned c = 0; c < 3; ++c) p[i][c] = static_cast<float>(out[c]);
    }
}

void sharpen(Picture& p, const Json& params, double scale) {
    if (const Json* amount = core::get(params, "amount"); amount != nullptr && !amount->is_null()) {
        const double strength = core::py_max(0.1, core::py_min(5.0, core::to_float(*amount)));
        unsharp(p, core::py_max(0.5, number(params, "radius", 2)) * scale, static_cast<double>(core::py_round_int(120 * strength)), 2);
        return;
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) p[i][c] = p[i][c] <= 1 ? std::min(1.0f, p[i][c] + 12.0f / 255) : p[i][c] + 12.0f / 255;
    // ImageEnhance.Contrast(1.3): from the picture's mean grey (a whole level)
    double sum = 0;
    for (std::size_t i = 0; i < p.size(); ++i) sum += luma(p[i]) * 255;
    const double mean = std::floor(sum / static_cast<double>(std::max<std::size_t>(1, p.size())) + 0.5) / 255;
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) p[i][c] = held(static_cast<float>(mean + 1.3 * (p[i][c] - mean)), p[i][c]);
    unsharp(p, 2 * scale, 150, 3);
}

void glow(Picture& p, const Json& params, double scale) {
    const double radius = core::py_max(0.5, number(params, "radius", 12)) * scale;
    const double amount = core::py_max(0.0, core::py_min(3.0, number(params, "amount", 0.8)));
    const double cut = whole(params, "threshold", 170);
    Picture light(p.width(), p.height());
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (std::floor(luma(p[i]) * 255 + 0.5) >= cut) light[i] = Px{p[i][0], p[i][1], p[i][2], 0};
    }
    gaussian(light, radius, kColour);
    brightness(light, amount);
    for (std::size_t i = 0; i < p.size(); ++i)  // ImageChops.screen
        for (unsigned c = 0; c < 3; ++c) p[i][c] = 1 - (1 - p[i][c]) * (1 - light[i][c]);
}

void mosaic(Picture& p, const Json& params, double scale) {
    const double block = core::py_max(2.0, std::round(core::py_max(2.0, whole(params, "block", 8)) * scale));
    const int w = p.width(), h = p.height();
    const int sw = static_cast<int>(std::max(1.0, std::floor(w / block))), sh = static_cast<int>(std::max(1.0, std::floor(h / block)));
    // resize NEAREST down and back: each pixel's centre through the scale, the pixel it falls in
    const auto from = [](int x, int to, int size) { return std::min(size - 1, static_cast<int>((x + 0.5) * size / to)); };
    const Picture source = p;
    for (int y = 0; y < h; ++y) {
        const int sy = from(from(y, h, sh), sh, h);
        for (int x = 0; x < w; ++x) {
            const int sx = from(from(x, w, sw), sw, w);
            for (unsigned c = 0; c < 3; ++c) p.at(x, y)[c] = held(source.at(sx, sy)[c] * 0.88f, source.at(sx, sy)[c]);
        }
    }
}

void motion_blur(Picture& p, const Json& params, double scale) {
    const double distance = core::py_max(1.0, std::round(core::py_max(1.0, whole(params, "distance", 12)) * scale));
    const double angle = number(params, "angle", 0) * (core::kPi / 180.0);
    const int steps = static_cast<int>(core::py_max(2.0, core::py_min(64.0, distance)));
    const int w = p.width(), h = p.height();
    Picture sum(w, h);
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * distance;
        // ImageChops.offset: the picture moved round (what leaves one edge comes in at the other)
        const auto ox = static_cast<long long>(core::py_round_whole(t * core::py_cos(angle)));
        const auto oy = static_cast<long long>(core::py_round_whole(t * core::py_sin(angle)));
        for (int y = 0; y < h; ++y) {
            const int sy = static_cast<int>(((y - oy) % h + h) % h);
            for (int x = 0; x < w; ++x) {
                const int sx = static_cast<int>(((x - ox) % w + w) % w);
                for (unsigned c = 0; c < 4; ++c) sum[std::size_t(y) * w + x][c] += p.at(sx, sy)[c];
            }
        }
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) p[i][c] = sum[i][c] / static_cast<float>(steps);
}

void radial_zoom(Picture& p, std::string_view kind, const Json& params) {
    const double cx = number(params, "cx", 0.5) * p.width(), cy = number(params, "cy", 0.5) * p.height();
    const double amount = core::py_max(0.0, number(params, "amount", 0.08));
    constexpr int steps = 10;
    Picture sum(p.width(), p.height());
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * amount;
        Picture moved = kind == "radial_blur"
                            ? rotated(p, t * (180.0 / core::kPi), cx, cy)
                            : affine(p, {1 / (1 + t), 0, cx - cx / (1 + t), 0, 1 / (1 + t), cy - cy / (1 + t)});
        for (std::size_t i = 0; i < p.size(); ++i)
            for (unsigned c = 0; c < 4; ++c) sum[i][c] += moved[i][c];
    }
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) p[i][c] = sum[i][c] / static_cast<float>(steps);
}

void noise(Picture& p, const Json& params) {
    const double amount = core::py_max(0.0, core::py_min(1.0, number(params, "amount", 0.15)));
    core::PyRandom rng = core::PyRandom::from_str(core::py_str(core::get_or(params, "seed", Json("genko"))));
    const bool mono = core::py_truthy(core::get_or(params, "mono", Json(true)));
    // Image.blend with a grain of random levels (one for all three colours, or one each, red first)
    std::vector<float> grain(p.size());
    for (unsigned c = 0; c < (mono ? 1u : 3u); ++c) {
        for (float& g : grain) g = static_cast<float>(rng.randrange(256)) / 255;
        for (std::size_t i = 0; i < p.size(); ++i) {
            for (unsigned band = mono ? 0 : c; band < (mono ? 3u : c + 1); ++band)
                p[i][band] = static_cast<float>(p[i][band] + amount * (grain[i] - p[i][band]));
        }
    }
}

void wave_twirl(Picture& p, std::string_view kind, const Json& params, double scale) {
    // the maps as filters._remap takes them (float32, numpy's sine and cosine), then bilinear (outside: clear)
    const int w = p.width(), h = p.height();
    std::vector<float> map_x(p.size()), map_y(p.size());
    if (kind == "wave") {
        const auto amplitude = static_cast<float>(number(params, "amplitude", 6) * scale);
        const auto wavelength = static_cast<float>(core::py_max(2.0, number(params, "wavelength", 60)) * scale);
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
    const Picture source = p;
    const auto at = [&](long long yy, long long xx, unsigned c) -> double {
        if (xx < 0 || xx >= w || yy < 0 || yy >= h) return 0.0;
        return source.at(static_cast<int>(xx), static_cast<int>(yy))[c];
    };
    for (std::size_t i = 0; i < p.size(); ++i) {
        const double mx = map_x[i], my = map_y[i];
        if (!std::isfinite(mx) || !std::isfinite(my)) { p[i] = Px{0, 0, 0, 0}; continue; }
        const auto x0 = static_cast<long long>(std::floor(mx)), y0 = static_cast<long long>(std::floor(my));
        const double fx = mx - static_cast<double>(x0), fy = my - static_cast<double>(y0);
        for (unsigned c = 0; c < 4; ++c) {
            const double top = at(y0, x0, c) * (1 - fx) + at(y0, x0 + 1, c) * fx;
            const double bottom = at(y0 + 1, x0, c) * (1 - fx) + at(y0 + 1, x0 + 1, c) * fx;
            p[i][c] = static_cast<float>(top * (1 - fy) + bottom * fy);
        }
    }
}

void rain(Picture& p, const Json& params, double scale) {
    Json scaled = params;  // (the streaks' length and width on the raster's pixels)
    scaled["length"] = core::py_max(2.0, number(params, "length", 40)) * scale;
    scaled["width"] = std::max(1.0, core::py_round_whole(number(params, "width", 1) * scale));
    const Image layer = filters::rain_layer(Size{p.width(), p.height()}, scaled);
    const std::string streaks = layer.tobytes();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const auto* s = reinterpret_cast<const unsigned char*>(streaks.data() + i * 4);
        const double sa = s[3] / 255.0;
        if (sa == 0) continue;
        // Image.alpha_composite (straight sRGB), then the alpha the lighter of the two
        Px& d = p[i];
        const double da = d[3], a = sa + da * (1 - sa);
        for (unsigned c = 0; c < 3; ++c) d[c] = static_cast<float>((s[c] / 255.0 * sa + d[c] * da * (1 - sa)) / a);
        d[3] = static_cast<float>(std::max(da, sa));
    }
}

void despeckle(Picture& p, const Json& params, double scale) {
    const double dpi = number(params, "dpi", 200);
    const Json* given = core::get(params, "size_px");
    const double size = (given != nullptr && !given->is_null() ? core::to_float(*given) : number(params, "size_mm", 0.3) / 25.4 * dpi) * scale;
    const Json* what_value = core::get(params, "what");
    const std::string what = what_value != nullptr && core::py_truthy(*what_value) ? core::py_str(*what_value) : std::string("ink");
    const int w = p.width(), h = p.height();
    const auto ink_of = [&] {
        BoolGrid ink(w, h);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const float lum = (p[i][0] * 255.0f) * 0.299f + (p[i][1] * 255.0f) * 0.587f + (p[i][2] * 255.0f) * 0.114f;
            ink.cells[i] = p[i][3] * 255 >= 128 && lum < 128.0f ? 1 : 0;
        }
        return ink;
    };
    BoolGrid ink = ink_of();
    const double limit = std::pow(std::max(1.0, size), 2.0);
    std::size_t solid = 0;
    for (std::size_t i = 0; i < p.size(); ++i) solid += p[i][3] * 255 > 250 ? 1 : 0;
    const bool opaque = !p.size() ? false : static_cast<double>(solid) / static_cast<double>(p.size()) > 0.9;
    for (const std::string& part : what == "both" ? std::vector<std::string>{"ink", "holes"} : std::vector<std::string>{what}) {
        BoolGrid target = ink;
        if (part != "ink") for (unsigned char& c : target.cells) c = c != 0 ? 0 : 1;
        const auto sizes = filters::component_sizes(target);
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (target.cells[i] == 0 || static_cast<double>(sizes[i]) >= limit) continue;
            if (part == "ink") p[i] = opaque ? Px{1, 1, 1, 1} : Px{0, 0, 0, 0};
            else p[i] = Px{0, 0, 0, 1};
        }
        ink = ink_of();
    }
}

void lineart(Picture& p, const Json& params) {
    // a line drawing of the layer as its 8-bit picture shows it
    std::string bytes(p.size() * 4, '\0');
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) bytes[i * 4 + c] = static_cast<char>(std::lround(std::clamp(p[i][c], 0.0f, 1.0f) * 255));
    const Image lines = filters::apply_filter(Image::frombytes("RGBA", Size{p.width(), p.height()}, bytes), "lineart", params);
    const std::string drawn = lines.convert("RGBA").tobytes();
    for (std::size_t i = 0; i < p.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) p[i][c] = static_cast<unsigned char>(drawn[i * 4 + c]) / 255.0f;
}

}  // namespace

std::string apply(const core::Page& page, const core::Layer& layer, std::string_view kind, const Json& params, const Json* area) {
    // the settings read and refused as the 8-bit filter reads them (on a picture of a few pixels)
    (void)filters::apply_filter(Image::create("RGBA", Size{2, 2}, Ink{0, 0, 0, 0}), kind, params);
    const core::ColorRasterView raster(*layer.color_raster);
    const bool floating = raster.metadata("")["precision"] == "f32";
    const double scale = color_edit::pixel_scale(page, raster);
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    Picture p = read(raster);
    const Picture original = area != nullptr ? p : Picture(1, 1);
    if (kind == "blur") {
        gaussian(p, number(params, "radius", 2) * scale, kAll);
    } else if (kind == "sharpen") {
        sharpen(p, params, scale);
    } else if (kind == "glow") {
        glow(p, params, scale);
    } else if (kind == "rain") {
        rain(p, params, scale);
    } else if (kind == "despeckle") {
        despeckle(p, params, scale);
    } else if (kind == "mosaic") {
        mosaic(p, params, scale);
    } else if (kind == "motion_blur") {
        motion_blur(p, params, scale);
    } else if (kind == "radial_blur" || kind == "zoom_blur") {
        radial_zoom(p, kind, params);
    } else if (kind == "noise") {
        noise(p, params);
    } else if (kind == "wave" || kind == "twirl") {
        wave_twirl(p, kind, params, scale);
    } else if (kind == "lineart") {
        lineart(p, params);
    } else {
        point(p, core::PreciseAdjustment(kind, params));
    }
    if (area != nullptr) {  // filters.within: the filter only inside the selection
        const std::string inside = color_edit::area_on(page, raster, *area).tobytes();
        for (std::size_t i = 0; i < p.size(); ++i) {
            const double m = static_cast<unsigned char>(inside[i]) / 255.0;
            if (m == 1) continue;
            for (unsigned c = 0; c < 4; ++c) p[i][c] = static_cast<float>(p[i][c] * m + original[i][c] * (1 - m));
        }
    }
    return write(p, floating);
}

}  // namespace genko::render::color_filters
