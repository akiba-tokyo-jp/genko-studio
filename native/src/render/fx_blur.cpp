// PC4 節 D (SPEC.md COLOR-08): フィルター → ぼかし's kernels — D0: ぼかし（ガウス） and ぼかし（ボックス） (and the presets
// ぼかし・ぼかし（強）, which are the Gaussian at fixed radii). PhotoCraft v0.2.0's algo/src/blur.rs (1-220: gaussian_kernel,
// conv_sep, gaussian, boxes_for_gauss, gaussian_boxes, boxed), MIT: native/third_party/photocraft/NOTICE.md. The box
// passes' running sums are kept in whole numbers (the samples scaled by a power of two): a pixel's sum is then the same
// whichever window holds it — upstream's float sums depend on where the window starts — so the result does not depend
// on the tiling. D1 adds the rest of ぼかし here (移動・放射状・表面: blur.rs 222-334).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/pyops.hpp"
#include "render/fx.hpp"

namespace genko::render::fx {

namespace {

using core::Json;

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() && !v->is_boolean() ? v->get<double>() : fallback;
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
    return all;
}

}  // namespace genko::render::fx
