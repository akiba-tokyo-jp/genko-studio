// PC4 節 D2 (SPEC.md COLOR-08「フィルター」): フィルター → 表現手法 → 油彩… — PhotoCraft v0.2.0's algo/src/oil.rs (flow-guided
// smoothing and a lit bristle relief: the structure tensor of the lightness gives the strokes' direction, a line integral
// convolution smears the colour along it and white noise along the same field makes the bristles), MIT:
// native/third_party/photocraft/NOTICE.md. Its lengths (the strokes, the tensor's and the cleaning's blurs, the relief's
// slope) are of the working 200 dpi (Ctx::s), the bristles' noise of the working pixels; its blurs' sums exact (whole
// units: the same bytes whatever the tiling). On a page of inks it works the lightness (CMYK and multichannel plates as
// 1 − ink, a Lab page's L, a and b — the relief lights its L alone); the layer's alpha is kept.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "render/fx_d2.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

using core::Json;

namespace {

using d2::number;

float stroke_len(float stylization) { return 1.5f + std::clamp(stylization, 0.1f, 10.0f) * 2.0f; }
float tensor_sigma(float scale) { return 0.8f + std::clamp(scale, 0.1f, 10.0f) * 0.6f; }

// The pixels it reads past an output tile, in working pixels (oil.rs reach).
float reach(float stylization, float scale) { return stroke_len(stylization) + 3.0f * tensor_sigma(scale) + 3.0f * 2.5f + 4.0f; }

// The bristles' white noise at a place of the layer's grid (its working pixels'; between them, bilinear — at 200 dpi
// PhotoCraft's own, a pixel's hash).
float bristle_noise(float x, float y, const Ctx& ctx) {
    if (std::fabs(ctx.s - 1.0) < 1e-9)
        return hash01(static_cast<int>(std::lround(x)) + ctx.x0, static_cast<int>(std::lround(y)) + ctx.y0, 71, 0);
    const float wx = (x + static_cast<float>(ctx.x0)) / static_cast<float>(ctx.s), wy = (y + static_cast<float>(ctx.y0)) / static_cast<float>(ctx.s);
    const float fx = std::floor(wx), fy = std::floor(wy);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float ax = wx - fx, ay = wy - fy;
    const float top = hash01(ix, iy, 71, 0) * (1.0f - ax) + hash01(ix + 1, iy, 71, 0) * ax;
    const float bottom = hash01(ix, iy + 1, 71, 0) * (1.0f - ax) + hash01(ix + 1, iy + 1, 71, 0) * ax;
    return top * (1.0f - ay) + bottom * ay;
}

std::vector<float> oil_paint(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const std::size_t cc = n - 1;
    const Rect win = src.rect;
    const std::size_t ww = static_cast<std::size_t>(win.width()), wh = static_cast<std::size_t>(win.height());
    if (ww == 0 || wh == 0 || out.empty()) return src.crop(out);
    const float s = static_cast<float>(ctx.s);
    const float stylization = static_cast<float>(number(params, "stylization", 4)), cleanliness = static_cast<float>(number(params, "cleanliness", 5));
    const float scale = static_cast<float>(number(params, "scale", 1)), bristle_detail = static_cast<float>(number(params, "bristle_detail", 5));
    const bool lighting = d2::flag(params, "lighting", true);
    const float angle = static_cast<float>(number(params, "angle", -60)), shine_setting = static_cast<float>(number(params, "shine", 1));
    // 1. the colours premultiplied, cleaned (smoothed) by 筆の清潔さ
    std::vector<float> p(src.base(), src.base() + ww * wh * n);
    premultiply(p, n, true);
    d2::gauss_blur_exact(p, ww, wh, n, std::clamp(cleanliness, 0.0f, 10.0f) * 0.25f * s, ctx.largest);
    // 2. the intensity of the structure tensor (the colours' mean: in every space)
    std::vector<float> lum(ww * wh);
    for (std::size_t i = 0; i < ww * wh; ++i) {
        float sum = 0;
        for (std::size_t c = 0; c < cc; ++c) sum += p[i * n + c];
        lum[i] = sum / static_cast<float>(cc);
    }
    // 3. the structure tensor (E, F, G), smoothed
    std::vector<float> efg(ww * wh * 3, 0.0f);
    for (std::size_t y = 1; y + 1 < wh; ++y)
        for (std::size_t x = 1; x + 1 < ww; ++x) {
            const auto l = [&](int dx, int dy) { return lum[(y + static_cast<std::size_t>(dy + 1) - 1) * ww + (x + static_cast<std::size_t>(dx + 1) - 1)]; };
            const float gx = (l(1, -1) + 2.0f * l(1, 0) + l(1, 1)) - (l(-1, -1) + 2.0f * l(-1, 0) + l(-1, 1));
            const float gy = (l(-1, 1) + 2.0f * l(0, 1) + l(1, 1)) - (l(-1, -1) + 2.0f * l(0, -1) + l(1, -1));
            const std::size_t o = (y * ww + x) * 3;
            efg[o] = gx * gx;
            efg[o + 1] = gx * gy;
            efg[o + 2] = gy * gy;
        }
    d2::gauss_blur_exact(efg, ww, wh, 3, tensor_sigma(scale) * s, 16.0 * ctx.largest * ctx.largest);
    // 4. the strokes' direction: the minor eigenvector (along the edges); flat, gentle diagonal strokes
    std::vector<std::pair<float, float>> tangent(ww * wh);
    for (std::size_t i = 0; i < ww * wh; ++i) {
        const float e = efg[i * 3], f = efg[i * 3 + 1], g = efg[i * 3 + 2];
        const float l1 = 0.5f * (e + g + std::sqrt((e - g) * (e - g) + 4.0f * f * f));
        const float tx = l1 - e, ty = -f;
        const float m = std::sqrt(tx * tx + ty * ty);
        if (m < 1e-6f || (l1 - (e + g - l1)) < 1e-6f) tangent[i] = {std::numbers::sqrt2_v<float> / 2.0f, std::numbers::sqrt2_v<float> / 2.0f};
        else tangent[i] = {tx / m, ty / m};
    }
    // 5. the line integral convolution over `out` grown by 1 (for the relief's slope)
    const Rect r1 = out.inflate(1).intersect(win);
    const std::size_t rw = static_cast<std::size_t>(r1.width()), rh = static_cast<std::size_t>(r1.height());
    const float len = stroke_len(stylization) * s;
    const float h = std::max(len / 10.0f, 1.0f);
    const int steps = static_cast<int>(std::ceil(len / h));
    std::vector<float> col(rw * rh * n, 0.0f), bristle(rw * rh, 0.0f);
    const auto sample = [&](float x, float y, float* acc, float k) {
        x = std::clamp(x, 0.0f, static_cast<float>(ww - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(wh - 1));
        const std::size_t x0 = static_cast<std::size_t>(std::floor(x)), y0 = static_cast<std::size_t>(std::floor(y));
        const std::size_t x1 = std::min(x0 + 1, ww - 1), y1 = std::min(y0 + 1, wh - 1);
        const float ax = x - static_cast<float>(x0), ay = y - static_cast<float>(y0);
        const float w00 = (1.0f - ax) * (1.0f - ay) * k, w10 = ax * (1.0f - ay) * k, w01 = (1.0f - ax) * ay * k, w11 = ax * ay * k;
        const std::size_t i00 = (y0 * ww + x0) * n, i10 = (y0 * ww + x1) * n, i01 = (y1 * ww + x0) * n, i11 = (y1 * ww + x1) * n;
        for (std::size_t c = 0; c < n; ++c) acc[c] += p[i00 + c] * w00 + p[i10 + c] * w10 + p[i01 + c] * w01 + p[i11 + c] * w11;
    };
    std::vector<float> acc(n);
    for (std::size_t ry = 0; ry < rh; ++ry)
        for (std::size_t rx = 0; rx < rw; ++rx) {
            const float dx = static_cast<float>(r1.x0 - win.x0) + static_cast<float>(rx), dy = static_cast<float>(r1.y0 - win.y0) + static_cast<float>(ry);
            std::fill(acc.begin(), acc.end(), 0.0f);
            float wsum = 1.0f;
            sample(dx, dy, acc.data(), 1.0f);
            float nsum = bristle_noise(static_cast<float>(win.x0) + dx, static_cast<float>(win.y0) + dy, ctx);
            for (const float dir : {1.0f, -1.0f}) {
                float qx = dx, qy = dy, px = 0, py = 0;
                for (int k = 1; k <= steps; ++k) {
                    const std::size_t ti = static_cast<std::size_t>(std::clamp(std::round(qy), 0.0f, static_cast<float>(wh - 1))) * ww +
                                           static_cast<std::size_t>(std::clamp(std::round(qx), 0.0f, static_cast<float>(ww - 1)));
                    auto [tx, ty] = tangent[ti];
                    if (k == 1) {
                        tx *= dir;
                        ty *= dir;
                    } else if (tx * px + ty * py < 0.0f) {
                        tx = -tx;
                        ty = -ty;
                    }
                    px = tx;
                    py = ty;
                    qx += tx * h;
                    qy += ty * h;
                    const float wk = 1.0f - static_cast<float>(k) / static_cast<float>(steps + 1);
                    sample(qx, qy, acc.data(), wk);
                    wsum += wk;
                    nsum += wk * bristle_noise(static_cast<float>(win.x0) + std::round(qx), static_cast<float>(win.y0) + std::round(qy), ctx);
                }
            }
            const std::size_t o = (ry * rw + rx) * n;
            for (std::size_t c = 0; c < n; ++c) col[o + c] = acc[c] / wsum;
            bristle[ry * rw + rx] = nsum / wsum - 0.5f;
        }
    // 6. the relief lit from the angle at 45°
    const double a = static_cast<double>(angle) * std::numbers::pi / 180.0;
    const float elev = std::numbers::pi_v<float> / 4.0f;
    const float light[3] = {static_cast<float>(std::cos(a)) * std::cos(elev), static_cast<float>(-std::sin(a)) * std::cos(elev), std::sin(elev)};
    float half[3] = {light[0], light[1], light[2] + 1.0f};
    {
        const float m = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
        for (float& v : half) v /= m;
    }
    const float amp = std::clamp(bristle_detail, 0.0f, 10.0f) / 10.0f;
    const float shine = std::clamp(shine_setting, 0.0f, 10.0f) / 10.0f;
    const auto height = [&](std::size_t rx, std::size_t ry) {
        const std::size_t o = (ry * rw + rx) * n;
        float l = 0;
        for (std::size_t c = 0; c < cc; ++c) l += col[o + c];
        return l / static_cast<float>(cc) * 2.0f + bristle[ry * rw + rx] * amp * 6.0f;
    };
    const float flat_spec = std::pow(half[2], 40.0f);
    const std::size_t lit = ctx.space == Space::Lab ? 1 : cc;  // (a Lab page's L alone)
    std::vector<float> res;
    res.reserve(out.area() * n);
    std::vector<float> px(n);
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x) {
            const std::size_t rx = static_cast<std::size_t>(x - r1.x0), ry = static_cast<std::size_t>(y - r1.y0);
            const std::size_t o = (ry * rw + rx) * n;
            std::copy(col.begin() + static_cast<std::ptrdiff_t>(o), col.begin() + static_cast<std::ptrdiff_t>(o + n), px.begin());
            const float al = px[n - 1];
            for (std::size_t c = 0; c < cc; ++c) px[c] = al > 1e-7f ? px[c] / al : 0.0f;
            px[n - 1] = src.get(x, y, n - 1);  // (painting restyles the colour, not the coverage)
            if (lighting) {
                const float hx = (height(std::min(rx + 1, rw - 1), ry) - height(rx > 0 ? rx - 1 : 0, ry)) * s;
                const float hy = (height(rx, std::min(ry + 1, rh - 1)) - height(rx, ry > 0 ? ry - 1 : 0)) * s;
                const float nv[3] = {-hx * 0.5f, -hy * 0.5f, 1.0f};
                const float m = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + 1.0f);
                const float ndl = (nv[0] * light[0] + nv[1] * light[1] + nv[2] * light[2]) / m;
                const float ndh = std::max((nv[0] * half[0] + nv[1] * half[1] + nv[2] * half[2]) / m, 0.0f);
                const float shade = 1.0f + (ndl - light[2]) * 1.2f;
                const float spec = shine * std::max(std::pow(ndh, 40.0f) - flat_spec, 0.0f) * 0.8f;
                for (std::size_t c = 0; c < lit; ++c) px[c] = px[c] * shade + spec;
            }
            res.insert(res.end(), px.begin(), px.end());
        }
    return res;
}

}  // namespace

Kernel oil_kernel() {
    // 油彩 (filter.stylize.oilPaint): 様式化 (the strokes' length), 筆の清潔さ, 拡大・縮小 (the strokes' scale), 密度
    // (the bristles), and 照明 (its angle, 光沢)
    Kernel k;
    k.kind = "stylize_oil_paint";
    k.halo = [](const Json& params, const Ctx& ctx) {
        return Halo::margin(static_cast<int>(std::ceil(reach(static_cast<float>(number(params, "stylization", 4)), static_cast<float>(number(params, "scale", 1))) * ctx.s)) + 1);
    };
    k.run = oil_paint;
    k.cost = [](const Json& params, const Ctx&) {
        const double len = stroke_len(static_cast<float>(number(params, "stylization", 4)));
        return 2.0 * std::ceil(len / std::max(len / 10.0, 1.0)) * 4.0 * 4.0 * 2.0 + 300.0;
    };
    k.work_bytes = 104;  // (a window's pixel: its premultiplied colours and their whole units, the tensor and its units, its direction)
    return k;
}

}  // namespace genko::render::fx
