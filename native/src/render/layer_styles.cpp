// PC4 E6 (layer-styles@1, render/layer_styles.hpp): PhotoCraft v0.2.0 crates/compose/src/effects.rs (lines 1-1300: the
// maps — exact Euclidean and 5 × 5 chamfer distance fields, the tent blur, spread and choke, the shadows', glows', satin's
// and bevel's coverage and height maps, the contours and gradients — and composite_with_effects) and crates/compose/src/
// lib.rs (Blend If, lines 708-767), crates/compose/src/adjust.rs (curve_lut), MIT: native/third_party/photocraft/NOTICE.md.
// Ported to Genko's pages: lengths in mm at the drawing's resolution, the maps made over the area drawn grown by the
// effects' reach (a part of a page drawn alone gives the same pixels there as the whole page), the colours and blends of
// the page's way (Mixing), the patterns through render/patterns. A layer's content is a picture (the vector outline of a
// Photoshop shape layer is not kept apart: every layer is a raster one's here).

#include "render/layer_styles.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>

#include "core/layer_styles.hpp"
#include "core/error.hpp"
#include "core/pyops.hpp"
#include "render/color_canvas.hpp"

namespace genko::render::layer_styles {

using core::Json;

namespace {

constexpr float kInsideEps = 0.5f / 255.0f;  // (alpha above which a pixel is of the shape: half an 8-bit step)
constexpr float kChamferInf = 1e20f;
constexpr float kFar = 1.0e9f;
constexpr float kBevelHEps = 1e-5f;
constexpr double kMaxReachPx = 1024;  // (an effect reaching further is cut there: the maps' size bound — 43 mm at 600 dpi)

std::atomic<std::size_t> g_maps{0};

// A single-channel map over a box.
struct Map {
    int w = 0, h = 0;
    std::vector<float> v;
    Map() = default;
    Map(int w_, int h_, float fill) : w(w_), h(h_), v(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), fill) {}
    float get(long x, long y) const { return x < 0 || y < 0 || x >= w || y >= h ? 0.f : v[static_cast<std::size_t>(y) * w + x]; }
};

// A copy shifted by (dx, dy) (bilinear); outside reads `outside`.
Map shifted(const Map& m, float dx, float dy, float outside) {
    Map out(m.w, m.h, 0.f);
    const float fx = std::floor(dx), fy = std::floor(dy);
    const float ax = dx - fx, ay = dy - fy;
    const auto s = [&](long x, long y) { return x < 0 || y < 0 || x >= m.w || y >= m.h ? outside : m.v[static_cast<std::size_t>(y) * m.w + x]; };
    for (long y = 0; y < m.h; ++y) {
        for (long x = 0; x < m.w; ++x) {
            const long sx = x - static_cast<long>(fx), sy = y - static_cast<long>(fy);
            const float top = s(sx, sy) * (1 - ax) + s(sx - 1, sy) * ax;
            const float bot = s(sx, sy - 1) * (1 - ax) + s(sx - 1, sy - 1) * ax;
            out.v[static_cast<std::size_t>(y) * m.w + x] = top * (1 - ay) + bot * ay;
        }
    }
    return out;
}

// 1D squared distance transform (Felzenszwalb & Huttenlocher), with each sample's nearest seed.
void dt1(const float* f, float* out, std::size_t n, std::vector<std::size_t>& v, std::vector<float>& z, std::size_t* near) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::size_t k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (std::size_t q = 1; q < n; ++q) {
        for (;;) {
            const std::size_t p = v[k];
            const float s = ((f[q] + static_cast<float>(q * q)) - (f[p] + static_cast<float>(p * p))) / (2.0f * static_cast<float>(q) - 2.0f * static_cast<float>(p));
            if (s <= z[k] && k > 0) {
                --k;
                continue;
            }
            if (s <= z[k]) {
                v[0] = q;
                z[0] = -inf;
                z[1] = inf;
                break;
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = inf;
            break;
        }
    }
    k = 0;
    for (std::size_t q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const std::size_t p = v[k];
        const float d = static_cast<float>(q) - static_cast<float>(p);
        out[q] = d * d + f[p];
        near[q] = p;
    }
}

// The exact Euclidean distance of every pixel to the nearest `inside` one (0 there), and that pixel's index.
std::pair<std::vector<float>, std::vector<std::size_t>> edt_nearest(const std::vector<char>& inside, int w, int h) {
    constexpr float kInf = 1e20f;
    const std::size_t count = static_cast<std::size_t>(w) * h;
    std::vector<float> g(count);
    for (std::size_t i = 0; i < count; ++i) g[i] = inside[i] ? 0.f : kInf;
    std::vector<std::size_t> near_row(count), nearest(count);
    const std::size_t n = static_cast<std::size_t>(std::max(w, h));
    std::vector<float> f(n), o(n), z(n + 1);
    std::vector<std::size_t> v(n), nr(n);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[y] = g[static_cast<std::size_t>(y) * w + x];
        dt1(f.data(), o.data(), static_cast<std::size_t>(h), v, z, nr.data());
        for (int y = 0; y < h; ++y) {
            g[static_cast<std::size_t>(y) * w + x] = o[y];
            near_row[static_cast<std::size_t>(y) * w + x] = nr[y];
        }
    }
    for (int y = 0; y < h; ++y) {
        std::copy(g.begin() + static_cast<std::ptrdiff_t>(y) * w, g.begin() + static_cast<std::ptrdiff_t>(y + 1) * w, f.begin());
        dt1(f.data(), o.data(), static_cast<std::size_t>(w), v, z, nr.data());
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            g[i] = std::sqrt(o[x]);
            const std::size_t px = nr[x];
            nearest[i] = near_row[static_cast<std::size_t>(y) * w + px] * static_cast<std::size_t>(w) + px;
        }
    }
    return {std::move(g), std::move(nearest)};
}

// A chamfer distance transform with a 5 × 5 mask of exact Euclidean step lengths (1, √2, √5) from per-pixel start values
// (kChamferInf: not a seed) — Photoshop's stroke distances follow it.
std::vector<float> chamfer_from(std::vector<float> d, int w, int h) {
    constexpr float S2 = std::numbers::sqrt2_v<float>;
    constexpr float S5 = 2.236068f;
    static constexpr std::array<std::tuple<int, int, float>, 8> kFwd{
        {{-1, 0, 1.0f}, {-1, -1, S2}, {0, -1, 1.0f}, {1, -1, S2}, {-1, -2, S5}, {1, -2, S5}, {-2, -1, S5}, {2, -1, S5}}};
    const auto relax = [&](int x, int y, int sign) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        float best = d[i];
        for (const auto& [ox, oy, wt] : kFwd) {
            const int nx = x + ox * sign, ny = y + oy * sign;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const float c = d[static_cast<std::size_t>(ny) * w + nx] + wt;
            if (c < best) best = c;
        }
        d[i] = best;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) relax(x, y, 1);
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) relax(x, y, -1);
    return d;
}

enum class Metric { Euclidean, Chamfer };

// The alpha against its 3 × 3 neighbourhood's highest: an edge's coverage apart from the fill's own opacity.
Map local_coverage(const Map& s) {
    Map out = s;
    for (long y = 0; y < s.h; ++y) {
        for (long x = 0; x < s.w; ++x) {
            const float a = s.get(x, y);
            if (a <= kInsideEps) continue;
            float mx = a;
            for (long dy = -1; dy <= 1; ++dy)
                for (long dx = -1; dx <= 1; ++dx) mx = std::max(mx, s.get(x + dx, y + dy));
            out.v[static_cast<std::size_t>(y) * s.w + x] = std::min(a / mx, 1.0f);
        }
    }
    return out;
}

// The distance from each pixel centre outside the shape to its edge (the edge inside the nearest pixel of the shape
// placed by that pixel's coverage, as Photoshop does); ≤ 0 inside.
std::vector<float> dist_outside(const Map& s, Metric m = Metric::Euclidean) {
    const std::size_t count = s.v.size();
    std::vector<float> out(count);
    if (m == Metric::Chamfer) {
        std::vector<float> start(count);
        for (std::size_t i = 0; i < count; ++i) start[i] = s.v[i] > kInsideEps ? 1.0f - std::min(s.v[i], 1.0f) : kChamferInf;
        const std::vector<float> d = chamfer_from(std::move(start), s.w, s.h);
        for (std::size_t i = 0; i < count; ++i) out[i] = s.v[i] > kInsideEps ? -0.5f : d[i] - 0.5f;
        return out;
    }
    std::vector<char> inside(count);
    for (std::size_t i = 0; i < count; ++i) inside[i] = s.v[i] > kInsideEps;
    const auto [d, near] = edt_nearest(inside, s.w, s.h);
    for (std::size_t i = 0; i < count; ++i) out[i] = inside[i] ? -0.5f : d[i] - 0.5f + (1.0f - std::min(s.v[near[i]], 1.0f));
    return out;
}

// The distance from each pixel centre inside the shape to its edge; ≤ 0 outside.
std::vector<float> dist_inside(const Map& s, Metric m = Metric::Euclidean) {
    const std::size_t count = s.v.size();
    std::vector<float> out(count);
    if (m == Metric::Chamfer) {
        const Map cov = local_coverage(s);
        std::vector<float> start(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float a = s.v[i], c = cov.v[i];
            start[i] = a <= kInsideEps ? 0.f : c < 1.0f - kInsideEps ? c : kChamferInf;
        }
        const std::vector<float> d = chamfer_from(std::move(start), s.w, s.h);
        for (std::size_t i = 0; i < count; ++i) out[i] = s.v[i] <= kInsideEps ? -0.5f : d[i] - 0.5f;
        return out;
    }
    std::vector<char> outside(count);
    for (std::size_t i = 0; i < count; ++i) outside[i] = s.v[i] <= kInsideEps;
    const auto [d, near] = edt_nearest(outside, s.w, s.h);
    (void)near;
    for (std::size_t i = 0; i < count; ++i) out[i] = outside[i] ? -0.5f : d[i] - 0.5f;
    return out;
}

// --- the tent: two box passes of a (fractional) width, as Photoshop blurs an effect's falloff ---------------------------

struct BoxGeom {
    long r;
    double f, norm;
};

BoxGeom box_geom(double bw) {
    const double half = std::max(bw, 1.0) / 2.0;
    const long r = std::max<long>(0, static_cast<long>(std::ceil(half - 0.5)));
    const double f = std::clamp(half - (static_cast<double>(r) - 0.5), 0.0, 1.0);
    return {r, f, 1.0 / std::max(bw, 1.0)};
}

// One box pass over `src` (zero outside it) at x0 .. x0 + n, a running sum.
void box_line(const double* src, long len, long x0, double* dst, long n, double bw) {
    const BoxGeom g = box_geom(bw);
    const auto at = [&](long i) { return i >= 0 && i < len ? src[i] : 0.0; };
    if (g.r == 0) {
        for (long k = 0; k < n; ++k) dst[k] = at(x0 + k);
        return;
    }
    double inner = 0;
    for (long i = x0 - (g.r - 1); i <= x0 + (g.r - 1); ++i) inner += at(i);
    for (long k = 0; k < n; ++k) {
        const long x = x0 + k;
        dst[k] = (inner + g.f * (at(x - g.r) + at(x + g.r))) * g.norm;
        inner += at(x + g.r) - at(x - g.r + 1);
    }
}

// Two box passes along a line (the first evaluated r beyond each end so the second sees what lies there).
void tent_line(const float* src, float* dst, long len, double bw, std::vector<double>& s, std::vector<double>& mid, std::vector<double>& out) {
    const long r = box_geom(bw).r;
    s.assign(src, src + len);
    mid.assign(static_cast<std::size_t>(len + 2 * r), 0.0);
    box_line(s.data(), len, -r, mid.data(), len + 2 * r, bw);
    out.assign(static_cast<std::size_t>(len), 0.0);
    box_line(mid.data(), len + 2 * r, r, out.data(), len, bw);
    for (long k = 0; k < len; ++k) dst[k] = static_cast<float>(out[k]);
}

// Rows of a `w`-wide buffer through tent_line, in parallel (each row on its own: the same values whatever the threads).
void tent_rows(const std::vector<float>& src, std::vector<float>& dst, int w, int h, double bw) {
    if (w <= 0 || h <= 0) return;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = src.size() < (1u << 16) ? 1u : std::min(hw, 8u);
    const auto run = [&](int y0, int y1) {
        std::vector<double> s, mid, out;
        for (int y = y0; y < y1; ++y) {
            tent_line(src.data() + static_cast<std::size_t>(y) * w, dst.data() + static_cast<std::size_t>(y) * w, w, bw, s, mid, out);
        }
    };
    if (threads == 1) {
        run(0, h);
        return;
    }
    const int rows = (h + static_cast<int>(threads) - 1) / static_cast<int>(threads);
    std::vector<std::thread> pool;
    for (int y = 0; y < h; y += rows) pool.emplace_back(run, y, std::min(h, y + rows));
    for (std::thread& t : pool) t.join();
}

std::vector<float> transpose(const std::vector<float>& v, int w, int h) {
    std::vector<float> out(v.size());
    constexpr int B = 32;
    for (int by = 0; by < h; by += B)
        for (int bx = 0; bx < w; bx += B)
            for (int y = by; y < std::min(by + B, h); ++y)
                for (int x = bx; x < std::min(bx + B, w); ++x) out[static_cast<std::size_t>(x) * h + y] = v[static_cast<std::size_t>(y) * w + x];
    return out;
}

void tent(Map& m, double width) {
    if (!(width > 1.0) || m.v.empty()) return;  // (a width of 1 or less is the identity)
    std::vector<float> a(m.v.size());
    tent_rows(m.v, a, m.w, m.h, width);
    const std::vector<float> t = transpose(a, m.w, m.h);
    std::vector<float> b(t.size());
    tent_rows(t, b, m.h, m.w, width);
    m.v = transpose(b, m.h, m.w);
}

void blur(Map& m, double size) {
    if (std::isfinite(size)) tent(m, size);
}

// Grows the shape by r pixels (anti-aliased), its own soft edge kept.
Map dilate(const Map& s, double r) {
    if (r <= 0) return s;
    const std::vector<float> d = dist_outside(s);
    Map out = s;
    for (std::size_t i = 0; i < out.v.size(); ++i) {
        float& o = out.v[i];
        o = d[i] < 0 ? std::min(o + static_cast<float>(r), 1.0f) : std::max(o, std::clamp(static_cast<float>(r) + 0.5f - d[i], 0.0f, 1.0f));
    }
    return out;
}

// --- contours ------------------------------------------------------------------------------------------------------

constexpr std::size_t kLut = 4096;

// A natural cubic spline through the points (adjust.rs curve_lut), 0..1.
std::vector<float> curve_lut(const Json& points) {
    std::vector<std::pair<double, double>> pts;
    for (const Json& p : points) pts.emplace_back(p[0].get<double>(), p[1].get<double>());
    std::sort(pts.begin(), pts.end());
    std::vector<std::pair<double, double>> kept;
    for (const auto& p : pts)
        if (kept.empty() || std::fabs(p.first - kept.back().first) >= 1e-6) kept.push_back(p);
    std::vector<float> out(kLut);
    if (kept.size() < 2) {
        for (std::size_t i = 0; i < kLut; ++i) out[i] = static_cast<float>(i) / (kLut - 1);
        return out;
    }
    const std::size_t n = kept.size();
    std::vector<double> m2(n, 0.0);
    if (n > 2) {
        std::vector<double> c(n, 0.0), d(n, 0.0);
        for (std::size_t i = 1; i + 1 < n; ++i) {
            const double h0 = kept[i].first - kept[i - 1].first, h1 = kept[i + 1].first - kept[i].first;
            const double a = h0 / 6.0, b = (h0 + h1) / 3.0, cc = h1 / 6.0;
            const double r = (kept[i + 1].second - kept[i].second) / h1 - (kept[i].second - kept[i - 1].second) / h0;
            const double denom = b - a * c[i - 1];
            c[i] = cc / denom;
            d[i] = (r - a * d[i - 1]) / denom;
        }
        for (std::size_t i = n - 2; i >= 1; --i) {
            m2[i] = d[i] - c[i] * m2[i + 1];
            if (i == 1) break;
        }
    }
    for (std::size_t k = 0; k < kLut; ++k) {
        const double xv = static_cast<double>(static_cast<float>(k) / (kLut - 1));
        double y;
        if (xv <= kept[0].first) {
            y = kept[0].second;
        } else if (xv >= kept[n - 1].first) {
            y = kept[n - 1].second;
        } else {
            std::size_t i = 0;
            while (i + 2 < n && xv > kept[i + 1].first) ++i;
            const double x0 = kept[i].first, y0 = kept[i].second, x1 = kept[i + 1].first, y1 = kept[i + 1].second;
            const double h = x1 - x0, a = (x1 - xv) / h, b = (xv - x0) / h;
            y = a * y0 + b * y1 + ((a * a * a - a) * m2[i] + (b * b * b - b) * m2[i + 1]) * h * h / 6.0;
        }
        out[k] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    return out;
}

float lut_at(const std::vector<float>& l, float v) {
    const float x = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(l.size() - 1);
    const std::size_t i = static_cast<std::size_t>(std::floor(x));
    const std::size_t j = std::min(i + 1, l.size() - 1);
    return l[i] + (l[j] - l[i]) * (x - static_cast<float>(i));
}

// A contour's table (none: linear).
std::optional<std::vector<float>> contour_lut(const Json* contour) {
    if (contour == nullptr || !contour->is_object()) return std::nullopt;
    const Json* points = core::get(*contour, "points");
    if (points == nullptr || !points->is_array()) return std::nullopt;
    return curve_lut(*points);
}

// A contour over a range: contour(min(v / range, 1)).
std::optional<std::vector<float>> ranged_lut(const Json* contour, double range) {
    std::optional<std::vector<float>> base = contour_lut(contour);
    range = std::isfinite(range) ? std::clamp(range, 0.01, 1.0) : 1.0;
    if (range >= 0.999) return base;
    std::vector<float> out(kLut);
    for (std::size_t k = 0; k < kLut; ++k) {
        const float v = std::min(static_cast<float>(k) / (kLut - 1) / static_cast<float>(range), 1.0f);
        out[k] = base ? lut_at(*base, v) : v;
    }
    return out;
}

void apply_lut(Map& m, const std::optional<std::vector<float>>& l) {
    if (!l) return;
    for (float& v : m.v) v = lut_at(*l, v);
}

// --- an effect's settings -------------------------------------------------------------------------------------------

double num(const Json& e, const char* key, double fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_number() ? v->get<double>() : fallback;
}
bool flag(const Json& e, const char* key, bool fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
}
std::string word(const Json& e, const char* key, const char* fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}
std::array<double, 3> srgb01(const Json& rgb) {
    std::array<double, 3> out{0, 0, 0};
    if (rgb.is_array() && rgb.size() == 3)
        for (std::size_t k = 0; k < 3; ++k) out[k] = std::clamp(rgb[k].get<double>(), 0.0, 255.0) / 255.0;
    return out;
}

struct Ctx {
    const Inputs& in;
    const Mixing& mx;
    double px_mm;  // (pixels a mm)
    double px(double mm) const { return std::min(mm * px_mm, kMaxReachPx); }
};

// The offset of an effect lit from `angle` at `distance` (pixels; the shadow falls away from the light), whole pixels.
std::pair<float, float> offset(double angle, double distance) {
    const double a = angle * std::numbers::pi / 180.0;
    return {static_cast<float>(std::round(-std::cos(a) * distance)), static_cast<float>(std::round(std::sin(a) * distance))};
}

std::pair<double, double> spread_split(double size, double spread) { return {std::round(size * spread), size * (1.0 - spread)}; }

double light_angle(const Ctx& c, const Json& e) { return flag(e, "global_light", true) ? c.in.light[0] : num(e, "angle", 120); }

Map shadow_map(const Ctx& c, const Map& shape, const Json& e, bool inner) {
    const auto [dx, dy] = offset(light_angle(c, e), c.px(num(e, "distance_mm", 0)));
    Map src = shape;
    if (inner)
        for (float& v : src.v) v = 1.0f - v;
    Map m = shifted(src, dx, dy, inner ? 1.0f : 0.0f);
    const auto [r, bw] = spread_split(c.px(num(e, "size_mm", 0)), num(e, inner ? "choke" : "spread", 0));
    m = dilate(m, r);
    blur(m, bw);
    apply_lut(m, contour_lut(core::get(e, "contour")));
    if (inner)
        for (std::size_t i = 0; i < m.v.size(); ++i) m.v[i] *= shape.v[i];
    return m;
}

Map glow_map(const Ctx& c, const Map& shape, const Json& e, bool inner) {
    const bool centre = inner && word(e, "source", "edge") == "center";
    Map src = shape;
    if (inner && !centre)
        for (float& v : src.v) v = 1.0f - v;
    const double size = c.px(num(e, "size_mm", 0)), spread = num(e, inner ? "choke" : "spread", 0);
    Map m;
    if (word(e, "technique", "softer") == "precise") {
        const std::vector<float> d = inner && !centre ? dist_inside(shape) : dist_outside(src);
        const double solid = size * spread, soft = std::max(size - solid, 1e-3);
        m = Map(shape.w, shape.h, 0.f);
        for (std::size_t i = 0; i < m.v.size(); ++i)
            m.v[i] = d[i] <= solid ? 1.0f : static_cast<float>(std::clamp(1.0 - (d[i] - solid) / soft, 0.0, 1.0));
        if (centre)
            for (float& v : m.v) v = 1.0f - v;
    } else {
        const auto [r, bw] = spread_split(size, spread);
        m = dilate(src, r);
        blur(m, bw);
        if (centre) {  // (brightest in the middle: its coverage from the distance to the edge)
            Map edge = shape;
            for (float& v : edge.v) v = 1.0f - v;
            Map e2 = dilate(edge, r);
            blur(e2, bw);
            for (std::size_t i = 0; i < m.v.size(); ++i) m.v[i] = 1.0f - e2.v[i];
        }
    }
    apply_lut(m, ranged_lut(core::get(e, "contour"), num(e, "range", 0.5)));
    if (inner)
        for (std::size_t i = 0; i < m.v.size(); ++i) m.v[i] *= shape.v[i];
    return m;
}

Map satin_map(const Ctx& c, const Map& shape, const Json& e) {
    const auto [dx, dy] = offset(num(e, "angle", 19), c.px(num(e, "distance_mm", 0)));
    Map a = shifted(shape, dx, dy, 0.f), b = shifted(shape, -dx, -dy, 0.f);
    const double size = c.px(num(e, "size_mm", 0));
    tent(a, size);
    tent(b, size);
    Map m(shape.w, shape.h, 0.f);
    for (std::size_t i = 0; i < m.v.size(); ++i) m.v[i] = std::fabs(a.v[i] - b.v[i]);
    if (flag(e, "invert", true))
        for (float& v : m.v) v = 1.0f - v;
    apply_lut(m, contour_lut(core::get(e, "contour")));
    for (std::size_t i = 0; i < m.v.size(); ++i) m.v[i] *= shape.v[i];
    return m;
}

// --- bevel and emboss ----------------------------------------------------------------------------------------------

enum class BevelPaint { Inner, Outer, Both };

struct BevelGeom {
    BevelPaint paint;
    double width, depth, chisel_soft;
    bool pillow;
};

BevelGeom bevel_geom(const Ctx& c, const Json& e) {
    const double size = std::max(c.px(num(e, "size_mm", 0)), 1.0);
    const std::string style = word(e, "style", "inner");
    const bool emboss = style == "emboss" || style == "pillow";
    const BevelPaint paint = style == "outer" ? BevelPaint::Outer : emboss ? BevelPaint::Both : BevelPaint::Inner;
    const double width = emboss ? size / 2.0 : size;
    const std::string technique = word(e, "technique", "smooth");
    const double sign = word(e, "direction", "up") == "up" ? 1.0 : -1.0;
    return {paint, width, num(e, "depth", 1) * (technique == "smooth" ? width : size) * sign,
            technique == "chisel_soft" ? std::max(size / 4.0, 1.0) : 0.0, style == "pillow"};
}

float chisel_h(BevelPaint paint, double size, float din, float dout) {
    switch (paint) {
        case BevelPaint::Outer:
            return 1.0f - static_cast<float>(std::clamp(dout / size, 0.0, 1.0));
        case BevelPaint::Both:
            if (din > 0) return 0.5f + 0.5f * static_cast<float>(std::clamp(din / (size / 2.0), 0.0, 1.0));
            return 0.5f - 0.5f * static_cast<float>(std::clamp(dout / (size / 2.0), 0.0, 1.0));
        case BevelPaint::Inner:
        default:
            return static_cast<float>(std::clamp(din / size, 0.0, 1.0));
    }
}

// The height map (0..1): smooth, the shape blurred by the tent of the bevel's width; chisel, ramps of the exact distance
// to the edge; the contour element, the texture's lightness as more height, softened.
Map bevel_height(const Ctx& c, const Map& shape, const Json& e, const BevelGeom& g) {
    const double size = std::max(c.px(num(e, "size_mm", 0)), 1.0);
    Map h;
    if (word(e, "technique", "smooth") == "smooth") {
        h = shape;
        tent(h, g.width);
    } else {
        const std::vector<float> din = dist_inside(shape), dout = dist_outside(shape);
        h = Map(shape.w, shape.h, 0.f);
        for (std::size_t i = 0; i < h.v.size(); ++i) h.v[i] = chisel_h(g.paint, size, din[i], dout[i]);
        if (g.chisel_soft > 0) tent(h, g.chisel_soft);
    }
    if (const Json* contour = core::get(e, "contour"); contour != nullptr && contour->is_object()) {
        std::optional<std::vector<float>> l = ranged_lut(core::get(*contour, "curve"), num(*contour, "range", 0.5));
        if (!l) {
            l.emplace(kLut);
            for (std::size_t k = 0; k < kLut; ++k) (*l)[k] = static_cast<float>(k) / (kLut - 1);
        }
        apply_lut(h, l);
    }
    if (const Json* texture = core::get(e, "texture"); texture != nullptr && texture->is_object()) {
        const Json* spec = core::get(*texture, "pattern");
        std::optional<patterns::Laid> laid;
        if (spec != nullptr && c.in.book != nullptr) {
            try {
                laid = patterns::laid(c.in.book, *spec, patterns::Target::srgb(), c.in.dpi);
            } catch (const std::exception&) {
                laid.reset();  // (a picture that cannot be made: no texture)
            }
        }
        if (laid) {
            const double depth = num(e, "depth", 1);
            const double unit = std::fabs(depth) > 1e-6 ? std::max(std::fabs(g.depth / depth), 1e-3) : std::max(g.width, 1.0);
            const double k = num(*texture, "depth", 1) / unit;
            const bool invert = flag(*texture, "invert", false);
            float px[5];
            for (int y = 0; y < h.h; ++y) {
                for (int x = 0; x < h.w; ++x) {
                    laid->at(c.in.wide.x0 + x, c.in.wide.y0 + y, px);
                    const double l = (0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]) * px[3];
                    h.v[static_cast<std::size_t>(y) * h.w + x] += static_cast<float>(k * (invert ? 1.0 - l : l));
                }
            }
        }
    }
    const double soften = c.px(num(e, "soften_mm", 0));
    if (soften >= 1.0) tent(h, soften);
    return h;
}

// The highlight and shadow maps: [hi, sh] (inner or outer) or [hi, sh, hi out, sh out] (the emboss styles).
std::vector<Map> bevel_maps(const Ctx& c, const Map& shape, const Json& e, BevelPaint& paint) {
    const BevelGeom g = bevel_geom(c, e);
    paint = g.paint;
    const Map hmap = bevel_height(c, shape, e, g);
    const bool global = flag(e, "global_light", true);
    const double angle = (global ? c.in.light[0] : num(e, "angle", 120)) * std::numbers::pi / 180.0;
    const double altitude = (global ? c.in.light[1] : num(e, "altitude", 30)) * std::numbers::pi / 180.0;
    const double sa = std::sin(angle), ca = std::cos(angle), se = std::sin(altitude), ce = std::cos(altitude);
    const std::array<double, 3> light{ca * ce, -sa * ce, se};
    const std::optional<std::vector<float>> gloss = contour_lut(core::get(e, "gloss_contour"));
    const auto shade_into = [&](double depth, bool outer) {
        Map hi(shape.w, shape.h, 0.f), sh(shape.w, shape.h, 0.f);
        for (long y = 0; y < shape.h; ++y) {
            for (long x = 0; x < shape.w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * shape.w + x;
                const double gx = (hmap.get(x + 1, y) - hmap.get(x - 1, y)) * 0.5 * depth;
                const double gy = (hmap.get(x, y + 1) - hmap.get(x, y - 1)) * 0.5 * depth;
                const double len = std::sqrt(gx * gx + gy * gy + 1.0);
                const double shade = (-gx * light[0] + -gy * light[1] + light[2]) / len;
                const double region = outer ? (shape.v[i] < 1.0f - kInsideEps && hmap.v[i] > kBevelHEps ? 1.0 : 0.0) : shape.v[i];
                const double k = shade - se;
                if (k > 0) hi.v[i] = static_cast<float>(std::clamp(k / std::max(1.0 - se, 1e-3), 0.0, 1.0) * region);
                else sh.v[i] = static_cast<float>(std::clamp(-k / std::max(se, 1e-3), 0.0, 1.0) * region);
            }
        }
        apply_lut(hi, gloss);
        apply_lut(sh, gloss);
        return std::pair{std::move(hi), std::move(sh)};
    };
    std::vector<Map> out;
    if (g.paint == BevelPaint::Both) {
        auto [hi, sh] = shade_into(g.depth, false);
        auto [ho, so] = shade_into(g.pillow ? -g.depth : g.depth, true);
        out.push_back(std::move(hi));
        out.push_back(std::move(sh));
        out.push_back(std::move(ho));
        out.push_back(std::move(so));
    } else {
        auto [hi, sh] = shade_into(g.depth, g.paint == BevelPaint::Outer);
        out.push_back(std::move(hi));
        out.push_back(std::move(sh));
    }
    return out;
}

// --- painting ------------------------------------------------------------------------------------------------------

// The gradient parameter t (0..1) of the pixel centre (x, y) in `bounds`.
double gradient_t(const std::string& style, double angle, double scale, bool reverse, std::array<double, 2> off, const Box& bounds, double x, double y) {
    const double w = std::max(bounds.width(), 1), h = std::max(bounds.height(), 1);
    const double cx = bounds.x0 + w / 2.0 + off[0] * w, cy = bounds.y0 + h / 2.0 + off[1] * h;
    const double a = angle * std::numbers::pi / 180.0, s = std::sin(a), c = std::cos(a);
    const double dx = x - cx, dy = y - cy;
    const double along = dx * c - dy * s, across = dx * s + dy * c;
    const double len = std::max(std::sqrt((c * w) * (c * w) + (s * h) * (s * h)), 1.0) * std::max(scale, 1e-3);
    const double chord = std::max(std::min(w / std::max(std::fabs(c), 1e-6), h / std::max(std::fabs(s), 1e-6)), 1.0) * std::max(scale, 1e-3);
    double t;
    if (style == "reflected") t = std::fabs(along / (chord / 2.0));
    else if (style == "radial") t = std::sqrt(dx * dx + dy * dy) / (len / 2.0);
    else if (style == "diamond") t = (std::fabs(along) + std::fabs(across)) / (len / 2.0);
    else if (style == "angle") {
        t = std::fmod((a - std::atan2(-dy, dx)) / (2 * std::numbers::pi), 1.0);
        if (t < 0) t += 1.0;
    } else t = along / chord + 0.5;
    t = std::clamp(t, 0.0, 1.0);
    return reverse ? 1.0 - t : t;
}

// A gradient made a table of the way's values (n colours and the alpha) over t in 0..1.
struct GradientTable {
    unsigned st = 4;
    std::vector<float> values;  // (1024 entries)
    const float* at(double t) const {
        const std::size_t i = static_cast<std::size_t>(std::lround(std::clamp(t, 0.0, 1.0) * 1023.0));
        return values.data() + i * st;
    }
};

GradientTable gradient_table(const Json& g, const Mixing& mx) {
    GradientTable out;
    out.st = mx.n + 1;
    out.values.resize(1024 * out.st);
    std::vector<std::pair<double, std::array<double, 3>>> stops;
    if (const Json* s = core::get(g, "stops"); s != nullptr && s->is_array())
        for (const Json& stop : *s) stops.emplace_back(stop[0].get<double>(), srgb01(stop[1]));
    std::vector<std::pair<double, double>> alpha;
    if (const Json* t = core::get(g, "transparency"); t != nullptr && t->is_array())
        for (const Json& stop : *t) alpha.emplace_back(stop[0].get<double>(), stop[1].get<double>());
    const auto sample = [](const auto& list, double t, auto zero) {
        using V = decltype(zero);
        if (list.empty()) return zero;
        if (list.size() == 1 || t <= list.front().first) return V(list.front().second);
        for (std::size_t i = 0; i + 1 < list.size(); ++i) {
            if (t <= list[i + 1].first) {
                const double span = list[i + 1].first - list[i].first;
                const double k = span > 0 ? (t - list[i].first) / span : 0.0;
                V mixed = list[i].second;
                if constexpr (std::is_same_v<V, double>) {
                    mixed = list[i].second + (list[i + 1].second - list[i].second) * k;
                } else {
                    for (std::size_t c = 0; c < mixed.size(); ++c) mixed[c] = list[i].second[c] + (list[i + 1].second[c] - list[i].second[c]) * k;
                }
                return mixed;
            }
        }
        return V(list.back().second);
    };
    for (std::size_t i = 0; i < 1024; ++i) {
        const double t = static_cast<double>(i) / 1023.0;
        const std::array<double, 3> rgb = sample(stops, t, std::array<double, 3>{t, t, t});
        float* v = out.values.data() + i * out.st;
        mx.colour(rgb, v);
        v[mx.n] = static_cast<float>(alpha.empty() ? 1.0 : sample(alpha, t, 1.0));
    }
    return out;
}

// `above` (n colours, its alpha) laid over `below` in place in `mode` (straight alpha).
inline void over(const Mixing& mx, float* below, const float* above, float above_alpha, std::string_view mode, bool normal) {
    const unsigned n = mx.n;
    const float ab = below[n], as = above_alpha;
    if (as <= 0) return;
    const float ao = as + ab * (1 - as);
    if (ao <= 0) {
        for (unsigned c = 0; c <= n; ++c) below[c] = 0;
        return;
    }
    if (normal || ab <= 0) {
        const float kb = ab * (1 - as), inv = 1.0f / ao;
        for (unsigned c = 0; c < n; ++c) below[c] = (kb * below[c] + as * above[c]) * inv;
    } else {
        float mixed[core::kPlatesMax + 1];
        mx.blend(mode, below, above, mixed);
        for (unsigned c = 0; c < n; ++c) below[c] = ((1 - as) * ab * below[c] + (1 - ab) * as * above[c] + as * ab * mixed[c]) / ao;
    }
    if (mx.clamp)
        for (unsigned c = 0; c < n; ++c) below[c] = std::clamp(below[c], 0.0f, 1.0f);
    below[n] = ao;
}

// A paint ({"rgb"} | {"gradient"} | {"pattern"}) of an effect: the way's colour of each area pixel.
class Paint {
public:
    Paint(const Ctx& c, const Json& e) : c_(c), st_(c.mx.n + 1) {
        if (const Json* g = core::get(e, "gradient"); g != nullptr && g->is_object()) {
            table_ = gradient_table(*g, c.mx);
            style_ = word(*g, "style", "linear");
            angle_ = num(*g, "angle", 90);
            scale_ = num(*g, "scale", 1);
            reverse_ = flag(*g, "reverse", false);
            if (const Json* o = core::get(*g, "offset"); o != nullptr && o->is_array() && o->size() == 2) off_ = {(*o)[0].get<double>(), (*o)[1].get<double>()};
            bounds_ = flag(*g, "align", true) && c.in.layer_box ? *c.in.layer_box : Box{0, 0, c.in.page.width, c.in.page.height};
            kind_ = Kind::Gradient;
        } else if (const Json* p = core::get(e, "pattern"); p != nullptr && p->is_object()) {
            kind_ = Kind::Pattern;
            if (c.in.book != nullptr) {
                try {
                    laid_ = patterns::laid(c.in.book, *p, c.mx.pattern_target, c.in.dpi);
                } catch (const std::exception&) {
                    laid_.reset();  // (a picture that cannot be made: the effect draws nothing)
                }
            }
        } else {
            colour_.resize(st_);
            c.mx.colour(srgb01(core::get_or(e, "rgb", Json::array({0, 0, 0}))), colour_.data());
            colour_[c.mx.n] = 1.0f;
        }
    }
    bool none() const { return kind_ == Kind::Pattern && !laid_; }
    // The colour at area pixel (x, y) (page pixels x0 + x, y0 + y).
    const float* at(int x, int y) {
        if (kind_ == Kind::Colour) return colour_.data();
        const double px = c_.in.area.x0 + x + 0.5, py = c_.in.area.y0 + y + 0.5;
        if (kind_ == Kind::Gradient) return table_.at(gradient_t(style_, angle_, scale_, reverse_, off_, bounds_, px, py));
        scratch_.resize(laid_->colours() + 1);
        laid_->at(c_.in.area.x0 + x, c_.in.area.y0 + y, scratch_.data());
        if (laid_->colours() != c_.mx.n) {  // (a target of other channels: not expected; drawn as grey)
            std::vector<float> fixed(st_, scratch_[0]);
            fixed[c_.mx.n] = scratch_.back();
            scratch_ = std::move(fixed);
        }
        if (c_.mx.pattern_in) c_.mx.pattern_in(scratch_.data());
        return scratch_.data();
    }

private:
    enum class Kind { Colour, Gradient, Pattern };
    const Ctx& c_;
    unsigned st_;
    Kind kind_ = Kind::Colour;
    std::vector<float> colour_, scratch_;
    GradientTable table_;
    std::string style_;
    double angle_ = 90, scale_ = 1;
    bool reverse_ = false;
    std::array<double, 2> off_{0, 0};
    Box bounds_{};
    std::optional<patterns::Laid> laid_;
};

// `m` (area) × opacity of the paint laid over `dst` (area, n + 1 a pixel) in `mode`.
void paint(const Ctx& c, std::vector<float>& dst, const std::vector<float>& m, Paint& p, std::string_view mode, double opacity) {
    if (p.none()) return;
    const unsigned n = c.mx.n, st = n + 1;
    const int w = c.in.area.width();
    const bool normal = mode == "normal";
    for (std::size_t i = 0; i < m.size(); ++i) {
        const float a = m[i] * static_cast<float>(opacity);
        if (a <= 0) continue;
        const float* colour = p.at(static_cast<int>(i % w), static_cast<int>(i / w));
        over(c.mx, dst.data() + i * st, colour, colour[n] * a, mode, normal);
    }
}

// A map over the wide box cut to the area.
std::vector<float> crop(const Ctx& c, const Map& m) {
    const int w = c.in.area.width(), h = c.in.area.height();
    const int ox = c.in.area.x0 - c.in.wide.x0, oy = c.in.area.y0 - c.in.wide.y0;
    std::vector<float> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) std::copy_n(m.v.begin() + static_cast<std::ptrdiff_t>(y + oy) * m.w + ox, w, out.begin() + static_cast<std::ptrdiff_t>(y) * w);
    return out;
}
std::vector<float> crop(const Ctx& c, const std::vector<float>& v, int wide_w) {
    Map m;
    m.w = wide_w;
    m.h = static_cast<int>(v.size() / std::max(wide_w, 1));
    m.v = v;
    return crop(c, m);
}

// Premultiplied mixing of two straight pixels by k.
void mix_premul(const float* a, const float* b, float k, float* out, unsigned n) {
    const float alpha = a[n] + (b[n] - a[n]) * k;
    if (alpha <= 0) {
        for (unsigned c = 0; c <= n; ++c) out[c] = 0;
        return;
    }
    for (unsigned c = 0; c < n; ++c) out[c] = (a[c] * a[n] + (b[c] * b[n] - a[c] * a[n]) * k) / alpha;
    out[n] = alpha;
}

// Blend If's weight of one range.
float range_weight(const Json& r, float v) {
    const auto ramp = [](double lo, double hi, double x) { return hi <= lo ? (x >= lo ? 1.0 : 0.0) : std::clamp((x - lo) / (hi - lo), 0.0, 1.0); };
    const double b0 = r[0].get<double>(), b1 = r[1].get<double>(), w0 = r[2].get<double>(), w1 = r[3].get<double>();
    return static_cast<float>(ramp(b0, b1, v) * ramp(255.0 - w1, 255.0 - w0, 255.0 - v));
}

}  // namespace

Mixing srgb_mixing() {
    Mixing mx;
    mx.n = 3;
    mx.blend = [](std::string_view mode, const float* b, const float* s, float* out) {
        const std::array<double, 3> r = blend_linear(mode, {b[0], b[1], b[2]}, {s[0], s[1], s[2]});
        for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<float>(std::clamp(r[c], 0.0, 1.0));
    };
    mx.colour = [](const std::array<double, 3>& rgb, float* out) {
        for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<float>(rgb[c]);
    };
    mx.pattern_target = patterns::Target::srgb();
    mx.blend_if_values = [](const float* p, float* out) {
        const auto v = [&](unsigned c) { return std::clamp(p[c], 0.0f, 1.0f) * 255.0f; };
        out[0] = 0.299f * v(0) + 0.587f * v(1) + 0.114f * v(2);
        for (unsigned c = 0; c < 3; ++c) out[c + 1] = v(c);
    };
    mx.blend_if_names = {"red", "green", "blue"};
    return mx;
}

int margin_px(const Json& style, double dpi) {
    const double reach = core::style_reach_mm(style);
    if (reach <= 0) return 0;
    return static_cast<int>(std::ceil(std::min(reach * dpi / 25.4, kMaxReachPx))) + 2;
}

bool drawn_below(const Json& e) {
    const std::string kind = word(e, "kind", "");
    if (kind == "drop_shadow" || kind == "outer_glow") return true;
    if (kind == "stroke") return word(e, "position", "outside") == "outside";
    if (kind == "bevel") {
        const std::string style = word(e, "style", "inner");
        return style == "outer" || style == "emboss" || style == "pillow";
    }
    return false;
}

std::size_t maps_made() { return g_maps.load(); }

void composite(const Inputs& in, const Mixing& mx, const std::vector<float>& content, std::vector<float>& backdrop, const std::vector<float>* weight) {
    const unsigned n = mx.n, st = n + 1;
    const int W = in.wide.width(), H = in.wide.height(), w = in.area.width(), h = in.area.height();
    if (w <= 0 || h <= 0 || W <= 0 || H <= 0 || in.style == nullptr) return;
    const std::size_t count = static_cast<std::size_t>(w) * h;
    if (content.size() != static_cast<std::size_t>(W) * H * st || backdrop.size() != count * st) throw core::Error("value", "layer style: the pictures do not match");
    const Json& style = *in.style;
    const Ctx c{in, mx, in.dpi / 25.4};
    const int ox = in.area.x0 - in.wide.x0, oy = in.area.y0 - in.wide.y0;
    const auto wide_at = [&](std::size_t i) {  // (the wide index of area pixel i)
        return static_cast<std::size_t>(static_cast<int>(i / w) + oy) * W + static_cast<int>(i % w) + ox;
    };
    Map shape(W, H, 0.f);
    for (std::size_t i = 0; i < shape.v.size(); ++i) shape.v[i] = std::clamp(content[i * st + n], 0.0f, 1.0f);
    std::vector<float> a(count);
    for (std::size_t i = 0; i < count; ++i) a[i] = shape.v[wide_at(i)];
    const auto inside = [](float alpha) { return alpha > kInsideEps; };
    const float fill = static_cast<float>(core::style_fill_opacity(style));
    std::vector<const Json*> items;
    if (core::style_shows_effects(style)) {
        for (const Json& e : style["effects"])
            if (e.is_object() && e.value("enabled", true)) items.push_back(&e);
    }
    const auto of = [&](const char* kind) {  // (the effects of a kind, the last listed first: the first listed on top)
        std::vector<const Json*> out;
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if ((*it)->value("kind", std::string()) == kind) out.push_back(*it);
        return out;
    };
    const std::vector<float> before = backdrop;
    std::vector<float>& work = backdrop;
    const auto count_map = [] { g_maps.fetch_add(1); };

    // drop shadows (knocked out only where the fill lets the backdrop through)
    for (const Json* e : of("drop_shadow")) {
        std::vector<float> m = crop(c, shadow_map(c, shape, *e, false));
        count_map();
        if (flag(*e, "knocks_out", true)) {
            const float see_through = 1.0f - std::clamp(fill, 0.0f, 1.0f);
            for (std::size_t i = 0; i < count; ++i) m[i] *= 1.0f - a[i] * see_through;
        }
        Paint p(c, *e);
        paint(c, work, m, p, word(*e, "blend", "multiply"), num(*e, "opacity", 0.75));
    }
    for (const Json* e : of("outer_glow")) {
        const std::vector<float> m = crop(c, glow_map(c, shape, *e, false));
        count_map();
        Paint p(c, *e);
        paint(c, work, m, p, word(*e, "blend", "screen"), num(*e, "opacity", 0.75));
    }

    // the layer: its pixels at the fill opacity, the interior effects relative to its shape, then its shape's alpha
    std::vector<float> lay(count * st);
    for (std::size_t i = 0; i < count; ++i) {
        const float* src = content.data() + wide_at(i) * st;
        std::copy_n(src, n, lay.data() + i * st);
        lay[i * st + n] = inside(a[i]) ? fill : 0.0f;
    }
    const auto rel = [&](std::vector<float> m) {
        for (std::size_t i = 0; i < count; ++i) m[i] = inside(a[i]) ? std::min(m[i] / a[i], 1.0f) : 0.0f;
        return m;
    };
    std::vector<float> full(count);
    for (std::size_t i = 0; i < count; ++i) full[i] = inside(a[i]) ? 1.0f : 0.0f;
    for (const Json* e : of("pattern_overlay")) {
        Paint p(c, *e);
        paint(c, lay, full, p, word(*e, "blend", "normal"), num(*e, "opacity", 1));
    }
    for (const Json* e : of("gradient_overlay")) {
        Paint p(c, *e);
        paint(c, lay, full, p, word(*e, "blend", "normal"), num(*e, "opacity", 1));
    }
    for (const Json* e : of("color_overlay")) {
        Paint p(c, *e);
        paint(c, lay, full, p, word(*e, "blend", "normal"), num(*e, "opacity", 1));
    }
    for (const Json* e : of("satin")) {
        const std::vector<float> m = rel(crop(c, satin_map(c, shape, *e)));
        count_map();
        Paint p(c, *e);
        paint(c, lay, m, p, word(*e, "blend", "multiply"), num(*e, "opacity", 0.5));
    }
    for (const Json* e : of("inner_glow")) {
        const std::vector<float> m = rel(crop(c, glow_map(c, shape, *e, true)));
        count_map();
        Paint p(c, *e);
        paint(c, lay, m, p, word(*e, "blend", "screen"), num(*e, "opacity", 0.75));
    }
    for (const Json* e : of("inner_shadow")) {
        const std::vector<float> m = rel(crop(c, shadow_map(c, shape, *e, true)));
        count_map();
        Paint p(c, *e);
        paint(c, lay, m, p, word(*e, "blend", "multiply"), num(*e, "opacity", 0.75));
    }
    // strokes: their inner parts over the layer (the last listed first), their outer parts beneath it
    const std::vector<const Json*> strokes = of("stroke");  // (bottom first)
    std::vector<float> din, dout;
    if (!strokes.empty()) {
        din = crop(c, dist_inside(shape, Metric::Chamfer), W);
        dout = crop(c, dist_outside(shape, Metric::Chamfer), W);
        count_map();
    }
    const auto widths = [&](const Json& st_) {
        const double size = c.px(num(st_, "size_mm", 0));
        const std::string position = word(st_, "position", "outside");
        if (position == "inside") return std::pair{size, 0.0};
        if (position == "center") return std::pair{size / 2.0, size / 2.0};
        return std::pair{0.0, size};
    };
    for (const Json* e : strokes) {
        const double in_w = widths(*e).first;
        if (in_w <= 0) continue;
        std::vector<float> m(count);
        for (std::size_t i = 0; i < count; ++i) m[i] = inside(a[i]) ? std::clamp(static_cast<float>(in_w) + 0.5f - din[i], 0.0f, 1.0f) : 0.0f;
        Paint p(c, *e);
        paint(c, lay, m, p, word(*e, "blend", "normal"), num(*e, "opacity", 1));
    }
    std::vector<std::pair<const Json*, std::vector<Map>>> bevels;  // (made once: the inner and outer halves)
    std::vector<BevelPaint> bevel_paints;
    for (const Json* e : of("bevel")) {
        BevelPaint bp = BevelPaint::Inner;
        std::vector<Map> maps = bevel_maps(c, shape, *e, bp);
        count_map();
        bevels.emplace_back(e, std::move(maps));
        bevel_paints.push_back(bp);
    }
    const auto bevel_tone = [&](const Json& e, const char* which, const std::vector<float>& m, std::vector<float>& dst) {
        const Json tone = core::get_or(e, which, Json::object());
        Json as_paint = Json::object({{"rgb", core::get_or(tone, "rgb", Json::array({0, 0, 0}))}});
        Paint p(c, as_paint);
        paint(c, dst, m, p, word(tone, "blend", which[0] == 'h' ? "screen" : "multiply"), num(tone, "opacity", 0.75));
    };
    for (std::size_t k = 0; k < bevels.size(); ++k) {
        if (bevel_paints[k] == BevelPaint::Outer) continue;
        bevel_tone(*bevels[k].first, "highlight", rel(crop(c, bevels[k].second[0])), lay);
        bevel_tone(*bevels[k].first, "shadow", rel(crop(c, bevels[k].second[1])), lay);
    }
    for (std::size_t i = 0; i < count; ++i) lay[i * st + n] *= std::min(a[i], 1.0f);

    // the strokes' outer parts onto what is beneath, each in its own mode, a higher one covering the lower
    if (std::any_of(strokes.begin(), strokes.end(), [&](const Json* e) { return widths(*e).second > 0; })) {
        const std::vector<float> base = work;
        std::vector<float> acc(count * st, 0.0f), cover(count, 0.0f);
        for (auto it = strokes.rbegin(); it != strokes.rend(); ++it) {  // (the top one first)
            const Json& e = **it;
            const double out_w = widths(e).second;
            if (out_w <= 0) continue;
            const float opacity = static_cast<float>(num(e, "opacity", 1));
            std::vector<float> share(count, 0.0f), ones(count, 0.0f);
            for (std::size_t i = 0; i < count; ++i) {
                const float k = inside(a[i]) ? 1.0f : std::clamp(static_cast<float>(out_w) + 0.5f - dout[i], 0.0f, 1.0f);
                share[i] = k * opacity * (1.0f - cover[i]);
                cover[i] += share[i];
                ones[i] = share[i] > 0 ? 1.0f : 0.0f;
            }
            std::vector<float> blended = base;
            Paint p(c, e);
            paint(c, blended, ones, p, word(e, "blend", "normal"), 1.0);
            for (std::size_t i = 0; i < count; ++i) {
                if (share[i] <= 0) continue;
                const float* b = blended.data() + i * st;
                for (unsigned ch = 0; ch < n; ++ch) acc[i * st + ch] += share[i] * b[ch] * b[n];
                acc[i * st + n] += share[i] * b[n];
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (cover[i] <= 0) continue;
            float* p = work.data() + i * st;
            const float k = 1.0f - std::min(cover[i], 1.0f);
            const float alpha = p[n] * k + acc[i * st + n];
            if (alpha > 0) {
                for (unsigned ch = 0; ch < n; ++ch) p[ch] = (p[ch] * p[n] * k + acc[i * st + ch]) / alpha;
                p[n] = std::min(alpha, 1.0f);
            }
        }
    }
    for (std::size_t k = 0; k < bevels.size(); ++k) {
        if (bevel_paints[k] == BevelPaint::Inner) continue;
        const std::size_t at = bevel_paints[k] == BevelPaint::Both ? 2 : 0;
        bevel_tone(*bevels[k].first, "highlight", crop(c, bevels[k].second[at]), work);
        bevel_tone(*bevels[k].first, "shadow", crop(c, bevels[k].second[at + 1]), work);
    }

    // the layer with its interior effects onto that, in its mode; the layer's opacity (and the clip) over all of it
    const bool normal = in.blend.empty() || in.blend == "normal";
    for (std::size_t i = 0; i < count; ++i) {
        const float* lp = lay.data() + i * st;
        if (lp[n] > 0) over(mx, work.data() + i * st, lp, lp[n], in.blend, normal);
    }
    std::vector<float> mixed(st);
    for (std::size_t i = 0; i < count; ++i) {
        const float k = static_cast<float>(std::clamp(in.opacity, 0.0, 1.0)) * (weight != nullptr ? std::clamp((*weight)[i], 0.0f, 1.0f) : 1.0f);
        mix_premul(before.data() + i * st, work.data() + i * st, k, mixed.data(), n);
        std::copy(mixed.begin(), mixed.end(), work.begin() + static_cast<std::ptrdiff_t>(i * st));
    }

    // Blend If: the result mixed back towards what was beneath where the layer's own colour or the colour beneath falls
    // outside the ranges
    const Json* blend_if = core::get(style, "blend_if");
    if (blend_if != nullptr && blend_if->is_array() && !blend_if->empty() && mx.blend_if_values) {
        std::vector<std::pair<std::size_t, const Json*>> ranges;  // (value index, entry)
        for (const Json& entry : *blend_if) {
            const std::string channel = entry.value("channel", std::string());
            if (channel == "gray") {
                ranges.emplace_back(0, &entry);
            } else {
                const auto it = std::find(mx.blend_if_names.begin(), mx.blend_if_names.end(), channel);
                if (it != mx.blend_if_names.end()) ranges.emplace_back(static_cast<std::size_t>(it - mx.blend_if_names.begin()) + 1, &entry);
            }
        }
        if (!ranges.empty()) {
            std::vector<float> self(mx.blend_if_names.size() + 1), under(self.size());
            for (std::size_t i = 0; i < count; ++i) {
                const float* own = content.data() + wide_at(i) * st;
                const float* b = before.data() + i * st;
                float k = 1.0f;
                if (own[n] > 0) {
                    mx.blend_if_values(own, self.data());
                    for (const auto& [index, entry] : ranges) k *= range_weight((*entry)["this"], self[index]);
                }
                if (b[n] > 0 && k > 0) {
                    mx.blend_if_values(b, under.data());
                    for (const auto& [index, entry] : ranges) k *= range_weight((*entry)["under"], under[index]);
                }
                if (k >= 1.0f) continue;
                mix_premul(b, work.data() + i * st, k, mixed.data(), n);
                std::copy(mixed.begin(), mixed.end(), work.begin() + static_cast<std::ptrdiff_t>(i * st));
            }
        }
    }
}

}  // namespace genko::render::layer_styles
