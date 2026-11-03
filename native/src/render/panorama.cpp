// Panorama registration and blending (render/panorama.hpp, PC5 I-g): PhotoCraft v0.2.0's algo/src/panorama.rs (MIT,
// native/third_party/photocraft/NOTICE.md), ported in its order on one thread (upstream's par_map keeps the order, so the
// results are the same).

#include "render/panorama.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/segment/segment.hpp"

namespace genko::render::panorama {

namespace {

constexpr Homography kIdentity{};

Homography translation(double x, double y) { return Homography{{1.0, 0.0, x, 0.0, 1.0, y, 0.0, 0.0, 1.0}}; }

Projection projection_of(Layout l) {
    switch (l) {
        case Layout::Cylindrical: return Projection::Cylinder;
        case Layout::Spherical: return Projection::Sphere;
        default: return Projection::Plane;
    }
}

Motion motion_of(Layout l) {
    switch (l) {
        case Layout::Auto:
        case Layout::Perspective: return Motion::Homography;
        case Layout::Cylindrical:
        case Layout::Spherical: return Motion::Euclidean;
        case Layout::Collage: return Motion::Similarity;
        case Layout::Reposition: return Motion::Translation;
    }
    return Motion::Homography;
}

std::size_t min_points(Motion m) {
    switch (m) {
        case Motion::Translation: return 1;
        case Motion::Euclidean:
        case Motion::Similarity: return 2;
        case Motion::Homography: return 4;
    }
    return 4;
}

std::size_t n_params(Motion m) {
    switch (m) {
        case Motion::Translation: return 2;
        case Motion::Euclidean: return 3;
        case Motion::Similarity: return 4;
        case Motion::Homography: return 8;
    }
    return 8;
}

std::vector<double> to_params(Motion motion, const Homography& h) {
    std::array<double, 9> m = h.m;
    for (double& v : m) v /= h.m[8];
    switch (motion) {
        case Motion::Translation: return {m[2], m[5]};
        case Motion::Euclidean: return {std::atan2(m[3], m[0]), m[2], m[5]};
        case Motion::Similarity: return {m[0], m[3], m[2], m[5]};
        case Motion::Homography: return std::vector<double>(m.begin(), m.begin() + 8);
    }
    return {};
}

Homography to_homography(Motion motion, const double* p) {
    switch (motion) {
        case Motion::Translation: return translation(p[0], p[1]);
        case Motion::Euclidean: {
            const double s = std::sin(p[0]), c = std::cos(p[0]);
            return Homography{{c, -s, p[1], s, c, p[2], 0.0, 0.0, 1.0}};
        }
        case Motion::Similarity: return Homography{{p[0], -p[1], p[2], p[1], p[0], p[3], 0.0, 0.0, 1.0}};
        case Motion::Homography: return Homography{{p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 1.0}};
    }
    return kIdentity;
}

}  // namespace

std::optional<Layout> layout_from(std::string_view text) {
    if (text == "auto") return Layout::Auto;
    if (text == "perspective") return Layout::Perspective;
    if (text == "cylindrical") return Layout::Cylindrical;
    if (text == "spherical") return Layout::Spherical;
    if (text == "collage") return Layout::Collage;
    if (text == "reposition") return Layout::Reposition;
    return std::nullopt;
}

std::string_view layout_name(Layout layout) {
    switch (layout) {
        case Layout::Auto: return "auto";
        case Layout::Perspective: return "perspective";
        case Layout::Cylindrical: return "cylindrical";
        case Layout::Spherical: return "spherical";
        case Layout::Collage: return "collage";
        case Layout::Reposition: return "reposition";
    }
    return "auto";
}

std::optional<Homography> fit_motion(Motion motion, const std::vector<XY>& src, const std::vector<XY>& dst) {
    switch (motion) {
        case Motion::Translation: return features::fit(features::Model::Translation, src, dst);
        case Motion::Similarity: return features::fit(features::Model::Similarity, src, dst);
        case Motion::Homography: return features::fit(features::Model::Homography, src, dst);
        case Motion::Euclidean: {
            const std::size_t n = src.size();
            if (n < 2) return std::nullopt;
            const double nf = static_cast<double>(n);
            XY cs{0.0, 0.0}, cd{0.0, 0.0};
            for (const XY& p : src) cs = XY{cs[0] + p[0] / nf, cs[1] + p[1] / nf};
            for (const XY& p : dst) cd = XY{cd[0] + p[0] / nf, cd[1] + p[1] / nf};
            double sxx = 0.0, sxy = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const XY a{src[i][0] - cs[0], src[i][1] - cs[1]}, b{dst[i][0] - cd[0], dst[i][1] - cd[1]};
                sxx += a[0] * b[0] + a[1] * b[1];
                sxy += a[0] * b[1] - a[1] * b[0];
            }
            const double th = std::atan2(sxy, sxx);
            const double s = std::sin(th), c = std::cos(th);
            return Homography{{c, -s, cd[0] - (c * cs[0] - s * cs[1]), s, c, cd[1] - (s * cs[0] + c * cs[1]), 0.0, 0.0, 1.0}};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<Homography, std::vector<std::size_t>>> ransac_motion(Motion motion, const std::vector<XY>& src, const std::vector<XY>& dst,
                                                                             std::size_t iters, double threshold, std::uint64_t seed) {
    const std::size_t k = min_points(motion), n = src.size();
    if (n < k) return std::nullopt;
    features::Rng rng{seed ^ 0x5EED0FA1ULL};
    const auto inliers_of = [&](const Homography& h) {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < n; ++i) {
            const auto [x, y] = h.apply(src[i][0], src[i][1]);
            if (std::isfinite(x) && std::hypot(x - dst[i][0], y - dst[i][1]) <= threshold) out.push_back(i);
        }
        return out;
    };
    std::optional<std::pair<Homography, std::vector<std::size_t>>> best;
    for (std::size_t it = 0; it < std::max<std::size_t>(iters, 1); ++it) {
        std::vector<std::size_t> idx;
        if (n == k) {
            for (std::size_t j = 0; j < k; ++j) idx.push_back(j);
        }
        while (idx.size() < k) {
            const std::size_t i = rng.below(n);
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i);
        }
        std::vector<XY> s, d;
        for (const std::size_t i : idx) {
            s.push_back(src[i]);
            d.push_back(dst[i]);
        }
        const auto h = fit_motion(motion, s, d);
        if (!h) continue;
        std::vector<std::size_t> inl = inliers_of(*h);
        if (!best || inl.size() > best->second.size()) best.emplace(*h, std::move(inl));
    }
    if (!best || best->second.size() < k) return std::nullopt;
    std::vector<XY> s, d;
    for (const std::size_t i : best->second) {
        s.push_back(src[i]);
        d.push_back(dst[i]);
    }
    const Homography h2 = fit_motion(motion, s, d).value_or(best->first);
    std::vector<std::size_t> inl2 = inliers_of(h2);
    if (inl2.size() >= best->second.size()) return std::pair<Homography, std::vector<std::size_t>>{h2, std::move(inl2)};
    return best;
}

// --- Placement -----------------------------------------------------------------------------------------------------------

XY Placement::project(double x, double y) const {
    double dx = x - center[0], dy = y - center[1];
    if (k1 != 0.0) {
        const double s = 1.0 + k1 * (dx * dx + dy * dy) / (focal * focal);
        dx *= s;
        dy *= s;
    }
    const double f = focal;
    switch (projection) {
        case Projection::Plane: return XY{dx, dy};
        case Projection::Cylinder: return XY{f * std::atan2(dx, f), f * dy / std::hypot(dx, f)};
        case Projection::Sphere: return XY{f * std::atan2(dx, f), f * std::atan2(dy, std::hypot(dx, f))};
    }
    return XY{dx, dy};
}

std::optional<XY> Placement::unproject(double a, double b) const {
    const double f = focal;
    double dx = a, dy = b;
    switch (projection) {
        case Projection::Plane: break;
        case Projection::Cylinder: {
            const double th = a / f;
            if (std::fabs(th) >= 1.55) return std::nullopt;
            dx = f * std::tan(th);
            dy = b / std::cos(th);
            break;
        }
        case Projection::Sphere: {
            const double th = a / f, ph = b / f;
            if (std::fabs(th) >= 1.55 || std::fabs(ph) >= 1.55) return std::nullopt;
            dx = f * std::tan(th);
            dy = f * std::tan(ph) / std::cos(th);
            break;
        }
    }
    if (k1 != 0.0) {
        // (r_u = r_d (1 + k1 r_d² / f²) undone by Newton's steps on r_d)
        const double ru = std::hypot(dx, dy);
        if (ru > 1e-9) {
            double rd = ru;
            for (int i = 0; i < 8; ++i) {
                const double g = rd * (1.0 + k1 * rd * rd / (f * f)) - ru;
                const double dg = 1.0 + 3.0 * k1 * rd * rd / (f * f);
                if (std::fabs(dg) < 1e-9) break;
                rd -= g / dg;
            }
            if (!(std::isfinite(rd) && rd > 0.0)) return std::nullopt;
            dx *= rd / ru;
            dy *= rd / ru;
        }
    }
    return XY{dx + center[0], dy + center[1]};
}

XY Placement::forward(double x, double y) const {
    const XY p = project(x, y);
    const auto [u, v] = h.apply(p[0], p[1]);
    return XY{u, v};
}

std::optional<XY> Placement::inverse(double u, double v) const {
    const auto inv = h.inverse();
    if (!inv) return std::nullopt;
    const auto& m = inv->m;
    const double w = m[6] * u + m[7] * v + m[8];
    if (w <= 1e-12) return std::nullopt;
    const auto [a, b] = inv->apply(u, v);
    return unproject(a, b);
}

Placement Placement::scaled(double k) const {
    const Homography s{{k, 0.0, 0.0, 0.0, k, 0.0, 0.0, 0.0, 1.0}};
    const Homography si{{1.0 / k, 0.0, 0.0, 0.0, 1.0 / k, 0.0, 0.0, 0.0, 1.0}};
    Placement out = *this;
    out.focal = focal * k;
    out.center = XY{center[0] * k, center[1] * k};
    out.h = s.mul(h).mul(si);
    return out;
}

Placement Placement::translated(double dx, double dy) const {
    Placement out = *this;
    out.h = translation(dx, dy).mul(h);
    return out;
}

std::array<double, 4> Placement::bounds(double w, double h) const {
    std::array<double, 4> b{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                            std::numeric_limits<double>::lowest()};
    const int n = 32;
    for (int i = 0; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        for (const XY& q : {XY{t * w, 0.0}, XY{t * w, h}, XY{0.0, t * h}, XY{w, t * h}}) {
            const XY p = forward(q[0], q[1]);
            if (std::isfinite(p[0]) && std::isfinite(p[1])) b = {std::min(b[0], p[0]), std::min(b[1], p[1]), std::max(b[2], p[0]), std::max(b[3], p[1])};
        }
    }
    return b;
}

std::optional<Homography> plane_matrix(const Placement& p) {
    if (p.projection != Projection::Plane || p.k1 != 0.0) return std::nullopt;
    return p.h.mul(translation(-p.center[0], -p.center[1]));
}

// --- features and pairs --------------------------------------------------------------------------------------------------

ImageFeatures detect(std::size_t w, std::size_t h, const std::vector<float>& luma, const std::vector<bool>* valid, std::size_t max) {
    const float min_dist = std::clamp(static_cast<float>(std::max(w, h)) / 70.0f, 3.0f, 20.0f);
    const std::vector<features::Corner> corners = features::harris(w, h, luma, max, min_dist, valid);
    ImageFeatures out;
    out.w = w;
    out.h = h;
    out.desc = features::describe(w, h, luma, corners, false);
    for (const features::Corner& c : corners) out.points.push_back(XY{c.x, c.y});
    return out;
}

std::vector<PairMatch> match_pairs(const std::vector<ImageFeatures>& feats) {
    std::vector<PairMatch> out;
    const std::size_t n = feats.size();
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = a + 1; b < n; ++b) {
            segment::check_stop();
            const ImageFeatures &fa = feats[a], &fb = feats[b];
            if (fa.points.size() < 6 || fb.points.size() < 6) continue;
            const auto m = features::match_descriptors(fb.desc, fa.desc, 0.8f);
            if (m.size() < 8) continue;
            std::vector<XY> src, dst;
            for (const auto& [i, j] : m) {
                src.push_back(fb.points[i]);
                dst.push_back(fa.points[j]);
            }
            const double thr = std::clamp(static_cast<double>(std::max(fa.w, fa.h)) / 300.0, 1.5, 4.0);
            const auto r = ransac_motion(Motion::Homography, src, dst, 1200, thr, static_cast<std::uint64_t>(a * 131 + b));
            if (!r) continue;
            const auto& inl = r->second;
            if (static_cast<double>(inl.size()) < 8.0 + 0.3 * static_cast<double>(m.size()) || inl.size() < 10) continue;
            PairMatch pm;
            pm.a = a;
            pm.b = b;
            for (const std::size_t i : inl) {
                pm.pts_a.push_back(dst[i]);
                pm.pts_b.push_back(src[i]);
            }
            pm.homography = r->first;
            out.push_back(std::move(pm));
        }
    }
    return out;
}

std::optional<double> focal_from_homography(const Homography& h, XY ca, XY cb) {
    const Homography ta = translation(-ca[0], -ca[1]);
    const Homography tb = translation(cb[0], cb[1]);
    const auto m = ta.mul(h).mul(tb).m;
    struct Cand {
        double f2, d;
    };
    const auto ok = [](double n, double d) -> std::optional<Cand> {
        if (std::fabs(d) > 1e-12 && n / d > 0.0) return Cand{n / d, std::fabs(d)};
        return std::nullopt;
    };
    const auto pick = [&](std::pair<double, double> c1, std::pair<double, double> c2) -> std::optional<double> {
        // (each a numerator and denominator of f²: the better conditioned)
        const auto a = ok(c1.first, c1.second), b = ok(c2.first, c2.second);
        if (a && b) return a->d >= b->d ? a->f2 : b->f2;
        if (a) return a->f2;
        if (b) return b->f2;
        return std::nullopt;
    };
    // (the columns of K⁻¹HK orthogonal and of one norm: b's focal length …)
    const auto f1 = pick({-(m[0] * m[1] + m[3] * m[4]), m[6] * m[7]}, {m[1] * m[1] + m[4] * m[4] - m[0] * m[0] - m[3] * m[3], m[6] * m[6] - m[7] * m[7]});
    // (… its rows likewise: a's)
    const auto f0 = pick({-(m[2] * m[5]), m[0] * m[3] + m[1] * m[4]}, {m[5] * m[5] - m[2] * m[2], m[0] * m[0] + m[1] * m[1] - m[3] * m[3] - m[4] * m[4]});
    if (f0 && f1) return std::sqrt(std::sqrt(*f0 * *f1));
    if (f0) return std::sqrt(*f0);
    if (f1) return std::sqrt(*f1);
    return std::nullopt;
}

// --- align -------------------------------------------------------------------------------------------------------------

namespace {

struct PairData {
    std::size_t a, b;
    std::vector<XY> pa, pb;
};

Placement base_placement(const ImageFeatures& f, Projection projection, double focal, double k1) {
    Placement p;
    p.projection = projection;
    p.focal = focal;
    p.center = XY{static_cast<double>(f.w) / 2.0, static_cast<double>(f.h) / 2.0};
    p.k1 = k1;
    return p;
}

std::optional<Alignment> align_layout(const std::vector<ImageFeatures>& feats, const std::vector<PairMatch>& matches, const AlignOptions& opts,
                                      Layout layout, double focal) {
    const std::size_t n = feats.size();
    const Projection projection = projection_of(layout);
    const Motion motion = motion_of(layout);
    // (the pairs' motions in the projected space)
    std::vector<PairData> pairs;
    struct Edge {
        std::size_t a, b, inliers;
        Homography h;  // (b → a)
    };
    std::vector<Edge> edges;
    for (const PairMatch& m : matches) {
        const Placement pa = base_placement(feats[m.a], projection, focal, 0.0), pb = base_placement(feats[m.b], projection, focal, 0.0);
        std::vector<XY> qa, qb;
        for (const XY& p : m.pts_a) qa.push_back(pa.project(p[0], p[1]));
        for (const XY& p : m.pts_b) qb.push_back(pb.project(p[0], p[1]));
        const double thr = std::clamp(static_cast<double>(std::max(feats[m.a].w, feats[m.a].h)) / 250.0, 2.0, 5.0);
        const auto r = ransac_motion(motion, qb, qa, 600, thr, static_cast<std::uint64_t>(m.a * 977 + m.b));
        if (!r) continue;
        const auto& inl = r->second;
        if (inl.size() < 8 || static_cast<double>(inl.size()) < 0.5 * static_cast<double>(m.pts_a.size())) continue;
        edges.push_back(Edge{m.a, m.b, inl.size(), r->first});
        // (the bundle adjustment takes at most 80 inliers of a pair, spread evenly)
        const std::size_t step = std::max<std::size_t>((inl.size() + 79) / 80, 1);
        PairData pd{m.a, m.b, {}, {}};
        for (std::size_t k = 0; k < inl.size(); k += step) {
            pd.pa.push_back(m.pts_a[inl[k]]);
            pd.pb.push_back(m.pts_b[inl[k]]);
        }
        pairs.push_back(std::move(pd));
    }
    if (edges.empty()) return std::nullopt;
    std::vector<std::size_t> degree(n, 0);
    for (const Edge& e : edges) {
        degree[e.a] += e.inliers;
        degree[e.b] += e.inliers;
    }
    std::size_t reference = 0;
    if (opts.reference && *opts.reference < n) {
        reference = *opts.reference;
    } else {
        // (the best joined; ties to the middle image)
        const auto mid = static_cast<std::int64_t>(n / 2);
        const auto off = [&](std::size_t i) { return std::llabs(static_cast<std::int64_t>(i) - mid); };
        for (std::size_t i = 1; i < n; ++i) {
            if (degree[i] > degree[reference] || (degree[i] == degree[reference] && off(i) <= off(reference))) reference = i;
        }
    }
    // (a maximum spanning tree, Prim's, from the reference; projected → panorama)
    std::vector<std::optional<Homography>> tf(n);
    const XY rc{static_cast<double>(feats[reference].w) / 2.0, static_cast<double>(feats[reference].h) / 2.0};
    tf[reference] = translation(rc[0], rc[1]);
    for (;;) {
        std::optional<std::tuple<std::size_t, std::size_t, Homography>> best;
        for (const Edge& e : edges) {
            std::optional<std::pair<std::size_t, Homography>> cand;
            if (tf[e.a] && !tf[e.b]) {
                cand.emplace(e.b, tf[e.a]->mul(e.h));
            } else if (!tf[e.a] && tf[e.b]) {
                if (const auto inv = e.h.inverse()) cand.emplace(e.a, tf[e.b]->mul(*inv));
            }
            if (cand && (!best || e.inliers > std::get<1>(*best))) best.emplace(cand->first, e.inliers, cand->second);
        }
        if (!best) break;
        tf[std::get<0>(*best)] = std::get<2>(*best);
    }
    std::vector<std::size_t> placed;
    for (std::size_t i = 0; i < n; ++i) {
        if (tf[i]) placed.push_back(i);
    }
    if (placed.size() < 2) return std::nullopt;
    // (the bundle adjustment: Levenberg–Marquardt, a numeric Jacobian)
    std::vector<std::size_t> movable;
    for (const std::size_t i : placed) {
        if (i != reference) movable.push_back(i);
    }
    const std::size_t mp = n_params(motion);
    const bool refine_focal = projection != Projection::Plane;
    const std::size_t n_glob = (opts.geometric ? 1 : 0) + (refine_focal ? 1 : 0);
    std::vector<double> params;
    for (const std::size_t i : movable) {
        const std::vector<double> p = to_params(motion, *tf[i]);
        params.insert(params.end(), p.begin(), p.end());
    }
    if (opts.geometric) params.push_back(0.0);
    if (refine_focal) params.push_back(std::log(focal));
    const std::size_t n_par = params.size();
    std::vector<std::optional<std::size_t>> slot(n);
    for (std::size_t k = 0; k < movable.size(); ++k) slot[movable[k]] = k;
    const auto decode = [&](const std::vector<double>& p) {
        const double k1 = opts.geometric ? p[movable.size() * mp] : 0.0;
        const double f = refine_focal ? std::exp(p[n_par - 1]) : focal;
        std::vector<std::optional<Placement>> out(n);
        for (std::size_t i = 0; i < n; ++i) {
            Homography h;
            if (i == reference) {
                if (!tf[reference]) continue;
                h = *tf[reference];
            } else {
                if (!slot[i]) continue;
                h = to_homography(motion, p.data() + *slot[i] * mp);
            }
            Placement pl = base_placement(feats[i], projection, f, k1);
            pl.h = h;
            out[i] = pl;
        }
        return out;
    };
    const auto residuals = [&](const std::vector<double>& p) {
        const auto pl = decode(p);
        std::vector<double> r;
        for (const PairData& pd : pairs) {
            if (!pl[pd.a] || !pl[pd.b]) continue;
            for (std::size_t k = 0; k < pd.pa.size(); ++k) {
                const XY ua = pl[pd.a]->forward(pd.pa[k][0], pd.pa[k][1]);
                const XY ub = pl[pd.b]->forward(pd.pb[k][0], pd.pb[k][1]);
                r.push_back(ua[0] - ub[0]);
                r.push_back(ua[1] - ub[1]);
            }
        }
        // (soft priors keep the global terms sane when few pairs hold them)
        if (opts.geometric) r.push_back(p[movable.size() * mp] * 20.0);
        if (refine_focal) r.push_back((p[n_par - 1] - std::log(focal)) * 2.0);
        return r;
    };
    const auto cost = [](const std::vector<double>& r, std::size_t count) {
        double s = 0.0;
        for (std::size_t i = 0; i < count; ++i) s += r[i] * r[i];
        return s;
    };
    std::vector<double> cur = residuals(params);
    double lambda = 1e-3;
    if (n_par > 0 && !movable.empty()) {
        for (int iteration = 0; iteration < 40; ++iteration) {
            segment::check_stop();
            const std::size_t m_res = cur.size();
            std::vector<std::vector<double>> cols(n_par);
            for (std::size_t j = 0; j < n_par; ++j) {
                const double step = 1e-6 * (1.0 + std::fabs(params[j]));
                std::vector<double> pp = params;
                pp[j] += step;
                const std::vector<double> rp = residuals(pp);
                pp[j] -= 2.0 * step;
                const std::vector<double> rm = residuals(pp);
                cols[j].resize(m_res);
                for (std::size_t i = 0; i < m_res; ++i) cols[j][i] = (rp[i] - rm[i]) / (2.0 * step);
            }
            std::vector<double> jtj(n_par * n_par, 0.0), jtr(n_par, 0.0);
            for (std::size_t a = 0; a < n_par; ++a) {
                double s = 0.0;
                for (std::size_t i = 0; i < m_res; ++i) s += cols[a][i] * cur[i];
                jtr[a] = -s;
                for (std::size_t b = a; b < n_par; ++b) {
                    double v = 0.0;
                    for (std::size_t i = 0; i < m_res; ++i) v += cols[a][i] * cols[b][i];
                    jtj[a * n_par + b] = v;
                    jtj[b * n_par + a] = v;
                }
            }
            bool improved = false;
            for (int tries = 0; tries < 8; ++tries) {
                std::vector<double> a = jtj;
                for (std::size_t d = 0; d < n_par; ++d) a[d * n_par + d] += lambda * (jtj[d * n_par + d] + 1e-9);
                const auto delta = features::solve_dense(n_par, std::move(a), jtr, 1e-14);
                if (!delta) {
                    lambda *= 10.0;
                    continue;
                }
                std::vector<double> trial(n_par);
                for (std::size_t j = 0; j < n_par; ++j) trial[j] = params[j] + (*delta)[j];
                const std::vector<double> rt = residuals(trial);
                const double before = cost(cur, cur.size()), after = cost(rt, rt.size());
                if (after < before) {
                    const double gain = before - after;
                    params = std::move(trial);
                    cur = rt;
                    lambda = std::max(lambda * 0.3, 1e-9);
                    improved = gain > 1e-10 * std::max(after, 1e-12);
                    break;
                }
                lambda *= 10.0;
            }
            if (!improved) break;
        }
    }
    Alignment out;
    out.layout = layout;
    out.reference = reference;
    out.placements = decode(params);
    std::size_t n_res = 0;
    for (const PairData& p : pairs) n_res += p.pa.size();
    n_res = std::max<std::size_t>(n_res, 1);
    out.rms = std::sqrt(cost(cur, cur.size() >= n_glob ? cur.size() - n_glob : cur.size()) / static_cast<double>(n_res));
    out.k1 = opts.geometric ? params[movable.size() * mp] : 0.0;
    out.focal = refine_focal ? std::exp(params[n_par - 1]) : focal;
    for (const Edge& e : edges) out.pairs.emplace_back(e.a, e.b, e.inliers);
    return out;
}

// A perspective panorama too stretched: an image grows or shrinks a lot or turns over, or the view is wider than about 110°.
bool too_stretched(const std::vector<ImageFeatures>& feats, const Alignment& a) {
    double x0 = std::numeric_limits<double>::max(), x1 = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < a.placements.size(); ++i) {
        if (!a.placements[i]) continue;
        const Placement& p = *a.placements[i];
        const double w = static_cast<double>(feats[i].w), h = static_cast<double>(feats[i].h);
        const std::array<XY, 4> corners{XY{0.0, 0.0}, XY{w, 0.0}, XY{w, h}, XY{0.0, h}};
        std::array<XY, 4> c{};
        for (std::size_t k = 0; k < 4; ++k) c[k] = p.forward(corners[k][0], corners[k][1]);
        double area = 0.0;
        for (std::size_t k = 0; k < 4; ++k) area += c[k][0] * c[(k + 1) % 4][1] - c[(k + 1) % 4][0] * c[k][1];
        area /= 2.0;
        const double ratio = area / (w * h);
        if (!std::isfinite(ratio) || ratio < 0.3 || ratio > 3.0) return true;
        const auto& m = p.h.m;
        for (const XY& q0 : corners) {
            const XY q = p.project(q0[0], q0[1]);
            if (m[6] * q[0] + m[7] * q[1] + m[8] <= 0.0) return true;
        }
        for (const XY& u : c) {
            x0 = std::min(x0, u[0]);
            x1 = std::max(x1, u[0]);
        }
    }
    const double fov = 2.0 * std::atan((x1 - x0) / 2.0 / a.focal);
    return fov > 110.0 * std::numbers::pi / 180.0;
}

}  // namespace

std::optional<Alignment> align(const std::vector<ImageFeatures>& feats, const std::vector<PairMatch>& matches, const AlignOptions& opts) {
    const std::size_t n = feats.size();
    if (n < 2 || matches.empty()) return std::nullopt;
    double focal = 0.0;
    if (opts.focal && *opts.focal > 1.0) {
        focal = *opts.focal;
    } else {
        std::vector<double> fs;
        for (const PairMatch& m : matches) {
            const ImageFeatures &fa = feats[m.a], &fb = feats[m.b];
            const auto f = focal_from_homography(m.homography, XY{static_cast<double>(fa.w) / 2.0, static_cast<double>(fa.h) / 2.0},
                                                 XY{static_cast<double>(fb.w) / 2.0, static_cast<double>(fb.h) / 2.0});
            if (f && std::isfinite(*f)) fs.push_back(*f);
        }
        std::size_t side_px = 1;
        for (const ImageFeatures& f : feats) side_px = std::max(side_px, std::max(f.w, f.h));
        const double side = static_cast<double>(side_px);
        std::erase_if(fs, [side](double f) { return !(f > side * 0.2 && f < side * 20.0); });
        std::sort(fs.begin(), fs.end());
        focal = fs.empty() ? side : fs[fs.size() / 2];
    }
    if (opts.layout != Layout::Auto) return align_layout(feats, matches, opts, opts.layout, focal);
    // (auto: perspective unless it is too stretched, then cylindrical — spherical when it is tall too)
    if (auto a = align_layout(feats, matches, opts, Layout::Perspective, focal); a && !too_stretched(feats, *a)) return a;
    auto cyl = align_layout(feats, matches, opts, Layout::Cylindrical, focal);
    if (!cyl) return std::nullopt;
    double y0 = std::numeric_limits<double>::max(), y1 = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < cyl->placements.size(); ++i) {
        if (!cyl->placements[i]) continue;
        const auto b = cyl->placements[i]->bounds(static_cast<double>(feats[i].w), static_cast<double>(feats[i].h));
        y0 = std::min(y0, b[1]);
        y1 = std::max(y1, b[3]);
    }
    if ((y1 - y0) / cyl->focal > 1.4) return align_layout(feats, matches, opts, Layout::Spherical, cyl->focal);
    return cyl;
}

// --- photometric -------------------------------------------------------------------------------------------------------

std::optional<std::size_t> RoiImage::at(int x, int y) const {
    const int lx = x - x0, ly = y - y0;
    if (lx >= 0 && ly >= 0 && static_cast<std::size_t>(lx) < w && static_cast<std::size_t>(ly) < h) return std::size_t(ly) * w + std::size_t(lx);
    return std::nullopt;
}

float Photometric::factor(std::size_t i, std::size_t c, double r2) const {
    const double v = std::exp(vignette[0] * r2 + vignette[1] * r2 * r2);
    return static_cast<float>(1.0 / (static_cast<double>(gains[i][c]) * v));
}

void Normal::add(const std::vector<std::pair<std::size_t, double>>& row, double rhs, double w) {
    const double w2 = w * w;
    for (const auto& [i, a] : row) {
        atb[i] += w2 * a * rhs;
        for (const auto& [j, b] : row) ata[i * n + j] += w2 * a * b;
    }
}

std::optional<std::vector<double>> Normal::solve() const { return features::solve_dense(n, ata, atb, 1e-14); }

Photometric photometric(const std::vector<RoiImage>& imgs, const std::vector<Placement>* placements,
                        const std::vector<std::pair<std::size_t, std::size_t>>* sizes, bool gain, bool vignette) {
    const bool geometry = placements != nullptr && sizes != nullptr;
    vignette = vignette && geometry;
    const std::size_t n = imgs.size();
    const std::size_t ch = imgs.empty() ? 1 : imgs.front().ch;
    Photometric identity;
    identity.gains.assign(n, std::vector<float>(ch, 1.0f));
    if (n == 0 || (!gain && !vignette)) return identity;
    // (the overlaps sampled on a grid)
    int x0 = std::numeric_limits<int>::max(), y0 = std::numeric_limits<int>::max(), x1 = std::numeric_limits<int>::min(), y1 = std::numeric_limits<int>::min();
    for (const RoiImage& im : imgs) {
        x0 = std::min(x0, im.x0);
        y0 = std::min(y0, im.y0);
        x1 = std::max(x1, im.x0 + static_cast<int>(im.w));
        y1 = std::max(y1, im.y0 + static_cast<int>(im.h));
    }
    const int step = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max<std::int64_t>(std::int64_t(x1 - x0) * std::int64_t(y1 - y0) / 40000, 1)))));
    const auto r2_of = [&](std::size_t i, int u, int v) -> std::optional<double> {
        if (!geometry) return 0.0;
        const auto s = (*placements)[i].inverse(u + 0.5, v + 0.5);
        if (!s) return std::nullopt;
        const double w = static_cast<double>((*sizes)[i].first), h = static_cast<double>((*sizes)[i].second);
        const double dx = (*s)[0] - w / 2.0, dy = (*s)[1] - h / 2.0;
        return (dx * dx + dy * dy) / (w * w / 4.0 + h * h / 4.0);
    };
    struct Sample {
        std::size_t a, b;
        std::vector<float> ca, cb;
        double ra, rb;
    };
    std::vector<Sample> samples;
    for (int y = y0; y < y1; y += step) {
        segment::check_stop();
        for (int x = x0; x < x1; x += step) {
            std::vector<std::pair<std::size_t, std::size_t>> cover;
            for (std::size_t i = 0; i < n; ++i) {
                if (const auto k = imgs[i].at(x, y); k && imgs[i].alpha[*k] > 0.999f) cover.emplace_back(i, *k);
            }
            for (std::size_t p = 0; p < cover.size(); ++p) {
                for (std::size_t q = p + 1; q < cover.size(); ++q) {
                    const auto [a, ka] = cover[p];
                    const auto [b, kb] = cover[q];
                    std::vector<float> ca(imgs[a].px.begin() + static_cast<std::ptrdiff_t>(ka * ch), imgs[a].px.begin() + static_cast<std::ptrdiff_t>((ka + 1) * ch));
                    std::vector<float> cb(imgs[b].px.begin() + static_cast<std::ptrdiff_t>(kb * ch), imgs[b].px.begin() + static_cast<std::ptrdiff_t>((kb + 1) * ch));
                    bool usable = true;
                    for (const float v : ca) usable = usable && v >= 0.02f && v <= 0.98f;
                    for (const float v : cb) usable = usable && v >= 0.02f && v <= 0.98f;
                    if (!usable) continue;
                    // (what disagrees — parallax, a thing that moved — says nothing of the exposure)
                    bool agree = true;
                    for (std::size_t c = 0; c < ch; ++c) agree = agree && std::fabs(std::log(ca[c] / cb[c])) <= 1.0f;
                    if (!agree) continue;
                    const auto ra = r2_of(a, x, y), rb = r2_of(b, x, y);
                    if (!ra || !rb) continue;
                    samples.push_back(Sample{a, b, std::move(ca), std::move(cb), *ra, *rb});
                }
            }
        }
    }
    if (samples.size() < 20) return identity;
    const double sigma_n = 0.05, sigma_g = gain ? 0.15 : 1e-4;
    Photometric out = identity;
    const std::size_t nv = vignette ? 2 : 0;
    // (the vignetting on the channels' mean, then each channel's gains with it fixed)
    Normal ne(n + nv);
    for (const Sample& s : samples) {
        double la = 0.0, lb = 0.0;
        for (const float v : s.ca) la += std::log(static_cast<double>(v));
        for (const float v : s.cb) lb += std::log(static_cast<double>(v));
        la /= static_cast<double>(ch);
        lb /= static_cast<double>(ch);
        std::vector<std::pair<std::size_t, double>> row{{s.a, 1.0}, {s.b, -1.0}};
        if (vignette) {
            row.emplace_back(n, s.ra - s.rb);
            row.emplace_back(n + 1, s.ra * s.ra - s.rb * s.rb);
        }
        ne.add(row, la - lb, 1.0 / sigma_n);
    }
    for (std::size_t i = 0; i < n; ++i) ne.add({{i, 1.0}}, 0.0, 1.0 / sigma_g);
    if (vignette) {
        ne.add({{n, 1.0}}, 0.0, 0.5);
        ne.add({{n + 1, 1.0}}, 0.0, 0.5);
    }
    const auto sol = ne.solve();
    if (!sol) return out;
    // (vignetting only darkens: a runaway brightening fit clamped)
    if (vignette) out.vignette = {std::clamp((*sol)[n], -2.0, 0.5), std::clamp((*sol)[n + 1], -2.0, 2.0)};
    if (gain) {
        for (std::size_t c = 0; c < ch; ++c) {
            Normal nc(n);
            for (const Sample& s : samples) {
                const double va = out.vignette[0] * s.ra + out.vignette[1] * s.ra * s.ra;
                const double vb = out.vignette[0] * s.rb + out.vignette[1] * s.rb * s.rb;
                nc.add({{s.a, 1.0}, {s.b, -1.0}}, (std::log(static_cast<double>(s.ca[c])) - va) - (std::log(static_cast<double>(s.cb[c])) - vb), 1.0 / sigma_n);
            }
            for (std::size_t i = 0; i < n; ++i) nc.add({{i, 1.0}}, 0.0, 1.0 / sigma_g);
            if (const auto g = nc.solve()) {
                for (std::size_t i = 0; i < n; ++i) out.gains[i][c] = static_cast<float>(std::clamp(std::exp((*g)[i]), 0.2, 5.0));
            }
        }
    }
    return out;
}

// --- seams -------------------------------------------------------------------------------------------------------------

std::vector<int> seam_labels(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& luma, const std::vector<std::vector<bool>>& cover,
                             const std::vector<std::size_t>& order) {
    std::vector<int> label(w * h, -1);
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    for (const std::size_t img : order) {
        segment::check_stop();
        const std::vector<bool>& cov = cover[img];
        std::vector<bool> overlap(w * h, false);
        double sx = 0.0, sy = 0.0, nx = 0.0, ny = 0.0;
        std::size_t no = 0, nn = 0;
        for (std::size_t i = 0; i < w * h; ++i) {
            if (!cov[i]) continue;
            nx += static_cast<double>(i % w);
            ny += static_cast<double>(i / w);
            ++nn;
            if (label[i] >= 0) {
                overlap[i] = true;
                sx += static_cast<double>(i % w);
                sy += static_cast<double>(i / w);
                ++no;
            } else {
                label[i] = static_cast<int>(img);
            }
        }
        if (no == 0 || nn == no) continue;
        const double ox = sx / static_cast<double>(no), oy = sy / static_cast<double>(no);
        const double cx = nx / static_cast<double>(nn), cy = ny / static_cast<double>(nn);
        const bool vertical = std::fabs(cx - ox) >= std::fabs(cy - oy);
        // (the image on the growing side across the seam?)
        const bool positive = vertical ? cx > ox : cy > oy;
        const auto cost = [&](std::size_t i) { return 0.002f + std::fabs(luma[img][i] - luma[static_cast<std::size_t>(label[i])][i]); };
        const std::size_t lines = vertical ? h : w, len = vertical ? w : h;
        const auto idx = [&](std::size_t line, std::size_t k) { return vertical ? line * w + k : k * w + line; };
        // (dynamic programming over the lines, within the overlap)
        std::vector<float> acc(w * h, std::numeric_limits<float>::infinity());
        std::vector<std::size_t> from(w * h, kNone);
        std::optional<std::size_t> prev_line;
        for (std::size_t line = 0; line < lines; ++line) {
            bool any = false;
            for (std::size_t k = 0; k < len && !any; ++k) any = overlap[idx(line, k)];
            if (!any) continue;
            for (std::size_t k = 0; k < len; ++k) {
                const std::size_t i = idx(line, k);
                if (!overlap[i]) continue;
                float best_v = 0.0f;
                std::size_t best_k = kNone;
                if (prev_line && *prev_line + 1 == line) {
                    best_v = std::numeric_limits<float>::infinity();
                    for (const int dk : {-1, 0, 1}) {
                        const std::int64_t kk = static_cast<std::int64_t>(k) + dk;
                        if (kk < 0 || kk >= static_cast<std::int64_t>(len)) continue;
                        const std::size_t j = idx(*prev_line, static_cast<std::size_t>(kk));
                        if (acc[j] < best_v) {
                            best_v = acc[j];
                            best_k = static_cast<std::size_t>(kk);
                        }
                    }
                    if (best_k == kNone) best_v = 0.0f;
                }
                acc[i] = best_v + cost(i);
                from[i] = best_k;
            }
            prev_line = line;
        }
        // (back from the cheapest end; lines not reached keep their own best cell)
        std::vector<std::size_t> seam(lines, kNone);
        for (std::size_t line = lines; line-- > 0;) {
            if (seam[line] != kNone) continue;
            std::size_t k = kNone;
            for (std::size_t kk = 0; kk < len; ++kk) {
                if (!overlap[idx(line, kk)]) continue;
                if (k == kNone || acc[idx(line, kk)] < acc[idx(line, k)]) k = kk;
            }
            if (k == kNone) continue;
            std::size_t l = line;
            for (;;) {
                seam[l] = k;
                const std::size_t f = from[idx(l, k)];
                if (f == kNone || l == 0) break;
                --l;
                if (seam[l] != kNone) break;
                k = f;
            }
        }
        for (std::size_t line = 0; line < lines; ++line) {
            const std::size_t s = seam[line];
            if (s == kNone) continue;
            for (std::size_t k = 0; k < len; ++k) {
                const std::size_t i = idx(line, k);
                if (overlap[i] && ((positive && k > s) || (!positive && k < s))) label[i] = static_cast<int>(img);
            }
        }
    }
    return label;
}

// --- multi-band blending -------------------------------------------------------------------------------------------------

std::size_t blend_levels(std::size_t w, std::size_t h) {
    std::size_t s = std::min(w, h), n = 1;
    while (s > 16 && n < 7) {
        s /= 2;
        ++n;
    }
    return n;
}

namespace {

std::size_t half_up(std::size_t v) { return (v + 1) / 2; }

// A separable 5-tap binomial, then every other pixel.
std::vector<float> reduce(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& src, std::size_t& nw, std::size_t& nh) {
    nw = half_up(w);
    nh = half_up(h);
    static constexpr std::array<float, 5> k{1.0f, 4.0f, 6.0f, 4.0f, 1.0f};
    std::vector<float> tmp(nw * h * ch, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < nw; ++x) {
            for (std::size_t c = 0; c < ch; ++c) {
                float s = 0.0f;
                for (std::size_t t = 0; t < 5; ++t) {
                    const std::size_t xx = static_cast<std::size_t>(std::clamp<std::int64_t>(std::int64_t(2 * x + t) - 2, 0, std::int64_t(w) - 1));
                    s += k[t] * src[(y * w + xx) * ch + c];
                }
                tmp[(y * nw + x) * ch + c] = s / 16.0f;
            }
        }
    }
    std::vector<float> out(nw * nh * ch, 0.0f);
    for (std::size_t y = 0; y < nh; ++y) {
        for (std::size_t x = 0; x < nw; ++x) {
            for (std::size_t c = 0; c < ch; ++c) {
                float s = 0.0f;
                for (std::size_t t = 0; t < 5; ++t) {
                    const std::size_t yy = static_cast<std::size_t>(std::clamp<std::int64_t>(std::int64_t(2 * y + t) - 2, 0, std::int64_t(h) - 1));
                    s += k[t] * tmp[(yy * nw + x) * ch + c];
                }
                out[(y * nw + x) * ch + c] = s / 16.0f;
            }
        }
    }
    return out;
}

// Bilinear upsampling at half-pixel-aligned places.
std::vector<float> expand(std::size_t sw, std::size_t sh, std::size_t ch, const std::vector<float>& src, std::size_t w, std::size_t h) {
    std::vector<float> out(w * h * ch, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) / 2.0f - 0.5f, 0.0f, static_cast<float>(sh - 1));
        const std::size_t y0 = static_cast<std::size_t>(std::floor(fy));
        const float ty = fy - std::floor(fy);
        const std::size_t y1 = std::min(y0 + 1, sh - 1);
        for (std::size_t x = 0; x < w; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) / 2.0f - 0.5f, 0.0f, static_cast<float>(sw - 1));
            const std::size_t x0 = static_cast<std::size_t>(std::floor(fx));
            const float tx = fx - std::floor(fx);
            const std::size_t x1 = std::min(x0 + 1, sw - 1);
            for (std::size_t c = 0; c < ch; ++c) {
                const auto p = [&](std::size_t xx, std::size_t yy) { return src[(yy * sw + xx) * ch + c]; };
                const float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
                const float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
                out[(y * w + x) * ch + c] = top + (bot - top) * ty;
            }
        }
    }
    return out;
}

// The pixels of no weight filled from around them (push-pull), so the levels draw in no black from outside an image.
void push_pull(std::size_t w, std::size_t h, std::size_t ch, std::vector<float>& px, const std::vector<float>& wt) {
    if (w <= 1 && h <= 1) return;
    const std::size_t nw = half_up(w), nh = half_up(h);
    std::vector<float> cpx(nw * nh * ch, 0.0f), cw(nw * nh, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x, j = (y / 2) * nw + x / 2;
            const float a = wt[i];
            cw[j] += a;
            for (std::size_t c = 0; c < ch; ++c) cpx[j * ch + c] += a * px[i * ch + c];
        }
    }
    bool hole = false, some = false;
    for (std::size_t j = 0; j < nw * nh; ++j) {
        if (cw[j] > 0.0f) {
            for (std::size_t c = 0; c < ch; ++c) cpx[j * ch + c] /= cw[j];
            cw[j] = 1.0f;
            some = true;
        } else {
            hole = true;
        }
    }
    if (hole && some) push_pull(nw, nh, ch, cpx, cw);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            if (wt[i] < 1.0f) {
                const std::size_t j = (y / 2) * nw + x / 2;
                const float a = wt[i];
                for (std::size_t c = 0; c < ch; ++c) px[i * ch + c] = a * px[i * ch + c] + (1.0f - a) * cpx[j * ch + c];
            }
        }
    }
}

}  // namespace

void fill_holes(std::size_t w, std::size_t h, std::size_t ch, std::vector<float>& px, const std::vector<float>& wt) { push_pull(w, h, ch, px, wt); }

std::pair<std::vector<float>, std::vector<float>> multiband(std::size_t w, std::size_t h, const std::vector<RoiImage>& imgs,
                                                            const std::vector<std::vector<float>>& weights, std::size_t levels) {
    const std::size_t ch = imgs.empty() ? 1 : imgs.front().ch;
    levels = std::max<std::size_t>(levels, 1);
    const int align = 1 << (levels - 1);
    std::vector<std::pair<std::size_t, std::size_t>> sizes{{w, h}};
    for (std::size_t l = 1; l < levels; ++l) sizes.emplace_back(half_up(sizes.back().first), half_up(sizes.back().second));
    std::vector<std::vector<float>> acc, wsum;
    for (const auto& [lw, lh] : sizes) {
        acc.emplace_back(lw * lh * ch, 0.0f);
        wsum.emplace_back(lw * lh, 0.0f);
    }
    const int margin = align * 4;
    // (each image over its box grown by a margin and set on the coarsest level's grid)
    for (std::size_t k = 0; k < imgs.size(); ++k) {
        segment::check_stop();
        const RoiImage& im = imgs[k];
        const int gx0 = (std::max(im.x0 - margin, 0) / align) * align;
        const int gy0 = (std::max(im.y0 - margin, 0) / align) * align;
        const int gx1 = std::min(im.x0 + static_cast<int>(im.w) + margin, static_cast<int>(w));
        const int gy1 = std::min(im.y0 + static_cast<int>(im.h) + margin, static_cast<int>(h));
        const std::size_t gw = static_cast<std::size_t>(std::max(gx1 - gx0, 1)), gh = static_cast<std::size_t>(std::max(gy1 - gy0, 1));
        std::vector<float> px(gw * gh * ch, 0.0f), cov(gw * gh, 0.0f), wt(gw * gh, 0.0f);
        for (std::size_t y = 0; y < im.h; ++y) {
            for (std::size_t x = 0; x < im.w; ++x) {
                const int gx = im.x0 + static_cast<int>(x) - gx0, gy = im.y0 + static_cast<int>(y) - gy0;
                if (gx < 0 || gy < 0 || static_cast<std::size_t>(gx) >= gw || static_cast<std::size_t>(gy) >= gh) continue;
                const std::size_t i = y * im.w + x, g = std::size_t(gy) * gw + std::size_t(gx);
                cov[g] = im.alpha[i];
                wt[g] = weights[k][i] * std::min(im.alpha[i], 1.0f);
                for (std::size_t c = 0; c < ch; ++c) px[g * ch + c] = im.px[i * ch + c];
            }
        }
        std::vector<float> bin(gw * gh);
        for (std::size_t i = 0; i < gw * gh; ++i) bin[i] = cov[i] > 0.5f ? 1.0f : 0.0f;
        push_pull(gw, gh, ch, px, bin);
        // (the colours' Laplacian, the weights' Gaussian, each level added in)
        std::size_t cw = gw, chh = gh;
        std::vector<float> cur = std::move(px), cwt = std::move(wt);
        for (std::size_t l = 0; l < levels; ++l) {
            std::vector<float> band;
            std::size_t nw = 0, nh = 0;
            std::vector<float> down;
            if (l + 1 == levels) {
                band = cur;
            } else {
                down = reduce(cw, chh, ch, cur, nw, nh);
                const std::vector<float> up = expand(nw, nh, ch, down, cw, chh);
                band.resize(cur.size());
                for (std::size_t i = 0; i < cur.size(); ++i) band[i] = cur[i] - up[i];
            }
            const std::size_t ox = static_cast<std::size_t>(gx0 >> l), oy = static_cast<std::size_t>(gy0 >> l);
            const auto [pw, ph] = sizes[l];
            for (std::size_t y = 0; y < chh; ++y) {
                const std::size_t py = oy + y;
                if (py >= ph) break;
                for (std::size_t x = 0; x < cw; ++x) {
                    const std::size_t pxx = ox + x;
                    if (pxx >= pw) break;
                    const std::size_t i = y * cw + x, p = py * pw + pxx;
                    const float kk = cwt[i];
                    if (kk <= 0.0f) continue;
                    wsum[l][p] += kk;
                    for (std::size_t c = 0; c < ch; ++c) acc[l][p * ch + c] += kk * band[i * ch + c];
                }
            }
            if (l + 1 == levels) break;
            std::size_t ww = 0, wh = 0;
            std::vector<float> wdown = reduce(cw, chh, 1, cwt, ww, wh);
            cw = nw;
            chh = nh;
            cur = std::move(down);
            cwt = std::move(wdown);
        }
    }
    for (std::size_t l = 0; l < levels; ++l) {
        const auto [pw, ph] = sizes[l];
        for (std::size_t p = 0; p < pw * ph; ++p) {
            const float k = wsum[l][p];
            for (std::size_t c = 0; c < ch; ++c) acc[l][p * ch + c] = k > 1e-8f ? acc[l][p * ch + c] / k : 0.0f;
        }
    }
    // (collapsed)
    std::vector<float> cur = std::move(acc.back());
    for (std::size_t l = levels - 1; l-- > 0;) {
        const auto [pw, ph] = sizes[l];
        const auto [sw, sh] = sizes[l + 1];
        const std::vector<float> up = expand(sw, sh, ch, cur, pw, ph);
        cur.resize(acc[l].size());
        for (std::size_t i = 0; i < acc[l].size(); ++i) cur[i] = acc[l][i] + up[i];
    }
    // (the coverage: the images' union)
    std::vector<float> cov(w * h, 0.0f);
    for (const RoiImage& im : imgs) {
        for (std::size_t y = 0; y < im.h; ++y) {
            for (std::size_t x = 0; x < im.w; ++x) {
                const int px = im.x0 + static_cast<int>(x), py = im.y0 + static_cast<int>(y);
                if (px < 0 || py < 0 || static_cast<std::size_t>(px) >= w || static_cast<std::size_t>(py) >= h) continue;
                const std::size_t p = std::size_t(py) * w + std::size_t(px);
                cov[p] = std::max(cov[p], im.alpha[y * im.w + x]);
            }
        }
    }
    return {std::move(cur), std::move(cov)};
}

}  // namespace genko::render::panorama
