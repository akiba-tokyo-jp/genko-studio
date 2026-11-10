// PC4 節 D1 (SPEC.md COLOR-08): フィルター → ノイズ's kernels — ノイズを加える, ダスト＆スクラッチ, 明るさの中間値 (render/fx_rank:
// a histogram slid along each row) and ノイズを軽減 (輪郭以外をぼかす is ぼかし（表面） at fixed settings: render/fx_blur.cpp).
// PhotoCraft v0.2.0's algo/src/noise.rs (add, median) and denoise.rs (guided, reduce_noise: He, Sun & Tang, "Guided Image
// Filtering", ECCV 2010), MIT: native/third_party/photocraft/NOTICE.md. ノイズを加える's grain is a layer pixel's (its
// place's hash: the same whatever the tiling); ノイズを軽減's radii are working pixels, its boxes' sums taken in a fixed
// order a pixel (the same whatever window holds it), its JPEG grid the layer's own 8 pixels.

#include <algorithm>
#include <cmath>
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

bool flag(const Json& params, std::string_view key) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_boolean() && v->get<bool>();
}

// --- ノイズを軽減 ---------------------------------------------------------------------------------------------------------

// A plane of doubles over a rectangle of the grid (past it: none — each is made over what the next one reads).
struct Plane {
    Rect rect;
    std::vector<double> v;
    explicit Plane(const Rect& r) : rect(r), v(r.area(), 0.0) {}
    double& at(int x, int y) { return v[std::size_t(y - rect.y0) * std::size_t(rect.width()) + std::size_t(x - rect.x0)]; }
    double at(int x, int y) const { return v[std::size_t(y - rect.y0) * std::size_t(rect.width()) + std::size_t(x - rect.x0)]; }
};

// The mean of the (2r + 1)² square of `in` over `region` (`in` holding it grown by r): each sum in a fixed order.
Plane box(const Plane& in, const Rect& region, int r) {
    Plane rows(Rect{region.x0, region.y0 - r, region.x1, region.y1 + r});
    for (int y = rows.rect.y0; y < rows.rect.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x) {
            double s = 0;
            for (int k = -r; k <= r; ++k) s += in.at(x + k, y);
            rows.at(x, y) = s;
        }
    Plane out(region);
    const double norm = 1.0 / (double(2 * r + 1) * double(2 * r + 1));
    for (int y = region.y0; y < region.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x) {
            double s = 0;
            for (int k = -r; k <= r; ++k) s += rows.at(x, y + k);
            out.at(x, y) = s * norm;
        }
    return out;
}

// The guided filter of `p` by the guide `g` over `region` (both holding it grown by 2r): its local linear model's
// mean coefficients times the guide (denoise.rs guided).
Plane guided(const Plane& g, const Plane& p, const Rect& region, int r, double eps) {
    const Rect inner = region.inflate(r);
    Plane gg(g.rect), gp(g.rect);
    for (int y = g.rect.y0; y < g.rect.y1; ++y)
        for (int x = g.rect.x0; x < g.rect.x1; ++x) {
            gg.at(x, y) = g.at(x, y) * g.at(x, y);
            gp.at(x, y) = g.at(x, y) * p.at(x, y);
        }
    const Plane mg = box(g, inner, r), mp = box(p, inner, r), mgg = box(gg, inner, r), mgp = box(gp, inner, r);
    Plane a(inner), b(inner);
    for (int y = inner.y0; y < inner.y1; ++y)
        for (int x = inner.x0; x < inner.x1; ++x) {
            const double var = std::max(0.0, mgg.at(x, y) - mg.at(x, y) * mg.at(x, y));
            const double k = (mgp.at(x, y) - mg.at(x, y) * mp.at(x, y)) / (var + eps);
            a.at(x, y) = k;
            b.at(x, y) = mp.at(x, y) - k * mg.at(x, y);
        }
    const Plane ma = box(a, region, r), mb = box(b, region, r);
    Plane out(region);
    for (int y = region.y0; y < region.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x) out.at(x, y) = ma.at(x, y) * g.at(x, y) + mb.at(x, y);
    return out;
}

// The Gaussian of σ over `region` (`in` holding it grown by its reach).
Plane gauss(const Plane& in, const Rect& region, double sigma, int reach) {
    std::vector<double> k;
    double sum = 0;
    for (int i = -reach; i <= reach; ++i) {
        k.push_back(std::exp(-double(i * i) / (2 * sigma * sigma)));
        sum += k.back();
    }
    for (double& v : k) v /= sum;
    Plane rows(Rect{region.x0, region.y0 - reach, region.x1, region.y1 + reach});
    for (int y = rows.rect.y0; y < rows.rect.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x) {
            double s = 0;
            for (int i = -reach; i <= reach; ++i) s += in.at(x + i, y) * k[std::size_t(i + reach)];
            rows.at(x, y) = s;
        }
    Plane out(region);
    for (int y = region.y0; y < region.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x) {
            double s = 0;
            for (int i = -reach; i <= reach; ++i) s += rows.at(x, y + i) * k[std::size_t(i + reach)];
            out.at(x, y) = s;
        }
    return out;
}

struct Denoise {
    double strength = 0, keep = 0, colour = 0, sharpen = 0;
    bool jpeg = false;
    int rl = 0, rc = 0, sharp_reach = 0;
    double sharp_sigma = 1;

    Denoise(const Json& params, double s) {
        strength = std::clamp(number(params, "strength", 6), 0.0, 10.0);
        keep = std::clamp(number(params, "preserve_details", 60), 0.0, 100.0) / 100.0;
        colour = std::clamp(number(params, "reduce_color_noise", 45), 0.0, 100.0) / 100.0;
        sharpen = std::clamp(number(params, "sharpen_details", 25), 0.0, 100.0) / 100.0;
        jpeg = flag(params, "remove_jpeg_artifact");
        // (PhotoCraft's radii — 5 and 9 at most — in working pixels)
        rl = std::max(1, static_cast<int>(std::lround(std::min(std::round(1.0 + strength * 0.4), 5.0) * s)));
        rc = std::max(1, static_cast<int>(std::lround(std::min(std::round(1.0 + colour * 8.0), 9.0) * s)));
        sharp_sigma = std::max(1.0, s);
        sharp_reach = static_cast<int>(std::ceil(3.0 * sharp_sigma));
    }
    bool nothing() const { return strength <= 0 && colour <= 0 && sharpen <= 0 && !jpeg; }
    // How far the luma that is smoothed reaches past what is made: the chroma's guide and the sharpening's blur.
    int after() const { return std::max(colour > 0 ? 2 * rc : 0, sharpen > 0 ? sharp_reach : 0); }
    int reach() const { return after() + (strength > 0 ? 2 * rl : 0) + (jpeg ? 1 : 0); }
};

// ノイズを軽減 on a window: the luma (and two chromas) smoothed edge-preserving by the guided filter — the luma by itself
// (ディテールを保持 shrinking its regularization), the chromas guided by the cleaned luma —, JPEG's 8 × 8 grid softened first,
// the luma sharpened last (ディテールをシャープに). RGB as Y and B − Y, R − Y; Lab as its L, a, b; grey its lightness; CMYK and a
// multichannel page each plate as a luma of its own (no chroma).
std::vector<float> reduce_noise(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const Denoise d(params, ctx.s);
    if (d.nothing() || out.empty()) return src.crop(out);
    const std::size_t n = src.ch, cc = n - 1;
    const bool chroma = ctx.space == Space::Rgb || ctx.space == Space::Lab;
    const bool plates = ctx.space == Space::Cmyk || ctx.space == Space::Multi;
    const std::size_t lumas = plates ? cc : 1;
    const Rect shown = out.inflate(d.after());                       // (the cleaned luma)
    const Rect jpeg = shown.inflate(d.strength > 0 ? 2 * d.rl : 0);  // (the luma JPEG's grid softened)
    const Rect raw = jpeg.inflate(d.jpeg ? 1 : 0);                   // (what is read)
    std::vector<Plane> luma;
    for (std::size_t i = 0; i < lumas; ++i) luma.emplace_back(raw);
    Plane c1(chroma ? raw : Rect{}), c2(chroma ? raw : Rect{});
    for (int y = raw.y0; y < raw.y1; ++y)
        for (int x = raw.x0; x < raw.x1; ++x) {
            if (plates) {
                for (std::size_t c = 0; c < cc; ++c) luma[c].at(x, y) = src.at(x, y, c);
            } else if (ctx.space == Space::Rgb) {
                const double r = src.at(x, y, 0), g = src.at(x, y, 1), b = src.at(x, y, 2);
                const double l = 0.299 * r + 0.587 * g + 0.114 * b;
                luma[0].at(x, y) = l;
                c1.at(x, y) = b - l;
                c2.at(x, y) = r - l;
            } else {
                luma[0].at(x, y) = src.at(x, y, 0);
                if (chroma) {
                    c1.at(x, y) = src.at(x, y, 1);
                    c2.at(x, y) = src.at(x, y, 2);
                }
            }
        }
    const double s = d.strength / 10.0;
    std::vector<Plane> clean;
    for (Plane& y : luma) {
        Plane soft(jpeg);
        for (int yy = jpeg.y0; yy < jpeg.y1; ++yy)
            for (int xx = jpeg.x0; xx < jpeg.x1; ++xx) {
                double v = y.at(xx, yy);
                const int gx = ((xx % 8) + 8) % 8, gy = ((yy % 8) + 8) % 8;
                if (d.jpeg && (gx == 0 || gx == 7 || gy == 0 || gy == 7)) {  // (across the grid's lines, the layer's own)
                    double a = 0;
                    for (int j = -1; j <= 1; ++j)
                        for (int k = -1; k <= 1; ++k) a += y.at(xx + k, yy + j);
                    v = 0.5 * v + 0.5 * a / 9.0;
                }
                soft.at(xx, yy) = v;
            }
        Plane done(shown);
        if (s > 0) {
            const double eps = std::pow(0.004 + 0.05 * s, 2) * (1.05 - d.keep);
            const Plane smooth = guided(soft, soft, shown, d.rl, eps);
            const double k = std::sqrt(s);
            for (int yy = shown.y0; yy < shown.y1; ++yy)
                for (int xx = shown.x0; xx < shown.x1; ++xx) done.at(xx, yy) = soft.at(xx, yy) + (smooth.at(xx, yy) - soft.at(xx, yy)) * k;
        } else {
            for (int yy = shown.y0; yy < shown.y1; ++yy)
                for (int xx = shown.x0; xx < shown.x1; ++xx) done.at(xx, yy) = soft.at(xx, yy);
        }
        clean.push_back(std::move(done));
    }
    std::optional<Plane> chroma1, chroma2;
    if (chroma && d.colour > 0) {
        const double eps = std::pow(0.01 + 0.05 * d.colour, 2);
        for (auto [plane, made] : {std::pair{&c1, &chroma1}, std::pair{&c2, &chroma2}}) {
            const Plane g = guided(clean[0], *plane, out, d.rc, eps);
            Plane p(out);
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x) p.at(x, y) = plane->at(x, y) + (g.at(x, y) - plane->at(x, y)) * d.colour;
            made->emplace(std::move(p));
        }
    }
    std::vector<Plane> sharpened;
    for (const Plane& y : clean) {
        Plane p(out);
        if (d.sharpen > 0) {
            const Plane bl = gauss(y, out, d.sharp_sigma, d.sharp_reach);
            for (int yy = out.y0; yy < out.y1; ++yy)
                for (int xx = out.x0; xx < out.x1; ++xx) p.at(xx, yy) = y.at(xx, yy) + (y.at(xx, yy) - bl.at(xx, yy)) * d.sharpen * 1.2;
        } else {
            for (int yy = out.y0; yy < out.y1; ++yy)
                for (int xx = out.x0; xx < out.x1; ++xx) p.at(xx, yy) = y.at(xx, yy);
        }
        sharpened.push_back(std::move(p));
    }
    std::vector<float> res = src.crop(out);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            float* px = res.data() + i;
            if (plates) {
                for (std::size_t c = 0; c < cc; ++c) px[c] = static_cast<float>(sharpened[c].at(x, y));
                continue;
            }
            const double l = sharpened[0].at(x, y);
            if (!chroma) {
                px[0] = static_cast<float>(l);
                continue;
            }
            const double a1 = chroma1 ? chroma1->at(x, y) : c1.at(x, y), a2 = chroma2 ? chroma2->at(x, y) : c2.at(x, y);
            if (ctx.space == Space::Lab) {
                px[0] = static_cast<float>(l);
                px[1] = static_cast<float>(a1);
                px[2] = static_cast<float>(a2);
            } else {
                const double b = a1 + l, r = a2 + l;
                px[0] = static_cast<float>(r);
                px[1] = static_cast<float>((l - 0.299 * r - 0.114 * b) / 0.587);
                px[2] = static_cast<float>(b);
            }
        }
    return res;
}

}  // namespace

Kernels noise_kernels() {
    Kernels all;
    {  // ノイズを加える (filter.noise.addNoise): each colour of each pixel that shows ± amount/2 % (均等に分布), or a Gaussian of
       // amount/4 % (ガウス分布); グレースケールノイズ: the same for every colour (a Lab page's L alone: grey grains); the
       // seed's
        Kernel k;
        k.kind = "noise_add";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const float a = static_cast<float>(std::max(0.0, number(params, "amount", 12.5)) / 100.0 * 0.5);
            const bool gaussian = text(params, "distribution", "uniform") == "gaussian";
            const bool mono = flag(params, "monochromatic");
            const std::size_t cc = n - 1, noisy = mono && ctx.space == Space::Lab ? std::min<std::size_t>(cc, 1) : cc;
            const auto seed = static_cast<std::uint32_t>(std::clamp(number(params, "seed", 0), 0.0, 4294967295.0));
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    if (!(res[i + cc] > 0.0f)) continue;
                    for (std::size_t c = 0; c < noisy; ++c) {
                        const auto ch = static_cast<std::uint32_t>(mono ? 0 : c);
                        float v = 0;
                        if (!gaussian) {
                            v = (hash01(x, y, ch, seed) - 0.5f) * 2.0f * a;
                        } else {
                            const float u1 = std::max(hash01(x, y, ch * 2 + 101, seed), 1e-7f), u2 = hash01(x, y, ch * 2 + 102, seed);
                            v = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * std::numbers::pi_v<float> * u2) * a * 0.5f;
                        }
                        res[i + c] += v;  // (a layer of whole samples clamps it as it is written; a float one keeps it)
                    }
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 6.0; };
        all.push_back(std::move(k));
    }
    const auto radius = [](const Json& params, const Ctx& ctx) { return static_cast<int>(std::lround(std::max(0.0f, ctx.px(number(params, "radius", 1))))); };
    const auto rank_cost = [radius](const Json& params, const Ctx& ctx) {
        // (as written below 3 pixels; the sliding histogram past it — on floats the window's values sorted first)
        const double r = radius(params, ctx);
        return r < 3 ? 4.0 * 3.0 * (r * r * 3.2 + 1) : 4.0 * (4.0 * r + 2.0) + 40.0 + (ctx.levels == 0 ? 4.0 * 100.0 : 0.0);
    };
    {  // ダスト＆スクラッチ (filter.noise.dustAndScratches): the median of the disc where a sample differs from it past the
       // threshold (levels)
        Kernel k;
        k.kind = "noise_dust_and_scratches";
        k.halo = [radius](const Json& params, const Ctx& ctx) { return Halo::margin(radius(params, ctx) + 1); };
        k.run = [radius](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return median(src, out, ctx, radius(params, ctx), static_cast<float>(number(params, "threshold", 0) / 255.0));
        };
        k.cost = rank_cost;
        all.push_back(std::move(k));
    }
    {  // 明るさの中間値 (filter.noise.median): each channel's median over the disc of the radius
        Kernel k;
        k.kind = "noise_median";
        k.halo = [radius](const Json& params, const Ctx& ctx) { return Halo::margin(radius(params, ctx) + 1); };
        k.run = [radius](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return median(src, out, ctx, radius(params, ctx), std::nullopt);
        };
        k.cost = rank_cost;
        all.push_back(std::move(k));
    }
    {  // ノイズを軽減 (filter.noise.reduceNoise)
        Kernel k;
        k.kind = "noise_reduce";
        k.halo = [](const Json& params, const Ctx& ctx) { return Halo::margin(Denoise(params, ctx.s).reach() + 1); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return reduce_noise(params, src, out, ctx); };
        k.work_bytes = 20 * sizeof(double);  // (its planes over the window: the lumas, chromas, moments and coefficients)
        k.cost = [](const Json& params, const Ctx& ctx) {
            const Denoise d(params, ctx.s);
            return 24.0 * (2.0 * d.rl + 1) + 3.0 * 24.0 * (2.0 * d.rc + 1) + 2.0 * (2.0 * d.sharp_reach + 1) + 40.0;
        };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
