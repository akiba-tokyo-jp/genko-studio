#include "render/path_trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>

// PhotoCraft v0.2.0 crates/vector/src/trace.rs (1-446), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::path_trace {

using core::vector::Knot;
using core::vector::Path;
using core::vector::PathOp;
using core::vector::Subpath;

namespace {

using P = XY;

// An edge between two pixel middles: horizontal (x, y)–(x + 1, y) is dir 0, vertical (x, y)–(x, y + 1) is dir 1.
struct EdgeId {
    std::int64_t x, y;
    int dir;
};

P sub(P a, P b) { return {a.first - b.first, a.second - b.second}; }
P add(P a, P b) { return {a.first + b.first, a.second + b.second}; }
P mul(P a, double k) { return {a.first * k, a.second * k}; }
double dot(P a, P b) { return a.first * b.first + a.second * b.second; }
double len(P a) { return std::hypot(a.first, a.second); }
P norm(P a) {
    const double l = len(a);
    return l > 1e-12 ? P{a.first / l, a.second / l} : P{0.0, 0.0};
}

// The point `window` px of arc length away from i (forwards or backwards) on a closed loop.
P walk(const std::vector<P>& lp, std::size_t i, double window, bool fwd) {
    const std::size_t n = lp.size();
    double acc = 0.0;
    std::size_t cur = i;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t nx = fwd ? (cur + 1) % n : (cur + n - 1) % n;
        const double d = len(sub(lp[nx], lp[cur]));
        if (acc + d >= window) {
            const double t = (window - acc) / std::max(d, 1e-12);
            return add(lp[cur], mul(sub(lp[nx], lp[cur]), t));
        }
        acc += d;
        cur = nx;
    }
    return lp[cur];
}

// The indices of the corners.
std::vector<std::size_t> corners(const std::vector<P>& lp) {
    const std::size_t n = lp.size();
    const double window = 1.5;
    const double cos_thresh = std::cos(60.0 * std::numbers::pi / 180.0);
    constexpr double kTie = 1e-12;
    std::vector<double> turn(n);
    for (std::size_t i = 0; i < n; ++i) {
        const P a = norm(sub(lp[i], walk(lp, i, window, false)));
        const P b = norm(sub(walk(lp, i, window, true), lp[i]));
        turn[i] = dot(a, b);
    }
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < n; ++i) {
        if (turn[i] >= cos_thresh) continue;
        // (the cosine's least within ±window; ties — within kTie, whatever the rounding — the first index)
        bool is_min = true;
        double acc = 0.0;
        std::size_t j = i;
        while (acc < window) {
            const std::size_t nx = (j + 1) % n;
            acc += len(sub(lp[nx], lp[j]));
            j = nx;
            if (j == i) break;
            if (turn[j] < turn[i] - kTie) {
                is_min = false;
                break;
            }
        }
        acc = 0.0;
        j = i;
        while (is_min && acc < window) {
            const std::size_t pv = (j + n - 1) % n;
            acc += len(sub(lp[j], lp[pv]));
            j = pv;
            if (j == i) break;
            if (turn[j] <= turn[i] + kTie) is_min = false;
        }
        if (is_min) out.push_back(i);
    }
    return out;
}

using Cubic = std::array<P, 4>;

P bezier(const Cubic& b, double t) {
    const double mt = 1.0 - t;
    const double a = mt * mt * mt, bb = 3.0 * mt * mt * t, c = 3.0 * mt * t * t, d = t * t * t;
    return {a * b[0].first + bb * b[1].first + c * b[2].first + d * b[3].first, a * b[0].second + bb * b[1].second + c * b[2].second + d * b[3].second};
}

P bezier_d1(const Cubic& b, double t) {
    const double mt = 1.0 - t;
    const P q0 = mul(sub(b[1], b[0]), 3.0);
    const P q1 = mul(sub(b[2], b[1]), 3.0);
    const P q2 = mul(sub(b[3], b[2]), 3.0);
    return add(add(mul(q0, mt * mt), mul(q1, 2.0 * mt * t)), mul(q2, t * t));
}

P bezier_d2(const Cubic& b, double t) {
    const P r0 = mul(add(sub(b[2], mul(b[1], 2.0)), b[0]), 6.0);
    const P r1 = mul(add(sub(b[3], mul(b[2], 2.0)), b[1]), 6.0);
    return add(mul(r0, 1.0 - t), mul(r1, t));
}

std::vector<double> chord_params(const std::vector<P>& pts) {
    std::vector<double> u(pts.size(), 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i) u[i] = u[i - 1] + len(sub(pts[i], pts[i - 1]));
    const double total = u.back();
    if (total > 0.0) {
        for (double& v : u) v /= total;
    }
    return u;
}

// The control points by least squares, the end tangents fixed.
Cubic generate(const std::vector<P>& pts, const std::vector<double>& u, P t1, P t2) {
    const P p0 = pts.front(), p3 = pts.back();
    double c[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    double x[2] = {0.0, 0.0};
    for (std::size_t i = 0; i < u.size(); ++i) {
        const double t = u[i], mt = 1.0 - t;
        const double b1 = 3.0 * mt * mt * t, b2 = 3.0 * mt * t * t;
        const P a1 = mul(t1, b1), a2 = mul(t2, b2);
        c[0][0] += dot(a1, a1);
        c[0][1] += dot(a1, a2);
        c[1][1] += dot(a2, a2);
        const double b0 = mt * mt * mt, b3 = t * t * t;
        const P tmp = sub(pts[i], add(mul(p0, b0 + b1), mul(p3, b2 + b3)));
        x[0] += dot(a1, tmp);
        x[1] += dot(a2, tmp);
    }
    c[1][0] = c[0][1];
    const double det = c[0][0] * c[1][1] - c[1][0] * c[0][1];
    const double det0 = c[0][0] * x[1] - c[1][0] * x[0];
    const double det1 = x[0] * c[1][1] - x[1] * c[0][1];
    double al = 0.0, ar = 0.0;
    if (std::fabs(det) > 1e-12) {
        al = det1 / det;
        ar = det0 / det;
    }
    const double seg = len(sub(p3, p0));
    const double eps = 1e-6 * seg;
    if (al < eps || ar < eps) {
        al = seg / 3.0;
        ar = seg / 3.0;
    }
    return {p0, add(p0, mul(t1, al)), add(p3, mul(t2, ar)), p3};
}

std::pair<double, std::size_t> max_error(const std::vector<P>& pts, const Cubic& b, const std::vector<double>& u) {
    std::pair<double, std::size_t> best{0.0, pts.size() / 2};
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        const P d = sub(bezier(b, u[i]), pts[i]);
        const double e = dot(d, d);
        if (e >= best.first) best = {e, i};
    }
    return best;
}

std::vector<double> reparam(const std::vector<P>& pts, const std::vector<double>& u, const Cubic& b) {
    std::vector<double> out(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const double t = u[i];
        const P q = bezier(b, t), d1 = bezier_d1(b, t), d2 = bezier_d2(b, t);
        const double num = dot(sub(q, pts[i]), d1);
        const double den = dot(d1, d1) + dot(sub(q, pts[i]), d2);
        out[i] = std::fabs(den) < 1e-12 ? t : std::clamp(t - num / den, 0.0, 1.0);
    }
    return out;
}

void fit_cubic(const std::vector<P>& pts, P t1, P t2, double err2, std::vector<Cubic>& out, unsigned depth) {
    const std::size_t n = pts.size();
    if (n == 2 || depth > 40) {
        const double d = len(sub(pts[n - 1], pts[0])) / 3.0;
        out.push_back({pts[0], add(pts[0], mul(t1, d)), add(pts[n - 1], mul(t2, d)), pts[n - 1]});
        return;
    }
    std::vector<double> u = chord_params(pts);
    Cubic b = generate(pts, u, t1, t2);
    auto [e, split] = max_error(pts, b, u);
    if (e < err2) {
        out.push_back(b);
        return;
    }
    if (e < err2 * 16.0) {
        for (int k = 0; k < 6; ++k) {
            std::vector<double> u2 = reparam(pts, u, b);
            const Cubic b2 = generate(pts, u2, t1, t2);
            const auto [e2, s2] = max_error(pts, b2, u2);
            u = std::move(u2);
            b = b2;
            e = e2;
            split = s2;
            if (e < err2) {
                out.push_back(b);
                return;
            }
        }
    }
    split = std::clamp<std::size_t>(split, 1, n - 2);
    P tc = norm(sub(pts[split - 1], pts[split + 1]));
    if (len(tc) < 0.5) tc = norm(sub(pts[split - 1], pts[split]));
    fit_cubic(std::vector<P>(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(split) + 1), t1, tc, err2, out, depth + 1);
    fit_cubic(std::vector<P>(pts.begin() + static_cast<std::ptrdiff_t>(split), pts.end()), mul(tc, -1.0), t2, err2, out, depth + 1);
}

// A closed loop fitted with cubic Béziers.
Subpath fit_closed(const std::vector<P>& lp, double tol) {
    const std::size_t n = lp.size();
    const std::vector<std::size_t> cs = corners(lp);
    std::vector<Cubic> segs;
    std::vector<bool> corner_at_start;
    if (cs.empty()) {
        // (a smooth loop: from 0, one tangent at the seam)
        std::vector<P> pts = lp;
        pts.push_back(lp[0]);
        const P t = norm(sub(lp[1 % n], lp[n - 1]));
        const std::size_t before = segs.size();
        fit_cubic(pts, t, mul(t, -1.0), tol * tol, segs, 0);
        corner_at_start.insert(corner_at_start.end(), segs.size() - before, false);
    } else {
        for (std::size_t k = 0; k < cs.size(); ++k) {
            const std::size_t c = cs[k], end = cs[(k + 1) % cs.size()];
            std::vector<P> pts{lp[c]};
            std::size_t j = c;
            while (true) {
                j = (j + 1) % n;
                pts.push_back(lp[j]);
                if (j == end) break;
            }
            const std::size_t m = pts.size();
            const P t1 = norm(sub(pts[std::min<std::size_t>(1, m - 1)], pts[0]));
            const P t2 = norm(sub(pts[m >= 2 ? m - 2 : 0], pts[m - 1]));
            const std::size_t before = segs.size();
            fit_cubic(pts, t1, t2, tol * tol, segs, 0);
            for (std::size_t q = before; q < segs.size(); ++q) corner_at_start.push_back(q == before);
        }
    }
    Subpath s;
    s.closed = true;
    const auto pt = [](P p) { return core::vector::XY{p.first, p.second}; };
    s.knots.reserve(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) s.knots.push_back(Knot{pt(segs[i][0]), pt(segs[i][0]), pt(segs[i][1]), !corner_at_start[i]});
    const std::size_t m = segs.size();
    for (std::size_t i = 0; i < m; ++i) s.knots[(i + 1) % m].in = pt(segs[i][2]);
    return s;
}

// The contours about the grid's top left corner (the pixel (0, 0)'s top left at (0, 0)): traced and fitted there, the
// same path comes wherever the grid lies on the page.
std::vector<std::vector<XY>> local_contours(const std::vector<float>& values, std::int64_t w, std::int64_t h, float outside) {
    const auto get = [&](std::int64_t x, std::int64_t y) -> float {
        if (x < 0 || y < 0 || x >= w || y >= h) return outside;
        return values[static_cast<std::size_t>(y * w + x)];
    };
    constexpr float lvl = 0.5f;
    const auto point = [&](std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by) -> P {
        const float va = get(ax, ay), vb = get(bx, by);
        const double t = std::fabs(vb - va) > 1e-9f ? std::clamp(static_cast<double>((lvl - va) / (vb - va)), 0.0, 1.0) : 0.5;
        const double fax = static_cast<double>(ax) + 0.5, fay = static_cast<double>(ay) + 0.5;
        const double fbx = static_cast<double>(bx) + 0.5, fby = static_cast<double>(by) + 0.5;
        return {fax + (fbx - fax) * t, fay + (fby - fay) * t};
    };
    // (an edge's key: its place in the grid of cells -1..w × -1..h, row by row — the order the loops start in)
    const std::int64_t stride = w + 2;
    const auto key = [&](std::int64_t x, std::int64_t y, int dir) -> std::int64_t { return ((y + 1) * stride + (x + 1)) * 2 + dir; };
    const auto edge_of = [&](std::int64_t k) -> EdgeId {
        const int dir = static_cast<int>(k % 2);
        const std::int64_t cell = k / 2;
        return EdgeId{cell % stride - 1, cell / stride - 1, dir};
    };
    // (segments from edge to edge, the inside on the left: y down)
    std::unordered_map<std::int64_t, std::int64_t> next;
    for (std::int64_t cy = -1; cy < h; ++cy) {
        for (std::int64_t cx = -1; cx < w; ++cx) {
            const bool tl = get(cx, cy) >= lvl, tr = get(cx + 1, cy) >= lvl;
            const bool br = get(cx + 1, cy + 1) >= lvl, bl = get(cx, cy + 1) >= lvl;
            const int idx = int(tl) | int(tr) << 1 | int(br) << 2 | int(bl) << 3;
            if (idx == 0 || idx == 15) continue;
            const std::int64_t top = key(cx, cy, 0), bottom = key(cx, cy + 1, 0), left = key(cx, cy, 1), right = key(cx + 1, cy, 1);
            const auto seg = [&](std::int64_t a, std::int64_t b) { next[a] = b; };
            const auto centre = [&] { return (get(cx, cy) + get(cx + 1, cy) + get(cx + 1, cy + 1) + get(cx, cy + 1)) / 4.0f; };
            switch (idx) {
                case 1: seg(left, top); break;
                case 2: seg(top, right); break;
                case 3: seg(left, right); break;
                case 4: seg(right, bottom); break;
                case 5:
                    if (centre() >= lvl) {
                        seg(left, bottom);
                        seg(right, top);
                    } else {
                        seg(left, top);
                        seg(right, bottom);
                    }
                    break;
                case 6: seg(top, bottom); break;
                case 7: seg(left, bottom); break;
                case 8: seg(bottom, left); break;
                case 9: seg(bottom, top); break;
                case 10:
                    if (centre() >= lvl) {
                        seg(top, left);
                        seg(bottom, right);
                    } else {
                        seg(top, right);
                        seg(bottom, left);
                    }
                    break;
                case 11: seg(bottom, right); break;
                case 12: seg(right, left); break;
                case 13: seg(right, top); break;
                case 14: seg(top, left); break;
                default: break;
            }
        }
    }
    const auto edge_point = [&](std::int64_t k) {
        const EdgeId e = edge_of(k);
        return e.dir == 0 ? point(e.x, e.y, e.x + 1, e.y) : point(e.x, e.y, e.x, e.y + 1);
    };
    std::vector<std::int64_t> keys;
    keys.reserve(next.size());
    for (const auto& [k, v] : next) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::vector<std::vector<XY>> out;
    for (const std::int64_t start : keys) {
        if (!next.contains(start)) continue;
        std::vector<P> lp;
        std::int64_t cur = start;
        while (true) {
            const auto it = next.find(cur);
            if (it == next.end()) break;
            const std::int64_t n = it->second;
            next.erase(it);
            lp.push_back(edge_point(cur));
            cur = n;
            if (cur == start) break;
        }
        // (consecutive points the same dropped: the interpolation can land on a corner)
        lp.erase(std::unique(lp.begin(), lp.end(),
                             [](const P& a, const P& b) { return std::fabs(a.first - b.first) < 1e-9 && std::fabs(a.second - b.second) < 1e-9; }),
                 lp.end());
        if (lp.size() >= 3) out.push_back(std::move(lp));
    }
    return out;
}

}  // namespace

std::vector<std::vector<XY>> contours(const std::vector<float>& values, const path_raster::PixelBox& box, float outside) {
    std::vector<std::vector<XY>> loops = local_contours(values, box.width(), box.height(), outside);
    for (std::vector<XY>& lp : loops) {
        for (XY& p : lp) p = {p.first + box.x0, p.second + box.y0};
    }
    return loops;
}

Path trace_mask(const std::vector<float>& values, const path_raster::PixelBox& box, float outside, double tolerance) {
    const std::vector<std::vector<XY>> loops = local_contours(values, box.width(), box.height(), outside);
    Path path;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (loops[i].size() < 3) continue;
        // (fitted to half the tolerance: the worst point reaches the whole only rarely)
        Subpath s = fit_closed(loops[i], std::max(tolerance * 0.5, 0.05));
        s.op = path.subpaths.empty() ? PathOp::Combine : PathOp::Exclude;
        path.subpaths.push_back(std::move(s));
    }
    return path.transformed({1.0, 0.0, 0.0, 1.0, static_cast<double>(box.x0), static_cast<double>(box.y0)});
}

}  // namespace genko::render::path_trace
