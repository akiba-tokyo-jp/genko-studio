// PC4 節 D5 (SPEC.md COLOR-08「フィルター」): フィルター → Camera Raw フィルター… as filter_raster's kind camera_raw on the three
// ways (render/fx.hpp) — PhotoCraft v0.2.0's algo/src/camera_raw.rs develop (white balance and exposure in linear light,
// かすみの除去 by the dark channel prior, tone on the luminance with its guided bases, presence, the tone curves, the colour
// mixer, colour grading, detail, the post-crop vignette and grain: each stage skipped when its controls are neutral, so the
// defaults are the identity) and photo_util.rs (hash01, the sRGB curve, bilinear), MIT
// (native/third_party/photocraft/NOTICE.md).
//
// Upstream develops a whole picture at once; Genko works it tile by tile (the layer's bytes written as each tile is done,
// on as many threads as the imaging budget holds): what the stages read of the whole frame — the haze's atmospheric light
// and transmission on a proxy of at most 768 pixels, the tone's guided bases (ハイライト・シャドウ, 明瞭度) on the frame's
// luminance averaged down — is worked out once (Kernel::prepare, its planes held in the imaging budget); each tile then
// works its part of the frame grown by what its local stages reach (テクスチャ's Gaussian, ディテール's guided filter and
// blurs), their sums exact in whole numbers (fx::whole_box_passes), so a tile has the same bytes whatever the tiling and the
// threads.
//
// The frame is the box of what shows on the layer (Kernel::shown_frame, as レンズ補正's): its long side sets the reach of
// the tone's bases, the haze's proxy and テクスチャ (upstream: the canvas's), 周辺光量 is about its centre. ディテール's radii
// are pixels of the working 200 dpi (upstream's pixel_scale: Ctx::s), 粒子 is placed by the working 200 dpi's grid. A page
// of inks is worked on the colours its inks mean (FilterInks::Meaning); a 32-bit precise layer keeps values past 1
// (upstream's float documents); a clear pixel keeps its values (upstream changes their colours unseen).

#include "render/fx_camera_raw.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/image_ops.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::camera_raw {

using core::Json;

// --- the settings ------------------------------------------------------------------------------------------------------

bool Settings::wb_neutral() const { return temperature == 0 && tint == 0 && exposure == 0; }
bool Settings::tone_neutral() const {
    return contrast == 0 && highlights == 0 && shadows == 0 && whites == 0 && blacks == 0 && texture == 0 && clarity == 0;
}
bool Settings::hsl_neutral() const {
    const auto zero = [](const std::array<float, 8>& v) { return std::all_of(v.begin(), v.end(), [](float x) { return x == 0; }); };
    return zero(hsl_hue) && zero(hsl_sat) && zero(hsl_lum);
}
bool Settings::grade_neutral() const {
    return grade_shadows.neutral() && grade_midtones.neutral() && grade_highlights.neutral() && grade_global.neutral();
}
bool Settings::curves_neutral() const {
    return curve_highlights == 0 && curve_lights == 0 && curve_darks == 0 && curve_shadows == 0 && is_linear(point_curve) &&
           is_linear(point_curve_red) && is_linear(point_curve_green) && is_linear(point_curve_blue);
}
bool Settings::identity() const {
    return wb_neutral() && tone_neutral() && dehaze == 0 && vibrance == 0 && saturation == 0 && curves_neutral() && hsl_neutral() &&
           grade_neutral() && sharpen_amount == 0 && noise_luminance == 0 && noise_color == 0 && grain_amount == 0 && vignette_amount == 0;
}

bool is_linear(const std::vector<std::array<float, 2>>& points) {
    return points.size() < 2 || std::all_of(points.begin(), points.end(), [](const auto& p) { return std::fabs(p[0] - p[1]) < 1e-3f; });
}

namespace {

float number(const Json& params, const char* key, float fallback) {
    const Json* v = core::get(params, key);
    if (v == nullptr || !v->is_number() || v->is_boolean()) return fallback;
    const double x = v->get<double>();
    return std::isfinite(x) ? static_cast<float>(x) : fallback;
}

template <std::size_t N>
std::array<float, N> numbers(const Json& params, const char* key, std::array<float, N> fallback) {
    const Json* v = core::get(params, key);
    if (v == nullptr || !v->is_array() || v->size() != N) return fallback;
    std::array<float, N> out = fallback;
    for (std::size_t i = 0; i < N; ++i) {
        if ((*v)[i].is_number() && !(*v)[i].is_boolean() && std::isfinite((*v)[i].get<double>())) out[i] = static_cast<float>((*v)[i].get<double>());
    }
    return out;
}

std::vector<std::array<float, 2>> points(const Json& params, const char* key) {
    std::vector<std::array<float, 2>> out;
    const Json* v = core::get(params, key);
    if (v == nullptr || !v->is_array()) return out;
    for (const Json& p : *v) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) continue;
        const double a = p[0].get<double>(), b = p[1].get<double>();
        if (std::isfinite(a) && std::isfinite(b)) out.push_back({static_cast<float>(a), static_cast<float>(b)});
    }
    return out;
}

Wheel wheel(const Json& params, const char* key) {
    Wheel out;
    const Json* v = core::get(params, key);
    if (v == nullptr || !v->is_object()) return out;
    out.hue = number(*v, "hue", 0);
    out.sat = number(*v, "sat", 0);
    out.lum = number(*v, "lum", 0);
    return out;
}

}  // namespace

Settings settings_of(const Json& params) {
    Settings p;
    p.temperature = number(params, "temperature", 0);
    p.tint = number(params, "tint", 0);
    p.exposure = number(params, "exposure", 0);
    p.contrast = number(params, "contrast", 0);
    p.highlights = number(params, "highlights", 0);
    p.shadows = number(params, "shadows", 0);
    p.whites = number(params, "whites", 0);
    p.blacks = number(params, "blacks", 0);
    p.texture = number(params, "texture", 0);
    p.clarity = number(params, "clarity", 0);
    p.dehaze = number(params, "dehaze", 0);
    p.vibrance = number(params, "vibrance", 0);
    p.saturation = number(params, "saturation", 0);
    p.curve_highlights = number(params, "curve_highlights", 0);
    p.curve_lights = number(params, "curve_lights", 0);
    p.curve_darks = number(params, "curve_darks", 0);
    p.curve_shadows = number(params, "curve_shadows", 0);
    p.curve_splits = numbers<3>(params, "curve_splits", {25, 50, 75});
    p.point_curve = points(params, "point_curve");
    p.point_curve_red = points(params, "point_curve_red");
    p.point_curve_green = points(params, "point_curve_green");
    p.point_curve_blue = points(params, "point_curve_blue");
    p.hsl_hue = numbers<8>(params, "hsl_hue", {});
    p.hsl_sat = numbers<8>(params, "hsl_sat", {});
    p.hsl_lum = numbers<8>(params, "hsl_lum", {});
    p.grade_shadows = wheel(params, "grade_shadows");
    p.grade_midtones = wheel(params, "grade_midtones");
    p.grade_highlights = wheel(params, "grade_highlights");
    p.grade_global = wheel(params, "grade_global");
    p.grade_blending = number(params, "grade_blending", 50);
    p.grade_balance = number(params, "grade_balance", 0);
    p.sharpen_amount = number(params, "sharpen_amount", 0);
    p.sharpen_radius = number(params, "sharpen_radius", 1);
    p.sharpen_detail = number(params, "sharpen_detail", 25);
    p.sharpen_masking = number(params, "sharpen_masking", 0);
    p.noise_luminance = number(params, "noise_luminance", 0);
    p.noise_luminance_detail = number(params, "noise_luminance_detail", 50);
    p.noise_color = number(params, "noise_color", 0);
    p.noise_color_detail = number(params, "noise_color_detail", 50);
    p.grain_amount = number(params, "grain_amount", 0);
    p.grain_size = number(params, "grain_size", 25);
    p.grain_roughness = number(params, "grain_roughness", 50);
    p.vignette_amount = number(params, "vignette_amount", 0);
    p.vignette_midpoint = number(params, "vignette_midpoint", 50);
    p.vignette_roundness = number(params, "vignette_roundness", 0);
    p.vignette_feather = number(params, "vignette_feather", 50);
    p.vignette_highlights = number(params, "vignette_highlights", 0);
    if (const Json* style = core::get(params, "vignette_style"); style != nullptr && style->is_string()) p.vignette_style = style->get<std::string>();
    if (const Json* seed = core::get(params, "seed"); seed != nullptr && seed->is_number_integer())
        p.seed = static_cast<std::uint32_t>(std::clamp<std::int64_t>(seed->get<std::int64_t>(), 0, 4294967295LL));
    return p;
}

// --- upstream's helpers (camera_raw.rs, photo_util.rs) ------------------------------------------------------------------

namespace {

float luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float luma(const std::array<float, 3>& c) { return luma(c.data()); }

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float rem_euclid(float a, float b) {
    const float r = std::fmod(a, b);
    return r < 0 ? r + std::fabs(b) : r;
}

// The sRGB transfer function's decoding (linear below 0, a power above 1) and encoding (photo_util.rs).
float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

// photo_util.rs hash01: an integer hash to 0..1 (position-keyed noise).
float hash01(std::int64_t x, std::int64_t y, std::uint64_t seed) {
    std::uint64_t z = (static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ULL) ^ (static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4FULL) ^
                      (seed * 0x165667B19E3779F9ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) / static_cast<float>(1ULL << 24);
}

float value_noise(float x, float y, std::uint64_t seed) {
    const float xi = std::floor(x), yi = std::floor(y);
    const float tx = x - xi, ty = y - yi;
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    const auto h = [&](std::int64_t dx, std::int64_t dy) { return hash01(static_cast<std::int64_t>(xi) + dx, static_cast<std::int64_t>(yi) + dy, seed); };
    const float top = h(0, 0) + (h(1, 0) - h(0, 0)) * sx;
    const float bot = h(0, 1) + (h(1, 1) - h(0, 1)) * sx;
    return top + (bot - top) * sy;
}

// photo_util.rs bilinear: a w × h plane sampled at (x, y), clamped to its edges.
float bilinear(const std::vector<float>& buf, int w, int h, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    const auto p = [&](int xx, int yy) { return buf[std::size_t(yy) * std::size_t(w) + std::size_t(xx)]; };
    const float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
    const float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
    return top + (bot - top) * ty;
}

std::array<float, 3> rgb_to_hsv(const std::array<float, 3>& c) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    const float d = mx - mn;
    float h = 0;
    if (d <= 1e-9f) h = 0;
    else if (mx == c[0]) h = 60.0f * rem_euclid((c[1] - c[2]) / d, 6.0f);
    else if (mx == c[1]) h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
    else h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
    return {h, mx > 1e-9f ? d / mx : 0.0f, mx};
}

std::array<float, 3> hsv_to_rgb(float h, float s, float v) {
    h = rem_euclid(h, 360.0f) / 60.0f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    switch (static_cast<unsigned>(h)) {
        case 0: r = c, g = x, b = 0; break;
        case 1: r = x, g = c, b = 0; break;
        case 2: r = 0, g = c, b = x; break;
        case 3: r = 0, g = x, b = c; break;
        case 4: r = x, g = 0, b = c; break;
        default: r = c, g = 0, b = x; break;
    }
    const float m = v - c;
    return {r + m, g + m, b + m};
}

// camera_raw.rs lut_eval: a table over 0..1, carried on straight past it (float documents).
float lut_eval(const std::vector<float>& lut, float v) {
    if (!(v >= 0.0f && v <= 1.0f)) return v < 0.0f ? v + lut.front() : lut.back() + (v - 1.0f);
    const float x = v * static_cast<float>(lut.size() - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(x), lut.size() - 2);
    const float t = x - static_cast<float>(i);
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

// camera_raw.rs parametric_lut: each region's slider moves it by up to a quarter of the range, smoothly; kept monotone.
std::vector<float> parametric_lut(const Settings& p, std::size_t n) {
    std::array<float, 3> s{};
    for (std::size_t k = 0; k < 3; ++k) s[k] = std::clamp(p.curve_splits[k], 5.0f, 95.0f) / 100.0f;
    const std::array<float, 4> centres{s[0] / 2.0f, (s[0] + s[1]) / 2.0f, (s[1] + s[2]) / 2.0f, (s[2] + 1.0f) / 2.0f};
    const std::array<float, 4> amounts{p.curve_shadows, p.curve_darks, p.curve_lights, p.curve_highlights};
    const std::array<float, 4> widths{std::max(s[0], 0.1f), std::max(s[1] - s[0], 0.1f), std::max(s[2] - s[1], 0.1f), std::max(1.0f - s[2], 0.1f)};
    std::vector<float> lut(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(n - 1);
        float y = x;
        for (std::size_t k = 0; k < 4; ++k) {
            const float d = (x - centres[k]) / (widths[k] * 1.2f);
            const float bump = std::exp(-d * d * 2.0f) * x * (1.0f - x) * 4.0f;
            y += amounts[k] / 100.0f * 0.25f * bump;
        }
        lut[i] = std::clamp(y, 0.0f, 1.0f);
    }
    for (std::size_t i = 1; i < n; ++i) lut[i] = std::max(lut[i], lut[i - 1]);
    return lut;
}

// camera_raw.rs box_mean: the mean of a (2r + 1) square of a w × h plane, its edges repeated (rows, then columns; running
// sums in f64) — for what is worked out once on the whole (the haze's proxy, the tone's subsampled bases).
std::vector<float> box_mean(const std::vector<float>& v, int w, int h, int r) {
    if (r <= 0 || w <= 0 || h <= 0) return v;
    const double n = 2.0 * r + 1.0;
    std::vector<float> tmp(v.size());
    for (int y = 0; y < h; ++y) {
        const float* src = v.data() + std::size_t(y) * std::size_t(w);
        const auto at = [&](int k) { return static_cast<double>(src[std::clamp(k, 0, w - 1)]); };
        double acc = 0;
        for (int k = -r; k <= r; ++k) acc += at(k);
        float* row = tmp.data() + std::size_t(y) * std::size_t(w);
        for (int x = 0; x < w; ++x) {
            row[x] = static_cast<float>(acc / n);
            acc += at(x + r + 1) - at(x - r);
        }
    }
    std::vector<float> out(v.size());
    std::vector<double> acc(static_cast<std::size_t>(w), 0.0);
    const auto row = [&](int y) { return tmp.data() + std::size_t(std::clamp(y, 0, h - 1)) * std::size_t(w); };
    for (int y = -r; y <= r; ++y) {
        const float* q = row(y);
        for (int x = 0; x < w; ++x) acc[std::size_t(x)] += q[x];
    }
    for (int y = 0; y < h; ++y) {
        float* o = out.data() + std::size_t(y) * std::size_t(w);
        for (int x = 0; x < w; ++x) o[x] = static_cast<float>(acc[std::size_t(x)] / n);
        const float* add = row(y + r + 1);
        const float* sub = row(y - r);
        for (int x = 0; x < w; ++x) acc[std::size_t(x)] += static_cast<double>(add[x] - sub[x]);
    }
    return out;
}

// The guided filter's coefficients of a plane guiding itself (He et al. 2010; camera_raw.rs guided with p = i): the mean
// a and b over a (2r + 1) square, so the filtered value at a pixel is a·i + b.
void guided_self(const std::vector<float>& i, int w, int h, int r, float eps, std::vector<float>& ma, std::vector<float>& mb) {
    const std::vector<float> mi = box_mean(i, w, h, r);
    std::vector<float> ii(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) ii[k] = i[k] * i[k];
    const std::vector<float> mii = box_mean(ii, w, h, r);
    std::vector<float> a(i.size()), b(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) {
        a[k] = (mii[k] - mi[k] * mi[k]) / (mii[k] - mi[k] * mi[k] + eps);
        b[k] = mi[k] - a[k] * mi[k];
    }
    ma = box_mean(a, w, h, r);
    mb = box_mean(b, w, h, r);
}

}  // namespace

std::array<float, 8> band_weights(float hue) {
    std::array<float, 8> w{};
    for (std::size_t k = 0; k < 8; ++k) {
        const float c = kHslBands[k], prev = kHslBands[(k + 7) % 8], next = kHslBands[(k + 1) % 8];
        const float diff = std::fmod(hue - c + 540.0f, 360.0f) - 180.0f;
        const float span = diff >= 0 ? rem_euclid(next - c, 360.0f) : rem_euclid(c - prev, 360.0f);
        w[k] = std::max(1.0f - std::fabs(diff) / std::max(span, 1e-3f), 0.0f);
    }
    return w;
}

std::vector<float> curve_lut(const std::vector<std::array<float, 2>>& given, std::size_t n) {
    std::vector<std::array<float, 2>> p;
    for (const auto& q : given) p.push_back({q[0] / 255.0f, q[1] / 255.0f});
    std::stable_sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    std::vector<std::array<float, 2>> kept;
    for (const auto& q : p) {
        if (!kept.empty() && std::fabs(q[0] - kept.back()[0]) < 1e-6f) continue;
        kept.push_back(q);
    }
    p = std::move(kept);
    std::vector<float> out(n);
    if (p.size() < 2) {
        for (std::size_t k = 0; k < n; ++k) out[k] = static_cast<float>(k) / static_cast<float>(n - 1);
        return out;
    }
    const std::size_t m = p.size();
    std::vector<float> d(m - 1), t(m, 0.0f);
    for (std::size_t k = 0; k + 1 < m; ++k) d[k] = (p[k + 1][1] - p[k][1]) / std::max(p[k + 1][0] - p[k][0], 1e-6f);
    t[0] = d[0];
    t[m - 1] = d[m - 2];
    for (std::size_t k = 1; k + 1 < m; ++k) t[k] = d[k - 1] * d[k] <= 0 ? 0.0f : (d[k - 1] + d[k]) / 2.0f;
    for (std::size_t k = 0; k + 1 < m; ++k) {
        if (d[k] == 0) {
            t[k] = 0;
            t[k + 1] = 0;
            continue;
        }
        const float a = t[k] / d[k], b = t[k + 1] / d[k];
        const float s = a * a + b * b;
        if (s > 9.0f) {
            const float tau = 3.0f / std::sqrt(s);
            t[k] = tau * a * d[k];
            t[k + 1] = tau * b * d[k];
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(n - 1);
        if (x <= p[0][0]) {
            out[i] = p[0][1];
            continue;
        }
        if (x >= p[m - 1][0]) {
            out[i] = p[m - 1][1];
            continue;
        }
        std::size_t k = m - 2;
        for (std::size_t j = 0; j + 1 < m; ++j) {
            if (x <= p[j + 1][0]) {
                k = j;
                break;
            }
        }
        const float hh = p[k + 1][0] - p[k][0];
        const float s = (x - p[k][0]) / hh;
        const float h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s, h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
        out[i] = std::clamp(h00 * p[k][1] + h10 * hh * t[k] + h01 * p[k + 1][1] + h11 * hh * t[k + 1], 0.0f, 1.0f);
    }
    return out;
}

}  // namespace genko::render::camera_raw

namespace genko::render::fx {

using core::Json;
namespace cr = camera_raw;

namespace {

using cr::luma;
using cr::smoothstep;

// The whole numbers the local stages' box passes count in (exact sums: the same bytes whatever the tile): a luminance's
// 0..1 in 2⁻²⁴ steps, the detail's planes (any value, HDR's) in 2⁻²⁴ steps up to ±2²⁰, the luminance's squares in 2⁻³⁰.
constexpr double kUnit = 1.0 / 16777216.0;
constexpr double kFineUnit = 1.0 / 1073741824.0;
constexpr double kMost = 1048576.0;

// upstream's gauss (three box passes, Kovesi; none below 0.3) as box radii, and how far they reach.
std::vector<int> gauss_radii(float sigma) {
    if (!(sigma >= 0.3f)) return {};
    const auto r = gauss_box_radii(sigma);
    return {r[0], r[1], r[2]};
}
int reach_of(const std::vector<int>& radii) { return std::accumulate(radii.begin(), radii.end(), 0); }

// How far each tile's local stages reach past it (the layer's pixels): テクスチャ's Gaussian of the luminance (before the
// tone), and ディテール's — its luminance's guided filter (two box means) then the sharpening's blur and its mask's blur and
// slope, or the colour noise's blur.
struct Reach {
    std::vector<int> texture;  // (box radii)
    int nl_radius = 0;         // (the guided filter's)
    float nl_eps = 0;
    std::vector<int> colour, sharpen, mask;
    int tex = 0, detail = 0;
};

Reach reach(const cr::Settings& p, double s, int long_side) {
    Reach out;
    const float ps = static_cast<float>(std::max(s, 0.01));
    const float long_f = static_cast<float>(long_side);
    if (p.texture != 0) out.texture = gauss_radii(std::max(long_f * 0.002f, 1.5f * ps));
    out.tex = reach_of(out.texture);
    if (p.noise_luminance > 0 || p.noise_color > 0 || p.sharpen_amount > 0) {
        int luminance = 0;
        if (p.noise_luminance > 0) {
            const float n = p.noise_luminance / 100.0f;
            out.nl_radius = static_cast<int>(std::max(std::round((1.0f + 3.0f * n) * ps), 1.0f));
            out.nl_eps = (n * 0.06f) * (n * 0.06f) + 1e-6f;
            luminance = 2 * out.nl_radius;
        }
        if (p.noise_color > 0) out.colour = gauss_radii(std::max(p.noise_color / 100.0f * 6.0f * ps, 0.3f));
        int sharp = 0;
        if (p.sharpen_amount > 0) {
            out.sharpen = gauss_radii(std::max(std::clamp(p.sharpen_radius, 0.5f, 3.0f) * ps, 0.3f));
            sharp = reach_of(out.sharpen);
            if (p.sharpen_masking > 0) {
                out.mask = gauss_radii(1.0f * std::max(ps, 0.5f));
                sharp = std::max(sharp, reach_of(out.mask) + 1);
            }
        }
        out.detail = std::max(luminance + sharp, reach_of(out.colour));
    }
    return out;
}

// A guided base's coefficients over the frame (camera_raw.rs guided_fast: at 1/step of the frame, upsampled bilinearly):
// the base at a pixel of the frame (lx, ly) whose luminance is i is a(…)·i + b(…).
struct Base {
    int w = 0, h = 0, step = 1;
    std::vector<float> a, b;
    float at(int lx, int ly, float i) const {
        const float sf = static_cast<float>(step);
        const float fx = (static_cast<float>(lx) + 0.5f) / sf - 0.5f, fy = (static_cast<float>(ly) + 0.5f) / sf - 0.5f;
        return cr::bilinear(a, w, h, fx, fy) * i + cr::bilinear(b, w, h, fx, fy);
    }
};

// What the kind works out once on the frame (Kernel::prepare) and the settings' tables, for its tiles.
struct Developing : Prepared {
    cr::Settings p;
    bool identity = true;
    bool floats = false;
    Rect frame;  // (on the surface's grid)
    int long_side = 1;
    double s = 1;
    int x0 = 0, y0 = 0;  // (Ctx::x0, y0: the grid's place of the surface's (0, 0), for 粒子)
    std::array<float, 3> gain{1, 1, 1};
    // かすみの除去
    bool haze = false;
    std::array<float, 3> air{};
    int k = 1, tw = 0, th = 0;
    std::vector<float> t;
    // the tone's bases
    std::optional<Base> base, clarity;
    Reach reach;
    // the colour stages' tables
    bool colour = false;
    std::vector<float> par, master, red, green, blue;
    std::array<std::array<float, 3>, 4> tints{};
    float blend = 0, bal = 0;
    std::optional<image_ops::Held> held;
};

// Stages 1–2 on a pixel's colours (at (lx, ly) of the frame): white balance and exposure in linear light, then the haze.
void early(const Developing& d, float* q, int lx, int ly) {
    if (!d.p.wb_neutral()) {
        for (std::size_t c = 0; c < 3; ++c) q[c] = cr::linear_to_srgb(std::max(cr::srgb_to_linear(q[c]) * d.gain[c], 0.0f));
    }
    if (d.haze) {
        const float ks = static_cast<float>(d.k);
        const float tv = std::clamp(cr::bilinear(d.t, d.tw, d.th, (static_cast<float>(lx) + 0.5f) / ks - 0.5f, (static_cast<float>(ly) + 0.5f) / ks - 0.5f), 0.1f, 1.0f);
        const float amount = d.p.dehaze / 100.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            const float i = q[c];
            const float j = (i - d.air[c]) / tv + d.air[c];
            q[c] = amount >= 0 ? i + (j - i) * amount : i + (d.air[c] - i) * (-amount) * 0.5f * (1.0f - tv * 0.5f);
        }
    }
}

// The window's pixel at (x, y) of the grid (past the window: its edge's).
const float* pixel_at(const Window& w, int x, int y) {
    return w.px(std::clamp(x, w.rect.x0, w.rect.x1 - 1), std::clamp(y, w.rect.y0, w.rect.y1 - 1));
}

// camera_raw.rs guided (p guided by i, He et al. 2010) on the haze's proxy: a·i + b of each pixel.
std::vector<float> guided(const std::vector<float>& i, const std::vector<float>& p, int w, int h, int r, float eps) {
    const std::vector<float> mi = cr::box_mean(i, w, h, r), mp = cr::box_mean(p, w, h, r);
    std::vector<float> ip(i.size()), ii(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) {
        ip[k] = i[k] * p[k];
        ii[k] = i[k] * i[k];
    }
    const std::vector<float> mip = cr::box_mean(ip, w, h, r), mii = cr::box_mean(ii, w, h, r);
    std::vector<float> a(i.size()), b(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) {
        a[k] = (mip[k] - mi[k] * mp[k]) / (mii[k] - mi[k] * mi[k] + eps);
        b[k] = mp[k] - a[k] * mi[k];
    }
    const std::vector<float> ma = cr::box_mean(a, w, h, r), mb = cr::box_mean(b, w, h, r);
    std::vector<float> out(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) out[k] = ma[k] * i[k] + mb[k];
    return out;
}

// What Camera Raw works out once on the whole frame (Kernel::prepare): the gains, the haze's atmospheric light and
// transmission (camera_raw.rs dehaze: the dark channel of a proxy of at most 768 pixels, its brightest 0.1 %, refined by
// the guided filter), the tone's guided bases (guided_fast: the frame's clamped luminance after stages 1–2 averaged over
// squares of r/8 pixels — 1 to 8 —, their coefficients upsampled bilinearly), the colour stages' tables. Its planes held in
// the imaging budget while it lives.
std::shared_ptr<const Prepared> prepare(const Json& params, const Window& whole, const Ctx& ctx) {
    auto d = std::make_shared<Developing>();
    d->p = cr::settings_of(params);
    d->identity = d->p.identity();
    d->floats = ctx.levels == 0 && ctx.ink == nullptr;
    d->frame = ctx.bounds;
    d->s = ctx.s > 0 ? ctx.s : 1.0;
    d->x0 = ctx.x0;
    d->y0 = ctx.y0;
    if (d->identity || whole.rect.empty() || whole.ch != 4 || d->frame.empty()) {
        d->identity = true;
        return d;
    }
    const cr::Settings& p = d->p;
    const int w = d->frame.width(), h = d->frame.height();
    d->long_side = std::max(w, h);
    d->reach = reach(p, d->s, d->long_side);
    // 1. the white balance's and exposure's gains (luminance kept)
    if (!p.wb_neutral()) {
        const float t = p.temperature / 100.0f, tn = p.tint / 100.0f;
        std::array<float, 3> g{1.0f + 0.35f * t, 1.0f - 0.25f * tn, 1.0f - 0.35f * t};
        const float norm = luma(g);
        for (float& v : g) v = v / norm * std::pow(2.0f, p.exposure);
        d->gain = g;
    }
    // (the planes worked out here, held before they are had)
    const int k = std::max(1, (std::max(w, h) + 767) / 768);
    const int sw = (w + k - 1) / k, sh = (h + k - 1) / k;
    const std::uint64_t haze_px = p.dehaze != 0 ? std::uint64_t(sw) * std::uint64_t(sh) : 0;
    const float long_f = static_cast<float>(d->long_side);
    const bool bases = !p.tone_neutral() && (p.highlights != 0 || p.shadows != 0 || p.clarity != 0);
    const int base_r = std::max(static_cast<int>(long_f * 0.015f), 2), clarity_r = std::max(static_cast<int>(long_f * 0.02f), 3);
    const int base_step = std::clamp(base_r / 8, 1, 8), clarity_step = std::clamp(clarity_r / 8, 1, 8);
    const auto cells = [w, h](int step) { return std::uint64_t((w + step - 1) / step) * std::uint64_t((h + step - 1) / step); };
    std::uint64_t base_px = 0;
    if (bases && (p.highlights != 0 || p.shadows != 0)) base_px += cells(base_step);
    if (bases && p.clarity != 0) base_px += cells(clarity_step);
    // (the proxy's colours, counts, channels and the guided filter's planes; the bases' sums and their guided planes)
    d->held.emplace(haze_px * (28 * sizeof(float) + sizeof(std::size_t)) + base_px * (2 * sizeof(double) + 12 * sizeof(float)));
    // 2. かすみの除去
    if (p.dehaze != 0) {
        std::vector<float> small(std::size_t(sw) * std::size_t(sh) * 3, 0.0f), count(std::size_t(sw) * std::size_t(sh), 0.0f);
        for (int ly = 0; ly < h; ++ly) {
            segment::check_stop();
            for (int lx = 0; lx < w; ++lx) {
                float q[4];
                std::copy_n(pixel_at(whole, d->frame.x0 + lx, d->frame.y0 + ly), 4, q);
                early(*d, q, lx, ly);  // (stage 1: the haze is not there yet)
                const std::size_t i = std::size_t(ly / k) * std::size_t(sw) + std::size_t(lx / k);
                for (std::size_t c = 0; c < 3; ++c) small[i * 3 + c] += std::clamp(q[c], 0.0f, 1.0f);
                count[i] += 1.0f;
            }
        }
        for (std::size_t i = 0; i < count.size(); ++i)
            for (std::size_t c = 0; c < 3; ++c) small[i * 3 + c] /= std::max(count[i], 1.0f);
        const int r = std::max(std::max(sw, sh) / 60, 2);
        const auto min_filter = [sw, sh, r](const std::vector<float>& v) {
            std::vector<float> a(v.size()), b(v.size());
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    float least = std::numeric_limits<float>::max();
                    for (int xx = std::max(0, x - r); xx <= std::min(x + r, sw - 1); ++xx) least = std::min(least, v[std::size_t(y) * std::size_t(sw) + std::size_t(xx)]);
                    a[std::size_t(y) * std::size_t(sw) + std::size_t(x)] = least;
                }
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    float least = std::numeric_limits<float>::max();
                    for (int yy = std::max(0, y - r); yy <= std::min(y + r, sh - 1); ++yy) least = std::min(least, a[std::size_t(yy) * std::size_t(sw) + std::size_t(x)]);
                    b[std::size_t(y) * std::size_t(sw) + std::size_t(x)] = least;
                }
            return b;
        };
        const std::size_t n = std::size_t(sw) * std::size_t(sh);
        std::vector<float> channel(n);
        for (std::size_t i = 0; i < n; ++i) channel[i] = std::min({small[i * 3], small[i * 3 + 1], small[i * 3 + 2]});
        const std::vector<float> dark = min_filter(channel);
        // (the atmospheric light: the mean colour of the brightest 0.1 % of the dark channel)
        std::vector<std::size_t> idx(n);
        std::iota(idx.begin(), idx.end(), std::size_t(0));
        std::stable_sort(idx.begin(), idx.end(), [&dark](std::size_t a, std::size_t b) { return dark[a] > dark[b]; });
        const std::size_t top = std::max<std::size_t>(n / 1000, 1);
        std::array<float, 3> air{};
        for (std::size_t j = 0; j < top; ++j)
            for (std::size_t c = 0; c < 3; ++c) air[c] += small[idx[j] * 3 + c] / static_cast<float>(top);
        for (float& v : air) v = std::max(v, 0.2f);
        for (std::size_t i = 0; i < n; ++i) channel[i] = std::min({small[i * 3] / air[0], small[i * 3 + 1] / air[1], small[i * 3 + 2] / air[2]});
        const std::vector<float> norm_dark = min_filter(channel);
        std::vector<float> raw(n), guide(n);
        for (std::size_t i = 0; i < n; ++i) {
            raw[i] = 1.0f - 0.95f * norm_dark[i];
            guide[i] = luma(&small[i * 3]);
        }
        d->t = guided(guide, raw, sw, sh, r * 2, 1e-3f);
        d->tw = sw;
        d->th = sh;
        d->k = k;
        d->air = air;
        d->haze = true;
    }
    // 3. the tone's guided bases: the frame's clamped luminance after stages 1–2, averaged over squares of `step`
    if (bases) {
        struct Wanted {
            int step, r;
            float eps;
            std::optional<Base>* into;
            std::vector<double> sum, count;
            int bw = 0, bh = 0;
        };
        std::vector<Wanted> wanted;
        if (p.highlights != 0 || p.shadows != 0) wanted.push_back(Wanted{base_step, base_r, 0.02f, &d->base, {}, {}});
        if (p.clarity != 0) wanted.push_back(Wanted{clarity_step, clarity_r, 0.005f, &d->clarity, {}, {}});
        for (Wanted& one : wanted) {
            one.bw = (w + one.step - 1) / one.step;
            one.bh = (h + one.step - 1) / one.step;
            one.sum.assign(std::size_t(one.bw) * std::size_t(one.bh), 0.0);
            one.count.assign(one.sum.size(), 0.0);
        }
        for (int ly = 0; ly < h; ++ly) {
            segment::check_stop();
            for (int lx = 0; lx < w; ++lx) {
                float q[4];
                std::copy_n(pixel_at(whole, d->frame.x0 + lx, d->frame.y0 + ly), 4, q);
                early(*d, q, lx, ly);
                const float lc = std::min(std::max(luma(q), 0.0f), 1.0f);
                for (Wanted& one : wanted) {
                    const std::size_t i = std::size_t(ly / one.step) * std::size_t(one.bw) + std::size_t(lx / one.step);
                    one.sum[i] += lc;
                    one.count[i] += 1.0;
                }
            }
        }
        for (Wanted& one : wanted) {
            std::vector<float> si(one.sum.size());
            for (std::size_t i = 0; i < si.size(); ++i) si[i] = static_cast<float>(one.sum[i] / std::max(one.count[i], 1.0));
            one.sum = {};
            one.count = {};
            Base base;
            base.w = one.bw;
            base.h = one.bh;
            base.step = one.step;
            cr::guided_self(si, one.bw, one.bh, std::max(one.r / one.step, 1), one.eps, base.a, base.b);
            *one.into = std::move(base);
        }
    }
    // 4–7. the colour stages' tables
    d->colour = p.vibrance != 0 || p.saturation != 0 || !p.curves_neutral() || !p.hsl_neutral() || !p.grade_neutral();
    if (d->colour) {
        if (p.curve_highlights != 0 || p.curve_lights != 0 || p.curve_darks != 0 || p.curve_shadows != 0) d->par = cr::parametric_lut(p, 1024);
        const auto lut = [](const std::vector<std::array<float, 2>>& c) { return cr::is_linear(c) ? std::vector<float>() : cr::curve_lut(c, 1024); };
        d->master = lut(p.point_curve);
        d->red = lut(p.point_curve_red);
        d->green = lut(p.point_curve_green);
        d->blue = lut(p.point_curve_blue);
        const auto tint = [](const cr::Wheel& wh) {
            const std::array<float, 3> c = cr::hsv_to_rgb(wh.hue, 1.0f, 1.0f);
            const float l = luma(c);
            return std::array<float, 3>{(c[0] - l) * wh.sat / 100.0f * 0.3f, (c[1] - l) * wh.sat / 100.0f * 0.3f, (c[2] - l) * wh.sat / 100.0f * 0.3f};
        };
        d->tints = {tint(p.grade_shadows), tint(p.grade_midtones), tint(p.grade_highlights), tint(p.grade_global)};
        d->blend = 0.15f + std::clamp(p.grade_blending, 0.0f, 100.0f) / 100.0f * 0.35f;
        d->bal = std::clamp(p.grade_balance, -100.0f, 100.0f) / 100.0f * 0.25f;
    }
    return d;
}

// Stages 4–7 on a pixel's colours: presence, the tone curves, the colour mixer, colour grading.
void colours(const Developing& d, float* q) {
    const cr::Settings& p = d.p;
    std::array<float, 3> c{q[0], q[1], q[2]};
    if (p.vibrance != 0 || p.saturation != 0) {
        const float l = luma(c);
        const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
        const float sat = mx > 1e-6f ? (mx - mn) / mx : 0.0f;
        // (vibrance protects skin-ish hues — oranges — a little, as ACR)
        const float hue = cr::rgb_to_hsv(c)[0];
        const float skin = std::clamp(1.0f - std::fabs(hue - 25.0f) / 25.0f, 0.0f, 1.0f) * 0.5f;
        const float kk = std::max(1.0f + p.saturation / 100.0f + p.vibrance / 100.0f * (1.0f - sat) * (1.0f - skin), 0.0f);
        for (float& v : c) v = l + (v - l) * kk;
    }
    if (!d.par.empty())
        for (float& v : c) v = cr::lut_eval(d.par, v);
    if (!d.master.empty())
        for (float& v : c) v = cr::lut_eval(d.master, v);
    if (!d.red.empty()) c[0] = cr::lut_eval(d.red, c[0]);
    if (!d.green.empty()) c[1] = cr::lut_eval(d.green, c[1]);
    if (!d.blue.empty()) c[2] = cr::lut_eval(d.blue, c[2]);
    if (!p.hsl_neutral()) {
        const std::array<float, 3> hsv = cr::rgb_to_hsv({std::max(c[0], 0.0f), std::max(c[1], 0.0f), std::max(c[2], 0.0f)});
        if (hsv[1] > 1e-4f) {
            const std::array<float, 8> wts = cr::band_weights(hsv[0]);
            float dh = 0, ds = 0, dl = 0;
            for (std::size_t k = 0; k < 8; ++k) {
                dh += wts[k] * p.hsl_hue[k];
                ds += wts[k] * p.hsl_sat[k];
                dl += wts[k] * p.hsl_lum[k];
            }
            const float nh = hsv[0] + dh / 100.0f * 30.0f;
            const float ns = std::clamp(hsv[1] * (1.0f + ds / 100.0f), 0.0f, 1.0f);
            std::array<float, 3> out = cr::hsv_to_rgb(nh, ns, hsv[2]);
            // (the luminance kept, then the luminance's shift weighted by the saturation)
            const float l0 = luma(c), l1 = luma(out);
            if (l1 > 1e-6f)
                for (float& v : out) v = v * l0 / l1;
            const float kk = std::pow(2.0f, dl / 100.0f * 1.0f * hsv[1]);
            for (std::size_t i = 0; i < 3; ++i) c[i] = out[i] * kk;
        }
    }
    if (!p.grade_neutral()) {
        const float l = std::clamp(luma(c), 0.0f, 1.0f);
        const float sh = 1.0f - smoothstep(0.0f, 0.5f + d.bal + d.blend, l);
        const float hi = smoothstep(0.5f + d.bal - d.blend, 1.0f, l);
        const float mid = std::max(1.0f - sh - hi, 0.0f);
        const std::array<float, 4> masks{sh, mid, hi, 1.0f};
        const std::array<float, 4> lums{p.grade_shadows.lum, p.grade_midtones.lum, p.grade_highlights.lum, p.grade_global.lum};
        for (std::size_t k = 0; k < 4; ++k) {
            for (std::size_t ch = 0; ch < 3; ++ch) c[ch] += masks[k] * d.tints[k][ch];
            if (lums[k] != 0) {
                const float f = std::pow(2.0f, masks[k] * lums[k] / 100.0f * 0.6f);
                for (float& v : c) v *= f;
            }
        }
    }
    q[0] = c[0];
    q[1] = c[1];
    q[2] = c[2];
}

// A box mean of radius r of a w × h plane in whole numbers of `unit` (exact sums: fx::whole_box_passes), its edges repeated.
void box_whole(std::vector<float>& v, int w, int h, int r, double unit) {
    if (r > 0) whole_box_passes(v, std::size_t(w), std::size_t(h), 1, {r}, unit, kMost);
}

// The guided filter of a plane guiding itself (camera_raw.rs detail's luminance noise reduction), its box means in whole
// numbers.
void guided_whole(std::vector<float>& y, int w, int h, int r, float eps) {
    std::vector<float> mi = y, mii(y.size());
    for (std::size_t k = 0; k < y.size(); ++k) mii[k] = y[k] * y[k];
    box_whole(mi, w, h, r, kFineUnit);
    box_whole(mii, w, h, r, kFineUnit);
    std::vector<float>& a = mii;  // (in place: what is left of them is a and b)
    std::vector<float> b(y.size());
    for (std::size_t k = 0; k < y.size(); ++k) {
        const float var = mii[k] - mi[k] * mi[k];
        a[k] = var / (var + eps);
        b[k] = mi[k] - a[k] * mi[k];
    }
    mi = {};
    box_whole(a, w, h, r, kUnit);
    box_whole(b, w, h, r, kUnit);
    for (std::size_t k = 0; k < y.size(); ++k) y[k] = a[k] * y[k] + b[k];
}

// The bytes a tile's run holds (Kernel::tile_bytes): its part grown by the local stages' reach — the pixels, their
// luminance and テクスチャ's plane in floats and whole numbers —, and the detail's planes over the part grown by its reach.
std::uint64_t tile_bytes(const cr::Settings& p, double s, int long_side, int side) {
    if (p.identity()) return 0;
    const Reach r = reach(p, s, long_side);
    const std::uint64_t r1 = std::uint64_t(side) + 2 * std::uint64_t(r.detail), r2 = r1 + 2 * std::uint64_t(r.tex);
    return r2 * r2 * (7 * sizeof(float) + sizeof(std::int64_t)) + (r.detail > 0 ? r1 * r1 * (10 * sizeof(float) + sizeof(std::int64_t)) : 0);
}

// One tile: its part of the frame grown by what the local stages reach (R2: stages 1–2, the luminance, テクスチャ's
// Gaussian; R1: stages 3–7, ディテール's planes), then stage 8 and the effects on the tile. A pixel outside the frame or clear
// keeps its values; the alpha is never changed.
std::vector<float> develop(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    std::shared_ptr<const Prepared> own;
    const auto* d = dynamic_cast<const Developing*>(ctx.prepared);
    if (d == nullptr) {  // (a run without the engine's prepare: the whole window its frame's)
        own = prepare(params, src, ctx);
        d = static_cast<const Developing*>(own.get());
    }
    std::vector<float> res = src.crop(out);
    if (d->identity || src.ch != 4) return res;
    const cr::Settings& p = d->p;
    const Rect img = d->frame.intersect(src.rect);
    const Rect inner = out.intersect(img);
    if (inner.empty()) return res;
    const Reach& reach = d->reach;
    const Rect r1 = inner.inflate(reach.detail).intersect(img);
    const Rect r2 = r1.inflate(reach.tex).intersect(img);
    const int w2 = r2.width(), h2 = r2.height();
    const std::size_t n2 = r2.area();
    const auto at2 = [&r2, w2](int x, int y) { return std::size_t(y - r2.y0) * std::size_t(w2) + std::size_t(x - r2.x0); };
    // 1–2 over R2, and the luminance
    std::vector<float> px(n2 * 4), l(n2), lc(n2);
    for (int y = r2.y0; y < r2.y1; ++y)
        for (int x = r2.x0; x < r2.x1; ++x) {
            const std::size_t i = at2(x, y);
            float* q = px.data() + i * 4;
            std::copy_n(src.px(x, y), 4, q);
            early(*d, q, x - d->frame.x0, y - d->frame.y0);
            l[i] = std::max(luma(q), 0.0f);
            lc[i] = std::min(l[i], 1.0f);
        }
    // 3. tone on the luminance (over R1), the colour's ratios kept
    if (!p.tone_neutral()) {
        std::vector<float> tex;
        if (!reach.texture.empty()) {
            tex = lc;
            whole_box_passes(tex, std::size_t(w2), std::size_t(h2), 1, reach.texture, kUnit, 1.0);
        }
        const float k_con = p.contrast / 100.0f * 0.6f;
        const float tau = 2.0f * std::numbers::pi_v<float>;
        for (int y = r1.y0; y < r1.y1; ++y)
            for (int x = r1.x0; x < r1.x1; ++x) {
                const std::size_t i = at2(x, y);
                const int lx = x - d->frame.x0, ly = y - d->frame.y0;
                const float l0 = l[i];
                float lv = lc[i];
                if (d->base) {
                    const float bv = std::clamp(d->base->at(lx, ly, lc[i]), 0.0f, 1.0f);
                    const float hm = smoothstep(0.45f, 0.95f, bv);
                    const float sm = 1.0f - smoothstep(0.05f, 0.5f, bv);
                    lv *= std::pow(2.0f, p.highlights / 100.0f * 1.2f * hm + p.shadows / 100.0f * 1.4f * sm);
                }
                if (d->clarity) {
                    const float mw = 1.0f - std::pow(2.0f * std::clamp(lv, 0.0f, 1.0f) - 1.0f, 2.0f);
                    lv += p.clarity / 100.0f * 0.8f * (lc[i] - d->clarity->at(lx, ly, lc[i])) * mw;
                }
                if (!tex.empty()) lv += p.texture / 100.0f * 0.9f * std::clamp(lc[i] - tex[i], -0.1f, 0.1f);
                if (p.whites != 0) lv += p.whites / 100.0f * 0.25f * smoothstep(0.4f, 1.0f, lv);
                if (p.blacks != 0) lv += p.blacks / 100.0f * 0.15f * (1.0f - smoothstep(0.0f, 0.45f, lv));
                if (k_con != 0) {
                    const float xx = std::clamp(lv, 0.0f, 1.0f);
                    lv += -k_con * std::sin(tau * xx) / tau;
                }
                lv = std::max(lv, 0.0f) + (l0 - lc[i]);
                const float ratio = l0 > 1e-5f ? lv / l0 : 1.0f;
                float* q = px.data() + i * 4;
                for (std::size_t c = 0; c < 3; ++c) q[c] = l0 > 1e-5f ? q[c] * ratio : q[c] + (lv - l0);
            }
    }
    // 4–7 over R1
    if (d->colour) {
        for (int y = r1.y0; y < r1.y1; ++y)
            for (int x = r1.x0; x < r1.x1; ++x) colours(*d, px.data() + at2(x, y) * 4);
    }
    // 8. detail (Y and the colour differences B − Y, R − Y over R1; good on the tile)
    if (p.noise_luminance > 0 || p.noise_color > 0 || p.sharpen_amount > 0) {
        const int w1 = r1.width(), h1 = r1.height();
        const std::size_t n1 = r1.area();
        std::vector<float> yl(n1), cb(n1), cr(n1);
        for (int y = r1.y0; y < r1.y1; ++y)
            for (int x = r1.x0; x < r1.x1; ++x) {
                const std::size_t j = std::size_t(y - r1.y0) * std::size_t(w1) + std::size_t(x - r1.x0);
                const float* q = px.data() + at2(x, y) * 4;
                yl[j] = luma(q);
                cb[j] = q[2] - yl[j];
                cr[j] = q[0] - yl[j];
            }
        if (p.noise_luminance > 0) {
            std::vector<float> smooth = yl;
            guided_whole(smooth, w1, h1, reach.nl_radius, reach.nl_eps);
            const float keep = p.noise_luminance_detail / 100.0f * 0.5f;
            for (std::size_t j = 0; j < n1; ++j) yl[j] = smooth[j] + (yl[j] - smooth[j]) * keep;
        }
        if (p.noise_color > 0 && !reach.colour.empty()) {
            const float keep = p.noise_color_detail / 100.0f * 0.3f;
            for (std::vector<float>* plane : {&cb, &cr}) {
                std::vector<float> blurred = *plane;
                whole_box_passes(blurred, std::size_t(w1), std::size_t(h1), 1, reach.colour, kUnit, kMost);
                for (std::size_t j = 0; j < n1; ++j) (*plane)[j] = blurred[j] + ((*plane)[j] - blurred[j]) * keep;
            }
        }
        if (p.sharpen_amount > 0) {
            std::vector<float> blur = yl;
            whole_box_passes(blur, std::size_t(w1), std::size_t(h1), 1, reach.sharpen, kUnit, kMost);
            // (masking: an edge mask from the slope of a smoothed luminance)
            std::vector<float> mask;
            if (p.sharpen_masking > 0) {
                std::vector<float> sm = yl;
                whole_box_passes(sm, std::size_t(w1), std::size_t(h1), 1, reach.mask, kUnit, kMost);
                const float t = p.sharpen_masking / 100.0f * 0.08f;
                mask.resize(n1);
                const auto s_at = [&sm, w1, h1](int x, int y) { return sm[std::size_t(std::clamp(y, 0, h1 - 1)) * std::size_t(w1) + std::size_t(std::clamp(x, 0, w1 - 1))]; };
                for (int y = 0; y < h1; ++y)
                    for (int x = 0; x < w1; ++x) {
                        const float gx = s_at(x + 1, y) - s_at(x - 1, y), gy = s_at(x, y + 1) - s_at(x, y - 1);
                        mask[std::size_t(y) * std::size_t(w1) + std::size_t(x)] = smoothstep(t * 0.5f, t + 1e-4f, std::hypot(gx, gy));
                    }
            }
            const float amt = p.sharpen_amount / 100.0f * 1.2f;
            const float det = p.sharpen_detail / 100.0f;
            // (a low Detail damps large — halo-making — differences)
            const float limit = 0.03f + det * 0.3f;
            for (std::size_t j = 0; j < n1; ++j) {
                const float hp = std::clamp(yl[j] - blur[j], -limit, limit) * (0.5f + 0.5f * det);
                yl[j] += amt * hp * (mask.empty() ? 1.0f : mask[j]);
            }
        }
        for (int y = inner.y0; y < inner.y1; ++y)
            for (int x = inner.x0; x < inner.x1; ++x) {
                const std::size_t j = std::size_t(y - r1.y0) * std::size_t(w1) + std::size_t(x - r1.x0);
                float* q = px.data() + at2(x, y) * 4;
                const float rr = cr[j] + yl[j], bb = cb[j] + yl[j];
                q[0] = rr;
                q[1] = (yl[j] - 0.2126f * rr - 0.0722f * bb) / 0.7152f;
                q[2] = bb;
            }
    }
    // 9. the effects on the tile: the post-crop vignette (about the frame's centre), then grain (by the working 200 dpi's
    // grid); the result kept within 0..1 but on a 32-bit precise layer
    const float fw = static_cast<float>(d->frame.width()), fh = static_cast<float>(d->frame.height());
    const float cx = fw / 2.0f, cy = fh / 2.0f;
    const float amt = p.vignette_amount / 100.0f;
    const float mid = std::clamp(p.vignette_midpoint, 0.0f, 100.0f) / 100.0f;
    const float feather = std::clamp(p.vignette_feather, 0.0f, 100.0f) / 100.0f;
    const float round = std::clamp(p.vignette_roundness, -100.0f, 100.0f) / 100.0f;
    const float hl = std::clamp(p.vignette_highlights, 0.0f, 100.0f) / 100.0f;
    float ax = cx, ay = cy;
    if (round >= 0) {  // (0 follows the frame's aspect, +1 a circle, −1 a rounded rectangle)
        const float r = std::min(cx, cy) + (std::max(cx, cy) - std::min(cx, cy)) * (1.0f - round);
        ax = cx >= cy ? r : cx;
        ay = cy > cx ? r : cy;
    }
    const float pw = 2.0f + std::max(-round, 0.0f) * 6.0f;
    const float inner_edge = 1.0f - mid * 0.8f;
    const float edge0 = inner_edge - feather * 0.5f * inner_edge, edge1 = inner_edge + feather * 0.5f * (1.4f - inner_edge) + 0.02f;
    const float cell = 1.0f + p.grain_size / 25.0f, rough = p.grain_roughness / 100.0f, grain = p.grain_amount / 100.0f * 0.12f;
    const auto seed = static_cast<std::uint64_t>(p.seed);
    const double s = d->s;
    for (int y = inner.y0; y < inner.y1; ++y)
        for (int x = inner.x0; x < inner.x1; ++x) {
            float* q = px.data() + at2(x, y) * 4;
            if (p.vignette_amount != 0) {
                const float dx = std::fabs((static_cast<float>(x - d->frame.x0) + 0.5f - cx) / ax), dy = std::fabs((static_cast<float>(y - d->frame.y0) + 0.5f - cy) / ay);
                const float r = std::pow(std::pow(dx, pw) + std::pow(dy, pw), 1.0f / pw);
                const float m = smoothstep(edge0, edge1, r);
                if (m > 0) {
                    const float lv = std::clamp(luma(q), 0.0f, 1.0f);
                    if (p.vignette_style == "paint_overlay") {
                        const float target = amt < 0 ? 0.0f : 1.0f;
                        const float kk = m * std::fabs(amt);
                        for (std::size_t c = 0; c < 3; ++c) q[c] += (target - q[c]) * kk;
                    } else {
                        float kk = std::pow(2.0f, amt * 2.0f * m);
                        if (amt < 0 && hl > 0) kk += (1.0f - kk) * smoothstep(0.6f, 1.0f, lv) * hl;  // (bright areas punch through)
                        if (p.vignette_style == "color_priority") {
                            for (std::size_t c = 0; c < 3; ++c) q[c] += lv * kk - lv;
                        } else {
                            for (std::size_t c = 0; c < 3; ++c) q[c] *= kk;
                        }
                    }
                }
            }
            if (p.grain_amount > 0) {
                const float fx = static_cast<float>((x + d->x0) / s) / cell, fy = static_cast<float>((y + d->y0) / s) / cell;
                const float nv = cr::value_noise(fx, fy, seed) * (1.0f - rough) + cr::value_noise(fx * 2.3f, fy * 2.3f, seed + 7) * rough;
                const float lv = std::clamp(luma(q), 0.0f, 1.0f);
                const float g = (nv - 0.5f) * 2.0f * grain * (0.4f + 2.4f * lv * (1.0f - lv));
                for (std::size_t c = 0; c < 3; ++c) q[c] += g;
            }
            if (!d->floats)
                for (std::size_t c = 0; c < 3; ++c) q[c] = std::clamp(q[c], 0.0f, 1.0f);
            float* o = res.data() + (std::size_t(y - out.y0) * std::size_t(out.width()) + std::size_t(x - out.x0)) * 4;
            if (!(o[3] > 0)) continue;  // (a clear pixel as it was)
            for (std::size_t c = 0; c < 3; ++c) o[c] = std::isfinite(q[c]) ? q[c] : 0.0f;
        }
    return res;
}

Kernel camera_raw_kernel() {
    Kernel k;
    k.kind = "camera_raw";
    k.shown_frame = true;
    k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
    // (a pixel's work by the stages on: the sRGB curve's powers, the haze, the tone and its texture, the colour stages, the
    // detail's box passes and guided filter, the effects — and the frame read once more for what is prepared)
    k.cost = [](const Json& params, const Ctx& ctx) {
        const cr::Settings p = cr::settings_of(params);
        if (p.identity()) return 1.0;
        const Reach r = reach(p, ctx.s, 0);
        double cost = 20;
        if (!p.wb_neutral()) cost += 2 * 60;
        if (p.dehaze != 0) cost += 60;
        if (!p.tone_neutral()) cost += 40 + 6.0 * static_cast<double>(r.texture.size()) * 2;
        if (p.highlights != 0 || p.shadows != 0 || p.clarity != 0) cost += 40;
        if (p.vibrance != 0 || p.saturation != 0 || !p.curves_neutral() || !p.hsl_neutral() || !p.grade_neutral()) cost += 80;
        if (r.detail > 0) cost += 60 + 24.0 * static_cast<double>(r.sharpen.size() + r.mask.size() + r.colour.size() * 2) + (r.nl_radius > 0 ? 80 : 0);
        if (p.vignette_amount != 0) cost += 40;
        if (p.grain_amount > 0) cost += 40;
        return cost;
    };
    k.prepare = prepare;
    k.tile_bytes = [](const Json& params, const Ctx& ctx, int side) {
        return tile_bytes(cr::settings_of(params), ctx.s, std::max(ctx.bounds.width(), ctx.bounds.height()), side);
    };
    k.run = develop;
    return k;
}

}  // namespace

Kernels camera_raw_kernels() { return {camera_raw_kernel()}; }

}  // namespace genko::render::fx
