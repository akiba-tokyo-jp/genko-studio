#include "render/resample.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "core/error.hpp"

// PhotoCraft v0.2.0 crates/algo/src/transform.rs (1-270), warp.rs (1-290), resample.rs (1-180) and
// crates/engine/src/canvas_geom.rs (200-235: Turn), MIT: native/third_party/photocraft/NOTICE.md. Unlike the upstream, a
// 32-bit plane's colours are not cut at 1 (HDR kept), and a grey plane's outside is its edge carried on (as the page's
// masks are moved, render/bookpages' relayout_mask).

namespace genko::render::resample {

namespace {

std::array<double, 4> catmull_rom(double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2};
}

// The source read premultiplied (alpha planes) as doubles, with how a pixel outside it reads.
class Source {
public:
    explicit Source(const Plane& p) : p_(p) {
        const std::size_t n = std::size_t(p.channels);
        pre_.resize(p.v.size());
        for (std::size_t i = 0; i < p.v.size(); i += n) {
            const double a = p.alpha ? static_cast<double>(p.v[i + n - 1]) : 1.0;
            for (std::size_t c = 0; c < n; ++c) {
                const double v = p.v[i + c];
                pre_[i + c] = p.alpha && c + 1 < n ? v * a : v;
            }
        }
    }
    double at(int x, int y, int c) const {
        if (x < 0 || y < 0 || x >= p_.width || y >= p_.height) {
            if (p_.alpha) return 0.0;
            x = std::clamp(x, 0, p_.width - 1);  // (grey: its edge carried on)
            y = std::clamp(y, 0, p_.height - 1);
        }
        return pre_[(std::size_t(y) * std::size_t(p_.width) + std::size_t(x)) * std::size_t(p_.channels) + std::size_t(c)];
    }
    const Plane& plane() const { return p_; }

private:
    const Plane& p_;
    std::vector<double> pre_;
};

// One destination pixel from the source at (u, v) (source pixels, centres at whole numbers): its values written into
// `out`; false when nothing of the source reaches it (left clear).
bool sample(const Source& s, double u, double v, Interp interp, float* out, bool snap) {
    const Plane& p = s.plane();
    const int n = p.channels;
    double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<double> wide;
    double* a = acc;
    if (n > 8) {
        wide.assign(std::size_t(n), 0.0);
        a = wide.data();
    }
    switch (interp) {
        case Interp::Nearest: {
            const int ix = static_cast<int>(std::floor(u + 0.5)), iy = static_cast<int>(std::floor(v + 0.5));
            for (int c = 0; c < n; ++c) a[c] = s.at(ix, iy, c);
            break;
        }
        case Interp::Bilinear: {
            const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
            const double fx = u - ix, fy = v - iy;
            for (const auto& [dy, wy] : {std::pair{0, 1.0 - fy}, std::pair{1, fy}}) {
                for (const auto& [dx, wx] : {std::pair{0, 1.0 - fx}, std::pair{1, fx}}) {
                    for (int c = 0; c < n; ++c) a[c] += s.at(ix + dx, iy + dy, c) * wx * wy;
                }
            }
            break;
        }
        case Interp::Bicubic: {
            if (snap) {  // (within float noise of the grid: on it, so the identity is exact)
                const auto near = [](double t) { return std::fabs(t - std::round(t)) < 1e-7 ? std::round(t) : t; };
                u = near(u);
                v = near(v);
            }
            const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
            const auto wx = catmull_rom(u - ix), wy = catmull_rom(v - iy);
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    const double k = wx[std::size_t(i)] * wy[std::size_t(j)];
                    if (k == 0.0) continue;
                    for (int c = 0; c < n; ++c) a[c] += s.at(ix - 1 + i, iy - 1 + j, c) * k;
                }
            }
            break;
        }
    }
    if (!p.alpha) {
        for (int c = 0; c < n; ++c) out[c] = static_cast<float>(std::clamp(a[c], 0.0, 1.0));
        return true;
    }
    const double al = std::clamp(a[n - 1], 0.0, 1.0);
    if (al <= 1e-6) return false;  // (float noise leaves no invisible colour behind)
    for (int c = 0; c + 1 < n; ++c) {
        const double value = a[c] / al;
        out[c] = static_cast<float>(p.hdr ? std::max(value, 0.0) : std::clamp(value, 0.0, 1.0));
    }
    out[n - 1] = static_cast<float>(al);
    return true;
}

Plane empty_like(const Plane& src, int width, int height) {
    Plane out(std::max(0, width), std::max(0, height), src.channels, src.alpha);
    out.hdr = src.hdr;
    return out;
}

// --- Image Size's separable filters (resample.rs) ------------------------------------------------------------------------

double support(Filter f) {
    switch (f) {
        case Filter::Nearest: return 0.5;
        case Filter::Bilinear: return 1.0;
        case Filter::Bicubic: return 2.0;
        case Filter::Lanczos:
        case Filter::PreserveDetails: return 3.0;
    }
    return 1.0;
}

double weight(Filter f, double x) {
    x = std::fabs(x);
    switch (f) {
        case Filter::Nearest: return x <= 0.5 ? 1.0 : 0.0;
        case Filter::Bilinear: return std::max(1.0 - x, 0.0);
        case Filter::Bicubic: {
            const double a = -0.5;  // (Keys, Catmull-Rom)
            if (x < 1.0) return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
            if (x < 2.0) return a * x * x * x - 5.0 * a * x * x + 8.0 * a * x - 4.0 * a;
            return 0.0;
        }
        case Filter::Lanczos:
        case Filter::PreserveDetails: {
            if (x < 1e-9) return 1.0;
            if (x < 3.0) {
                const double px = std::numbers::pi * x;
                return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
            }
            return 0.0;
        }
    }
    return 0.0;
}

struct Taps {
    int start = 0;
    std::vector<double> weights;
};

std::vector<Taps> taps(int count, double scale, Filter filter) {
    const double stretch = scale < 1.0 ? 1.0 / scale : 1.0;
    const double reach = support(filter) * stretch;
    std::vector<Taps> out;
    out.reserve(std::size_t(std::max(0, count)));
    for (int o = 0; o < count; ++o) {
        const double u = (o + 0.5) / scale - 0.5;
        if (filter == Filter::Nearest && scale >= 1.0) {
            out.push_back(Taps{static_cast<int>(std::floor(u + 0.5)), {1.0}});
            continue;
        }
        const int lo = static_cast<int>(std::ceil(u - reach)), hi = static_cast<int>(std::floor(u + reach));
        Taps t{lo, {}};
        double sum = 0.0;
        for (int i = lo; i <= hi; ++i) {
            t.weights.push_back(weight(filter, (i - u) / stretch));
            sum += t.weights.back();
        }
        if (std::fabs(sum) > 1e-12)
            for (double& w : t.weights) w /= sum;
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace

std::optional<Interp> interp_from(std::string_view text) {
    if (text == "nearest") return Interp::Nearest;
    if (text == "bilinear") return Interp::Bilinear;
    if (text == "bicubic") return Interp::Bicubic;
    return std::nullopt;
}

std::optional<Filter> filter_from(std::string_view text) {
    if (text == "nearest") return Filter::Nearest;
    if (text == "bilinear") return Filter::Bilinear;
    if (text == "bicubic") return Filter::Bicubic;
    if (text == "lanczos") return Filter::Lanczos;
    if (text == "preserve_details") return Filter::PreserveDetails;
    return std::nullopt;
}

std::pair<double, double> Homography::apply(double x, double y) const {
    const double w = m[6] * x + m[7] * y + m[8];
    return {(m[0] * x + m[1] * y + m[2]) / w, (m[3] * x + m[4] * y + m[5]) / w};
}

Homography Homography::mul(const Homography& o) const {
    Homography r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += m[std::size_t(i * 3 + k)] * o.m[std::size_t(k * 3 + j)];
            r.m[std::size_t(i * 3 + j)] = s;
        }
    }
    return r;
}

std::optional<Homography> Homography::inverse() const {
    const auto& a = m;
    const std::array<double, 9> c{a[4] * a[8] - a[5] * a[7], a[2] * a[7] - a[1] * a[8], a[1] * a[5] - a[2] * a[4],
                                  a[5] * a[6] - a[3] * a[8], a[0] * a[8] - a[2] * a[6], a[2] * a[3] - a[0] * a[5],
                                  a[3] * a[7] - a[4] * a[6], a[1] * a[6] - a[0] * a[7], a[0] * a[4] - a[1] * a[3]};
    const double det = a[0] * c[0] + a[1] * c[3] + a[2] * c[6];
    if (std::fabs(det) < 1e-12) return std::nullopt;
    Homography out;
    for (std::size_t i = 0; i < 9; ++i) out.m[i] = c[i] / det;
    return out;
}

Homography Homography::affine(const std::array<double, 6>& t) { return Homography{{t[0], t[2], t[4], t[1], t[3], t[5], 0.0, 0.0, 1.0}}; }

std::optional<Homography> Homography::rect_to_quad(const std::array<double, 4>& r, const std::array<std::array<double, 2>, 4>& q) {
    const double w = r[2] - r[0], h = r[3] - r[1];
    if (!(w > 0.0) || !(h > 0.0)) return std::nullopt;
    const Homography to_unit{{1.0 / w, 0.0, -r[0] / w, 0.0, 1.0 / h, -r[1] / h, 0.0, 0.0, 1.0}};
    const auto& [p0, p1, p2, p3] = q;
    const double x0 = p0[0], y0 = p0[1], x1 = p1[0], y1 = p1[1], x2 = p2[0], y2 = p2[1], x3 = p3[0], y3 = p3[1];
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    Homography sq;
    if (std::fabs(sx) < 1e-12 && std::fabs(sy) < 1e-12) {
        sq = Homography{{x1 - x0, x3 - x0, x0, y1 - y0, y3 - y0, y0, 0.0, 0.0, 1.0}};
    } else {
        const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(den) < 1e-12) return std::nullopt;
        const double g = (sx * dy2 - dx2 * sy) / den, hh = (dx1 * sy - sx * dy1) / den;
        sq = Homography{{x1 - x0 + g * x1, x3 - x0 + hh * x3, x0, y1 - y0 + g * y1, y3 - y0 + hh * y3, y0, g, hh, 1.0}};
    }
    return sq.mul(to_unit);
}

double Homography::local_scale(double x, double y) const {
    const double e = 0.5;
    const auto a = apply(x - e, y), b = apply(x + e, y), c = apply(x, y - e), d = apply(x, y + e);
    const double jx0 = b.first - a.first, jx1 = b.second - a.second, jy0 = d.first - c.first, jy1 = d.second - c.second;
    return std::sqrt(std::fabs(jx0 * jy1 - jx1 * jy0));
}

Plane project(const Plane& src_in, const Homography& h_in, int width, int height, Interp interp, int x0, int y0) {
    Plane out = empty_like(src_in, width, height);
    if (src_in.width <= 0 || src_in.height <= 0 || width <= 0 || height <= 0) return out;
    // (a move by whole pixels: the pixels moved, exact)
    const auto& m = h_in.m;
    if (h_in.is_affine() && m[0] == 1.0 && m[1] == 0.0 && m[3] == 0.0 && m[4] == 1.0 && m[2] == std::floor(m[2]) && m[5] == std::floor(m[5]) &&
        std::fabs(m[2]) < 1e9 && std::fabs(m[5]) < 1e9) {
        return translate(src_in, static_cast<int>(m[2]) - x0, static_cast<int>(m[5]) - y0, width, height);
    }
    Homography h = h_in;
    const Plane* src = &src_in;
    Plane reduced;
    const double scale = h.local_scale(src_in.width / 2.0, src_in.height / 2.0);
    if (interp != Interp::Nearest && scale < 0.5 && scale > 0.0) {
        // (pre-reduced by a power of 2, the rest of the scale in [0.5, 1): bicubic alone aliases below half)
        const double f = std::min(std::pow(2.0, std::ceil(std::log2(scale))), 1.0);
        const int rw = std::max(1, static_cast<int>(std::ceil(src_in.width * f))), rh = std::max(1, static_cast<int>(std::ceil(src_in.height * f)));
        reduced = resize(src_in, rw, rh, Filter::Bicubic);
        src = &reduced;
        const double fx = static_cast<double>(rw) / src_in.width, fy = static_cast<double>(rh) / src_in.height;
        h = h.mul(Homography{{1.0 / fx, 0.0, 0.0, 0.0, 1.0 / fy, 0.0, 0.0, 0.0, 1.0}});
    }
    const auto inv = h.inverse();
    if (!inv) return out;
    const Source s(*src);
    const double sw = src->width, sh = src->height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto [u, v] = inv->apply(x0 + x + 0.5, y0 + y + 0.5);
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            u -= 0.5;
            v -= 0.5;
            if (src->alpha && (u < -1.0 || v < -1.0 || u > sw || v > sh)) continue;
            sample(s, u, v, interp, out.at(x, y), false);
        }
    }
    return out;
}

Plane mesh(const Plane& src, const Forward& f, int width, int height, Interp interp, int x0, int y0) {
    Plane out = empty_like(src, width, height);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) return out;
    constexpr double kCell = 4.0;
    constexpr int kMaxCells = 512;
    const double sw = src.width, sh = src.height;
    const int nx = std::clamp(static_cast<int>(std::ceil(sw / kCell)), 1, kMaxCells);
    const int ny = std::clamp(static_cast<int>(std::ceil(sh / kCell)), 1, kMaxCells);
    std::vector<Corner> verts;
    verts.reserve(std::size_t(nx + 1) * std::size_t(ny + 1));
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double sx = sw * i / nx, sy = sh * j / ny;
            const auto [dx, dy] = f(sx, sy);
            if (!std::isfinite(dx) || !std::isfinite(dy)) return out;
            verts.push_back(Corner{dx, dy, sx, sy});
        }
    }
    std::vector<std::array<std::size_t, 3>> tris;
    const std::size_t w1 = std::size_t(nx) + 1;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t a = std::size_t(j) * w1 + std::size_t(i);
            tris.push_back({a, a + 1, a + w1 + 1});
            tris.push_back({a, a + w1 + 1, a + w1});
        }
    }
    return triangles(src, verts, tris, width, height, interp, x0, y0);
}

namespace {

// The places of the destination pixels of rows [r0, r1) and columns [c0, c1) of the corners' own coordinates (each pixel
// (x, y) at its centre x + 0.5, y + 0.5) through the triangles, into `uv` (row-major over that box, NaN where none);
// whether any was found.
bool places_in(const std::vector<Corner>& verts, const std::vector<std::array<std::size_t, 3>>& tris, int c0, int r0, int c1, int r1,
               std::vector<std::array<double, 2>>& uv) {
    const int width = c1 - c0;
    uv.assign(std::size_t(std::max(width, 0)) * std::size_t(std::max(r1 - r0, 0)), {std::numeric_limits<double>::quiet_NaN(), 0.0});
    bool any = false;
    for (const auto& tri : tris) {
        if (tri[0] >= verts.size() || tri[1] >= verts.size() || tri[2] >= verts.size()) continue;
        const Corner& p0 = verts[tri[0]];
        const Corner& p1 = verts[tri[1]];
        const Corner& p2 = verts[tri[2]];
        const double det = (p1.dx - p0.dx) * (p2.dy - p0.dy) - (p2.dx - p0.dx) * (p1.dy - p0.dy);
        if (std::fabs(det) < 1e-12) continue;
        const int bx0 = std::max(c0, static_cast<int>(std::floor(std::min({p0.dx, p1.dx, p2.dx}) - 0.5)));
        const int by0 = std::max(r0, static_cast<int>(std::floor(std::min({p0.dy, p1.dy, p2.dy}) - 0.5)));
        const int bx1 = std::min(c1, static_cast<int>(std::ceil(std::max({p0.dx, p1.dx, p2.dx}) + 0.5)));
        const int by1 = std::min(r1, static_cast<int>(std::ceil(std::max({p0.dy, p1.dy, p2.dy}) + 0.5)));
        const double eps = -1e-9;
        for (int y = by0; y < by1; ++y) {
            const double py = y + 0.5;
            for (int x = bx0; x < bx1; ++x) {
                const double px = x + 0.5;
                const double l1 = ((px - p0.dx) * (p2.dy - p0.dy) - (p2.dx - p0.dx) * (py - p0.dy)) / det;
                const double l2 = ((p1.dx - p0.dx) * (py - p0.dy) - (px - p0.dx) * (p1.dy - p0.dy)) / det;
                const double l0 = 1.0 - l1 - l2;
                if (l0 < eps || l1 < eps || l2 < eps) continue;
                uv[std::size_t(y - r0) * std::size_t(width) + std::size_t(x - c0)] = {l0 * p0.sx + l1 * p1.sx + l2 * p2.sx, l0 * p0.sy + l1 * p1.sy + l2 * p2.sy};
                any = true;
            }
        }
    }
    return any;
}

}  // namespace

std::vector<std::array<double, 2>> triangle_places(const std::vector<Corner>& corners, const std::vector<std::array<std::size_t, 3>>& tris, int x0, int y0,
                                                   int width, int height) {
    std::vector<std::array<double, 2>> uv;
    for (const Corner& c : corners) {
        if (!std::isfinite(c.dx) || !std::isfinite(c.dy) || !std::isfinite(c.sx) || !std::isfinite(c.sy)) {
            uv.assign(std::size_t(std::max(width, 0)) * std::size_t(std::max(height, 0)), {std::numeric_limits<double>::quiet_NaN(), 0.0});
            return uv;
        }
    }
    places_in(corners, tris, x0, y0, x0 + width, y0 + height, uv);
    return uv;
}

Plane triangles(const Plane& src, const std::vector<Corner>& corners, const std::vector<std::array<std::size_t, 3>>& tris, int width, int height,
                Interp interp, int x0, int y0) {
    Plane out = empty_like(src, width, height);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) return out;
    std::vector<Corner> verts;
    verts.reserve(corners.size());
    for (const Corner& c : corners) {
        if (!std::isfinite(c.dx) || !std::isfinite(c.dy) || !std::isfinite(c.sx) || !std::isfinite(c.sy)) return out;
        verts.push_back(Corner{c.dx - x0, c.dy - y0, c.sx, c.sy});
    }
    const Source s(src);
    // (worked in bands of rows: each band's source positions, then sampled)
    constexpr int kBand = 256;
    std::vector<std::array<double, 2>> uv;
    for (int b0 = 0; b0 < height; b0 += kBand) {
        const int b1 = std::min(height, b0 + kBand);
        if (!places_in(verts, tris, 0, b0, width, b1, uv)) continue;
        for (int y = b0; y < b1; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto& q = uv[std::size_t(y - b0) * std::size_t(width) + std::size_t(x)];
                if (std::isnan(q[0])) continue;
                sample(s, q[0] - 0.5, q[1] - 0.5, interp, out.at(x, y), true);
            }
        }
    }
    return out;
}

Plane remap(const Plane& src, const Backward& g, int width, int height, Interp interp, int x0, int y0) {
    Plane out = empty_like(src, width, height);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) return out;
    const Source s(src);
    const double sw = src.width, sh = src.height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto [u, v] = g(x0 + x + 0.5, y0 + y + 0.5);
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            u -= 0.5;
            v -= 0.5;
            if (src.alpha && (u < -1.0 || v < -1.0 || u > sw || v > sh)) continue;
            sample(s, u, v, interp, out.at(x, y), true);
        }
    }
    return out;
}

Plane resize(const Plane& src, int width, int height, Filter filter, bool edges) {
    Plane out = empty_like(src, width, height);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) return out;
    const int n = src.channels;
    const bool alpha = src.alpha;
    const auto hx = taps(width, static_cast<double>(width) / src.width, filter);
    const auto vy = taps(height, static_cast<double>(height) / src.height, filter);
    const auto read = [&](int x, int y, int c) -> double {
        if (x < 0 || y < 0 || x >= src.width || y >= src.height) {
            if (alpha && !edges) return 0.0;
            x = std::clamp(x, 0, src.width - 1);  // (grey, or a picture as big as its page: its edge carried on)
            y = std::clamp(y, 0, src.height - 1);
        }
        const float* p = src.at(x, y);
        return alpha && c + 1 < n ? static_cast<double>(p[c]) * p[n - 1] : static_cast<double>(p[c]);
    };
    // (in bands of 64 destination rows: the horizontal pass over the source rows the band reads, then the vertical one —
    // the work held is the band's, not the whole picture's)
    constexpr int kBand = 64;
    std::vector<double> tmp;
    std::vector<double> acc(static_cast<std::size_t>(n));
    for (int b0 = 0; b0 < height; b0 += kBand) {
        const int b1 = std::min(height, b0 + kBand);
        int y_lo = std::numeric_limits<int>::max(), y_hi = std::numeric_limits<int>::min();
        for (int oy = b0; oy < b1; ++oy) {
            const Taps& t = vy[std::size_t(oy)];
            y_lo = std::min(y_lo, t.start);
            y_hi = std::max(y_hi, t.start + static_cast<int>(t.weights.size()));
        }
        const int rows = y_hi - y_lo;
        tmp.assign(std::size_t(rows) * std::size_t(width) * std::size_t(n), 0.0);
        for (int r = 0; r < rows; ++r) {
            const int sy = y_lo + r;
            for (int ox = 0; ox < width; ++ox) {
                double* d = &tmp[(std::size_t(r) * std::size_t(width) + std::size_t(ox)) * std::size_t(n)];
                const Taps& t = hx[std::size_t(ox)];
                for (std::size_t k = 0; k < t.weights.size(); ++k) {
                    for (int c = 0; c < n; ++c) d[c] += read(t.start + static_cast<int>(k), sy, c) * t.weights[k];
                }
            }
        }
        for (int oy = b0; oy < b1; ++oy) {
            const Taps& t = vy[std::size_t(oy)];
            for (int ox = 0; ox < width; ++ox) {
                std::fill(acc.begin(), acc.end(), 0.0);
                for (std::size_t k = 0; k < t.weights.size(); ++k) {
                    const double* sp = &tmp[(std::size_t(t.start + static_cast<int>(k) - y_lo) * std::size_t(width) + std::size_t(ox)) * std::size_t(n)];
                    for (int c = 0; c < n; ++c) acc[std::size_t(c)] += sp[c] * t.weights[k];
                }
                float* o = out.at(ox, oy);
                if (!alpha) {
                    for (int c = 0; c < n; ++c) o[c] = static_cast<float>(std::clamp(acc[std::size_t(c)], 0.0, 1.0));
                    continue;
                }
                const double al = std::clamp(acc[std::size_t(n - 1)], 0.0, 1.0);
                o[n - 1] = static_cast<float>(al);
                for (int c = 0; c + 1 < n; ++c) {
                    const double value = al > 0.0 ? acc[std::size_t(c)] / al : 0.0;
                    o[c] = static_cast<float>(src.hdr ? std::max(value, 0.0) : std::clamp(value, 0.0, 1.0));
                }
            }
        }
    }
    if (filter == Filter::PreserveDetails && (width > src.width || height > src.height)) {
        // (a light unsharp mask on the colours: 30 %, radius 0.6 × the scale, as PhotoCraft's)
        const float sigma = static_cast<float>(0.6 * std::max(static_cast<double>(width) / src.width, static_cast<double>(height) / src.height));
        std::vector<float> blurred = out.v;
        image_ops::gauss_blur(blurred, std::size_t(width), std::size_t(height), std::size_t(n), sigma);
        for (std::size_t i = 0; i < out.v.size(); ++i) {
            if (alpha && (i % std::size_t(n)) == std::size_t(n - 1)) continue;
            const float v = out.v[i] + 0.3f * (out.v[i] - blurred[i]);
            out.v[i] = src.hdr ? std::max(v, 0.0f) : std::clamp(v, 0.0f, 1.0f);
        }
    }
    return out;
}

std::optional<Turn> turn_from(std::string_view text) {
    if (text == "90cw") return Turn::Cw90;
    if (text == "90ccw") return Turn::Ccw90;
    if (text == "180") return Turn::Rotate180;
    if (text == "flip_h") return Turn::FlipH;
    if (text == "flip_v") return Turn::FlipV;
    return std::nullopt;
}

Plane turn(const Plane& src, Turn t) {
    const bool swaps = t == Turn::Cw90 || t == Turn::Ccw90;
    Plane out = empty_like(src, swaps ? src.height : src.width, swaps ? src.width : src.height);
    const int w = src.width, h = src.height;
    const std::size_t n = std::size_t(src.channels);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int nx = x, ny = y;
            switch (t) {
                case Turn::FlipH: nx = w - 1 - x; break;
                case Turn::FlipV: ny = h - 1 - y; break;
                case Turn::Rotate180: nx = w - 1 - x; ny = h - 1 - y; break;
                case Turn::Cw90: nx = h - 1 - y; ny = x; break;
                case Turn::Ccw90: nx = y; ny = w - 1 - x; break;
            }
            std::copy_n(src.at(x, y), n, out.at(nx, ny));
        }
    }
    return out;
}

Plane translate(const Plane& src, int dx, int dy, int width, int height) {
    Plane out = empty_like(src, width, height);
    const std::size_t n = std::size_t(src.channels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sx = x - dx, sy = y - dy;
            if (sx < 0 || sy < 0 || sx >= src.width || sy >= src.height) {
                if (src.alpha || src.width <= 0 || src.height <= 0) continue;
                sx = std::clamp(sx, 0, src.width - 1);  // (grey: its edge carried on)
                sy = std::clamp(sy, 0, src.height - 1);
            }
            std::copy_n(src.at(sx, sy), n, out.at(x, y));
        }
    }
    return out;
}

std::array<double, 6> turn_affine(Turn t, double w, double h) {
    switch (t) {
        case Turn::FlipH: return {-1.0, 0.0, 0.0, 1.0, w, 0.0};
        case Turn::FlipV: return {1.0, 0.0, 0.0, -1.0, 0.0, h};
        case Turn::Rotate180: return {-1.0, 0.0, 0.0, -1.0, w, h};
        case Turn::Cw90: return {0.0, 1.0, -1.0, 0.0, h, 0.0};
        case Turn::Ccw90: return {0.0, -1.0, 1.0, 0.0, 0.0, w};
    }
    return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
}

Plane from_rgba8(const Image& rgba) {
    const Image picture = rgba.mode() == "RGBA" ? rgba : rgba.convert("RGBA");
    const std::string bytes = picture.tobytes();
    Plane out(picture.width(), picture.height(), 4, true);
    for (std::size_t i = 0; i < bytes.size(); ++i) out.v[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

Image to_rgba8(const Plane& plane) {
    std::string bytes(plane.v.size(), '\0');
    for (std::size_t i = 0; i < plane.v.size(); ++i) {
        bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(static_cast<double>(plane.v[i]), 0.0, 1.0) * 255.0)));
    }
    return Image::frombytes("RGBA", Size{plane.width, plane.height}, bytes);
}

Plane from_gray(const Image& l) {
    const Image picture = l.mode() == "L" ? l : l.convert("L");
    const std::string bytes = picture.tobytes();
    Plane out(picture.width(), picture.height(), 1, false);
    for (std::size_t i = 0; i < bytes.size(); ++i) out.v[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

Image to_gray(const Plane& plane) {
    std::string bytes(std::size_t(plane.width) * std::size_t(plane.height), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(static_cast<unsigned char>(
            std::lround(std::clamp(static_cast<double>(plane.v[i * std::size_t(plane.channels)]), 0.0, 1.0) * 255.0)));
    }
    return Image::frombytes("L", Size{plane.width, plane.height}, bytes);
}

Plane from_pixels(const image_ops::Pixels& p) {
    Plane out(static_cast<int>(p.width()), static_cast<int>(p.height()), static_cast<int>(p.all()), true);
    out.hdr = p.kind() == image_ops::Pixels::Kind::Precise && p.floating;
    for (std::size_t i = 0; i < p.size(); ++i) std::copy_n(p.at(i), p.all(), out.v.data() + i * p.all());
    return out;
}

image_ops::Pixels to_pixels(const Plane& plane, const image_ops::Pixels& like) {
    if (plane.channels != static_cast<int>(like.all()) || !plane.alpha) throw core::Error("value", "the plane is not of the layer's kind of pixels");
    image_ops::Pixels out(like.kind(), static_cast<std::uint32_t>(plane.width), static_cast<std::uint32_t>(plane.height), like.colours());
    out.floating = like.floating;
    out.ink = like.ink;
    for (std::size_t i = 0; i < out.size(); ++i) std::copy_n(plane.v.data() + i * out.all(), out.all(), out.at(i));
    return out;
}

}  // namespace genko::render::resample
