// PC4 節 D4 (SPEC.md COLOR-08「フィルター」): フィルター → ぼかしギャラリー's kernels — フィールドぼかし・虹彩絞りぼかし・チルト
// シフト (a blur that changes over the picture: Gaussian levels — σ 0 and a geometric series up to the largest — mixed by
// each pixel's σ), スピンぼかし (along arcs about each pin's centre) and パスぼかし (a motion blur along the nearest paths).
// PhotoCraft v0.2.0's algo/src/gallery.rs (1-277: sigma_of, reach, variable_blur, tilt_shift, pin_distance, iris, field,
// spin, path; fxutil.rs add_sample_premul), MIT: native/third_party/photocraft/NOTICE.md. Places are shares of the base
// box (a pin's ellipse its shorter side's), パスぼかし's paths the page's mm; the blurs are pixels of the working 200 dpi —
// each kernel's distances and sample counts measured there (the same look at every resolution, design 4.2). The levels'
// box passes keep their sums in whole numbers (render/fx_d2 gauss_blur_exact): a pixel is the same whatever tile holds it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_d2.hpp"
#include "render/image_ops.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::fx {

namespace {

using core::Json;
using d2::number;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr double kWorkingPerMm = 200.0 / 25.4;

float radians(float degrees) { return degrees * kPi / 180.0f; }

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A gallery blur's amount (pixels of the working 200 dpi) as a Gaussian's σ on the layer's grid.
float sigma_of(double blur, double s) { return static_cast<float>(std::clamp(blur, 0.0, 500.0) * 0.6 * s); }

// The pixels read past an output tile for a largest σ (gallery.rs reach, upstream's r(): ⌈v⌉ + 1).
int reach_of(float sigma) { return static_cast<int>(std::ceil(3.0f * sigma + 2.0f)) + 1; }

float short_side(const Rect& b) { return std::max(static_cast<float>(std::min(b.width(), b.height())), 1.0f); }

constexpr std::size_t kLevels = 6;

// The levels of a variable blur of largest σ `smax`: σ 0, then smax / 2^(kLevels − 1) … smax.
std::vector<float> levels_of(float smax) {
    std::vector<float> lv{0.0f};
    for (std::size_t k = 0; k < kLevels; ++k) lv.push_back(smax / std::pow(2.0f, static_cast<float>(kLevels - 1 - k)));
    return lv;
}

// (review P2-3) A level of σ at least kReduceFrom is made on the picture reduced by f = 2^m — blocks of f × f of the
// grid's own places (the same blocks whatever the tile, a preview's part too), averaged, blurred there with what is left
// of σ (σ² − f²/4: the blocks' and the bilinear read's own spread) and read back bilinearly —, f the largest that keeps
// 16 pixels of σ: its work and memory the reduced picture's, made once for the whole base box (Kernel::prepare,
// Halo::whole). A smaller level is made by each tile over its part of the window grown by the level's own reach.
constexpr float kReduceFrom = 64.0f;
int reduction_of(float sigma) {
    int f = 1;
    while (sigma / static_cast<float>(2 * f) >= 16.0f && f < (1 << 20)) f *= 2;
    return f;
}
bool reduced_kind(float smax) { return smax >= kReduceFrom; }

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The reduced levels of a whole base box (Kernel::prepare): each its factor, its first block's place on the grid (in
// blocks), its size and its premultiplied blurred samples; held in the imaging budget while it lives.
struct Reduced : Prepared {
    struct Level {
        std::size_t k = 0;
        int f = 1, bx0 = 0, by0 = 0, bw = 0, bh = 0;
        std::vector<float> data;
    };
    std::vector<Level> levels;
    std::unique_ptr<image_ops::Held> held;
    const Level* level(std::size_t k) const {
        for (const Level& l : levels)
            if (l.k == k) return &l;
        return nullptr;
    }
};

std::shared_ptr<const Prepared> reduce_levels(float smax, const Window& whole, const Ctx& ctx) {
    if (!reduced_kind(smax) || whole.rect.empty()) return nullptr;
    const std::size_t n = whole.ch;
    const std::vector<float> lv = levels_of(smax);
    int fmin = 0;
    for (std::size_t k = 1; k < lv.size(); ++k)
        if (lv[k] >= kReduceFrom) fmin = fmin == 0 ? reduction_of(lv[k]) : std::min(fmin, reduction_of(lv[k]));
    if (fmin < 2) return nullptr;
    const Rect& w = whole.rect;
    const auto blocks = [&](int f) {  // (the blocks of f the window touches: first and count, across and down)
        const int bx0 = floor_div(w.x0 + ctx.x0, f), by0 = floor_div(w.y0 + ctx.y0, f);
        return std::array<int, 4>{bx0, by0, floor_div(w.x1 - 1 + ctx.x0, f) - bx0 + 1, floor_div(w.y1 - 1 + ctx.y0, f) - by0 + 1};
    };
    // what it keeps (every reduced level), then the sums of the finest blocks and their counts while it works
    auto out = std::make_shared<Reduced>();
    std::uint64_t kept = 0;
    for (std::size_t k = 1; k < lv.size(); ++k) {
        if (lv[k] < kReduceFrom) continue;
        const auto b = blocks(reduction_of(lv[k]));
        kept += std::uint64_t(b[2]) * std::uint64_t(b[3]) * n * sizeof(float);
    }
    out->held = std::make_unique<image_ops::Held>(kept);
    auto b = blocks(fmin);
    const image_ops::Held working(std::uint64_t(b[2]) * std::uint64_t(b[3]) *
                                  (n * (2 * sizeof(double) + sizeof(std::int64_t)) + 2 * sizeof(std::uint32_t)));
    std::vector<double> sum(std::size_t(b[2]) * std::size_t(b[3]) * n, 0.0);
    std::vector<std::uint32_t> count(std::size_t(b[2]) * std::size_t(b[3]), 0);
    for (int y = w.y0; y < w.y1; ++y) {
        if ((y & 63) == 0) segment::check_stop();
        const std::size_t by = std::size_t(floor_div(y + ctx.y0, fmin) - b[1]);
        for (int x = w.x0; x < w.x1; ++x) {
            const std::size_t i = by * std::size_t(b[2]) + std::size_t(floor_div(x + ctx.x0, fmin) - b[0]);
            const float* px = whole.px(x, y);
            const double a = ctx.alpha ? static_cast<double>(px[n - 1]) : 1.0;
            for (std::size_t c = 0; c < n; ++c) sum[i * n + c] += ctx.alpha && c + 1 < n ? static_cast<double>(px[c]) * a : static_cast<double>(px[c]);
            ++count[i];
        }
    }
    int f = fmin;
    for (;;) {
        for (std::size_t k = 1; k < lv.size(); ++k) {
            if (lv[k] < kReduceFrom || reduction_of(lv[k]) != f) continue;
            Reduced::Level level;
            level.k = k;
            level.f = f;
            level.bx0 = b[0];
            level.by0 = b[1];
            level.bw = b[2];
            level.bh = b[3];
            level.data.resize(sum.size());
            for (std::size_t i = 0; i < count.size(); ++i)
                for (std::size_t c = 0; c < n; ++c) level.data[i * n + c] = count[i] > 0 ? static_cast<float>(sum[i * n + c] / count[i]) : 0.0f;
            const float ff = static_cast<float>(f);
            const float rest = std::sqrt(std::max(lv[k] * lv[k] / (ff * ff) - 0.25f, 0.25f));
            d2::gauss_blur_exact(level.data, std::size_t(b[2]), std::size_t(b[3]), n, rest, ctx.largest);
            out->levels.push_back(std::move(level));
            segment::check_stop();
        }
        // (the next factor's blocks: four of these, the grid's places nesting)
        bool more = false;
        for (std::size_t k = 1; k < lv.size(); ++k) more = more || (lv[k] >= kReduceFrom && reduction_of(lv[k]) > f);
        if (!more) break;
        const auto c2 = blocks(2 * f);
        std::vector<double> sum2(std::size_t(c2[2]) * std::size_t(c2[3]) * n, 0.0);
        std::vector<std::uint32_t> count2(std::size_t(c2[2]) * std::size_t(c2[3]), 0);
        for (int j = 0; j < b[3]; ++j)
            for (int i = 0; i < b[2]; ++i) {
                const std::size_t from = std::size_t(j) * std::size_t(b[2]) + std::size_t(i);
                const std::size_t to = std::size_t(floor_div(b[1] + j, 2) - c2[1]) * std::size_t(c2[2]) + std::size_t(floor_div(b[0] + i, 2) - c2[0]);
                for (std::size_t c = 0; c < n; ++c) sum2[to * n + c] += sum[from * n + c];
                count2[to] += count[from];
            }
        sum = std::move(sum2);
        count = std::move(count2);
        b = c2;
        f *= 2;
    }
    return out;
}

// The premultiplied sample of a reduced level at the pixel (x, y) of the surface (bilinear between the blocks' centres).
void add_reduced(const Reduced::Level& l, int x, int y, const Ctx& ctx, std::size_t n, float wk, float* acc) {
    const float ff = static_cast<float>(l.f);
    const float u = std::clamp((static_cast<float>(x + ctx.x0) + 0.5f) / ff - 0.5f - static_cast<float>(l.bx0), 0.0f, static_cast<float>(l.bw - 1));
    const float v = std::clamp((static_cast<float>(y + ctx.y0) + 0.5f) / ff - 0.5f - static_cast<float>(l.by0), 0.0f, static_cast<float>(l.bh - 1));
    const int i0 = static_cast<int>(u), j0 = static_cast<int>(v);
    const int i1 = std::min(i0 + 1, l.bw - 1), j1 = std::min(j0 + 1, l.bh - 1);
    const float au = u - static_cast<float>(i0), av = v - static_cast<float>(j0);
    const float* p00 = &l.data[(std::size_t(j0) * std::size_t(l.bw) + std::size_t(i0)) * n];
    const float* p10 = &l.data[(std::size_t(j0) * std::size_t(l.bw) + std::size_t(i1)) * n];
    const float* p01 = &l.data[(std::size_t(j1) * std::size_t(l.bw) + std::size_t(i0)) * n];
    const float* p11 = &l.data[(std::size_t(j1) * std::size_t(l.bw) + std::size_t(i1)) * n];
    for (std::size_t c = 0; c < n; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * au, bottom = p01[c] + (p11[c] - p01[c]) * au;
        acc[c] += (top + (bottom - top) * av) * wk;
    }
}

// `src` blurred over `out` with each pixel's σ (`sigma(x, y)` at its centre, ≤ smax): the Gaussian levels (σ 0, then
// smax / 2^(kLevels − 1) … smax) mixed by hat weights (gallery.rs variable_blur) — a level a tile makes over its part of the
// window grown by the level's reach (the same sums whatever the tile), a large one read from the base box's reduced levels.
template <typename Sigma>
std::vector<float> variable_blur(const Window& src, const Rect& out, const Ctx& ctx, float smax, Sigma&& sigma) {
    const std::size_t n = src.ch;
    if (smax < 0.3f) return src.crop(out);
    const std::vector<float> lv = levels_of(smax);
    const auto* reduced = dynamic_cast<const Reduced*>(ctx.prepared);
    std::vector<float> sig(out.area());
    {
        std::size_t i = 0;
        for (int y = out.y0; y < out.y1; ++y)
            for (int x = out.x0; x < out.x1; ++x) sig[i++] = std::clamp(sigma(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), 0.0f, smax);
    }
    const auto weight = [&lv](std::size_t k, float s) -> float {
        const float c = lv[k];
        if (k > 0 && s >= lv[k - 1] && s <= c) return (s - lv[k - 1]) / std::max(c - lv[k - 1], 1e-6f);
        if (k + 1 < lv.size() && s >= c && s <= lv[k + 1]) return (lv[k + 1] - s) / std::max(lv[k + 1] - c, 1e-6f);
        if (k + 1 == lv.size() && s >= c) return 1.0f;
        return 0.0f;
    };
    std::vector<float> acc(out.area() * n, 0.0f), buf;
    for (std::size_t k = 0; k < lv.size(); ++k) {
        if (std::none_of(sig.begin(), sig.end(), [&](float s) { return weight(k, s) > 0.0f; })) continue;
        const Reduced::Level* small = reduced != nullptr ? reduced->level(k) : nullptr;
        if (k == 0 || small != nullptr) {  // (σ 0: the pixel itself; a large level: the reduced one)
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, ++i) {
                    const float wk = weight(k, sig[i]);
                    if (wk <= 0.0f) continue;
                    if (small != nullptr) {
                        add_reduced(*small, x, y, ctx, n, wk, &acc[i * n]);
                        continue;
                    }
                    const float* px = src.px(x, y);
                    const float a = ctx.alpha ? px[n - 1] : 1.0f;
                    for (std::size_t c = 0; c < n; ++c) acc[i * n + c] += (ctx.alpha && c + 1 < n ? px[c] * a : px[c]) * wk;
                }
            continue;
        }
        // (the window premultiplied over the tile grown by this level's reach, cut to the window: past the grid's edge its
        // edge pixel — the window is cut to the grid)
        const Rect part = out.inflate(reach_of(lv[k])).intersect(src.rect);
        const std::size_t pw = static_cast<std::size_t>(part.width()), ph = static_cast<std::size_t>(part.height());
        buf.resize(pw * ph * n);
        for (std::size_t y = 0; y < ph; ++y) {
            const float* row = src.px(part.x0, part.y0 + static_cast<int>(y));
            std::copy(row, row + pw * n, buf.begin() + static_cast<std::ptrdiff_t>(y * pw * n));
        }
        premultiply(buf, n, ctx.alpha);
        d2::gauss_blur_exact(buf, pw, ph, n, lv[k], ctx.largest);
        std::size_t i = 0;
        for (int y = out.y0; y < out.y1; ++y)
            for (int x = out.x0; x < out.x1; ++x, ++i) {
                const float wk = weight(k, sig[i]);
                if (wk <= 0.0f) continue;
                const std::size_t o = (std::size_t(y - part.y0) * pw + std::size_t(x - part.x0)) * n;
                for (std::size_t c = 0; c < n; ++c) acc[i * n + c] += buf[o + c] * wk;
            }
    }
    unpremultiply(acc, n, ctx.alpha);
    return acc;
}

// The premultiplied bilinear sample of the window at (x, y) (pixel centres at .5, its edges clamped to `clip`, inside the
// window) added into `acc` (fxutil.rs add_sample_premul).
void add_sample_premul(const Window& src, float x, float y, const Rect& clip, bool alpha, float* acc) {
    const std::size_t n = src.ch;
    const float fx = std::clamp(x - 0.5f, static_cast<float>(clip.x0), static_cast<float>(clip.x1 - 1));
    const float fy = std::clamp(y - 0.5f, static_cast<float>(clip.y0), static_cast<float>(clip.y1 - 1));
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, clip.x1 - 1), y1 = std::min(y0 + 1, clip.y1 - 1);
    const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
    const struct {
        int x, y;
        float k;
    } corners[4] = {{x0, y0, (1.0f - ax) * (1.0f - ay)}, {x1, y0, ax * (1.0f - ay)}, {x0, y1, (1.0f - ax) * ay}, {x1, y1, ax * ay}};
    for (const auto& corner : corners) {
        if (corner.k <= 0.0f) continue;
        const float* px = src.px(corner.x, corner.y);
        const float a = alpha ? px[n - 1] * corner.k : corner.k;
        if (alpha) {
            for (std::size_t c = 0; c + 1 < n; ++c) acc[c] += px[c] * a;
            acc[n - 1] += a;
        } else {
            for (std::size_t c = 0; c < n; ++c) acc[c] += px[c] * a;
        }
    }
}

void unpremultiply_px(float* px, std::size_t n, bool alpha) {
    if (!alpha) return;
    const float a = px[n - 1];
    for (std::size_t c = 0; c + 1 < n; ++c) px[c] = a > 1e-7f ? px[c] / a : 0.0f;
}

// The pins of the settings (their normal form).
struct Pin {
    float x = 0.5f, y = 0.5f, radius_x = 0.35f, radius_y = 0.25f, angle = 0, roundness = 0, feather = 0.5f, blur = 15, blur_angle = 15;
};

std::vector<Pin> pins_of(const Json& params) {
    std::vector<Pin> out;
    const Json* pins = core::get(params, "pins");
    if (pins == nullptr || !pins->is_array()) return out;
    for (const Json& p : *pins) {
        if (!p.is_object()) continue;
        Pin pin;
        pin.x = static_cast<float>(number(p, "x", 0.5));
        pin.y = static_cast<float>(number(p, "y", 0.5));
        pin.radius_x = static_cast<float>(number(p, "radius_x", 0.35));
        pin.radius_y = static_cast<float>(number(p, "radius_y", 0.25));
        pin.angle = static_cast<float>(number(p, "angle", 0));
        pin.roundness = static_cast<float>(number(p, "roundness", 0));
        pin.feather = static_cast<float>(number(p, "feather", 0.5));
        pin.blur = static_cast<float>(number(p, "blur", 15));
        pin.blur_angle = static_cast<float>(number(p, "blur_angle", 15));
        out.push_back(pin);
    }
    return out;
}

float largest_blur(const std::vector<Pin>& pins) {
    float most = 0.0f;
    for (const Pin& p : pins) most = std::max(most, p.blur);
    return most;
}

// A place's normalized (super)elliptical distance from a pin's ellipse, and its place (u, v) in the ellipse's units
// (gallery.rs pin_distance).
struct Distance {
    float e, u, v;
};
Distance pin_distance(float x, float y, const Rect& b, float px, float py, float rx, float ry, float angle, float m) {
    const float ss = short_side(b);
    const float cx = static_cast<float>(b.x0) + px * static_cast<float>(b.width()), cy = static_cast<float>(b.y0) + py * static_cast<float>(b.height());
    const float s = std::sin(radians(angle)), c = std::cos(radians(angle));
    const float dx = x - cx, dy = y - cy;
    const float u = (dx * c + dy * s) / (std::max(rx, 1e-3f) * ss), v = (-dx * s + dy * c) / (std::max(ry, 1e-3f) * ss);
    const float e = std::fabs(m - 2.0f) < 1e-3f ? std::sqrt(u * u + v * v) : std::pow(std::pow(std::fabs(u), m) + std::pow(std::fabs(v), m), 1.0f / m);
    return {e, u, v};
}

// チルトシフト: a sharp band through the centre along `angle`, blurred with the distance beyond the focus over the
// transition (shares of the base box's shorter side).
std::vector<float> tilt_shift(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const Rect& b = ctx.bounds;
    const float ss = short_side(b);
    const float cx = static_cast<float>(b.x0) + static_cast<float>(number(params, "center_x", 0.5)) * static_cast<float>(b.width());
    const float cy = static_cast<float>(b.y0) + static_cast<float>(number(params, "center_y", 0.5)) * static_cast<float>(b.height());
    const float a = radians(static_cast<float>(number(params, "angle", 0)));
    const float nx = std::sin(a), ny = std::cos(a);  // (the band's normal)
    const float smax = sigma_of(number(params, "blur", 15), ctx.s);
    const float focus = std::max(static_cast<float>(number(params, "focus", 0.1)), 0.0f);
    const float f1 = focus + std::max(static_cast<float>(number(params, "transition", 0.15)), 1e-3f);
    return variable_blur(src, out, ctx, smax, [&](float x, float y) { return smax * smoothstep(focus, f1, std::fabs((x - cx) * nx + (y - cy) * ny) / ss); });
}

// 虹彩絞りぼかし: everything outside each pin's ellipse blurred, its sharp core `feather` of it; the sharpest pin wins.
std::vector<float> iris(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::vector<Pin> pins = pins_of(params);
    if (pins.empty()) return src.crop(out);
    const Rect& b = ctx.bounds;
    const float smax = sigma_of(largest_blur(pins), ctx.s);
    return variable_blur(src, out, ctx, smax, [&](float x, float y) {
        float least = std::numeric_limits<float>::max();
        for (const Pin& p : pins) {
            const float m = 2.0f + std::clamp(p.roundness, 0.0f, 100.0f) / 100.0f * 6.0f;
            const Distance d = pin_distance(x, y, b, p.x, p.y, p.radius_x, p.radius_y, p.angle, m);
            least = std::min(least, sigma_of(p.blur, ctx.s) * smoothstep(std::clamp(p.feather, 0.0f, 0.999f), 1.0f, d.e));
        }
        return least;
    });
}

// フィールドぼかし: the pins' blurs mixed by inverse squared distance (measured in working pixels).
std::vector<float> field(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::vector<Pin> pins = pins_of(params);
    if (pins.empty()) return src.crop(out);
    const Rect& b = ctx.bounds;
    const float smax = sigma_of(largest_blur(pins), ctx.s);
    const float s = static_cast<float>(ctx.s);
    std::vector<std::array<float, 3>> pts;
    for (const Pin& p : pins)
        pts.push_back({static_cast<float>(b.x0) + p.x * static_cast<float>(b.width()), static_cast<float>(b.y0) + p.y * static_cast<float>(b.height()),
                       sigma_of(p.blur, ctx.s)});
    return variable_blur(src, out, ctx, smax, [&](float x, float y) {
        float ws = 0.0f, sum = 0.0f;
        for (const auto& [px, py, sg] : pts) {
            const float dx = (x - px) / s, dy = (y - py) / s;
            const float w = 1.0f / (dx * dx + dy * dy + 1.0f);
            ws += w;
            sum += w * sg;
        }
        return sum / ws;
    });
}

// スピンぼかし: what is inside each pin's ellipse blurred along arcs about its centre (blur_angle degrees of turn), feathered
// at its rim; the first pin holding a pixel turns it. A kind of the whole base box (its arcs reach across it).
std::vector<float> spin(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const Rect& b = ctx.bounds;
    const float ss = short_side(b);
    std::vector<float> res = src.crop(out);
    const Rect clip = b.intersect(src.rect);
    if (clip.empty()) return res;
    const std::vector<Pin> pins = pins_of(params);
    std::vector<float> acc(n);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            for (const Pin& p : pins) {
                const Distance d = pin_distance(fx, fy, b, p.x, p.y, p.radius_x, p.radius_y, p.angle, 2.0f);
                if (d.e >= 1.0f) continue;
                const float theta = radians(std::clamp(p.blur_angle, 0.0f, 360.0f));
                // (about 2 working pixels between the samples along the arc)
                const float rpx = d.e * std::max(p.radius_x, p.radius_y) * ss / static_cast<float>(ctx.s);
                const std::size_t m = static_cast<std::size_t>(std::clamp(std::ceil(theta * rpx / 2.0f), 2.0f, 64.0f));
                const float cx = static_cast<float>(b.x0) + p.x * static_cast<float>(b.width());
                const float cy = static_cast<float>(b.y0) + p.y * static_cast<float>(b.height());
                const float sa = std::sin(radians(p.angle)), ca = std::cos(radians(p.angle));
                std::fill(acc.begin(), acc.end(), 0.0f);
                for (std::size_t j = 0; j < m; ++j) {
                    const float phi = theta * (static_cast<float>(j) / static_cast<float>(m - 1) - 0.5f);
                    const float sp = std::sin(phi), cp = std::cos(phi);
                    // (turned in the ellipse's own units, then back)
                    const float ru = d.u * cp - d.v * sp, rv = d.u * sp + d.v * cp;
                    const float lu = ru * p.radius_x * ss, lv = rv * p.radius_y * ss;
                    add_sample_premul(src, cx + lu * ca - lv * sa, cy + lu * sa + lv * ca, clip, ctx.alpha, acc.data());
                }
                for (float& v : acc) v /= static_cast<float>(m);
                unpremultiply_px(acc.data(), n, ctx.alpha);
                const float k = 1.0f - smoothstep(0.85f, 1.0f, d.e);
                float* px = res.data() + i;
                for (std::size_t c = 0; c < n; ++c) px[c] += (acc[c] - px[c]) * k;
                break;
            }
        }
    return res;
}

// パスぼかし's paths on the layer's grid: their points (the page's mm — none given: upstream's one path across the middle
// of the base box), their speed (layer pixels) and taper.
struct BlurPath {
    std::vector<std::array<float, 2>> points;
    float speed = 50, taper = 0;
};

std::vector<BlurPath> paths_of(const Json& params, const Ctx& ctx) {
    std::vector<BlurPath> out;
    const Json* paths = core::get(params, "paths");
    const Rect& b = ctx.bounds;
    if (paths == nullptr || !paths->is_array()) {  // (upstream's default: [[0.2, 0.5], [0.8, 0.5]] of the base box, 50 px)
        BlurPath one;
        one.points = {{static_cast<float>(b.x0) + 0.2f * static_cast<float>(b.width()), static_cast<float>(b.y0) + 0.5f * static_cast<float>(b.height())},
                      {static_cast<float>(b.x0) + 0.8f * static_cast<float>(b.width()), static_cast<float>(b.y0) + 0.5f * static_cast<float>(b.height())}};
        one.speed = static_cast<float>(50.0 * ctx.s);
        out.push_back(std::move(one));
        return out;
    }
    const double px = kWorkingPerMm * ctx.s;
    for (const Json& p : *paths) {
        if (!p.is_object()) continue;
        BlurPath made;
        if (const Json* points = core::get(p, "points"); points != nullptr && points->is_array())
            for (const Json& q : *points) {
                if (!q.is_array() || q.size() != 2 || !q[0].is_number() || !q[1].is_number()) continue;
                made.points.push_back({static_cast<float>(q[0].get<double>() * px - ctx.x0), static_cast<float>(q[1].get<double>() * px - ctx.y0)});
            }
        made.speed = static_cast<float>(std::clamp(number(p, "speed", 50), 0.0, 500.0) * ctx.s);
        made.taper = static_cast<float>(number(p, "taper", 0));
        out.push_back(std::move(made));
    }
    return out;
}

// パスぼかし: a motion blur whose direction follows the nearest paths (their tangents weighed by inverse squared distance,
// in working pixels) and whose length is the paths' speed, tapering toward each one's end.
std::vector<float> path_blur(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const Rect& b = ctx.bounds;
    const std::vector<BlurPath> paths = paths_of(params, ctx);
    struct Seg {
        float ax, ay, bx, by;
        std::size_t path;
        float t0, t1;
    };
    std::vector<Seg> segs;
    for (std::size_t pi = 0; pi < paths.size(); ++pi) {
        const auto& pts = paths[pi].points;
        float total = 0.0f;
        for (std::size_t k = 1; k < pts.size(); ++k) total += std::hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]);
        total = std::max(total, 1e-3f);
        float run = 0.0f;
        for (std::size_t k = 1; k < pts.size(); ++k) {
            const float l = std::hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]);
            segs.push_back(Seg{pts[k - 1][0], pts[k - 1][1], pts[k][0], pts[k][1], pi, run / total, (run + l) / total});
            run += l;
        }
    }
    std::vector<float> res = src.crop(out);
    const Rect clip = b.intersect(src.rect);
    if (segs.empty() || clip.empty()) return res;
    const float s = static_cast<float>(ctx.s);
    std::vector<float> acc(n);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y) {
        if ((y - out.y0) % 16 == 0) segment::check_stop();
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            float dx = 0, dy = 0, len = 0, ws = 0;
            for (const Seg& g : segs) {
                const float sx = g.bx - g.ax, sy = g.by - g.ay;
                const float l2 = std::max(sx * sx + sy * sy, 1e-6f);
                const float t = std::clamp(((fx - g.ax) * sx + (fy - g.ay) * sy) / l2, 0.0f, 1.0f);
                const float ex = (fx - g.ax - sx * t) / s, ey = (fy - g.ay - sy * t) / s;
                const float w = 1.0f / (ex * ex + ey * ey + 1.0f);
                const float l = std::sqrt(l2);
                const BlurPath& p = paths[g.path];
                const float along = g.t0 + (g.t1 - g.t0) * t;
                dx += w * sx / l;
                dy += w * sy / l;
                len += w * std::fabs(p.speed) * (1.0f - std::clamp(p.taper, 0.0f, 100.0f) / 100.0f * along);
                ws += w;
            }
            const float m = std::sqrt(dx * dx + dy * dy);
            len /= ws;
            if (m < 1e-6f || len < 0.5f * s) continue;
            const float ux = dx / m, uy = dy / m;
            // (samples about 1.5 working pixels apart — bilinear between —, at most 64)
            const std::size_t steps = static_cast<std::size_t>(std::clamp(std::ceil(len / s / 1.5f) + 1.0f, 2.0f, 64.0f));
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (std::size_t j = 0; j < steps; ++j) {
                const float o = len * (static_cast<float>(j) / static_cast<float>(steps - 1) - 0.5f);
                add_sample_premul(src, fx + ux * o, fy + uy * o, clip, ctx.alpha, acc.data());
            }
            for (float& v : acc) v /= static_cast<float>(steps);
            unpremultiply_px(acc.data(), n, ctx.alpha);
            std::copy(acc.begin(), acc.end(), res.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return res;
}

std::size_t path_segments(const Json& params) {
    const Json* paths = core::get(params, "paths");
    if (paths == nullptr || !paths->is_array()) return 1;
    std::size_t count = 0;
    for (const Json& p : *paths) {
        if (const Json* points = core::get(p, "points"); points != nullptr && points->is_array() && !points->empty()) count += points->size() - 1;
    }
    return count;
}

double largest_speed(const Json& params) {
    const Json* paths = core::get(params, "paths");
    if (paths == nullptr || !paths->is_array()) return 50.0;
    double most = 0.0;
    for (const Json& p : *paths) most = std::max(most, std::clamp(std::fabs(number(p, "speed", 50)), 0.0, 500.0));
    return most;
}

// What a variable blur holds a pixel of its window past the engine's count (its level's float copy beside the
// premultiplied one and the exact sums: a word a sample of at most five).
constexpr std::size_t kLevelBytes = 5 * sizeof(float);
constexpr int kWholeTile = 256;  // (the engine's tile of a whole kind)

// A level's own box passes over the tile grown by its reach: their work a pixel of the tile.
double level_cost(float sigma, double tile) {
    const double margin = reach_of(sigma);
    return 14.0 * std::pow((tile + 2.0 * margin) / tile, 2.0);
}

// A variable blur's work a pixel (review P2-3: as the tiles do it): each level a tile makes itself over the tile grown by
// its reach — the tile the engine takes for that reach —; a reduced level once on its blocks and a bilinear read.
double levels_cost(float smax) {
    double each = 20.0;
    const double tile = reduced_kind(smax) ? kWholeTile : std::min(2048.0, std::max(256.0, std::ceil(2.0 * reach_of(smax) / 64.0) * 64.0));
    for (const float s : levels_of(smax)) {
        if (s <= 0.0f) continue;
        each += reduced_kind(smax) && s >= kReduceFrom ? 8.0 + 4.0 / static_cast<double>(reduction_of(s)) : level_cost(s, tile);
    }
    return each;
}

// The bytes a tile of a whole variable blur holds of its own (its largest level made by the tile: the tile grown by its
// reach, a float copy and the exact sums a sample).
std::uint64_t level_tile_bytes(float smax, const Ctx& ctx, int side) {
    if (!reduced_kind(smax)) return 0;
    float most = 0.0f;
    for (const float s : levels_of(smax))
        if (s < kReduceFrom) most = std::max(most, s);
    const std::uint64_t part = std::uint64_t(side) + 2 * std::uint64_t(reach_of(most));
    return part * part * (std::uint64_t(ctx.colours) + 1) * (sizeof(float) + sizeof(std::int64_t));
}

// A variable blur's kernel: its window the tile grown by its reach, or — a large blur — the whole base box with its
// large levels reduced once (Kernel::prepare).
template <typename Smax, typename Run>
Kernel variable_kernel(std::string_view kind, Smax smax_of, Run run, double each) {
    Kernel k;
    k.kind = kind;
    k.halo = [smax_of](const Json& params, const Ctx& ctx) {
        const float smax = smax_of(params, ctx);
        return reduced_kind(smax) ? Halo::whole() : Halo::margin(reach_of(smax));
    };
    k.work_bytes = kLevelBytes;
    k.run = run;
    k.prepare = [smax_of](const Json& params, const Window& whole, const Ctx& ctx) { return reduce_levels(smax_of(params, ctx), whole, ctx); };
    k.tile_bytes = [smax_of](const Json& params, const Ctx& ctx, int side) { return level_tile_bytes(smax_of(params, ctx), ctx, side); };
    k.cost = [smax_of, each](const Json& params, const Ctx& ctx) { return levels_cost(smax_of(params, ctx)) + each * static_cast<double>(pins_of(params).size()); };
    return k;
}

}  // namespace

Kernels blur_gallery_kernels() {
    Kernels all;
    const auto pins_smax = [](const Json& params, const Ctx& ctx) { return sigma_of(largest_blur(pins_of(params)), ctx.s); };
    // フィールドぼかし (filter.blurGallery.fieldBlur)
    all.push_back(variable_kernel(
        "blur_field", pins_smax, [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return field(params, src, out, ctx); }, 2.0));
    // 虹彩絞りぼかし (filter.blurGallery.irisBlur)
    all.push_back(variable_kernel(
        "blur_iris", pins_smax, [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return iris(params, src, out, ctx); }, 20.0));
    // チルトシフト (filter.blurGallery.tiltShift)
    all.push_back(variable_kernel(
        "blur_tilt_shift", [](const Json& params, const Ctx& ctx) { return sigma_of(number(params, "blur", 15), ctx.s); },
        [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return tilt_shift(params, src, out, ctx); }, 0.0));
    {  // パスぼかし (filter.blurGallery.pathBlur)
        Kernel k;
        k.kind = "blur_path";
        k.halo = [](const Json& params, const Ctx& ctx) {
            return d2::halo_of(static_cast<int>(std::ceil(largest_speed(params) * ctx.s / 2.0 + 1.0)) + 1);
        };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return path_blur(params, src, out, ctx); };
        k.cost = [](const Json& params, const Ctx&) { return 30.0 + 12.0 * static_cast<double>(path_segments(params)) + 64.0 * 6.0; };
        all.push_back(std::move(k));
    }
    {  // スピンぼかし (filter.blurGallery.spinBlur): its arcs reach across the base box (upstream's Halo::Bounds)
        Kernel k;
        k.kind = "blur_spin";
        k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return spin(params, src, out, ctx); };
        k.cost = [](const Json& params, const Ctx&) { return 20.0 + 64.0 * 6.0 + 10.0 * pins_of(params).size(); };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
