// PC4 節 D6: the geometry of レンズ補正 and 広角補正 (core/lens.hpp) — PhotoCraft v0.2.0's algo/src/lens.rs and
// algo/src/wideangle.rs (with photo_util.rs's SparseLs, the CGLS solver), MIT (native/third_party/photocraft/NOTICE.md).

#include "core/lens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

#include "core/pyops.hpp"

namespace genko::core::lens {

namespace {

double num(const Json& params, const char* key, double fallback) {
    const Json* v = get(params, key);
    if (v == nullptr || !v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) return fallback;
    return v->get<double>();
}

bool flag(const Json& params, const char* key, bool fallback) {
    const Json* v = get(params, key);
    return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
}

std::string text(const Json& params, const char* key, const char* fallback) {
    const Json* v = get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

using V3 = std::array<double, 3>;

double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 norm(const V3& v) {
    const double l = std::max(std::sqrt(dot(v, v)), 1e-300);
    return {v[0] / l, v[1] / l, v[2] / l};
}

// photo_util.rs's SparseLs: rows of a weighted linear least-squares problem, solved by CGLS from a start.
class SparseLs {
public:
    explicit SparseLs(std::size_t n) : n_(n) {}
    void add(std::vector<std::pair<std::size_t, double>> row, double rhs, double w) {
        for (auto& [i, c] : row) c *= w;
        rows_.push_back({std::move(row), rhs * w});
    }
    std::vector<double> solve(const std::vector<double>& x0, std::size_t iters, const std::function<void()>& check) const {
        std::vector<double> x = x0;
        const auto ax = [this](const std::vector<double>& v) {
            std::vector<double> out(rows_.size(), 0.0);
            for (std::size_t r = 0; r < rows_.size(); ++r) {
                double s = 0;
                for (const auto& [i, c] : rows_[r].first) s += c * v[i];
                out[r] = s;
            }
            return out;
        };
        const auto atv = [this](const std::vector<double>& v) {
            std::vector<double> out(n_, 0.0);
            for (std::size_t r = 0; r < rows_.size(); ++r)
                for (const auto& [i, c] : rows_[r].first) out[i] += c * v[r];
            return out;
        };
        std::vector<double> r = ax(x);
        for (std::size_t k = 0; k < r.size(); ++k) r[k] = rows_[k].second - r[k];
        std::vector<double> s = atv(r), p = s;
        double gamma = 0;
        for (const double v : s) gamma += v * v;
        const double g0 = std::max(gamma, 1e-30);
        for (std::size_t it = 0; it < iters; ++it) {
            if (check && it % 20 == 0) check();
            if (gamma <= g0 * 1e-20) break;
            const std::vector<double> q = ax(p);
            double qq = 0;
            for (const double v : q) qq += v * v;
            if (qq <= 1e-300) break;
            const double alpha = gamma / qq;
            for (std::size_t i = 0; i < n_; ++i) x[i] += alpha * p[i];
            for (std::size_t k = 0; k < r.size(); ++k) r[k] -= alpha * q[k];
            s = atv(r);
            double g1 = 0;
            for (const double v : s) g1 += v * v;
            const double beta = g1 / gamma;
            gamma = g1;
            for (std::size_t i = 0; i < n_; ++i) p[i] = s[i] + beta * p[i];
        }
        return x;
    }

private:
    std::size_t n_;
    std::vector<std::pair<std::vector<std::pair<std::size_t, double>>, double>> rows_;
};

}  // namespace

// --- レンズ補正 ------------------------------------------------------------------------------------------------------------

Profile generic_profile(double focal_35mm) {
    // (focal, k1, k2, v1, v2)
    static constexpr std::array<std::array<double, 5>, 7> kTable{{{12.0, -0.16, 0.04, -0.60, 0.15},
                                                                  {16.0, -0.11, 0.025, -0.50, 0.12},
                                                                  {24.0, -0.06, 0.012, -0.38, 0.08},
                                                                  {35.0, -0.025, 0.004, -0.28, 0.05},
                                                                  {50.0, 0.0, 0.0, -0.22, 0.04},
                                                                  {85.0, 0.012, 0.0, -0.16, 0.02},
                                                                  {200.0, 0.02, 0.0, -0.10, 0.01}}};
    const double f = std::clamp(focal_35mm, kTable[0][0], kTable[6][0]);
    std::size_t k = 5;
    for (std::size_t i = 0; i + 1 < kTable.size(); ++i) {
        if (f <= kTable[i + 1][0]) {
            k = i;
            break;
        }
    }
    const auto& a = kTable[k];
    const auto& b = kTable[k + 1];
    const double t = (std::log(f) - std::log(a[0])) / (std::log(b[0]) - std::log(a[0]));
    const auto l = [t](double x, double y) { return x + (y - x) * t; };
    Profile out;
    char name[64];
    std::snprintf(name, sizeof name, "Generic Lens (%.0f mm equivalent)", focal_35mm);
    out.name = name;
    out.k1 = l(a[1], b[1]);
    out.k2 = l(a[2], b[2]);
    out.vignette = {l(a[3], b[3]), l(a[4], b[4]), 0.0};
    return out;
}

Correction correction_of(const Json& params) {
    Correction lc;
    if (text(params, "profile", "none") != "none") {
        const double focal = num(params, "focal_length", 0.0);
        lc.profile = generic_profile(focal > 0 ? focal : 50.0);
    }
    lc.correct_distortion = flag(params, "correct_distortion", true);
    lc.correct_vignette = flag(params, "correct_vignette", true);
    lc.correct_ca = flag(params, "correct_ca", true);
    lc.distortion = std::clamp(num(params, "distortion", 0), -100.0, 100.0);
    lc.red_cyan = std::clamp(num(params, "red_cyan", 0), -100.0, 100.0);
    lc.blue_yellow = std::clamp(num(params, "blue_yellow", 0), -100.0, 100.0);
    lc.vignette_amount = std::clamp(num(params, "vignette_amount", 0), -100.0, 100.0);
    lc.vignette_midpoint = std::clamp(num(params, "vignette_midpoint", 50), 0.0, 100.0);
    lc.vertical = std::clamp(num(params, "vertical", 0), -100.0, 100.0);
    lc.horizontal = std::clamp(num(params, "horizontal", 0), -100.0, 100.0);
    lc.angle = num(params, "angle", 0);
    lc.scale = std::clamp(num(params, "scale", 100), 50.0, 150.0);
    const std::string edge = text(params, "edge", "transparency");
    if (edge == "extension") {
        lc.edge = Edge::Extension;
    } else if (edge == "black" || edge == "white" || edge == "color") {
        lc.edge = Edge::Colour;
        if (edge == "white") lc.edge_colour = {1, 1, 1};
        if (edge == "color") {
            const Json* rgb = get(params, "edge_color");
            if (rgb != nullptr && rgb->is_array() && rgb->size() == 3)
                for (std::size_t c = 0; c < 3; ++c) lc.edge_colour[c] = std::clamp((*rgb)[c].is_number() ? (*rgb)[c].get<double>() : 0.0, 0.0, 255.0) / 255.0;
        }
    }
    lc.auto_scale = flag(params, "auto_scale", false);
    return lc;
}

Map::Map(const Correction& lc, double x0, double y0, double x1, double y1) {
    c_ = {(x0 + x1) / 2.0, (y0 + y1) / 2.0};
    r_ = std::max(std::hypot(x1 - x0, y1 - y0), 2.0) / 2.0;
    const Profile prof = lc.profile.value_or(Profile{});
    const bool d = lc.correct_distortion;
    // (the keystone x' = x / (1 + h x + g y), y' = y / (1 + h x + g y), normalised by r; its inverse's last row −h, −g, 1)
    const double g = -lc.vertical / 100.0 * 0.5, h = -lc.horizontal / 100.0 * 0.5;
    hinv_ = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -h, -g, 1.0};
    const double a = lc.angle * std::numbers::pi / 180.0;
    sin_ = std::sin(a);
    cos_ = std::cos(a);
    k1_ = (d ? prof.k1 : 0.0) - lc.distortion / 100.0 * 0.25;
    k2_ = d ? prof.k2 : 0.0;
    p1_ = d ? prof.p1 : 0.0;
    p2_ = d ? prof.p2 : 0.0;
    ca_r_ = (lc.correct_ca ? prof.ca_red : 0.0) - lc.red_cyan * 4e-5;
    ca_b_ = (lc.correct_ca ? prof.ca_blue : 0.0) - lc.blue_yellow * 4e-5;
    vig_profile_ = lc.correct_vignette ? prof.vignette : std::array<double, 3>{0, 0, 0};
    vig_amount_ = lc.vignette_amount / 100.0;
    vig_mid_ = std::clamp(lc.vignette_midpoint, 0.0, 100.0) / 100.0;
    scale_ = std::clamp(lc.scale / 100.0, 0.05, 20.0);
}

std::optional<Sample> Map::sample(double x, double y) const {
    // (the scale and the angle undone about the centre)
    const double dx = (x - c_[0]) / scale_, dy = (y - c_[1]) / scale_;
    const double rx = cos_ * dx + sin_ * dy, ry = -sin_ * dx + cos_ * dy;
    // (the keystone undone)
    const double nx = rx / r_, ny = ry / r_;
    const auto& m = hinv_;
    const double w = m[6] * nx + m[7] * ny + m[8];
    if (w <= 0.05) return std::nullopt;
    const double ux = nx / w, uy = ny / w;
    // (the lens's distortion: Brown–Conrady)
    const double r2 = ux * ux + uy * uy;
    const double radial = 1.0 + k1_ * r2 + k2_ * r2 * r2;
    const double sx = ux * radial + 2.0 * p1_ * ux * uy + p2_ * (r2 + 2.0 * ux * ux);
    const double sy = uy * radial + p1_ * (r2 + 2.0 * uy * uy) + 2.0 * p2_ * ux * uy;
    const auto to_px = [&](double k) { return XY{c_[0] + sx * k * r_, c_[1] + sy * k * r_}; };
    // (the vignetting, on the source's radius)
    const double rs2 = sx * sx + sy * sy;
    const double falloff = std::max(1.0 + vig_profile_[0] * rs2 + vig_profile_[1] * rs2 * rs2 + vig_profile_[2] * rs2 * rs2 * rs2, 0.05);
    const double start = (1.0 - vig_mid_) * 0.9;
    const double t = std::clamp((std::sqrt(rs2) - start) / (1.0 - start + 1e-9), 0.0, 1.0);
    const double ramp = t * t * (3.0 - 2.0 * t);
    const double gain = std::pow(2.0, vig_amount_ * 1.5 * ramp) / falloff;
    return Sample{to_px(1.0), to_px(1.0 + ca_r_), to_px(1.0 + ca_b_), static_cast<float>(gain)};
}

double auto_scale(const Correction& lc_in, double x0, double y0, double x1, double y1) {
    const auto covers = [&](double scale) {
        Correction lc = lc_in;
        lc.scale = scale;
        const Map m(lc, x0, y0, x1, y1);
        for (int i = 0; i <= 40; ++i) {
            const double t = i / 40.0;
            const XY edges[4] = {{x0 + t * (x1 - x0), y0}, {x0 + t * (x1 - x0), y1}, {x0, y0 + t * (y1 - y0)}, {x1, y0 + t * (y1 - y0)}};
            for (const XY& e : edges) {
                const auto s = m.sample(e[0], e[1]);
                if (!s || !(s->p[0] >= x0 - 0.5 && s->p[0] <= x1 + 0.5 && s->p[1] >= y0 - 0.5 && s->p[1] <= y1 + 0.5)) return false;
            }
        }
        return true;
    };
    if (covers(lc_in.scale)) return lc_in.scale;
    double lo = lc_in.scale, hi = lc_in.scale;
    while (!covers(hi) && hi < 400.0) hi *= 1.25;
    for (int i = 0; i < 30; ++i) {
        const double mid = (lo + hi) / 2.0;
        if (covers(mid)) hi = mid;
        else lo = mid;
    }
    return hi;
}

double straighten_angle(const XY& a, const XY& b) {
    const double deg = std::atan2(b[1] - a[1], b[0] - a[0]) * 180.0 / std::numbers::pi;
    const double target = std::round(deg / 90.0) * 90.0;  // (Rust's round: half away from zero, as std::round)
    return -(deg - target);
}

// --- 広角補正 --------------------------------------------------------------------------------------------------------------

std::string_view to_string(Model model) {
    switch (model) {
        case Model::Auto: return "auto";
        case Model::Fisheye: return "fisheye";
        case Model::Perspective: return "perspective";
        case Model::FullSpherical: return "full_spherical";
    }
    return "auto";
}

Camera Camera::of(const WideAngle& p, double x0, double y0, double x1, double y1) {
    const double w = std::max(x1 - x0, 1.0), h = std::max(y1 - y0, 1.0);
    double f35 = p.focal_length * std::max(p.crop_factor, 0.1);
    Model model = p.model;
    if (model == Model::Auto) {
        if (std::fabs(w / h - 2.0) < 0.01) model = Model::FullSpherical;
        else if (f35 > 0.0 && f35 < 17.0) model = Model::Fisheye;
        else model = Model::Perspective;
    }
    if (!(f35 > 0.0)) f35 = model == Model::Fisheye ? 12.0 : 24.0;
    Camera out;
    out.model = model;
    out.f = f35 / 36.0 * std::max(w, h);
    out.c = {(x0 + x1) / 2.0, (y0 + y1) / 2.0};
    out.size = {w, h};
    return out;
}

std::array<double, 3> Camera::ray(double x, double y) const {
    const double dx = x - c[0], dy = y - c[1];
    switch (model) {
        case Model::FullSpherical: {
            const double lon = dx / size[0] * 2.0 * std::numbers::pi, lat = dy / size[1] * std::numbers::pi;
            return norm({std::sin(lon) * std::cos(lat), std::sin(lat), std::cos(lon) * std::cos(lat)});
        }
        case Model::Fisheye: {
            const double r = std::hypot(dx, dy);
            const double th = r / f;
            if (r < 1e-12) return {0.0, 0.0, 1.0};
            return norm({std::sin(th) * dx / r, std::sin(th) * dy / r, std::cos(th)});
        }
        default: return norm({dx, dy, f});
    }
}

std::optional<XY> Camera::pixel(const std::array<double, 3>& d) const {
    switch (model) {
        case Model::FullSpherical: {
            const double lon = std::atan2(d[0], d[2]), lat = std::asin(std::clamp(d[1], -1.0, 1.0));
            return XY{c[0] + lon / (2.0 * std::numbers::pi) * size[0], c[1] + lat / std::numbers::pi * size[1]};
        }
        case Model::Fisheye: {
            const double th = std::acos(std::clamp(d[2], -1.0, 1.0));
            const double s = std::hypot(d[0], d[1]);
            if (s < 1e-12) return c;
            return XY{c[0] + f * th * d[0] / s, c[1] + f * th * d[1] / s};
        }
        default:
            if (!(d[2] > 1e-6)) return std::nullopt;
            return XY{c[0] + f * d[0] / d[2], c[1] + f * d[1] / d[2]};
    }
}

std::vector<XY> Camera::arc(const XY& a, const XY& b, std::size_t n) const {
    const auto da = ray(a[0], a[1]), db = ray(b[0], b[1]);
    const double omega = std::acos(std::clamp(dot(da, db), -1.0, 1.0));
    std::vector<XY> out;
    for (std::size_t k = 0; k <= n; ++k) {
        const double t = static_cast<double>(k) / static_cast<double>(n);
        std::array<double, 3> d = da;
        if (!(omega < 1e-9)) {
            const double sa = std::sin((1.0 - t) * omega) / std::sin(omega), sb = std::sin(t * omega) / std::sin(omega);
            d = norm({sa * da[0] + sb * db[0], sa * da[1] + sb * db[1], sa * da[2] + sb * db[2]});
        }
        if (const auto p = pixel(d)) out.push_back(*p);
    }
    return out;
}

std::vector<std::array<std::size_t, 3>> Mesh::triangles() const {
    const std::size_t w1 = nx + 1;
    std::vector<std::array<std::size_t, 3>> t;
    t.reserve(nx * ny * 2);
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const std::size_t a = j * w1 + i;
            t.push_back({a, a + 1, a + w1 + 1});
            t.push_back({a, a + w1 + 1, a + w1});
        }
    return t;
}

XY Mesh::map(double x0, double y0, double x1, double y1, const XY& p) const {
    const double cw = (x1 - x0) / static_cast<double>(nx), ch = (y1 - y0) / static_cast<double>(ny);
    const double gx = std::clamp((p[0] - x0) / cw, 0.0, static_cast<double>(nx) - 1e-9);
    const double gy = std::clamp((p[1] - y0) / ch, 0.0, static_cast<double>(ny) - 1e-9);
    const auto i = static_cast<std::size_t>(std::floor(gx)), j = static_cast<std::size_t>(std::floor(gy));
    const double tx = gx - static_cast<double>(i), ty = gy - static_cast<double>(j);
    const std::size_t w1 = nx + 1;
    const auto v = [&](std::size_t ii, std::size_t jj) { return verts[jj * w1 + ii].first; };
    const XY a = v(i, j), b = v(i + 1, j), c = v(i, j + 1), d = v(i + 1, j + 1);
    return {(a[0] * (1.0 - tx) + b[0] * tx) * (1.0 - ty) + (c[0] * (1.0 - tx) + d[0] * tx) * ty,
            (a[1] * (1.0 - tx) + b[1] * tx) * (1.0 - ty) + (c[1] * (1.0 - tx) + d[1] * tx) * ty};
}

Mesh solve(const WideAngle& p, double x0, double y0, double x1, double y1, const std::function<void()>& check) {
    const Camera cam = Camera::of(p, x0, y0, x1, y1);
    const double fw = std::max(x1 - x0, 1.0), fh = std::max(y1 - y0, 1.0);
    const double cell = std::max(fw, fh) / 48.0;
    const auto nx = static_cast<std::size_t>(std::clamp(std::round(fw / cell), 2.0, 96.0));
    const auto ny = static_cast<std::size_t>(std::clamp(std::round(fh / cell), 2.0, 96.0));
    const double cw = fw / static_cast<double>(nx), ch = fh / static_cast<double>(ny);
    const std::size_t w1 = nx + 1, nv = w1 * (ny + 1);
    std::vector<XY> src(nv);
    for (std::size_t k = 0; k < nv; ++k) src[k] = {x0 + static_cast<double>(k % w1) * cw, y0 + static_cast<double>(k / w1) * ch};
    std::vector<std::vector<XY>> curves;
    for (const Constraint& c : p.constraints) curves.push_back(cam.arc(c.a, c.b, 24));
    std::vector<double> x;
    x.reserve(2 * nv);
    for (const XY& s : src) {
        x.push_back(s[0]);
        x.push_back(s[1]);
    }
    // (a source point's bilinear weights in the grid)
    const auto weights = [&](const XY& q) {
        const double gx = std::clamp((q[0] - x0) / cw, 0.0, static_cast<double>(nx) - 1e-9);
        const double gy = std::clamp((q[1] - y0) / ch, 0.0, static_cast<double>(ny) - 1e-9);
        const auto i = static_cast<std::size_t>(std::floor(gx)), j = static_cast<std::size_t>(std::floor(gy));
        const double tx = gx - static_cast<double>(i), ty = gy - static_cast<double>(j);
        const std::size_t a = j * w1 + i;
        return std::array<std::pair<std::size_t, double>, 4>{
            {{a, (1.0 - tx) * (1.0 - ty)}, {a + 1, tx * (1.0 - ty)}, {a + w1, (1.0 - tx) * ty}, {a + w1 + 1, tx * ty}}};
    };
    std::vector<std::array<std::size_t, 3>> tris;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const std::size_t a = j * w1 + i;
            tris.push_back({a, a + 1, a + w1 + 1});
            tris.push_back({a, a + w1 + 1, a + w1});
            // (both diagonals: the shape term symmetric)
            tris.push_back({a, a + 1, a + w1});
            tris.push_back({a + 1, a + w1 + 1, a + w1});
        }
    std::vector<std::size_t> active;
    for (std::size_t k = 0; k < curves.size(); ++k)
        if (curves[k].size() >= 3) active.push_back(k);
    if (!active.empty()) {
        std::vector<XY> normals(active.size(), XY{0.0, 1.0});
        for (int round = 0; round < 4; ++round) {
            SparseLs ls(2 * nv);
            // the shape: v0 = v1 + u (v2 − v1) + v R90 (v2 − v1), (u, v) from the source
            for (const auto& t : tris)
                for (std::size_t rot = 0; rot < 3; ++rot) {
                    const std::size_t i0 = t[rot], i1 = t[(rot + 1) % 3], i2 = t[(rot + 2) % 3];
                    const XY p0 = src[i0], p1 = src[i1], p2 = src[i2];
                    const XY e{p2[0] - p1[0], p2[1] - p1[1]};
                    const double l2 = e[0] * e[0] + e[1] * e[1];
                    const XY d{p0[0] - p1[0], p0[1] - p1[1]};
                    const double u = (d[0] * e[0] + d[1] * e[1]) / l2;
                    const double v = (d[0] * -e[1] + d[1] * e[0]) / l2;
                    ls.add({{2 * i0, 1.0}, {2 * i1, -1.0 + u}, {2 * i2, -u}, {2 * i2 + 1, v}, {2 * i1 + 1, -v}}, 0.0, 1.0);
                    ls.add({{2 * i0 + 1, 1.0}, {2 * i1 + 1, -1.0 + u}, {2 * i2 + 1, -u}, {2 * i2, -v}, {2 * i1, v}}, 0.0, 1.0);
                }
            // a weak anchor to the source (fixes the whole's similarity)
            for (std::size_t k = 0; k < nv; ++k) {
                ls.add({{2 * k, 1.0}}, src[k][0], 0.02);
                ls.add({{2 * k + 1, 1.0}}, src[k][1], 0.02);
            }
            // the lines
            for (std::size_t ci = 0; ci < active.size(); ++ci) {
                const std::vector<XY>& c = curves[active[ci]];
                XY n = normals[ci];
                switch (p.constraints[active[ci]].orientation) {
                    case Orientation::Horizontal: n = {0.0, 1.0}; break;
                    case Orientation::Vertical: n = {1.0, 0.0}; break;
                    case Orientation::Free: break;
                }
                const auto w0 = weights(c[0]);
                for (std::size_t k = 1; k < c.size(); ++k) {
                    const auto wq = weights(c[k]);
                    std::vector<std::pair<std::size_t, double>> row;
                    row.reserve(16);
                    for (const auto& [i, w] : wq) {
                        row.emplace_back(2 * i, n[0] * w);
                        row.emplace_back(2 * i + 1, n[1] * w);
                    }
                    for (const auto& [i, w] : w0) {
                        row.emplace_back(2 * i, -n[0] * w);
                        row.emplace_back(2 * i + 1, -n[1] * w);
                    }
                    ls.add(std::move(row), 0.0, 10.0);
                }
            }
            x = ls.solve(x, static_cast<std::size_t>(400 + 200 * round), check);
            // the free normals again, from the ends as they are now
            const auto pos = [&](const XY& q) {
                XY acc{0.0, 0.0};
                for (const auto& [i, w] : weights(q)) {
                    acc[0] += w * x[2 * i];
                    acc[1] += w * x[2 * i + 1];
                }
                return acc;
            };
            bool changed = false;
            for (std::size_t ci = 0; ci < active.size(); ++ci) {
                const std::vector<XY>& c = curves[active[ci]];
                const XY a = pos(c.front()), b = pos(c.back());
                const XY d{b[0] - a[0], b[1] - a[1]};
                const double l = std::max(std::hypot(d[0], d[1]), 1e-9);
                const XY nn{-d[1] / l, d[0] / l};
                if (std::fabs(nn[0] - normals[ci][0]) + std::fabs(nn[1] - normals[ci][1]) > 1e-3) changed = true;
                normals[ci] = nn;
            }
            if (round == 0 && std::all_of(active.begin(), active.end(), [&](std::size_t k) { return p.constraints[k].orientation != Orientation::Free; }))
                break;
            if (round > 0 && !changed) break;
        }
    }
    // the scale about the centre
    const double s = std::clamp(p.scale / 100.0, 0.1, 10.0);
    Mesh mesh;
    mesh.nx = nx;
    mesh.ny = ny;
    mesh.verts.reserve(nv);
    for (std::size_t k = 0; k < nv; ++k)
        mesh.verts.push_back({XY{cam.c[0] + (x[2 * k] - cam.c[0]) * s, cam.c[1] + (x[2 * k + 1] - cam.c[1]) * s}, src[k]});
    mesh.curves = std::move(curves);
    // (straightness: each curve's mapped samples' distance from the chord of its mapped ends)
    double worst = 0;
    for (const std::vector<XY>& cv : mesh.curves) {
        if (cv.size() < 3) continue;
        std::vector<XY> pts;
        for (const XY& q : cv) pts.push_back(mesh.map(x0, y0, x1, y1, q));
        const XY a = pts.front(), b = pts.back();
        const XY d{b[0] - a[0], b[1] - a[1]};
        const double l = std::max(std::hypot(d[0], d[1]), 1e-9);
        for (const XY& q : pts) worst = std::max(worst, std::fabs((q[0] - a[0]) * d[1] - (q[1] - a[1]) * d[0]) / l);
    }
    mesh.residual = worst;
    return mesh;
}

}  // namespace genko::core::lens
