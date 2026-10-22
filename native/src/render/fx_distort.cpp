// PC4 節 D2 (SPEC.md COLOR-08「フィルター」): フィルター → 変形's kernels — 置き換え, つまむ, 極座標, 波紋, シアー, 球面, 渦巻き,
// 波形, ジグザグ — and the pieces D2's kernels share (render/fx_d2.hpp). PhotoCraft v0.2.0's algo/src/distort.rs (remap,
// centre, twirl, pinch, spherize, wave, ripple, polar) and distort2.rs (displace, curve_at, shear, zigzag), MIT:
// native/third_party/photocraft/NOTICE.md. Each maps a pixel's centre back into the base box (a selection's box, else the
// layer's grid) and samples the window there (the whole base box is read once, Halo::whole); the lengths — 波形's
// wavelengths and amplitudes, 波紋's sizes, 置き換え's 128 px at 100 % and its map's pixels when tiled — are pixels of the
// working 200 dpi (Ctx::s of the layer's).

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "core/pyops.hpp"
#include "render/fx_d2.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

using core::Json;

// --- the pieces D2's kernels share ---------------------------------------------------------------------------------------

namespace d2 {

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() && !v->is_boolean() && std::isfinite(v->get<double>()) ? v->get<double>() : fallback;
}

std::string text(const Json& params, std::string_view key, std::string_view fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

bool flag(const Json& params, std::string_view key, bool fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
}

std::uint32_t seed_of(const Json& params) {
    const double v = number(params, "seed", 0);
    return v >= 0 && v <= 4294967295.0 ? static_cast<std::uint32_t>(std::llround(v)) : 0u;
}

std::array<float, 3> colour(const Json& params, std::string_view key, std::array<float, 3> fallback) {
    const Json* v = core::get(params, key);
    if (v == nullptr || !v->is_array() || v->size() < 3) return fallback;
    std::array<float, 3> out{};
    for (std::size_t c = 0; c < 3; ++c) {
        const Json& each = (*v)[c];
        if (!each.is_number() || each.is_boolean()) return fallback;
        out[c] = static_cast<float>(std::clamp(each.get<double>(), 0.0, 255.0) / 255.0);
    }
    return out;
}

Edge edge_of(const std::string& undefined) {
    if (undefined == "repeat") return Edge::Repeat;
    if (undefined == "transparent") return Edge::Transparent;
    return Edge::Wrap;
}

Centre centre(const Rect& b) {
    const float w = static_cast<float>(b.width()), h = static_cast<float>(b.height());
    return Centre{static_cast<float>(b.x0) + w / 2.0f, static_cast<float>(b.y0) + h / 2.0f, std::min(w, h) / 2.0f};
}

std::vector<float> remap(const Window& src, const Rect& out, const Ctx& ctx, Edge edge, const std::function<std::pair<float, float>(float, float)>& f) {
    const std::size_t n = src.ch;
    std::vector<float> res(out.area() * n);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const auto [sx, sy] = f(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            src.sample(sx, sy, edge, ctx.bounds, ctx.alpha, res.data() + i);
        }
    return res;
}

float light(const Ctx& ctx, const float* px) {
    switch (ctx.space) {
        case Space::Rgb: return 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
        case Space::Gray:
        case Space::Lab: return px[0];
        case Space::Cmyk: return (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) * px[3];
        case Space::Multi: {
            float l = 1.0f;
            for (unsigned c = 0; c < ctx.colours; ++c) l *= std::clamp(px[c], 0.0f, 1.0f);
            return l;
        }
    }
    return px[0];
}

bool lightness_channel(const Ctx& ctx, std::size_t c) { return !(ctx.space == Space::Lab && c > 0); }

void gauss_blur_exact(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, float sigma, double largest) {
    if (sigma < 0.3f || w == 0 || h == 0) return;
    const std::array<int, 3> radii = gauss_box_radii(sigma);
    // (whole units of `largest` × 2^-bits: a line's sum of 2r + 1 of them within 2^62)
    const int widest = 2 * std::max({radii[0], radii[1], radii[2]}) + 1;
    const int bits = std::min(48, 61 - static_cast<int>(std::bit_width(static_cast<unsigned>(widest))));
    const double most = std::max(1e-30, largest);
    whole_box_passes(buf, w, h, n, {radii[0], radii[1], radii[2]}, std::ldexp(most, -bits), most);
}

}  // namespace d2

namespace {

using d2::number;
using d2::text;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

// 球面's one-dimensional mapping (distort.rs sphere_1d): rn in [0, 1); a positive amount magnifies the centre.
float sphere_1d(float rn, float a) {
    const float target = a >= 0.0f ? std::asin(rn) * 2.0f / kPi : std::sin(rn * std::numbers::pi_v<float> / 2.0f);
    return rn + std::fabs(a) * (target - rn);
}

// 波形's shapes (distort.rs shape).
float wave_shape(const std::string& type, float phase) {
    float p = std::fmod(phase, kTau);
    if (p < 0) p += kTau;
    p /= kTau;
    if (type == "triangle") return 1.0f - 4.0f * std::fabs(p - 0.5f);
    if (type == "square") return p < 0.5f ? 1.0f : -1.0f;
    return std::sin(phase);
}

// シアー's Catmull–Rom curve through sorted [t, value] points, its ends clamped (distort2.rs curve_at).
float curve_at(const std::vector<std::array<float, 2>>& pts, float t) {
    if (pts.empty()) return 0.0f;
    if (pts.size() == 1) return pts[0][1];
    if (t <= pts[0][0]) return pts[0][1];
    const std::size_t last = pts.size() - 1;
    if (t >= pts[last][0]) return pts[last][1];
    std::size_t i = 0;
    for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
        if (t >= pts[k][0] && t <= pts[k + 1][0]) {
            i = k;
            break;
        }
    }
    const auto& p1 = pts[i];
    const auto& p2 = pts[i + 1];
    const auto& p0 = i > 0 ? pts[i - 1] : p1;
    const auto& p3 = i + 2 <= last ? pts[i + 2] : p2;
    const float span = std::max(p2[0] - p1[0], 1e-6f);
    const float s = (t - p1[0]) / span;
    const float m1 = i > 0 ? (p2[1] - p0[1]) / std::max(p2[0] - p0[0], 1e-6f) * span : p2[1] - p1[1];
    const float m2 = i + 2 <= last ? (p3[1] - p1[1]) / std::max(p3[0] - p1[0], 1e-6f) * span : p2[1] - p1[1];
    const float s2 = s * s, s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * p1[1] + (s3 - 2.0f * s2 + s) * m1 + (-2.0f * s3 + 3.0f * s2) * p2[1] + (s3 - s2) * m2;
}

// A map's value as a move, −1..1: its 128 none (as Photoshop's, so a mid-grey — and a clear pixel, laid over it — moves
// nothing), 255 the whole scale, 0 the whole the other way (PhotoCraft's (v − 0.5) · 2 but for 128).
float displacement(float v) {
    const float level = v * 255.0f;
    return level >= 128.0f ? (level - 128.0f) / 127.0f : (level - 128.0f) / 128.0f;
}

// 置き換え's map (distort2.rs map_sample): channel c's bilinear value at the map's (u, v), its edges clamped.
float map_sample(const Picture& map, float u, float v, unsigned c) {
    const float fx = u - 0.5f, fy = v - 0.5f;
    const float bx = std::floor(fx), by = std::floor(fy);
    const float ax = fx - bx, ay = fy - by;
    const int x0 = static_cast<int>(bx), y0 = static_cast<int>(by);
    const float top = map.at(x0, y0, c) + (map.at(x0 + 1, y0, c) - map.at(x0, y0, c)) * ax;
    const float bottom = map.at(x0, y0 + 1, c) + (map.at(x0 + 1, y0 + 1, c) - map.at(x0, y0 + 1, c)) * ax;
    return top + (bottom - top) * ay;
}

float rem_euclid(float a, float b) {
    const float r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}

// (a pixel's inverse mapping: a bilinear sample of four pixels a channel)
double remap_cost(double extra = 0) { return 4.0 * 5.0 * 2.0 + extra; }

}  // namespace

Kernels distort_kernels() {
    Kernels all;
    const auto whole = [](const Json&, const Ctx&) { return Halo::whole(); };
    {  // 置き換え (filter.distort.displace): the map's red shifts a pixel across, its green down — 128 none, 255 or 0 the
       // whole scale, 128 working pixels at 100 % —, the map stretched over the base box or tiled from its corner (a map's
       // pixel a working pixel); without a map (a caller that gives none) the identity
        Kernel k;
        k.kind = "distort_displace";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            if (ctx.map == nullptr || ctx.map->empty()) return src.crop(out);
            const Picture& map = *ctx.map;
            const Rect& b = ctx.bounds;
            const float mw = static_cast<float>(map.width), mh = static_cast<float>(map.height);
            const float bw = static_cast<float>(std::max(1, b.width())), bh = static_cast<float>(std::max(1, b.height()));
            const bool stretch = text(params, "fit", "stretch") != "tile";
            const float s = static_cast<float>(ctx.s);
            const float sx = static_cast<float>(number(params, "horizontal", 10) / 100.0 * 128.0) * s;
            const float sy = static_cast<float>(number(params, "vertical", 10) / 100.0 * 128.0) * s;
            return d2::remap(src, out, ctx, d2::edge_of(text(params, "undefined_areas", "repeat")), [&](float x, float y) {
                const float lx = x - static_cast<float>(b.x0), ly = y - static_cast<float>(b.y0);
                const float u = stretch ? lx / bw * mw : rem_euclid(lx / s, mw);
                const float v = stretch ? ly / bh * mh : rem_euclid(ly / s, mh);
                const float dx = displacement(map_sample(map, u, v, 0)) * sx;
                const float dy = displacement(map_sample(map, u, v, 1)) * sy;
                return std::pair{x + dx, y + dy};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(24); };
        // (its map read — a file decoded — or drawn — a page at 200 dpi —: a heavy filter's work, whatever the layer — its
        // window's preview and the run on a thread of their own: D2's review P3-3)
        k.setup = [](const Json& params) {
            const Json* map = core::get(params, "map");
            const Json* data = core::get(params, "map_data");
            return (map != nullptr && !map->is_null()) || (data != nullptr && !data->is_null()) ? kHeavyWork : 0.0;
        };
        all.push_back(std::move(k));
    }
    {  // つまむ (filter.distort.pinch): the circle inside the base box pulled in (a positive amount) or pushed out
        Kernel k;
        k.kind = "distort_pinch";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const d2::Centre c = d2::centre(ctx.bounds);
            const float a = static_cast<float>(std::clamp(number(params, "amount", 50) / 100.0, -1.0, 1.0));
            return d2::remap(src, out, ctx, Edge::Repeat, [&](float x, float y) {
                const float dx = x - c.x, dy = y - c.y;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r >= c.r || r <= 0.0f) return std::pair{x, y};
                const float rs = c.r * std::pow(r / c.r, 1.0f - a * 0.5f);
                return std::pair{c.x + dx * rs / r, c.y + dy * rs / r};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(20); };
        all.push_back(std::move(k));
    }
    {  // 極座標 (filter.distort.polarCoordinates): 直交座標を極座標に (an angle from 12 o'clock clockwise across, the radius
       // down) or back; outside, clear
        Kernel k;
        k.kind = "distort_polar_coordinates";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const Rect& b = ctx.bounds;
            const float w = static_cast<float>(std::max(1, b.width())), h = static_cast<float>(std::max(1, b.height()));
            const float cx = static_cast<float>(b.x0) + w / 2.0f, cy = static_cast<float>(b.y0) + h / 2.0f;
            const float rmax = std::min(w, h) / 2.0f;
            const bool to_polar = text(params, "mode", "rectangular_to_polar") != "polar_to_rectangular";
            return d2::remap(src, out, ctx, Edge::Transparent, [&](float x, float y) {
                if (to_polar) {
                    const float dx = x - cx, dy = y - cy;
                    const float theta = rem_euclid(std::atan2(dx, -dy), kTau);
                    const float r = std::sqrt(dx * dx + dy * dy);
                    return std::pair{static_cast<float>(b.x0) + theta / kTau * w, static_cast<float>(b.y0) + r / rmax * h};
                }
                const float theta = (x - static_cast<float>(b.x0)) / w * kTau;
                const float r = (y - static_cast<float>(b.y0)) / h * rmax;
                return std::pair{cx + r * std::sin(theta), cy - r * std::cos(theta)};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(30); };
        all.push_back(std::move(k));
    }
    {  // 波紋 (filter.distort.ripple): sine ripples across and down, 8, 16 or 32 working pixels long (小・中・大), the amount %
       // of an eighth of that
        Kernel k;
        k.kind = "distort_ripple";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::string size = text(params, "size", "medium");
            const float len = (size == "small" ? 8.0f : size == "large" ? 32.0f : 16.0f) * static_cast<float>(ctx.s);
            const float amp = static_cast<float>(number(params, "amount", 100) / 100.0) * len / 8.0f;
            const Rect& b = ctx.bounds;
            return d2::remap(src, out, ctx, Edge::Repeat, [&](float x, float y) {
                const float lx = x - static_cast<float>(b.x0), ly = y - static_cast<float>(b.y0);
                return std::pair{x + amp * std::sin(kTau * ly / len), y + amp * std::sin(kTau * lx / len)};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(20); };
        all.push_back(std::move(k));
    }
    {  // シアー (filter.distort.shear): each row of the base box moved across by the curve through its points (t from the top
       // to the bottom, the offset in halves of its width)
        Kernel k;
        k.kind = "distort_shear";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            std::vector<std::array<float, 2>> pts;
            if (const Json* points = core::get(params, "points"); points != nullptr && points->is_array()) {
                for (const Json& p : *points) {
                    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) continue;
                    pts.push_back({std::clamp(p[0].get<float>(), 0.0f, 1.0f), std::clamp(p[1].get<float>(), -1.0f, 1.0f)});
                }
            }
            std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
            const Rect& b = ctx.bounds;
            const float bw = static_cast<float>(std::max(1, b.width())), bh = static_cast<float>(std::max(1, b.height()));
            std::vector<float> rows;  // (each row's offset: shared by its pixels)
            for (int y = out.y0; y < out.y1; ++y)
                rows.push_back(curve_at(pts, (static_cast<float>(y) + 0.5f - static_cast<float>(b.y0)) / bh) * bw / 2.0f);
            return d2::remap(src, out, ctx, d2::edge_of(text(params, "undefined_areas", "wrap")), [&](float x, float y) {
                const int i = std::clamp(static_cast<int>(y - 0.5f) - out.y0, 0, static_cast<int>(rows.size()) - 1);
                return std::pair{x - rows[static_cast<std::size_t>(i)], y};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(); };
        all.push_back(std::move(k));
    }
    {  // 球面 (filter.distort.spherize): the circle inside the base box (or its width, or its height) bulged out or in
        Kernel k;
        k.kind = "distort_spherize";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const Rect& b = ctx.bounds;
            const d2::Centre c = d2::centre(b);
            const float hx = static_cast<float>(b.width()) / 2.0f, hy = static_cast<float>(b.height()) / 2.0f;
            const float a = static_cast<float>(std::clamp(number(params, "amount", 100) / 100.0, -1.0, 1.0));
            const std::string mode = text(params, "mode", "normal");
            return d2::remap(src, out, ctx, Edge::Repeat, [&](float x, float y) {
                const float dx = x - c.x, dy = y - c.y;
                if (mode == "horizontal_only") {
                    if (std::fabs(dx) >= hx) return std::pair{x, y};
                    return std::pair{c.x + (dx < 0 ? -1.0f : dx > 0 ? 1.0f : 0.0f) * hx * sphere_1d(std::fabs(dx) / hx, a), y};
                }
                if (mode == "vertical_only") {
                    if (std::fabs(dy) >= hy) return std::pair{x, y};
                    return std::pair{x, c.y + (dy < 0 ? -1.0f : dy > 0 ? 1.0f : 0.0f) * hy * sphere_1d(std::fabs(dy) / hy, a)};
                }
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r >= c.r || r <= 0.0f) return std::pair{x, y};
                const float rs = c.r * sphere_1d(r / c.r, a);
                return std::pair{c.x + dx * rs / r, c.y + dy * rs / r};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(20); };
        all.push_back(std::move(k));
    }
    {  // 渦巻き (filter.distort.twirl): the circle inside the base box turned by the angle at its centre, less towards its edge
        Kernel k;
        k.kind = "distort_twirl";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const d2::Centre c = d2::centre(ctx.bounds);
            const float a = static_cast<float>(number(params, "angle", 50)) * kPi / 180.0f;
            return d2::remap(src, out, ctx, Edge::Repeat, [&](float x, float y) {
                const float dx = x - c.x, dy = y - c.y;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r >= c.r || c.r <= 0.0f) return std::pair{x, y};
                const float t = a * (1.0f - r / c.r);
                const float s = std::sin(t), co = std::cos(t);
                return std::pair{c.x + dx * co - dy * s, c.y + dx * s + dy * co};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(20); };
        all.push_back(std::move(k));
    }
    {  // 波形 (filter.distort.wave): the mean of up to 64 generators — each a wave of its own wavelength and amplitude between
       // the least and the most (working pixels), phase, of the seed — moving a pixel across by its row and down by its
       // column
        Kernel k;
        k.kind = "distort_wave";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const int n = std::clamp(static_cast<int>(std::lround(number(params, "generators", 5))), 1, 64);
            const std::uint32_t seed = d2::seed_of(params);
            const float s = static_cast<float>(ctx.s);
            const float wl0 = static_cast<float>(number(params, "wavelength_min", 10)), wl1 = static_cast<float>(number(params, "wavelength_max", 120));
            const float am0 = static_cast<float>(number(params, "amplitude_min", 5)), am1 = static_cast<float>(number(params, "amplitude_max", 35));
            const auto lerp = [](float a, float b, float t) { return a + std::max(b - a, 0.0f) * t; };
            struct Generator {
                float len, amp, p1, p2;
            };
            std::vector<Generator> gens;
            for (int i = 0; i < n; ++i) {
                const auto r = [i, seed](int key) { return hash01(i, key, 7, seed); };
                gens.push_back({std::max(lerp(wl0, wl1, r(1)), 1.0f) * s, lerp(am0, am1, r(2)) * s, r(3) * kTau, r(4) * kTau});
            }
            const std::string type = text(params, "type", "sine");
            const Rect& b = ctx.bounds;
            const float count = static_cast<float>(n);
            return d2::remap(src, out, ctx, d2::edge_of(text(params, "undefined_areas", "wrap")), [&](float x, float y) {
                const float lx = x - static_cast<float>(b.x0), ly = y - static_cast<float>(b.y0);
                float dx = 0, dy = 0;
                for (const Generator& g : gens) {
                    dx += g.amp * wave_shape(type, kTau * ly / g.len + g.p1);
                    dy += g.amp * wave_shape(type, kTau * lx / g.len + g.p2);
                }
                return std::pair{x + dx / count, y + dy / count};
            });
        };
        k.cost = [](const Json& params, const Ctx&) { return remap_cost(12.0 * std::clamp(number(params, "generators", 5), 1.0, 64.0)); };
        all.push_back(std::move(k));
    }
    {  // ジグザグ (filter.distort.zigZag): ripples in the circle inside the base box, out from its centre, around it, or both
       // (水面の波紋), fading to its edge
        Kernel k;
        k.kind = "distort_zigzag";
        k.halo = whole;
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const d2::Centre c = d2::centre(ctx.bounds);
            const float a = static_cast<float>(std::clamp(number(params, "amount", 10) / 100.0, -1.0, 1.0));
            const float ridges = std::max(static_cast<float>(std::clamp(number(params, "ridges", 5), 0.0, 20.0)), 0.5f);
            const std::string style = text(params, "style", "pond_ripples");
            return d2::remap(src, out, ctx, Edge::Repeat, [&](float x, float y) {
                const float dx = x - c.x, dy = y - c.y;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r >= c.r || r <= 0.0f || a == 0.0f) return std::pair{x, y};
                const float rn = r / c.r;
                const float wave = std::sin(kTau * ridges * rn) * (1.0f - rn);
                float radial = 0, angular = 0;
                if (style == "out_from_center") {
                    radial = a * wave * c.r / (4.0f * ridges);
                } else if (style == "around_center") {
                    angular = a * wave * kPi / (2.0f * ridges);
                } else {
                    radial = a * wave * c.r / (8.0f * ridges);
                    angular = a * wave * kPi / (4.0f * ridges);
                }
                const float th = std::atan2(dy, dx) + angular;
                const float rs = std::max(r + radial, 0.0f);
                return std::pair{c.x + rs * std::cos(th), c.y + rs * std::sin(th)};
            });
        };
        k.cost = [](const Json&, const Ctx&) { return remap_cost(40); };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
