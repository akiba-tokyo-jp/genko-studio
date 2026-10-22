// PC4 節 D1 (SPEC.md COLOR-08): フィルター → ぼかし's ぼかし（詳細）, ぼかし（シェイプ） and ぼかし（レンズ）. PhotoCraft v0.2.0's
// algo/src/blur2.rs (Spans, Prefix, shape_inside, shape_blur, smart_blur, lens_blur), MIT:
// native/third_party/photocraft/NOTICE.md. A shaped kernel is a set of horizontal runs taken from each row's prefix sums
// (a cost of its radius a pixel, not its area); the sums are kept in whole numbers (the samples scaled by a power of
// two), so a pixel's mean is the same whichever window — whatever tile — holds it (upstream's f64 sums depend on where
// the window starts). Lengths are working pixels, scaled to the layer's grid (Ctx::s); ぼかし（詳細）'s edges are a
// working pixel wide.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

namespace {

using core::Json;

constexpr float kPi = std::numbers::pi_v<float>;

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

// --- span kernels ------------------------------------------------------------------------------------------------------

// A kernel of ones as horizontal runs (dy, dx0, dx1 inclusive) and their count: `inside(u, v)` (u, v in −1…1) over a disc
// of radius r (blur2.rs Spans::new).
struct Spans {
    struct Run {
        int dy, a, b;
    };
    std::vector<Run> rows;
    double count = 1;
    int reach = 0;
    Spans(float r, const std::function<bool(float, float)>& inside) {
        const int ri = static_cast<int>(std::ceil(std::max(0.0f, r)));
        if (r < 0.5f) {
            rows.push_back({0, 0, 0});
            return;
        }
        count = 0;
        for (int dy = -ri; dy <= ri; ++dy) {
            int start = 0;
            bool run = false;
            for (int dx = -ri; dx <= ri + 1; ++dx) {
                const bool hit = dx <= ri && inside(static_cast<float>(dx) / r, static_cast<float>(dy) / r);
                if (hit && !run) {
                    start = dx;
                    run = true;
                } else if (!hit && run) {
                    rows.push_back({dy, start, dx - 1});
                    count += dx - start;
                    run = false;
                }
            }
        }
        if (rows.empty()) {
            rows.push_back({0, 0, 0});
            count = 1;
        }
        reach = ri;
    }
};

// The power of two the exact sums scale samples by: a sample's magnitude times the terms of a sum below 2^62.
int exact_bits(double largest, double terms) {
    const double room = 61.0 - std::ceil(std::log2(std::max(1.0, largest))) - std::ceil(std::log2(std::max(1.0, terms)));
    return static_cast<int>(std::clamp(room, 0.0, 40.0));
}

// Each row's prefix sums of a premultiplied window in whole numbers of 2^-bits (its channels interleaved), read past the
// window's edge as its edge pixel (the window is cut to the grid: the grid's edge repeated).
class Prefix {
public:
    Prefix(const Window& src, const std::vector<float>& premultiplied, int bits) : rect_(src.rect), n_(src.ch) {
        const double scale = std::ldexp(1.0, bits);
        unit_ = std::ldexp(1.0, -bits);
        const std::size_t w = std::size_t(rect_.width()), h = std::size_t(rect_.height());
        sums_.assign((w + 1) * h * n_, 0);
        first_.resize(h * n_);
        last_.resize(h * n_);
        for (std::size_t y = 0; y < h; ++y) {
            std::int64_t* row = sums_.data() + y * (w + 1) * n_;
            for (std::size_t x = 0; x < w; ++x)
                for (std::size_t c = 0; c < n_; ++c) {
                    const std::int64_t q = std::llround(static_cast<double>(premultiplied[(y * w + x) * n_ + c]) * scale);
                    row[(x + 1) * n_ + c] = row[x * n_ + c] + q;
                    if (x == 0) first_[y * n_ + c] = q;
                    if (x + 1 == w) last_[y * n_ + c] = q;
                }
        }
    }
    // The sums of the runs about (x, y) into `acc` (n_ channels, as samples: whole numbers × unit).
    void sum(const Spans& spans, int x, int y, double* acc) const {
        const std::size_t w = std::size_t(rect_.width());
        std::fill(acc, acc + n_, 0.0);
        std::vector<std::int64_t>& total = scratch_;
        total.assign(n_, 0);
        for (const Spans::Run& run : spans.rows) {
            const std::size_t yy = std::size_t(std::clamp(y + run.dy, rect_.y0, rect_.y1 - 1) - rect_.y0);
            const std::int64_t* row = sums_.data() + yy * (w + 1) * n_;
            const int xa = x + run.a, xb = x + run.b;
            const int left = std::max(0, std::min(xb, rect_.x0 - 1) - xa + 1);
            const int right = std::max(0, xb - std::max(xa, rect_.x1) + 1);
            const int a = std::max(xa, rect_.x0), b = std::min(xb, rect_.x1 - 1);
            for (std::size_t c = 0; c < n_; ++c) {
                std::int64_t s = 0;
                if (a <= b) s = row[std::size_t(b - rect_.x0 + 1) * n_ + c] - row[std::size_t(a - rect_.x0) * n_ + c];
                s += std::int64_t(left) * first_[yy * n_ + c] + std::int64_t(right) * last_[yy * n_ + c];
                total[c] += s;
            }
        }
        for (std::size_t c = 0; c < n_; ++c) acc[c] = static_cast<double>(total[c]) * unit_;
    }

private:
    Rect rect_;
    std::size_t n_;
    double unit_ = 1;
    std::vector<std::int64_t> sums_, first_, last_;
    mutable std::vector<std::int64_t> scratch_;
};

std::vector<float> premultiplied(const Window& src) {
    std::vector<float> p = src.data;
    premultiply(p, src.ch, true);
    return p;
}

void unpremultiply_px(float* px, std::size_t n) {
    const float a = px[n - 1];
    for (std::size_t c = 0; c + 1 < n; ++c) px[c] = a > 1e-7f ? px[c] / a : 0.0f;
}

// The radius of a regular n-gon (circumradius 1) in the direction theta (blur2.rs polygon_radius).
float polygon_radius(float theta, unsigned n, float rot) {
    const float seg = 2.0f * kPi / static_cast<float>(n);
    float a = std::fmod(theta - rot, seg);
    if (a < 0) a += seg;
    a -= seg / 2.0f;
    return std::cos(kPi / static_cast<float>(n)) / std::cos(a);
}

bool in_poly(float u, float v, const std::vector<std::pair<float, float>>& pts) {
    bool inside = false;
    std::size_t j = pts.size() - 1;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto [xi, yi] = pts[i];
        const auto [xj, yj] = pts[j];
        if ((yi > v) != (yj > v) && u < (xj - xi) * (v - yi) / (yj - yi) + xi) inside = !inside;
        j = i;
    }
    return inside;
}

// PhotoCraft's built-in shapes of ぼかし（シェイプ） (blur2.rs shape_inside).
std::function<bool(float, float)> shape_inside(const std::string& shape) {
    if (shape == "ring") return [](float u, float v) { const float d = u * u + v * v; return d >= 0.36f && d <= 1.0f; };
    if (shape == "square") return [](float, float) { return true; };
    if (shape == "diamond") return [](float u, float v) { return std::fabs(u) + std::fabs(v) <= 1.0f; };
    if (shape == "triangle") return [](float u, float v) { return v <= 0.75f && std::fabs(u) <= (v + 1.0f) / 1.75f; };
    if (shape == "hexagon") return [](float u, float v) { return std::sqrt(u * u + v * v) <= polygon_radius(std::atan2(v, u), 6, 0.0f); };
    if (shape == "star") {
        std::vector<std::pair<float, float>> pts;
        for (int i = 0; i < 10; ++i) {
            const float r = i % 2 == 0 ? 1.0f : 0.42f;
            const float a = -kPi / 2.0f + static_cast<float>(i) * kPi / 5.0f;
            pts.emplace_back(r * std::cos(a), r * std::sin(a));
        }
        return [pts](float u, float v) { return in_poly(u, v, pts); };
    }
    if (shape == "heart")
        return [](float u, float v) {  // ((x² + y² − 1)³ − x²y³ ≤ 0 in the unit square, y up)
            const float x = u * 1.2f, y = -v * 1.2f + 0.15f;
            const float d = x * x + y * y - 1.0f;
            return d * d * d - x * x * y * y * y <= 0.0f;
        };
    if (shape == "cross") return [](float u, float v) { return std::fabs(u) <= 0.3f || std::fabs(v) <= 0.3f; };
    return [](float u, float v) { return u * u + v * v <= 1.0f; };  // (circle)
}

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// White and black in the kernel's samples' space (their colours; blur2.rs native): CMYK and a multichannel page's
// white no ink, CMYK's black K alone, a multichannel page's every plate; Lab's L with no colour.
std::vector<float> plain(const Ctx& ctx, bool white) {
    std::vector<float> out(ctx.colours, 0.0f);
    switch (ctx.space) {
        case Space::Rgb:
        case Space::Gray: std::fill(out.begin(), out.end(), white ? 1.0f : 0.0f); break;
        case Space::Lab:
            out[0] = white ? 1.0f : 0.0f;
            for (unsigned c = 1; c < ctx.colours; ++c) out[c] = 128.0f / 255.0f;
            break;
        case Space::Cmyk:
            if (!white && ctx.colours >= 4) out[3] = 1.0f;
            break;
        case Space::Multi: std::fill(out.begin(), out.end(), white ? 0.0f : 1.0f); break;
    }
    return out;
}

constexpr int kLensLevels = 8;  // (the depth levels the iris's radius is taken between)

}  // namespace

Kernels blur_shaped_kernels() {
    Kernels all;
    {  // ぼかし（詳細） (filter.blur.smartBlur): the mean of the neighbours within the threshold of the pixel in every colour —
       // sampled on a grid of the quality's (高 12, 中 7, 低 4 a radius) —; エッジのみ / エッジオーバーレイ: where a neighbour a
       // working pixel off differs past it, white (over black, or over the picture)
        Kernel k;
        k.kind = "blur_smart";
        const auto reach = [](const Json& params, const Ctx& ctx) {
            return static_cast<int>(std::ceil(std::clamp(number(params, "radius", 3), 0.1, 100.0) * ctx.s));
        };
        k.halo = [reach](const Json& params, const Ctx& ctx) { return Halo::margin(std::max(reach(params, ctx), static_cast<int>(std::lround(ctx.s))) + 1); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch, cc = n - 1;
            const float r = static_cast<float>(std::clamp(number(params, "radius", 3), 0.1, 100.0) * ctx.s);
            const float t = static_cast<float>(std::clamp(number(params, "threshold", 25), 0.1, 100.0) / 255.0);
            const int ri = static_cast<int>(std::ceil(r));
            const std::string quality = text(params, "quality", "high");
            const int target = quality == "low" ? 4 : quality == "medium" ? 7 : 12;
            const int stride = std::max(1, (ri + target - 1) / target);
            std::vector<std::pair<int, int>> offs;
            for (int dy = -ri; dy <= ri; dy += stride)
                for (int dx = -ri; dx <= ri; dx += stride)
                    if (static_cast<float>(dx * dx + dy * dy) <= r * r + 0.5f) offs.emplace_back(dx, dy);
            const std::string mode = text(params, "mode", "normal");
            const std::vector<float> white = plain(ctx, true), black = plain(ctx, false);
            const int edge_reach = std::max(1, static_cast<int>(std::lround(ctx.s)));
            std::vector<float> res = src.crop(out);
            std::vector<float> acc(n);
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x) {
                    float* px = res.data() + (std::size_t(y - out.y0) * std::size_t(out.width()) + std::size_t(x - out.x0)) * n;
                    const auto differs = [&](int qx, int qy) {
                        for (std::size_t c = 0; c < cc; ++c)
                            if (std::fabs(src.at(qx, qy, c) - px[c]) > t) return true;
                        return false;
                    };
                    if (mode != "normal") {
                        bool edge = false;
                        for (int d = 1; d <= edge_reach && !edge; ++d)
                            for (const auto& [dx, dy] : {std::pair{d, 0}, std::pair{0, d}, std::pair{-d, 0}, std::pair{0, -d}})
                                if (differs(x + dx, y + dy)) {
                                    edge = true;
                                    break;
                                }
                        if (edge) {
                            std::copy(white.begin(), white.end(), px);
                        } else if (mode == "edge_only") {
                            std::copy(black.begin(), black.end(), px);
                        }
                        continue;
                    }
                    std::fill(acc.begin(), acc.end(), 0.0f);
                    float wsum = 0;
                    for (const auto& [dx, dy] : offs) {
                        if (differs(x + dx, y + dy)) continue;
                        const float a = src.at(x + dx, y + dy, cc);
                        for (std::size_t c = 0; c < cc; ++c) acc[c] += src.at(x + dx, y + dy, c) * a;
                        acc[cc] += a;
                        wsum += 1;
                    }
                    if (wsum > 0) {
                        for (std::size_t c = 0; c < n; ++c) px[c] = acc[c] / wsum;
                        unpremultiply_px(px, n);
                    }
                }
            return res;
        };
        k.cost = [](const Json& params, const Ctx&) {
            const std::string quality = text(params, "quality", "high");
            const double target = quality == "low" ? 4 : quality == "medium" ? 7 : 12;
            return 3.0 * 3.2 * target * target * 2.0;
        };
        all.push_back(std::move(k));
    }
    {  // ぼかし（シェイプ） (filter.blur.shapeBlur): the mean over the shape of the radius
        Kernel k;
        k.kind = "blur_shape";
        const auto radius = [](const Json& params, const Ctx& ctx) { return static_cast<float>(std::max(0.0, number(params, "radius", 10)) * ctx.s); };
        k.halo = [radius](const Json& params, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(radius(params, ctx))) + 1); };
        k.run = [radius](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const Spans spans(radius(params, ctx), shape_inside(text(params, "shape", "circle")));
            const std::size_t n = src.ch;
            const Prefix prefix(src, premultiplied(src), exact_bits(ctx.largest, spans.count));
            std::vector<float> res;
            res.reserve(out.area() * n);
            std::vector<double> acc(n);
            std::vector<float> px(n);
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x) {
                    prefix.sum(spans, x, y, acc.data());
                    for (std::size_t c = 0; c < n; ++c) px[c] = static_cast<float>(acc[c] / spans.count);
                    unpremultiply_px(px.data(), n);
                    res.insert(res.end(), px.begin(), px.end());
                }
            return res;
        };
        k.cost = [radius](const Json& params, const Ctx& ctx) { return 2.0 * (2.0 * std::ceil(radius(params, ctx)) + 1.0) * 5.0 + 20.0; };
        all.push_back(std::move(k));
    }
    {  // ぼかし（レンズ） (filter.blur.lensBlur): an iris (a polygon of its blades rounded by their curvature) as the kernel,
       // the highlights past the threshold brightened first so they bloom into its shape; with a depth (the layer's
       // transparency or its mask) the iris grows with the distance from the focal plane; the noise put back last
        Kernel k;
        k.kind = "blur_lens";
        const auto radius = [](const Json& params, const Ctx& ctx) { return static_cast<float>(std::clamp(number(params, "radius", 15), 0.0, 100.0) * ctx.s); };
        k.halo = [radius](const Json& params, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(radius(params, ctx))) + 1); };
        k.run = [radius](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch, cc = n - 1;
            const float rmax = radius(params, ctx);
            const std::string shape = text(params, "shape", "hexagon");
            const unsigned blades = shape == "triangle" ? 3 : shape == "square" ? 4 : shape == "pentagon" ? 5 : shape == "heptagon" ? 7 : shape == "octagon" ? 8 : 6;
            const float curv = static_cast<float>(std::clamp(number(params, "blade_curvature", 0), 0.0, 100.0) / 100.0);
            const float rot = static_cast<float>(number(params, "rotation", 0) * std::numbers::pi / 180.0);
            const auto iris = [blades, curv, rot](float u, float v) {
                const float rho = std::sqrt(u * u + v * v);
                const float pr = polygon_radius(std::atan2(v, u), blades, rot - kPi / 2.0f);
                return rho <= pr + (1.0f - pr) * curv;
            };
            std::vector<Spans> levels;
            for (int i = 0; i <= kLensLevels; ++i) levels.emplace_back(rmax * static_cast<float>(i) / kLensLevels, iris);
            // the highlights brightened (premultiplied; a page of inks as less ink, Lab's lightness alone)
            std::vector<float> p = premultiplied(src);
            const float boost = static_cast<float>(std::clamp(number(params, "brightness", 0), 0.0, 100.0) / 100.0 * 4.0);
            if (boost > 0) {
                const float th = static_cast<float>(std::clamp(number(params, "threshold", 255), 0.0, 255.0) / 255.0);
                const bool sub = ctx.space == Space::Cmyk || ctx.space == Space::Multi;
                std::vector<float> st(n);
                for (std::size_t i = 0; i < src.data.size(); i += n) {
                    const float a = src.data[i + cc];
                    if (!(a > 0)) continue;
                    for (std::size_t c = 0; c < n; ++c) st[c] = src.data[i + c];
                    const float kk = 1.0f + boost * smoothstep(th, th + 0.02f, lightness(ctx, st.data()));
                    const std::size_t colours = ctx.space == Space::Lab ? 1 : cc;
                    for (std::size_t c = 0; c < colours; ++c) p[i + c] = sub ? a - (a - p[i + c]) / kk : p[i + c] * kk;
                }
            }
            const Prefix prefix(src, p, exact_bits(ctx.largest * (1.0 + boost), levels.back().count));
            const std::string depth = text(params, "depth_map", "none");
            const bool invert = flag(params, "invert");
            const float focal = static_cast<float>(std::clamp(number(params, "focal_distance", 0), 0.0, 255.0) / 255.0);
            const float amount = static_cast<float>(std::clamp(number(params, "noise", 0), 0.0, 100.0) / 100.0 * 0.25);
            const bool gaussian = text(params, "distribution", "uniform") == "gaussian";
            const bool mono = flag(params, "monochromatic");
            const auto seed = static_cast<std::uint32_t>(std::clamp(number(params, "seed", 0), 0.0, 4294967295.0));
            const auto depth_at = [&](int x, int y) -> std::optional<float> {
                float d = 0;
                if (depth == "transparency") {
                    d = src.at(x, y, cc);
                } else if (depth == "layer_mask" && ctx.plane != nullptr) {
                    d = (*ctx.plane)[std::size_t(y) * ctx.grid_width + std::size_t(x)];
                } else {
                    return std::nullopt;
                }
                return invert ? 1.0f - d : d;
            };
            std::vector<float> res;
            res.reserve(out.area() * n);
            std::vector<double> acc(n), acc2(n);
            std::vector<float> px(n);
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x) {
                    const std::optional<float> d = depth_at(x, y);
                    const float level = d ? std::min(std::fabs(*d - focal) * kLensLevels, static_cast<float>(kLensLevels)) : static_cast<float>(kLensLevels);
                    const int lo = static_cast<int>(std::floor(level));
                    const float f = level - static_cast<float>(lo);
                    prefix.sum(levels[std::size_t(lo)], x, y, acc.data());
                    for (std::size_t c = 0; c < n; ++c) acc[c] /= levels[std::size_t(lo)].count;
                    if (f > 1e-4f && lo < kLensLevels) {
                        prefix.sum(levels[std::size_t(lo + 1)], x, y, acc2.data());
                        for (std::size_t c = 0; c < n; ++c) acc[c] += (acc2[c] / levels[std::size_t(lo + 1)].count - acc[c]) * f;
                    }
                    for (std::size_t c = 0; c < n; ++c) px[c] = static_cast<float>(acc[c]);
                    unpremultiply_px(px.data(), n);
                    if (amount > 0 && px[cc] > 0) {
                        for (std::size_t c = 0; c < cc; ++c) {
                            const auto ch = static_cast<std::uint32_t>(mono ? 0 : c);
                            if (!gaussian) {
                                px[c] += (hash01(x, y, ch + 200, seed) - 0.5f) * 2.0f * amount;
                            } else {
                                const float u1 = std::max(hash01(x, y, ch * 2 + 301, seed), 1e-7f), u2 = hash01(x, y, ch * 2 + 302, seed);
                                px[c] += std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * kPi * u2) * amount * 0.5f;
                            }
                        }
                    }
                    res.insert(res.end(), px.begin(), px.end());
                }
            return res;
        };
        k.cost = [radius](const Json& params, const Ctx& ctx) { return 4.0 * (2.0 * std::ceil(radius(params, ctx)) + 1.0) * 5.0 + 40.0; };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
