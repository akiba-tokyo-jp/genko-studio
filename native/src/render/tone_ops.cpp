// PC2c (局所の補正, SPEC.md COLOR-04): the local corrections of filter_raster — shadows_highlights, replace_color,
// match_color — and the op hdr_toning (design 3.4; render/ops_adjust.hpp). Their formulas are PhotoCraft v0.2.0's
// algo/tone.rs (1-364), algo/selection.rs:360-385 (color_range), algo/fxutil.rs (gauss_blur_n), algo/other2.rs (HSL)
// and color/convert.rs:150-207 (Lab), computed in float as it computes them (MIT: third_party/photocraft/NOTICE.md).
//
// Each works the RGB a layer's pixels hold — an 8-bit layer's bytes, a precise layer's samples (the page's values; HDR
// cut to 0..1 where PhotoCraft cuts), the colours an ink layer's inks mean, back by the page's rules (render/ink:
// what it leaves as it was keeps its inks). Sizes in pixels (radii) are the working 200 dpi's, scaled to the layer's
// resolution.

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

#include "core/areas.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ops_adjust.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace image_ops {

namespace {

using Rgb = std::array<float, 3>;

float luma(const Rgb& c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
float luma(const Rgba& c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Rust's f32::rem_euclid
float rem_euclid(float a, float b) {
    const float r = std::fmod(a, b);
    return r < 0.0f ? r + std::fabs(b) : r;
}

// One box pass of radius r along every row of a w × n interleaved buffer (sums in double: where a window starts does
// not change its rounding; the edges repeated).
void box_rows(std::vector<float>& buf, std::size_t w, std::size_t n, std::size_t r, std::vector<float>& scratch) {
    if (r == 0 || w == 0) return;
    const double norm = 1.0 / static_cast<double>(2 * r + 1);
    const auto last = static_cast<std::ptrdiff_t>(w) - 1;
    const auto rr = static_cast<std::ptrdiff_t>(r);
    for (std::size_t start = 0; start + w * n <= buf.size(); start += w * n) {
        float* row = buf.data() + start;
        scratch.assign(row, row + w * n);
        const auto at = [&](std::ptrdiff_t x, std::size_t c) { return static_cast<double>(scratch[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(x, 0, last)) * n + c]); };
        for (std::size_t c = 0; c < n; ++c) {
            double acc = 0;
            for (std::ptrdiff_t x = -rr; x <= rr; ++x) acc += at(x, c);
            for (std::size_t x = 0; x < w; ++x) {
                row[x * n + c] = static_cast<float>(acc * norm);
                const auto xi = static_cast<std::ptrdiff_t>(x);
                acc += at(xi + rr + 1, c) - at(xi - rr, c);
            }
        }
    }
}

// The same along the columns: every column's running sum kept at once, row by row (each column's sums in the same order
// as one column walked alone).
void box_cols(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, std::size_t r, std::vector<float>& scratch) {
    if (r == 0 || h == 0) return;
    const double norm = 1.0 / static_cast<double>(2 * r + 1);
    const auto last = static_cast<std::ptrdiff_t>(h) - 1;
    const auto rr = static_cast<std::ptrdiff_t>(r);
    const std::size_t span = w * n;
    scratch.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(span * h));
    const auto row = [&](std::ptrdiff_t y) { return scratch.data() + static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(y, 0, last)) * span; };
    std::vector<double> acc(span, 0.0);
    for (std::ptrdiff_t y = -rr; y <= rr; ++y) {
        const float* in = row(y);
        for (std::size_t i = 0; i < span; ++i) acc[i] += static_cast<double>(in[i]);
    }
    for (std::size_t y = 0; y < h; ++y) {
        float* out = buf.data() + y * span;
        for (std::size_t i = 0; i < span; ++i) out[i] = static_cast<float>(acc[i] * norm);
        const auto yi = static_cast<std::ptrdiff_t>(y);
        const float* add = row(yi + rr + 1);
        const float* take = row(yi - rr);
        for (std::size_t i = 0; i < span; ++i) acc[i] += static_cast<double>(add[i]) - static_cast<double>(take[i]);
    }
}

// Blurred luminance (Gaussian, `radius` px ≈ 2σ as Photoshop's radius fields).
std::vector<float> blurred_luma(const std::vector<Rgba>& px, std::size_t w, std::size_t h, float radius) {
    std::vector<float> l(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) l[i] = luma(px[i]);
    gauss_blur(l, w, h, 1, std::max(radius, 0.0f) / 2.0f);
    return l;
}

// Levels stretched so `black`% of the pixels clip to 0 and `white`% to 1 (a luminance histogram of 1024 bins).
void clip_stretch(std::vector<Rgba>& px, float black, float white) {
    if (black <= 0.0f && white <= 0.0f) return;
    std::array<std::uint32_t, 1024> hist{};
    std::uint32_t n = 0;
    for (const Rgba& q : px) {
        if (!(q[3] > 0.0f)) continue;
        hist[static_cast<std::size_t>(std::round(std::clamp(luma(q), 0.0f, 1.0f) * 1023.0f))] += 1;
        n += 1;
    }
    if (n == 0) return;
    const auto find = [&](float pct, bool rev) {
        const auto target = static_cast<std::uint32_t>(std::clamp(pct, 0.0f, 50.0f) / 100.0f * static_cast<float>(n));
        std::uint32_t acc = 0;
        for (std::size_t k = 0; k < 1024; ++k) {
            const std::size_t i = rev ? 1023 - k : k;
            acc += hist[i];
            if (acc > target) return static_cast<float>(i) / 1023.0f;
        }
        return rev ? 0.0f : 1.0f;
    };
    const float lo = find(black, false);
    const float hi = find(white, true);
    if (hi - lo < 1e-3f) return;
    for (Rgba& q : px) {
        if (!(q[3] > 0.0f)) continue;
        for (unsigned c = 0; c < 3; ++c) q[c] = std::clamp((q[c] - lo) / (hi - lo), 0.0f, 1.0f);
    }
}

float hue_of(const Rgb& c, float mx, float d) {
    if (d <= 0.0f) return 0.0f;
    float h = 0;
    if (mx == c[0]) {
        h = rem_euclid((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return h / 6.0f;
}

Rgb from_hcm(float h, float chroma, float m) {
    const float hp = rem_euclid(h, 1.0f) * 6.0f;
    const float x = chroma * (1.0f - std::fabs(rem_euclid(hp, 2.0f) - 1.0f));
    Rgb c{};
    switch (static_cast<int>(hp)) {
        case 0: c = {chroma, x, 0}; break;
        case 1: c = {x, chroma, 0}; break;
        case 2: c = {0, chroma, x}; break;
        case 3: c = {0, x, chroma}; break;
        case 4: c = {x, 0, chroma}; break;
        default: c = {chroma, 0, x}; break;
    }
    return {c[0] + m, c[1] + m, c[2] + m};
}

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

constexpr std::array<float, 3> kD50{0.96422f, 1.0f, 0.82521f};

float curve_eval(const std::vector<std::pair<float, float>>& pts, float x) {
    if (pts.size() < 2) return x;
    if (x <= pts[0].first) return pts[0].second;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        if (x <= pts[i + 1].first) {
            const float span = std::max(pts[i + 1].first - pts[i].first, 1e-6f);
            return pts[i].second + (pts[i + 1].second - pts[i].second) * (x - pts[i].first) / span;
        }
    }
    return pts.back().second;
}

}  // namespace

std::array<std::size_t, 3> gauss_box_radii(float sigma) {
    const float n = 3.0f;
    const float w_ideal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = static_cast<int>(std::floor(w_ideal));
    if (wl % 2 == 0) wl -= 1;
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const float m_value = (12.0f * sigma * sigma - n * static_cast<float>(wl * wl) - 4.0f * n * static_cast<float>(wl) - 3.0f * n) /
                          (-4.0f * static_cast<float>(wl) - 4.0f);
    const auto m = static_cast<std::size_t>(std::clamp(std::round(m_value), 0.0f, n));
    std::array<std::size_t, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = i < m ? static_cast<std::size_t>(wl - 1) / 2 : static_cast<std::size_t>(wu - 1) / 2;
    return out;
}

void gauss_blur(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, float sigma) {
    if (sigma < 0.3f) return;
    // (box_cols copies the buffer whole, and keeps a row of sums in double: held in the budget)
    const Held held(std::uint64_t(w) * h * n * sizeof(float) + std::uint64_t(w) * n * sizeof(double));
    std::vector<float> scratch;
    scratch.reserve(std::max(w, h) * n);
    const auto radii = gauss_box_radii(sigma);
    for (const std::size_t r : radii) box_rows(buf, w, n, r, scratch);
    for (const std::size_t r : radii) box_cols(buf, w, h, n, r, scratch);
}

void shadows_highlights(std::vector<Rgba>& px, std::size_t w, std::size_t h, const ShadowsHighlights& p) {
    if (px.empty()) return;
    const bool one = std::fabs(p.highlight_radius - p.shadow_radius) < 1e-3f;
    const Held lumas(std::uint64_t(px.size()) * sizeof(float) * (one ? 1 : 2));  // (the blurred luminances, in the budget)
    const std::vector<float> ls = blurred_luma(px, w, h, p.shadow_radius);
    const std::vector<float> own = one ? std::vector<float>() : blurred_luma(px, w, h, p.highlight_radius);
    const std::vector<float>& lh = one ? ls : own;
    const float sa = p.shadow_amount / 100.0f, ha = p.highlight_amount / 100.0f;
    const float st = std::max(p.shadow_tone / 100.0f, 0.01f), ht = std::max(p.highlight_tone / 100.0f, 0.01f);
    const float color = p.color / 100.0f;
    const float mid = p.midtone / 100.0f;
    for (std::size_t i = 0; i < px.size(); ++i) {
        Rgba& q = px[i];
        if (q[3] <= 0.0f) continue;
        // mask weights: 1 in deep shadows (bright highlights), fading out at the tonal width
        const float ms = 1.0f - smoothstep(0.0f, st, ls[i]);
        const float mh = smoothstep(1.0f - ht, 1.0f, lh[i]);
        Rgb out{q[0], q[1], q[2]};
        if (sa > 0.0f && ms > 0.0f) {  // (lifted: a gamma, stronger for darker neighbourhoods)
            const float g = 1.0f / (1.0f + 2.0f * sa * ms);
            for (float& v : out) v = std::pow(std::max(v, 0.0f), g);
        }
        if (ha > 0.0f && mh > 0.0f) {
            const float g = 1.0f / (1.0f + 2.0f * ha * mh);
            for (float& v : out) v = 1.0f - std::pow(1.0f - std::min(v, 1.0f), g);
        }
        if (mid != 0.0f) {
            const float k = 1.0f + mid * std::clamp(1.0f - ms - mh, 0.0f, 1.0f);
            for (float& v : out) v = (v - 0.5f) * k + 0.5f;
        }
        const float changed = std::min(ms * sa + mh * ha, 1.0f);
        if (color != 0.0f && changed > 0.0f) {
            const float l1 = luma(out);
            const float s = 1.0f + color * changed;
            for (float& v : out) v = l1 + (v - l1) * s;
        }
        for (unsigned c = 0; c < 3; ++c) q[c] = std::clamp(out[c], 0.0f, 1.0f);
    }
    clip_stretch(px, p.black_clip, p.white_clip);
}

std::vector<float> color_range(const std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness) {
    const float f = std::max(fuzziness, 0.0f) + 1.0f;
    std::vector<float> out(px.size(), 0.0f);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& p = px[i];
        if (p[3] <= 0.0f) continue;
        float d = 0.0f;
        for (unsigned c = 0; c < 3; ++c) d = std::max(d, std::fabs(p[c] - color[c]) * 255.0f);
        out[i] = std::clamp(1.0f - d / f, 0.0f, 1.0f);
    }
    return out;
}

std::array<float, 3> rgb_to_hsl(const std::array<float, 3>& c) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    const float d = mx - mn;
    const float l = (mx + mn) / 2.0f;
    const float s = d <= 0.0f ? 0.0f : d / std::max(1.0f - std::fabs(2.0f * l - 1.0f), 1e-7f);
    return {hue_of(c, mx, d), std::clamp(s, 0.0f, 1.0f), l};
}

std::array<float, 3> hsl_to_rgb(const std::array<float, 3>& c) {
    const float chroma = (1.0f - std::fabs(2.0f * c[2] - 1.0f)) * c[1];
    return from_hcm(c[0], chroma, c[2] - chroma / 2.0f);
}

std::vector<float> replace_color(std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness, float hue, float saturation,
                                 float lightness) {
    const Held held(std::uint64_t(px.size()) * sizeof(float));  // (its coverage, in the budget while it works)
    std::vector<float> mask = color_range(px, color, fuzziness);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const float k = mask[i];
        if (k <= 0.0f) continue;
        Rgba& q = px[i];
        const Rgb c{q[0], q[1], q[2]};
        const auto [hh, ss, ll] = rgb_to_hsl(c);
        const float s = saturation >= 0.0f ? ss + (1.0f - ss) * saturation / 100.0f : ss * (1.0f + saturation / 100.0f);
        const float l = lightness >= 0.0f ? ll + (1.0f - ll) * lightness / 100.0f : ll * (1.0f + lightness / 100.0f);
        const Rgb o = hsl_to_rgb({rem_euclid(hh + hue / 360.0f, 1.0f), std::clamp(s, 0.0f, 1.0f), std::clamp(l, 0.0f, 1.0f)});
        for (unsigned j = 0; j < 3; ++j) q[j] = std::clamp(c[j] + (o[j] - c[j]) * k, 0.0f, 1.0f);
    }
    return mask;
}

std::array<float, 3> srgb_to_lab(const std::array<float, 3>& rgb) {
    const Rgb lin{srgb_to_linear(rgb[0]), srgb_to_linear(rgb[1]), srgb_to_linear(rgb[2])};
    constexpr float m[3][3] = {{0.436074f, 0.385064f, 0.143080f}, {0.222504f, 0.716878f, 0.060618f}, {0.013932f, 0.097104f, 0.714173f}};
    Rgb xyz{};
    for (unsigned i = 0; i < 3; ++i) xyz[i] = (m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2]) / kD50[i];
    const float e = 6.0f / 29.0f;
    const auto f = [e](float t) { return t > e * e * e ? std::cbrt(t) : t / (3.0f * (e * e)) + 4.0f / 29.0f; };
    const float fx = f(xyz[0]), fy = f(xyz[1]), fz = f(xyz[2]);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

std::array<float, 3> lab_to_srgb(const std::array<float, 3>& lab) {
    const float fy = (lab[0] + 16.0f) / 116.0f;
    const float fx = fy + lab[1] / 500.0f;
    const float fz = fy - lab[2] / 200.0f;
    const float e = 6.0f / 29.0f;
    const auto finv = [e](float t) { return t > e ? t * t * t : 3.0f * (e * e) * (t - 4.0f / 29.0f); };
    const Rgb xyz{finv(fx) * kD50[0], finv(fy) * kD50[1], finv(fz) * kD50[2]};
    constexpr float m[3][3] = {{3.133856f, -1.616867f, -0.490615f}, {-0.978768f, 1.916142f, 0.033454f}, {0.071945f, -0.228991f, 1.405243f}};
    Rgb out{};
    for (unsigned i = 0; i < 3; ++i) {
        const float lin = m[i][0] * xyz[0] + m[i][1] * xyz[1] + m[i][2] * xyz[2];
        out[i] = linear_to_srgb(std::clamp(lin, 0.0f, 1.0f));
    }
    return out;
}

std::optional<LabStats> lab_stats(const std::vector<Rgba>& px, const std::vector<float>* mask) {
    std::array<double, 3> sum{}, sq{};
    double wsum = 0;
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& q = px[i];
        const double w = static_cast<double>(q[3] * (mask == nullptr ? 1.0f : i < mask->size() ? (*mask)[i] : 0.0f));
        if (w <= 0.0) continue;
        const Rgb lab = srgb_to_lab({q[0], q[1], q[2]});
        for (unsigned k = 0; k < 3; ++k) {
            sum[k] += w * static_cast<double>(lab[k]);
            sq[k] += w * static_cast<double>(lab[k]) * static_cast<double>(lab[k]);
        }
        wsum += w;
    }
    if (wsum <= 0.0) return std::nullopt;
    LabStats out;
    for (unsigned k = 0; k < 3; ++k) {
        const double mean = sum[k] / wsum;
        out.mean[k] = static_cast<float>(mean);
        out.std[k] = static_cast<float>(std::sqrt(std::max(sq[k] / wsum - mean * mean, 0.0)));
    }
    return out;
}

void match_color(std::vector<Rgba>& px, const LabStats& target, const LabStats* source, const MatchColor& o) {
    const LabStats src = source != nullptr ? *source : target;
    const float lum = o.luminance / 100.0f;
    const float inten = o.intensity / 100.0f;
    const float fade = std::clamp(o.fade / 100.0f, 0.0f, 1.0f);
    const auto scale = [&](unsigned k) { return target.std[k] > 1e-4f ? src.std[k] / target.std[k] : 1.0f; };
    std::array<float, 3> dst_mean = src.mean;
    if (o.neutralize) dst_mean[1] = dst_mean[2] = 0.0f;
    for (Rgba& q : px) {
        if (!(q[3] > 0.0f)) continue;
        const Rgb c{q[0], q[1], q[2]};
        const Rgb lab = srgb_to_lab(c);
        const float l = ((lab[0] - target.mean[0]) * scale(0) + dst_mean[0]) * lum;
        const float a = ((lab[1] - target.mean[1]) * scale(1) + dst_mean[1]) * inten;
        const float b = ((lab[2] - target.mean[2]) * scale(2) + dst_mean[2]) * inten;
        const Rgb out = lab_to_srgb({std::clamp(l, 0.0f, 100.0f), a, b});
        for (unsigned i = 0; i < 3; ++i) q[i] = std::clamp(out[i] + (c[i] - out[i]) * fade, 0.0f, 1.0f);
    }
}

void hdr_toning(std::vector<Rgba>& px, std::size_t w, std::size_t h, const HdrToning& p) {
    if (px.empty()) return;
    constexpr float kEps = 1e-4f;
    const Held held(std::uint64_t(px.size()) * sizeof(float) * 2);  // (the log luminance and its blur, in the budget)
    std::vector<float> logl(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) logl[i] = std::log(std::max(luma(px[i]), 0.0f) + kEps);
    std::vector<float> base = logl;
    gauss_blur(base, w, h, 1, std::max(p.radius, 1.0f) / 2.0f);
    std::size_t opaque = 0;
    float total = 0.0f;
    for (std::size_t i = 0; i < px.size(); ++i) {
        if (!(px[i][3] > 0.0f)) continue;
        ++opaque;
        total += base[i];
    }
    const float mean = total / static_cast<float>(std::max<std::size_t>(opaque, 1));
    const float compress = 1.0f / (1.0f + std::max(p.strength, 0.0f));
    const float detail = 1.0f + p.detail / 100.0f;
    const float exposure = std::pow(2.0f, p.exposure);
    const float gamma = std::clamp(p.gamma, 0.1f, 4.0f);
    std::vector<std::pair<float, float>> sorted = p.curve;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < px.size(); ++i) {
        Rgba& q = px[i];
        if (q[3] <= 0.0f) continue;
        const Rgb c{q[0], q[1], q[2]};
        const float l0 = std::max(luma(c), 0.0f) + kEps;
        const float new_log = mean + (base[i] - mean) * compress + (logl[i] - base[i]) * detail;
        float l1 = std::pow(std::max(std::exp(new_log) * exposure, 0.0f), gamma);
        l1 += p.shadow / 100.0f * ((1.0f - l1) * (1.0f - l1) * (1.0f - l1)) * 0.5f - p.highlight / 100.0f * (l1 * l1 * l1) * 0.5f;
        l1 = curve_eval(sorted, std::clamp(l1, 0.0f, 1.0f));
        const float ratio = l1 / l0;
        Rgb out{c[0] * ratio, c[1] * ratio, c[2] * ratio};
        // saturation, then vibrance (the less saturated move more), round the new luminance
        const float ll = luma(out);
        const float mx = std::max({out[0], out[1], out[2]}), mn = std::min({out[0], out[1], out[2]});
        const float sat_now = mx > 0.0f ? (mx - mn) / mx : 0.0f;
        const float s = 1.0f + p.saturation / 100.0f + p.vibrance / 100.0f * (1.0f - sat_now);
        for (float& v : out) v = ll + (v - ll) * std::max(s, 0.0f);
        for (unsigned k = 0; k < 3; ++k) q[k] = std::clamp(out[k], 0.0f, 1.0f);
    }
}

}  // namespace image_ops

namespace adjust_ops {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using image_ops::Rgba;

// A setting of the normal form (the table read it: a number, clamped, its default filled in).
float number(const Json& params, std::string_view key, float fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() ? static_cast<float>(v->get<double>()) : fallback;
}
bool flag(const Json& params, std::string_view key) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_boolean() && v->get<bool>();
}

// The pixels' straight RGBA (image_ops::rgba_of), held in the imaging budget.
class Buffer {
public:
    explicit Buffer(const Pixels& p) : held_(std::uint64_t(p.size()) * sizeof(Rgba)) { px = image_ops::rgba_of(p); }
    std::vector<Rgba> px;

private:
    image_ops::Held held_;
};

// The worked RGB back into the pixels, only where `cover` (none: everywhere) reaches, by its share: an ink layer's by the
// page's rules first (a colour left as it was keeps its inks).
void put_rgb(Pixels& p, const std::vector<Rgba>& px, const std::vector<float>* cover) {
    if (p.kind() == Pixels::Kind::Inks) {
        const Pixels original = cover != nullptr ? p : Pixels(Pixels::Kind::Inks, 1, 1, p.colours());
        image_ops::from_rgba(p, px);
        if (cover == nullptr) return;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const float m = (*cover)[i];
            if (m >= 1.0f) continue;
            for (unsigned c = 0; c < p.all(); ++c) p.at(i)[c] = original.at(i)[c] + (p.at(i)[c] - original.at(i)[c]) * m;
        }
        return;
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float m = cover != nullptr ? (*cover)[i] : 1.0f;
        if (m <= 0.0f) continue;
        float* v = p.at(i);
        for (unsigned c = 0; c < 4; ++c) v[c] = m >= 1.0f ? px[i][c] : v[c] + (px[i][c] - v[c]) * m;
    }
}

// What a local correction does to a layer's RGB: the pixels (rows of `w`), the grid's scale (its pixels per pixel of
// the working 200 dpi), the area's coverage on the grid (none: no area).
using Work = std::function<void(std::vector<Rgba>& px, std::size_t w, std::size_t h, double scale, const std::vector<float>* cover)>;

void run(Pixels& p, const core::Page& page, const Json* area, const Work& work) {
    const image_ops::Held covered(area != nullptr ? std::uint64_t(p.size()) * sizeof(float) : 0);
    const std::optional<std::vector<float>> cover =
        area != nullptr ? std::optional<std::vector<float>>(image_ops::coverage(page, *area, p.width(), p.height(), p.kind() == Pixels::Kind::Eight))
                        : std::nullopt;
    Buffer buffer(p);
    work(buffer.px, p.width(), p.height(), image_ops::grid_scale(page, p.width(), p.height()), cover ? &*cover : nullptr);
    put_rgb(p, buffer.px, cover ? &*cover : nullptr);
}

// The three ways of a local filter by its work (8 bits: the caller keeps the result to the area, filters.within; the
// area given to the work too when the filter reads it, match_color's stats_in_area).
LocalFilter local(const std::function<Work(const Json& params, const core::Page& page, const core::Document& doc)>& made) {
    LocalFilter out;
    out.eight = [made](const Image& rgba, const Json& params, const core::Page& page, const core::Document& doc) {
        const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
        Pixels p = image_ops::from_image(rgba);
        Buffer buffer(p);
        made(params, page, doc)(buffer.px, p.width(), p.height(), image_ops::grid_scale(page, p.width(), p.height()), nullptr);
        put_rgb(p, buffer.px, nullptr);
        return image_ops::to_image(p);
    };
    out.eight_in = [made](const Image& rgba, const Json& params, const core::Page& page, const core::Document& doc, const Json& area) {
        const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
        Pixels p = image_ops::from_image(rgba);
        const image_ops::Held covered(std::uint64_t(p.size()) * sizeof(float));
        const std::vector<float> cover = image_ops::coverage(page, area, p.width(), p.height(), true);
        Buffer buffer(p);
        made(params, page, doc)(buffer.px, p.width(), p.height(), image_ops::grid_scale(page, p.width(), p.height()), &cover);
        put_rgb(p, buffer.px, nullptr);  // (the caller keeps it to the area)
        return image_ops::to_image(p);
    };
    out.precise = [made](const core::Page& page, const core::Layer& layer, const Json& params, const Json* area, const core::Document& doc) {
        const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
        Pixels p = image_ops::from_precise(*layer.color_raster);
        run(p, page, area, made(params, page, doc));
        return image_ops::to_precise(p);
    };
    out.inks = [made](const core::Page& page, const core::InkPixels& inks, const Json& params, const Json* area, const core::Document& doc) {
        if (!page.color) throw OpError("an RGB page holds no ink pixels");
        if (inks.space != page.color->space || inks.depth != page.color->depth) throw OpError("the ink pixels are not in the page's colour mode");
        const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
        Pixels p = image_ops::from_inks(inks, *page.color);
        run(p, page, area, made(params, page, doc));
        return image_ops::to_inks(p);
    };
    return out;
}

// --- shadows_highlights ---------------------------------------------------------------------------------------------

Work shadows_highlights_work(const Json& params, const core::Page&, const core::Document&) {
    image_ops::ShadowsHighlights sh;
    sh.shadow_amount = number(params, "shadow_amount", sh.shadow_amount);
    sh.shadow_tone = number(params, "shadow_tone", sh.shadow_tone);
    sh.shadow_radius = number(params, "shadow_radius", sh.shadow_radius);
    sh.highlight_amount = number(params, "highlight_amount", sh.highlight_amount);
    sh.highlight_tone = number(params, "highlight_tone", sh.highlight_tone);
    sh.highlight_radius = number(params, "highlight_radius", sh.highlight_radius);
    sh.color = number(params, "color", sh.color);
    sh.midtone = number(params, "midtone", sh.midtone);
    sh.black_clip = number(params, "black_clip", sh.black_clip);
    sh.white_clip = number(params, "white_clip", sh.white_clip);
    return [sh](std::vector<Rgba>& px, std::size_t w, std::size_t h, double scale, const std::vector<float>*) {
        image_ops::ShadowsHighlights scaled = sh;
        scaled.shadow_radius = static_cast<float>(sh.shadow_radius * scale);
        scaled.highlight_radius = static_cast<float>(sh.highlight_radius * scale);
        image_ops::shadows_highlights(px, w, h, scaled);
    };
}

// --- replace_color --------------------------------------------------------------------------------------------------

// A colour given as sRGB [r, g, b] (0..255) in the values a page's pixels hold: an RGB page of a profile's values;
// sRGB elsewhere (an ink page's pixels are worked as the sRGB their inks mean).
std::array<float, 3> page_colour(const Json& rgb, const core::Page& page) {
    std::vector<float> c{static_cast<float>(rgb[0].get<double>() / 255.0), static_cast<float>(rgb[1].get<double>() / 255.0),
                         static_cast<float>(rgb[2].get<double>() / 255.0), 1.0f};
    if (!page.color && page.rgb && !page.rgb->profile.empty()) ink::rgb_to_rgb(c, nullptr, *page.rgb);
    return {c[0], c[1], c[2]};
}

Work replace_color_work(const Json& params, const core::Page& page, const core::Document&) {
    const Json* given = core::get(params, "color");
    if (given == nullptr || !given->is_array() || given->size() != 3) throw OpError("adjust replace_color.color is required");
    const std::array<float, 3> colour = page_colour(*given, page);
    const float fuzziness = number(params, "fuzziness", 40), hue = number(params, "hue", 0), saturation = number(params, "saturation", 0),
                lightness = number(params, "lightness", 0);
    return [=](std::vector<Rgba>& px, std::size_t, std::size_t, double, const std::vector<float>*) {
        image_ops::replace_color(px, colour, fuzziness, hue, saturation, lightness);
    };
}

// --- match_color ----------------------------------------------------------------------------------------------------

std::array<float, 3> three_numbers(const Json& v, const std::string& where, float lo, float hi) {
    if (!v.is_array() || v.size() != 3) throw OpError(where + " must be [L, a, b]");
    std::array<float, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) {
        if (!v[k].is_number() || v[k].is_boolean() || !std::isfinite(v[k].get<double>())) throw OpError(where + " must be [L, a, b] (finite numbers)");
        const float l = k == 0 ? lo : -128.0f, h = k == 0 ? hi : 128.0f;
        out[k] = std::clamp(static_cast<float>(v[k].get<double>()), l, h);
    }
    return out;
}

// source_stats: {"mean": [L, a, b], "std": [L, a, b]} (another book's, the window worked out).
image_ops::LabStats stats_given(const Json& v) {
    if (!v.is_object()) throw OpError("adjust match_color.source_stats must be {\"mean\": [L, a, b], \"std\": [L, a, b]}");
    for (const auto& [key, value] : v.items()) {
        if (key != "mean" && key != "std") throw OpError("adjust match_color.source_stats has no setting " + core::py_repr_str(key));
    }
    if (!v.contains("mean") || !v.contains("std")) throw OpError("adjust match_color.source_stats must be {\"mean\": [L, a, b], \"std\": [L, a, b]}");
    image_ops::LabStats out;
    out.mean = three_numbers(v["mean"], "adjust match_color.source_stats.mean", 0, 100);
    out.std = three_numbers(v["std"], "adjust match_color.source_stats.std", 0, 100);
    for (float& s : out.std) s = std::max(s, 0.0f);
    return out;
}

// A selection inside an op's own settings (match_color's source.area): resolved as the bus resolves an op's area.
Json resolved_area(const core::Document& doc, std::size_t at, const Json& area) {
    if (area.is_object() && (core::truthy_at(area, "poly") || core::truthy_at(area, "mask"))) return core::op_area(Json{{"area", area}});
    const auto& resolver = ops_registry().area_resolver();
    if (!resolver) throw OpError("adjust match_color.source.area cannot be read");
    return core::op_area(Json{{"area", resolver(doc, at, area)}});
}

image_ops::LabStats stats_of_source(const Json& source, const core::Page& target, const core::Document& doc) {
    if (!source.is_object()) throw OpError("adjust match_color.source must be {\"page\"?, \"layer\": id or \"merged\", \"area\"?}");
    for (const auto& [key, value] : source.items()) {
        if (key != "page" && key != "layer" && key != "area") throw OpError("adjust match_color.source has no setting " + core::py_repr_str(key));
    }
    std::size_t at = 0;
    while (at < doc.pages.size() && doc.pages[at]->id != target.id) ++at;
    if (const Json* page = core::get(source, "page"); page != nullptr && !page->is_null()) {
        try {
            at = core::require_page(doc, Json{{"page", *page}});
        } catch (const core::Error& error) {
            throw OpError(std::string("adjust match_color.source.page: ") + error.what());
        }
    }
    if (at >= doc.pages.size() || doc.is_deferred(at)) throw OpError("adjust match_color.source: the page is not in the book");
    const core::Page& page = doc.pages[at]->id == target.id ? target : doc.page(at);
    const Json* layer = core::get(source, "layer");
    if (layer != nullptr && !layer->is_null() && !layer->is_string()) throw OpError("adjust match_color.source.layer must be a layer's id or \"merged\"");
    const std::string layer_id = layer == nullptr || layer->is_null() ? std::string("merged") : layer->get<std::string>();
    // (its pixels: a layer's as they are, any other layer as it shows; merged: the page as it shows)
    const core::Layer* found = nullptr;
    for (const core::Layer& each : page.layers) {
        if (each.id == layer_id) found = &each;
    }
    if (layer_id != "merged" && found == nullptr) throw OpError("adjust match_color.source: no layer " + layer_id + " on page " + page.index.repr());
    const Pixels pixels = found != nullptr ? image_ops::shown_layer_pixels(doc, page, *found) : image_ops::page_pixels(doc, page);
    const image_ops::Held held(std::uint64_t(pixels.size()) * (sizeof(Rgba) + sizeof(float)));  // (their RGBA and the area's coverage)
    const std::vector<Rgba> px = image_ops::rgba_of(pixels);
    std::optional<std::vector<float>> mask;
    if (const Json* area = core::get(source, "area"); area != nullptr && !area->is_null()) {
        mask = image_ops::coverage(page, resolved_area(doc, at, *area), pixels.width(), pixels.height(), false);
    }
    const auto stats = image_ops::lab_stats(px, mask ? &*mask : nullptr);
    if (!stats) throw OpError("adjust match_color.source has no opaque pixels");
    return *stats;
}

Work match_color_work(const Json& params, const core::Page& page, const core::Document& doc) {
    const Json* source = core::get(params, "source");
    const Json* given = core::get(params, "source_stats");
    if (source != nullptr && source->is_null()) source = nullptr;
    if (given != nullptr && given->is_null()) given = nullptr;
    if (source != nullptr && given != nullptr) throw OpError("adjust match_color: give source or source_stats, not both");
    std::optional<image_ops::LabStats> stats;
    if (given != nullptr) stats = stats_given(*given);
    if (source != nullptr) stats = stats_of_source(*source, page, doc);
    image_ops::MatchColor options;
    options.luminance = number(params, "luminance", 100);
    options.intensity = number(params, "intensity", 100);
    options.fade = number(params, "fade", 0);
    options.neutralize = flag(params, "neutralize");
    const bool in_area = flag(params, "stats_in_area");
    return [stats, options, in_area](std::vector<Rgba>& px, std::size_t, std::size_t, double, const std::vector<float>* cover) {
        // (the target's statistics: the layer's, inside the area when asked — PhotoCraft's useSelectionInTarget)
        const auto target = image_ops::lab_stats(px, in_area ? cover : nullptr);
        if (!target) return;
        image_ops::match_color(px, *target, stats ? &*stats : nullptr, options);
    };
}

// --- hdr_toning -----------------------------------------------------------------------------------------------------

float hdr_number(const Json& op, const char* key, float fallback, float lo, float hi) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_number() || v->is_boolean()) throw OpError(std::string(key) + " must be a number");
    const double x = v->get<double>();
    if (!std::isfinite(x)) throw OpError(std::string(key) + " must be finite");
    return static_cast<float>(std::clamp(x, static_cast<double>(lo), static_cast<double>(hi)));
}

image_ops::HdrToning hdr_settings(const Json& op) {
    image_ops::HdrToning p;
    p.radius = hdr_number(op, "radius", 30, 1, 500);
    p.strength = hdr_number(op, "strength", 0.5f, 0.1f, 4);
    p.gamma = hdr_number(op, "gamma", 1, 0.1f, 2);
    p.exposure = hdr_number(op, "exposure", 0, -5, 5);
    p.detail = hdr_number(op, "detail", 30, -100, 300);
    p.shadow = hdr_number(op, "shadow", 0, -100, 100);
    p.highlight = hdr_number(op, "highlight", 0, -100, 100);
    p.vibrance = hdr_number(op, "vibrance", 0, -100, 100);
    p.saturation = hdr_number(op, "saturation", 20, -100, 100);
    if (const Json* curve = core::get(op, "curve"); curve != nullptr && !curve->is_null()) {
        const std::string message = "curve must be a list of 2 to 19 points [in, out] in 0..255";
        if (!curve->is_array() || curve->size() < 2 || curve->size() > 19) throw OpError(message);
        for (const Json& point : *curve) {
            if (!point.is_array() || point.size() != 2) throw OpError(message);
            std::array<float, 2> v{};
            for (std::size_t k = 0; k < 2; ++k) {
                if (!point[k].is_number() || point[k].is_boolean() || !std::isfinite(point[k].get<double>())) throw OpError(message);
                v[k] = static_cast<float>(std::clamp(point[k].get<double>(), 0.0, 255.0)) / 255.0f;
            }
            p.curve.emplace_back(v[0], v[1]);
        }
    }
    return p;
}

}  // namespace

void hdr_toning(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const image_ops::HdrToning settings = hdr_settings(op);
    const std::string page_id = doc.page(at).id;
    // the page's pixel layers flattened into one, over its paper (PC1's decision 1, as PhotoCraft flattens: its frames,
    // lines and balloons kept, and its name and draft layers; its hidden layers discarded)
    const int merged = flatten_count(doc.page(at)).merged;
    std::int64_t discarded = 0;
    const auto target = flatten_page(c, at, &discarded);
    if (!target) throw OpError("no layer is showing");
    std::size_t p = 0;
    while (p < doc.pages.size() && doc.pages[p]->id != page_id) ++p;
    if (p >= doc.pages.size()) throw OpError("the flattened page is not in the book");
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    Pixels pixels = image_ops::layer_pixels(c, p, *target);
    const core::Page& page = doc.page(p);
    run(pixels, page, nullptr, [&settings](std::vector<Rgba>& px, std::size_t w, std::size_t h, double scale, const std::vector<float>*) {
        image_ops::HdrToning scaled = settings;
        scaled.radius = static_cast<float>(settings.radius * scale);
        image_ops::hdr_toning(px, w, h, scaled);
    });
    image_ops::put_layer_pixels(doc, p, *target, pixels);
    c.report = Json{{"layer", *target}, {"flattened", merged}};
    if (discarded > 0) (*c.report)["discarded"] = discarded;
}

LocalFilters tone_local_filters() {
    return {{"shadows_highlights", local(shadows_highlights_work)},
            {"replace_color", local(replace_color_work)},
            {"match_color", local(match_color_work)}};
}

}  // namespace adjust_ops

}  // namespace genko::render
