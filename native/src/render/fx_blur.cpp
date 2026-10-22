// PC4 節 D (SPEC.md COLOR-08): フィルター → ぼかし's kernels — D0: ぼかし（ガウス） and ぼかし（ボックス） (and the presets
// ぼかし・ぼかし（強）, which are the Gaussian at fixed radii). PhotoCraft v0.2.0's algo/src/blur.rs (1-220: gaussian_kernel,
// conv_sep, gaussian, boxes_for_gauss, gaussian_boxes, boxed), MIT: native/third_party/photocraft/NOTICE.md. The box
// passes' running sums are kept in whole numbers (the samples scaled by a power of two): a pixel's sum is then the same
// whichever window holds it — upstream's float sums depend on where the window starts — so the result does not depend
// on the tiling. D1: the rest of ぼかし — 平均 (engine/extra_cmds.rs 187-205: the selection's mean colour), ぼかし（移動）・
// ぼかし（放射状） (blur.rs 222-298: average_samples, motion, radial), ぼかし（表面） (blur.rs 300-334, by render/fx_rank) —;
// ぼかし（詳細）・ぼかし（シェイプ）・ぼかし（レンズ） in render/fx_blur2.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

namespace {

using core::Json;

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() && !v->is_boolean() ? v->get<double>() : fallback;
}

std::string text(const Json& params, std::string_view key, std::string_view fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

// The mean of the samples at the places `offsets` gives each pixel (blur.rs average_samples): each bilinear, premultiplied,
// past the window's edge its edge pixel's (the window is cut to the grid: the grid's edge repeated).
template <typename Offsets>
std::vector<float> average_samples(const Window& src, const Rect& out, Offsets&& offsets) {
    const std::size_t n = src.ch;
    std::vector<float> res;
    res.reserve(out.area() * n);
    std::vector<std::pair<float, float>> pts;
    std::vector<float> tmp(n), acc(n);
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x) {
            const float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f;
            pts.clear();
            offsets(cx, cy, pts);
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (const auto& [sx, sy] : pts) {
                src.sample(sx, sy, Edge::Repeat, src.rect, true, tmp.data());
                const float a = tmp[n - 1];
                for (std::size_t c = 0; c + 1 < n; ++c) acc[c] += tmp[c] * a;
                acc[n - 1] += a;
            }
            const float k = 1.0f / static_cast<float>(std::max<std::size_t>(pts.size(), 1));
            for (float& v : acc) v *= k;
            const float a = acc[n - 1];
            for (std::size_t c = 0; c + 1 < n; ++c) acc[c] = a > 1e-7f ? acc[c] / a : 0.0f;
            res.insert(res.end(), acc.begin(), acc.end());
        }
    return res;
}

// Box widths (odd) whose threefold convolution has standard deviation `sigma` (Kovesi; upstream's boxes_for_gauss).
std::vector<int> boxes_for_gauss(float sigma, int n) {
    const float nf = static_cast<float>(n);
    const float w_ideal = std::sqrt(12.0f * sigma * sigma / nf + 1.0f);
    int wl = static_cast<int>(std::floor(w_ideal));
    if (wl % 2 == 0) wl -= 1;
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const float m_ideal = (12.0f * sigma * sigma - nf * static_cast<float>(wl * wl) - 4.0f * nf * static_cast<float>(wl) - 3.0f * nf) /
                          (-4.0f * static_cast<float>(wl) - 4.0f);
    const int m = static_cast<int>(std::clamp(std::round(m_ideal), 0.0f, nf));
    std::vector<int> out;
    for (int i = 0; i < n; ++i) out.push_back(i < m ? wl : wu);
    return out;
}

// The window the three box passes need around the output (the sum of their radii, and one).
int boxes_margin(const std::vector<int>& boxes) {
    int margin = 1;
    for (const int w : boxes) margin += (w - 1) / 2;
    return margin;
}

// The power of two the exact sums scale the samples by: a sample's magnitude times the terms of a sum below 2^62.
int exact_bits(double largest, double terms) {
    const double room = 61.0 - std::ceil(std::log2(std::max(1.0, largest))) - std::ceil(std::log2(std::max(1.0, terms)));
    return static_cast<int>(std::clamp(room, 0.0, 40.0));
}

// a / w rounded to the nearest whole number (half away from zero), w > 0.
std::int64_t div_round(std::int64_t a, std::int64_t w) { return a >= 0 ? (a + w / 2) / w : -((-a + w / 2) / w); }

using Exact = std::vector<std::int64_t>;

// The window `win` of `src` (inside it) premultiplied, in whole numbers of 2^-bits, into `q`.
void exact_window(const Window& src, const Rect& win, bool alpha, int bits, Exact& q) {
    const std::size_t n = src.ch;
    const double scale = std::ldexp(1.0, bits);
    q.resize(win.area() * n);
    std::size_t o = 0;
    for (int y = win.y0; y < win.y1; ++y)
        for (int x = win.x0; x < win.x1; ++x) {
            const float* p = src.px(x, y);
            const float a = alpha ? p[n - 1] : 1.0f;
            for (std::size_t c = 0; c < n; ++c) {
                const float v = alpha && c + 1 < n ? p[c] * a : p[c];
                q[o++] = std::llround(static_cast<double>(v) * scale);
            }
        }
}

// `out` of an exact window `win` back as straight floats.
std::vector<float> from_exact(const Exact& q, const Rect& win, const Rect& out, std::size_t n, bool alpha, int bits) {
    const double unit = std::ldexp(1.0, -bits);
    const std::size_t ww = static_cast<std::size_t>(win.width());
    std::vector<float> res;
    res.reserve(out.area() * n);
    for (int y = out.y0; y < out.y1; ++y) {
        const std::size_t row = (std::size_t(y - win.y0) * ww + std::size_t(out.x0 - win.x0)) * n;
        for (std::size_t k = 0; k < std::size_t(out.width()) * n; ++k) res.push_back(static_cast<float>(static_cast<double>(q[row + k]) * unit));
    }
    unpremultiply(res, n, alpha);
    return res;
}

// Box passes of the radii given, one after another, along a line of `len` samples (`stride` words apart, `n` channels
// each) read as going on past both its ends with its end samples as far as the passes reach (`reach`: their radii's sum
// at least) — so a line cut at the grid's edge is worked as the grid's edge repeated, and one cut inside the grid far
// enough from what is kept —, each pass's mean rounded (div_round); the results back in place. `a` and `b`: scratch.
void line_passes(std::int64_t* first, std::size_t len, std::size_t stride, std::size_t n, const std::vector<std::size_t>& radii, std::size_t reach,
                 Exact& a, Exact& b) {
    if (len == 0) return;
    const std::size_t total = len + 2 * reach;
    a.resize(total * n);
    b.resize(total * n);
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t at = i < reach ? 0 : std::min(i - reach, len - 1);
        std::copy(first + at * stride, first + at * stride + n, a.begin() + static_cast<std::ptrdiff_t>(i * n));
    }
    std::vector<std::int64_t> acc(n);
    const auto clamped = [total](std::ptrdiff_t i) { return std::size_t(std::clamp<std::ptrdiff_t>(i, 0, std::ptrdiff_t(total) - 1)); };
    for (const std::size_t r : radii) {
        if (r == 0) continue;
        const auto width = static_cast<std::int64_t>(2 * r + 1);
        std::fill(acc.begin(), acc.end(), 0);
        for (std::ptrdiff_t k = -std::ptrdiff_t(r); k <= std::ptrdiff_t(r); ++k)
            for (std::size_t c = 0; c < n; ++c) acc[c] += a[clamped(k) * n + c];
        for (std::size_t x = 0; x < total; ++x) {
            const std::size_t add = clamped(std::ptrdiff_t(x + r + 1)) * n, sub = clamped(std::ptrdiff_t(x) - std::ptrdiff_t(r)) * n;
            for (std::size_t c = 0; c < n; ++c) {
                b[x * n + c] = div_round(acc[c], width);
                acc[c] += a[add + c] - a[sub + c];
            }
        }
        a.swap(b);
    }
    for (std::size_t i = 0; i < len; ++i)
        std::copy(a.begin() + static_cast<std::ptrdiff_t>((reach + i) * n), a.begin() + static_cast<std::ptrdiff_t>((reach + i + 1) * n), first + i * stride);
}

}  // namespace

int gaussian_halo(float sigma) {
    const int reach = static_cast<int>(std::ceil(std::max(0.0f, sigma) * 3.0f)) + 1;
    if (sigma <= 4.0f) return reach;
    return std::max(reach, boxes_margin(boxes_for_gauss(sigma, 3)));
}

std::vector<float> gaussian(const Window& src, const Rect& out, bool alpha, float sigma, double largest) {
    // (the exact kernel for small radii; beyond, three box passes: a cost that does not grow with the radius — Kovesi,
    // "Fast almost-Gaussian filtering", 2010)
    if (sigma <= 4.0f) {
        const std::vector<float> k = gaussian_kernel(sigma);
        return conv_sep(src, out, k, k, alpha);
    }
    const std::vector<int> boxes = boxes_for_gauss(sigma, 3);
    const int bits = exact_bits(largest, boxes.back());
    if (bits < 16) {  // (samples too large for exact sums: the Gaussian itself)
        const std::vector<float> k = gaussian_kernel(sigma);
        return conv_sep(src, out, k, k, alpha);
    }
    // (what it reads: the output grown by the passes' reach, cut to the window — the layer's grid —; each line read on past
    // its ends with its end samples: the grid's edge repeated, as the window would hold it)
    const std::size_t n = src.ch;
    const int margin = boxes_margin(boxes);
    const Rect win = out.inflate(margin).intersect(src.rect);
    const std::size_t ww = static_cast<std::size_t>(win.width()), wh = static_cast<std::size_t>(win.height());
    std::vector<std::size_t> radii;
    for (const int w : boxes) radii.push_back(static_cast<std::size_t>((w - 1) / 2));
    Exact q, a, b;
    exact_window(src, win, alpha, bits, q);
    for (std::size_t y = 0; y < wh; ++y) line_passes(q.data() + y * ww * n, ww, n, n, radii, std::size_t(margin), a, b);
    for (int x = out.x0; x < out.x1; ++x) line_passes(q.data() + std::size_t(x - win.x0) * n, wh, ww * n, n, radii, std::size_t(margin), a, b);
    return from_exact(q, win, out, n, alpha, bits);
}

std::vector<float> box(const Window& src, const Rect& out, bool alpha, int r, double largest) {
    if (r <= 0) return src.crop(out);
    const std::size_t n = src.ch;
    const std::int64_t side = 2 * std::int64_t(r) + 1;
    const int bits = exact_bits(largest, static_cast<double>(side) * static_cast<double>(side));
    if (bits < 16) {  // (samples too large for exact sums: the box as a convolution)
        const std::vector<float> k(static_cast<std::size_t>(side), 1.0f / static_cast<float>(side));
        return conv_sep(src, out, k, k, alpha);
    }
    // (the sums of each row's 2r + 1, then of the column's 2r + 1 of those: the square's sum, divided once — what it reads
    // cut to the window, the layer's grid, its edge pixel repeated past it)
    const Rect win = out.inflate(r).intersect(src.rect);
    const std::size_t wh = static_cast<std::size_t>(win.height());
    const std::size_t ow = static_cast<std::size_t>(out.width()), oh = static_cast<std::size_t>(out.height());
    const auto at_x = [&win](int x) { return std::size_t(std::clamp(x, win.x0, win.x1 - 1) - win.x0); };
    const auto at_y = [&win](int y) { return std::size_t(std::clamp(y, win.y0, win.y1 - 1) - win.y0); };
    Exact rows(wh * ow * n), line;
    std::vector<std::int64_t> acc(n);
    for (std::size_t y = 0; y < wh; ++y) {
        exact_window(src, Rect{win.x0, win.y0 + int(y), win.x1, win.y0 + int(y) + 1}, alpha, bits, line);
        std::int64_t* dst = rows.data() + y * ow * n;
        std::fill(acc.begin(), acc.end(), 0);
        for (int k = -r; k <= r; ++k)
            for (std::size_t c = 0; c < n; ++c) acc[c] += line[at_x(out.x0 + k) * n + c];
        for (std::size_t x = 0; x < ow; ++x) {
            const int ox = out.x0 + int(x);
            for (std::size_t c = 0; c < n; ++c) {
                dst[x * n + c] = acc[c];
                acc[c] += line[at_x(ox + r + 1) * n + c] - line[at_x(ox - r) * n + c];
            }
        }
    }
    Exact sums(oh * ow * n);
    std::vector<std::int64_t> col(ow * n, 0);
    for (int k = -r; k <= r; ++k) {
        const std::int64_t* from = rows.data() + at_y(out.y0 + k) * ow * n;
        for (std::size_t j = 0; j < ow * n; ++j) col[j] += from[j];
    }
    for (std::size_t y = 0; y < oh; ++y) {
        const int oy = out.y0 + int(y);
        const std::int64_t* add = rows.data() + at_y(oy + r + 1) * ow * n;
        const std::int64_t* sub = rows.data() + at_y(oy - r) * ow * n;
        for (std::size_t j = 0; j < ow * n; ++j) {
            sums[y * ow * n + j] = div_round(col[j], side * side);
            col[j] += add[j] - sub[j];
        }
    }
    return from_exact(sums, out, out, n, alpha, bits);
}

Kernels blur_kernels() {
    Kernels all;
    {  // ぼかし（ガウス） (filter.blur.gaussianBlur; ぼかし and ぼかし（強） at 0.6 and 1.4): sigma = the radius
        Kernel k;
        k.kind = "blur_gaussian";
        k.halo = [](const Json& params, const Ctx& ctx) { return Halo::margin(gaussian_halo(ctx.px(number(params, "radius", 1)))); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return gaussian(src, out, ctx.alpha, ctx.px(number(params, "radius", 1)), ctx.largest);
        };
        k.cost = [](const Json& params, const Ctx& ctx) {
            const float sigma = ctx.px(number(params, "radius", 1));
            if (sigma <= 4.0f) return 8.0 * (2.0 * std::ceil(3.0 * sigma) + 1.0);
            const double margin = gaussian_halo(sigma), tile = std::max(256.0, std::ceil(2.0 * margin / 64.0) * 64.0);
            return 60.0 * std::pow((tile + 2.0 * margin) / tile, 2.0);  // (six passes over a window larger than the tile)
        };
        all.push_back(std::move(k));
    }
    {  // ぼかし（ボックス） (filter.blur.boxBlur): the mean of the (2r + 1)² square, r the radius rounded
        Kernel k;
        k.kind = "blur_box";
        k.halo = [](const Json& params, const Ctx& ctx) {
            return Halo::margin(static_cast<int>(std::lround(std::max(0.0f, ctx.px(number(params, "radius", 1))))) + 1);
        };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return box(src, out, ctx.alpha, static_cast<int>(std::lround(std::max(0.0f, ctx.px(number(params, "radius", 1))))), ctx.largest);
        };
        k.cost = [](const Json& params, const Ctx& ctx) {
            const double r = std::round(std::max(0.0f, ctx.px(number(params, "radius", 1))));
            const double tile = std::max(256.0, std::ceil(2.0 * (r + 1) / 64.0) * 64.0);
            return 16.0 * (tile + 2.0 * r) / tile;
        };
        all.push_back(std::move(k));
    }
    {  // 平均 (filter.blur.average): the base box — the selection's, else the layer's — filled with the mean colour of what
       // the selection covers (each colour weighed by its alpha and the coverage; the alpha the coverage's mean of it);
       // the engine mixes it by the coverage, as PhotoCraft's map_pixels
        Kernel k;
        k.kind = "blur_average";
        k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
        k.once = [](const Json&, const Window& whole, const Ctx& ctx) {
            const std::size_t n = whole.ch;
            std::vector<double> sum(n - 1, 0.0);
            double wsum = 0, asum = 0, ksum = 0;
            const Rect box = ctx.bounds.intersect(whole.rect);
            for (int y = box.y0; y < box.y1; ++y)
                for (int x = box.x0; x < box.x1; ++x) {
                    const float* p = whole.px(x, y);
                    const double cover = ctx.coverage(x, y), a = p[n - 1], w = a * cover;
                    for (std::size_t c = 0; c + 1 < n; ++c) sum[c] += p[c] * w;
                    wsum += w;
                    asum += a * cover;
                    ksum += cover;
                }
            if (!(wsum > 0)) return Json::object();  // (nothing shows: as it was)
            Json mean = Json::array();
            for (const double v : sum) mean.push_back(v / wsum);
            mean.push_back(asum / std::max(ksum, 1e-9));
            return Json{{"mean", std::move(mean)}};
        };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx&) {
            const Json* mean = core::get(params, "mean");
            if (mean == nullptr || !mean->is_array() || mean->size() != src.ch) return src.crop(out);
            std::vector<float> px;
            for (const Json& v : *mean) px.push_back(static_cast<float>(v.get<double>()));
            std::vector<float> res;
            res.reserve(out.area() * src.ch);
            for (std::size_t i = 0; i < out.area(); ++i) res.insert(res.end(), px.begin(), px.end());
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 6.0; };
        all.push_back(std::move(k));
    }
    {  // ぼかし（移動） (filter.blur.motionBlur): the mean of ⌈d⌉ + 1 samples along a line of the distance d through the
       // pixel at the angle (degrees anticlockwise, the page's way up)
        Kernel k;
        k.kind = "blur_motion";
        k.halo = [](const Json& params, const Ctx& ctx) {
            return Halo::margin(static_cast<int>(std::ceil(std::fabs(ctx.px(number(params, "distance", 10))) / 2.0)) + 2);
        };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const float d = std::fabs(ctx.px(number(params, "distance", 10)));
            if (d < 0.5f) return src.crop(out);
            const double angle = number(params, "angle", 0) * std::numbers::pi / 180.0;
            const float sn = static_cast<float>(std::sin(angle)), cs = static_cast<float>(std::cos(angle));
            const int steps = static_cast<int>(std::ceil(d));
            return average_samples(src, out, [&](float x, float y, std::vector<std::pair<float, float>>& pts) {
                for (int i = 0; i <= steps; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(steps) - 0.5f;
                    pts.emplace_back(x + cs * d * t, y - sn * d * t);
                }
            });
        };
        k.cost = [](const Json& params, const Ctx& ctx) { return 8.0 * (std::ceil(std::fabs(ctx.px(number(params, "distance", 10)))) + 1.0); };
        all.push_back(std::move(k));
    }
    {  // ぼかし（放射状） (filter.blur.radialBlur): 回転 — the mean along an arc of `amount` degrees about the centre — or ズーム
       // — along the ray, up to amount/2 % nearer the centre —; the centre a share of the base box; samples up to 64 a
       // working pixel's worth (more on a finer grid: the same look)
        Kernel k;
        k.kind = "blur_radial";
        k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const Rect& b = ctx.bounds;
            const float cx = static_cast<float>(b.x0 + b.width() * number(params, "center_x", 0.5));
            const float cy = static_cast<float>(b.y0 + b.height() * number(params, "center_y", 0.5));
            const float amount = static_cast<float>(std::clamp(number(params, "amount", 10), 0.0, 100.0));
            const bool zoom = text(params, "method", "spin") == "zoom";
            const int most = std::max(1, static_cast<int>(std::ceil(64.0 * ctx.s)));
            return average_samples(src, out, [&](float x, float y, std::vector<std::pair<float, float>>& pts) {
                const float dx = x - cx, dy = y - cy;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (!zoom) {
                    const float arc = amount * std::numbers::pi_v<float> / 180.0f;
                    const int n = std::clamp(static_cast<int>(std::ceil(arc * r)), 1, most);
                    for (int i = 0; i <= n; ++i) {
                        const float t = (static_cast<float>(i) / static_cast<float>(n) - 0.5f) * arc;
                        const float s = std::sin(t), c = std::cos(t);
                        pts.emplace_back(cx + dx * c - dy * s, cy + dx * s + dy * c);
                    }
                } else {
                    const float span = amount / 200.0f;
                    const int n = std::clamp(static_cast<int>(std::ceil(span * r)), 1, most);
                    for (int i = 0; i <= n; ++i) {
                        const float f = 1.0f - span * static_cast<float>(i) / static_cast<float>(n);
                        pts.emplace_back(cx + dx * f, cy + dy * f);
                    }
                }
            });
        };
        k.cost = [](const Json&, const Ctx& ctx) { return 8.0 * 40.0 * std::max(1.0, ctx.s); };
        all.push_back(std::move(k));
    }
    {  // ぼかし（表面） (filter.blur.surfaceBlur; 輪郭以外をぼかす at 2 and 12): the square of side 2r + 1 weighed by how near
       // each sample is to the pixel's (threshold levels × 2.5): render/fx_rank's
        Kernel k;
        k.kind = "blur_surface";
        const auto radius = [](const Json& params, const Ctx& ctx) { return static_cast<int>(std::lround(std::max(0.0f, ctx.px(number(params, "radius", 5))))); };
        const auto limit = [](const Json& params) { return static_cast<float>(std::max(1.0, number(params, "threshold", 15)) / 255.0 * 2.5); };
        k.halo = [radius](const Json& params, const Ctx& ctx) { return Halo::margin(radius(params, ctx) + 1); };
        k.run = [radius, limit](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return surface(src, out, ctx, radius(params, ctx), limit(params));
        };
        k.cost = [radius](const Json& params, const Ctx& ctx) {
            const double r = radius(params, ctx);
            return r <= 8 ? 3.0 * (2 * r + 1) * (2 * r + 1) + 4 : 16.0 * (2 * r + 1) + 600;
        };
        all.push_back(std::move(k));
    }
    for (Kernel& shaped : blur_shaped_kernels()) all.push_back(std::move(shaped));
    return all;
}

}  // namespace genko::render::fx
