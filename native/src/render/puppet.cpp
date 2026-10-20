#include "render/puppet.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>

// PhotoCraft v0.2.0 crates/algo/src/puppet.rs (1-771, the tests from 773), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::puppet {

namespace {

constexpr float kAlphaMin = 0.05f;  // (a cell's alpha above this is part of the puppet)
constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

double cells_of(Density d) {
    switch (d) {
        case Density::Fewer: return 12.0;
        case Density::Normal: return 20.0;
        case Density::More: return 32.0;
    }
    return 20.0;
}

double dist2(const XY& a, const XY& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }

// The cotangent of the angle at a.
double cot(const XY& a, const XY& b, const XY& c) {
    const XY u{b[0] - a[0], b[1] - a[1]}, v{c[0] - a[0], c[1] - a[1]};
    const double cross = std::fabs(u[0] * v[1] - u[1] * v[0]);
    return cross < 1e-12 ? 0.0 : (u[0] * v[0] + u[1] * v[1]) / cross;
}

// A symmetric matrix in compressed rows (the conjugate gradients').
struct Csr {
    std::vector<std::size_t> rows;
    std::vector<std::size_t> cols;
    std::vector<double> vals;
    std::vector<double> diag;

    static Csr from_triplets(std::size_t n, std::vector<std::tuple<std::size_t, std::size_t, double>> t) {
        std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b)); });
        Csr m;
        m.rows.assign(n + 1, 0);
        std::size_t li = kNone, lj = kNone;
        for (const auto& [i, j, v] : t) {
            if (i == li && j == lj) {
                m.vals.back() += v;
                continue;
            }
            li = i;
            lj = j;
            m.cols.push_back(j);
            m.vals.push_back(v);
            m.rows[i + 1] = m.cols.size();
        }
        for (std::size_t i = 0; i < n; ++i) m.rows[i + 1] = std::max(m.rows[i + 1], m.rows[i]);
        m.diag.assign(n, 1.0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = m.rows[i]; k < m.rows[i + 1]; ++k)
                if (m.cols[k] == i && m.vals[k] > 0.0) m.diag[i] = m.vals[k];
        }
        return m;
    }

    void mul(const std::vector<double>& x, std::vector<double>& out) const {
        for (std::size_t i = 0; i + 1 < rows.size(); ++i) {
            double s = 0.0;
            for (std::size_t k = rows[i]; k < rows[i + 1]; ++k) s += vals[k] * x[cols[k]];
            out[i] = s;
        }
    }

    // Jacobi-preconditioned conjugate gradients, warm-started from x.
    void solve(const std::vector<double>& b, std::vector<double>& x) const {
        const std::size_t n = b.size();
        std::vector<double> r(n, 0.0), ap(n, 0.0);
        mul(x, r);
        for (std::size_t i = 0; i < n; ++i) r[i] = b[i] - r[i];
        double bn = 0.0;
        for (const double v : b) bn += v * v;
        bn = std::max(std::sqrt(bn), 1e-30);
        std::vector<double> z(n);
        for (std::size_t i = 0; i < n; ++i) z[i] = r[i] / diag[i];
        std::vector<double> p = z;
        double rz = 0.0;
        for (std::size_t i = 0; i < n; ++i) rz += r[i] * z[i];
        const std::size_t steps = std::clamp<std::size_t>(4 * n, 50, 2000);
        for (std::size_t it = 0; it < steps; ++it) {
            double rn = 0.0;
            for (const double v : r) rn += v * v;
            if (std::sqrt(rn) <= 1e-12 * bn) break;
            mul(p, ap);
            double pap = 0.0;
            for (std::size_t i = 0; i < n; ++i) pap += p[i] * ap[i];
            if (std::fabs(pap) < 1e-300) break;
            const double alpha = rz / pap;
            for (std::size_t i = 0; i < n; ++i) {
                x[i] += alpha * p[i];
                r[i] -= alpha * ap[i];
            }
            for (std::size_t i = 0; i < n; ++i) z[i] = r[i] / diag[i];
            double rz2 = 0.0;
            for (std::size_t i = 0; i < n; ++i) rz2 += r[i] * z[i];
            const double beta = rz2 / rz;
            rz = rz2;
            for (std::size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
        }
    }
};

// A banded Cholesky factor A = L·Lᵀ (a grid mesh numbers its vertices row by row, so the band is about two rows): made
// once per set of pins, each global step then two triangular solves.
struct BandChol {
    std::size_t n = 0, p = 0;
    std::vector<double> l;  // (row i holds L[i][i − p ..= i])

    static std::optional<BandChol> make(const Csr& a) {
        const std::size_t n = a.diag.size();
        std::size_t p = 0;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t k = a.rows[i]; k < a.rows[i + 1]; ++k) p = std::max(p, i > a.cols[k] ? i - a.cols[k] : a.cols[k] - i);
        if (n * (p + 1) > 40'000'000) return std::nullopt;
        const std::size_t w = p + 1;
        BandChol c;
        c.n = n;
        c.p = p;
        c.l.assign(n * w, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = a.rows[i]; k < a.rows[i + 1]; ++k) {
                const std::size_t j = a.cols[k];
                if (j <= i) c.l[i * w + (j + p - i)] = a.vals[k];
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t lo = i > p ? i - p : 0;
            for (std::size_t j = lo; j <= i; ++j) {
                const std::size_t jlo = std::max(j > p ? j - p : 0, lo);
                double s = c.l[i * w + (j + p - i)];
                for (std::size_t k = jlo; k < j; ++k) s -= c.l[i * w + (k + p - i)] * c.l[j * w + (k + p - j)];
                if (i == j) {
                    if (s <= 0.0 || !std::isfinite(s)) return std::nullopt;
                    c.l[i * w + p] = std::sqrt(s);
                } else {
                    c.l[i * w + (j + p - i)] = s / c.l[j * w + p];
                }
            }
        }
        return c;
    }

    void solve(const std::vector<double>& b, std::vector<double>& x) const {
        const std::size_t w = p + 1;
        std::vector<double> y = b;
        for (std::size_t i = 0; i < n; ++i) {
            double s = y[i];
            for (std::size_t k = i > p ? i - p : 0; k < i; ++k) s -= l[i * w + (k + p - i)] * y[k];
            y[i] = s / l[i * w + p];
        }
        for (std::size_t i = n; i-- > 0;) {
            double s = y[i];
            for (std::size_t k = i + 1; k < std::min(i + p + 1, n); ++k) s -= l[k * w + (i + p - k)] * x[k];
            x[i] = s / l[i * w + p];
        }
    }
};

// A pin bound to the mesh: barycentric weights on a triangle's corners (off the mesh: the nearest vertex, weight 1).
struct Bound {
    std::array<std::size_t, 3> verts{0, 0, 0};
    std::array<double, 3> bary{1, 0, 0};
};

Bound bind(const Mesh& mesh, const XY& p) {
    if (const auto hit = mesh.locate(p[0], p[1])) return Bound{mesh.tris[hit->first], hit->second};
    std::size_t best = 0;
    for (std::size_t i = 1; i < mesh.verts.size(); ++i)
        if (dist2(mesh.verts[i], p) < dist2(mesh.verts[best], p)) best = i;
    return Bound{{best, best, best}, {1.0, 0.0, 0.0}};
}

std::optional<std::array<double, 3>> barycentric(const XY& a, const XY& b, const XY& c, const XY& p) {
    const double det = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (std::fabs(det) < 1e-12) return std::nullopt;
    const double l1 = ((p[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (p[1] - a[1])) / det;
    const double l2 = ((b[0] - a[0]) * (p[1] - a[1]) - (p[0] - a[0]) * (b[1] - a[1])) / det;
    const double l0 = 1.0 - l1 - l2;
    if (l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9) return std::array<double, 3>{l0, l1, l2};
    return std::nullopt;
}

}  // namespace

std::optional<Mode> mode_from(std::string_view text) {
    if (text == "rigid") return Mode::Rigid;
    if (text == "normal") return Mode::Normal;
    if (text == "distort") return Mode::Distort;
    return std::nullopt;
}

std::optional<Density> density_from(std::string_view text) {
    if (text == "fewer") return Density::Fewer;
    if (text == "normal") return Density::Normal;
    if (text == "more") return Density::More;
    return std::nullopt;
}

std::string_view to_string(Mode mode) {
    switch (mode) {
        case Mode::Rigid: return "rigid";
        case Mode::Normal: return "normal";
        case Mode::Distort: return "distort";
    }
    return "normal";
}

std::string_view to_string(Density density) {
    switch (density) {
        case Density::Fewer: return "fewer";
        case Density::Normal: return "normal";
        case Density::More: return "more";
    }
    return "normal";
}

bool Warp::is_identity() const {
    return std::all_of(pins.begin(), pins.end(), [](const Pin& p) {
        const bool turned = p.rotate && std::fmod(std::fmod(*p.rotate, 360.0) + 360.0, 360.0) != 0.0;
        return p.src == p.dst && !turned;
    });
}

std::optional<std::pair<std::size_t, std::array<double, 3>>> Mesh::locate(double x, double y) const {
    if (tris.empty() || spacing <= 0) return std::nullopt;
    const double gx = (x - ox) / spacing, gy = (y - oy) / spacing;
    if (!std::isfinite(gx) || !std::isfinite(gy)) return std::nullopt;
    const long ci = static_cast<long>(std::floor(gx)), cj = static_cast<long>(std::floor(gy));
    // (the cell under the point first, then its neighbours: a point on a cell's edge may lie in the next one's triangle)
    for (const long dj : {0L, -1L, 1L}) {
        for (const long di : {0L, -1L, 1L}) {
            const long i = ci + di, j = cj + dj;
            if (i < 0 || j < 0 || i >= static_cast<long>(cols) || j >= static_cast<long>(rows)) continue;
            for (const std::size_t t : cell[static_cast<std::size_t>(j) * cols + static_cast<std::size_t>(i)]) {
                if (t == kNone) continue;
                const auto& tri = tris[t];
                if (const auto w = barycentric(verts[tri[0]], verts[tri[1]], verts[tri[2]], XY{x, y})) return std::make_pair(t, *w);
            }
        }
    }
    return std::nullopt;
}

Mesh build_mesh(const Coverage& cov, Density density, double expansion) {
    Mesh mesh;
    if (cov.width <= 0 || cov.height <= 0) return mesh;
    const double longer = std::max(cov.width, cov.height);
    const double spacing = std::max(longer / cells_of(density), 2.0);
    const double e = std::isfinite(expansion) ? std::clamp(expansion, -200.0, 200.0) : 0.0;
    // (covered on a grid of blocks, a quarter cell or less)
    const int g = std::max(static_cast<int>(std::floor(spacing / 4.0)), 1);
    const int pad = static_cast<int>(std::ceil(std::max(e, 0.0))) + g;
    const int ax0 = cov.x0 - pad, ay0 = cov.y0 - pad;
    const int aw = cov.width + 2 * pad, ah = cov.height + 2 * pad;
    const std::size_t bw = static_cast<std::size_t>((aw + g - 1) / g), bh = static_cast<std::size_t>((ah + g - 1) / g);
    std::vector<char> blocks(bw * bh, 0);
    for (int y = 0; y < cov.height; ++y) {
        const std::size_t by = static_cast<std::size_t>((cov.y0 + y - ay0) / g);
        for (int x = 0; x < cov.width; ++x) {
            if (cov.alpha[std::size_t(y) * std::size_t(cov.width) + std::size_t(x)] > kAlphaMin) {
                blocks[by * bw + static_cast<std::size_t>((cov.x0 + x - ax0) / g)] = 1;
            }
        }
    }
    // (grown or shrunk by the expansion: a round stamp in blocks)
    const int eb = static_cast<int>(std::round(std::fabs(e) / g));
    if (eb > 0) {
        const std::vector<char> was = blocks;
        const bool grow = e > 0.0;
        for (int by = 0; by < static_cast<int>(bh); ++by) {
            for (int bx = 0; bx < static_cast<int>(bw); ++bx) {
                bool hit = !grow;
                for (int dy = -eb; dy <= eb && hit != grow; ++dy) {
                    for (int dx = -eb; dx <= eb; ++dx) {
                        if (dx * dx + dy * dy > eb * eb) continue;
                        const int x = bx + dx, y = by + dy;
                        const bool v = x >= 0 && y >= 0 && x < static_cast<int>(bw) && y < static_cast<int>(bh) && was[std::size_t(y) * bw + std::size_t(x)];
                        if (grow && v) {
                            hit = true;
                            break;
                        }
                        if (!grow && !v) {
                            hit = false;
                            break;
                        }
                    }
                }
                blocks[std::size_t(by) * bw + std::size_t(bx)] = hit ? 1 : 0;
            }
        }
    }
    // (the grid's cells that touch covered blocks)
    mesh.spacing = spacing;
    mesh.ox = ax0;
    mesh.oy = ay0;
    const std::size_t cw = static_cast<std::size_t>(std::ceil(static_cast<double>(bw) * g / spacing));
    const std::size_t ch = static_cast<std::size_t>(std::ceil(static_cast<double>(bh) * g / spacing));
    mesh.cols = cw;
    mesh.rows = ch;
    mesh.cell.assign(cw * ch, {kNone, kNone});
    std::vector<std::size_t> index((cw + 1) * (ch + 1), kNone);
    const auto vertex = [&](std::size_t i, std::size_t j) {
        const std::size_t k = j * (cw + 1) + i;
        if (index[k] == kNone) {
            index[k] = mesh.verts.size();
            mesh.verts.push_back(XY{ax0 + static_cast<double>(i) * spacing, ay0 + static_cast<double>(j) * spacing});
        }
        return index[k];
    };
    for (std::size_t cj = 0; cj < ch; ++cj) {
        for (std::size_t ci = 0; ci < cw; ++ci) {
            const std::size_t bx0 = static_cast<std::size_t>(std::floor(static_cast<double>(ci) * spacing / g));
            const std::size_t by0 = static_cast<std::size_t>(std::floor(static_cast<double>(cj) * spacing / g));
            const std::size_t bx1 = static_cast<std::size_t>(std::min(std::ceil(static_cast<double>(ci + 1) * spacing / g), static_cast<double>(bw)));
            const std::size_t by1 = static_cast<std::size_t>(std::min(std::ceil(static_cast<double>(cj + 1) * spacing / g), static_cast<double>(bh)));
            bool any = false;
            for (std::size_t y = by0; y < by1 && !any; ++y)
                for (std::size_t x = bx0; x < bx1 && !any; ++x) any = blocks[y * bw + x] != 0;
            if (!any) continue;
            const std::size_t a = vertex(ci, cj), b = vertex(ci + 1, cj), c = vertex(ci + 1, cj + 1), d = vertex(ci, cj + 1);
            const std::size_t t = mesh.tris.size();
            if ((ci + cj) % 2 == 0) {
                mesh.tris.push_back({a, b, c});
                mesh.tris.push_back({a, c, d});
            } else {
                mesh.tris.push_back({a, b, d});
                mesh.tris.push_back({b, c, d});
            }
            mesh.cell[cj * cw + ci] = {t, t + 1};
        }
    }
    return mesh;
}

// --- the solver -------------------------------------------------------------------------------------------------------

struct Solver::Impl {
    Mesh mesh;
    std::vector<std::array<double, 3>> w;  // (each triangle's edge weights: the edge opposite corner k)
    std::vector<Bound> bound;
    std::vector<std::vector<std::size_t>> pin_tris;  // (the triangles a turned pin fixes)
    std::vector<bool> anchored;                      // (vertices of a part without a pin: held where they are)
    Csr matrix;
    std::optional<BandChol> chol;
    double pin_weight = 1.0;

    double energy_with(const std::vector<XY>& v, const std::vector<std::array<double, 4>>& rots, const std::vector<XY>& pin_dst) const {
        const std::vector<XY>& x = mesh.verts;
        double e = 0.0;
        for (std::size_t t = 0; t < mesh.tris.size(); ++t) {
            const auto& tri = mesh.tris[t];
            const auto& r = rots[t];
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t i = tri[(k + 1) % 3], j = tri[(k + 2) % 3];
                const XY d{v[i][0] - v[j][0], v[i][1] - v[j][1]};
                const XY ex{x[i][0] - x[j][0], x[i][1] - x[j][1]};
                const XY re{r[0] * ex[0] + r[1] * ex[1], r[2] * ex[0] + r[3] * ex[1]};
                e += w[t][k] * ((d[0] - re[0]) * (d[0] - re[0]) + (d[1] - re[1]) * (d[1] - re[1]));
            }
        }
        for (std::size_t k = 0; k < bound.size() && k < pin_dst.size(); ++k) e += pin_weight * dist2(point(bound[k], v), pin_dst[k]);
        return e;
    }

    static XY point(const Bound& b, const std::vector<XY>& v) {
        XY p{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            p[0] += b.bary[k] * v[b.verts[k]][0];
            p[1] += b.bary[k] * v[b.verts[k]][1];
        }
        return p;
    }

    std::vector<std::array<double, 4>> local(const std::vector<XY>& v, Mode mode, const std::vector<std::optional<double>>& fixed) const {
        const std::vector<XY>& x = mesh.verts;
        std::vector<std::array<double, 4>> rots(mesh.tris.size());
        for (std::size_t t = 0; t < mesh.tris.size(); ++t) {
            const auto& tri = mesh.tris[t];
            std::array<double, 4> s{0, 0, 0, 0};
            double norm = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t i = tri[(k + 1) % 3], j = tri[(k + 2) % 3];
                const XY d{v[i][0] - v[j][0], v[i][1] - v[j][1]};
                const XY e{x[i][0] - x[j][0], x[i][1] - x[j][1]};
                s[0] += w[t][k] * d[0] * e[0];
                s[1] += w[t][k] * d[0] * e[1];
                s[2] += w[t][k] * d[1] * e[0];
                s[3] += w[t][k] * d[1] * e[1];
                norm += w[t][k] * (e[0] * e[0] + e[1] * e[1]);
            }
            const double c = s[0] + s[3], sn = s[2] - s[1];
            const double th = std::atan2(sn, c);
            double scale = 1.0;
            if (mode == Mode::Normal) scale = std::sqrt(std::hypot(c, sn) / std::max(norm, 1e-12));
            if (mode == Mode::Distort) scale = std::hypot(c, sn) / std::max(norm, 1e-12);
            if (!(std::isfinite(scale) && scale > 0.0)) scale = 1.0;
            const double sth = std::sin(th), cth = std::cos(th);
            rots[t] = {scale * cth, -scale * sth, scale * sth, scale * cth};
        }
        for (std::size_t k = 0; k < pin_tris.size(); ++k) {
            if (k >= fixed.size() || !fixed[k]) continue;
            const double rad = *fixed[k] * std::numbers::pi / 180.0;
            const double s = std::sin(rad), c = std::cos(rad);
            for (const std::size_t t : pin_tris[k]) rots[t] = {c, -s, s, c};
        }
        return rots;
    }

    void global(const std::vector<std::array<double, 4>>& rots, const std::vector<XY>& pin_dst, std::vector<XY>& v) const {
        const std::size_t nv = v.size();
        const std::vector<XY>& x = mesh.verts;
        std::vector<double> bx(nv, 0.0), by(nv, 0.0);
        for (std::size_t t = 0; t < mesh.tris.size(); ++t) {
            const auto& tri = mesh.tris[t];
            const auto& r = rots[t];
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t i = tri[(k + 1) % 3], j = tri[(k + 2) % 3];
                const XY e{x[i][0] - x[j][0], x[i][1] - x[j][1]};
                const XY re{r[0] * e[0] + r[1] * e[1], r[2] * e[0] + r[3] * e[1]};
                bx[i] += w[t][k] * re[0];
                by[i] += w[t][k] * re[1];
                bx[j] -= w[t][k] * re[0];
                by[j] -= w[t][k] * re[1];
            }
        }
        for (std::size_t p = 0; p < bound.size() && p < pin_dst.size(); ++p) {
            for (std::size_t k = 0; k < 3; ++k) {
                bx[bound[p].verts[k]] += pin_weight * bound[p].bary[k] * pin_dst[p][0];
                by[bound[p].verts[k]] += pin_weight * bound[p].bary[k] * pin_dst[p][1];
            }
        }
        for (std::size_t i = 0; i < nv; ++i) {
            if (!anchored[i]) continue;
            bx[i] += pin_weight * x[i][0];
            by[i] += pin_weight * x[i][1];
        }
        std::vector<double> sx(nv), sy(nv);
        for (std::size_t i = 0; i < nv; ++i) {
            sx[i] = v[i][0];
            sy[i] = v[i][1];
        }
        if (chol) {
            chol->solve(bx, sx);
            chol->solve(by, sy);
        } else {
            matrix.solve(bx, sx);
            matrix.solve(by, sy);
        }
        for (std::size_t i = 0; i < nv; ++i) v[i] = XY{sx[i], sy[i]};
    }
};

Solver::Solver(Mesh mesh, const std::vector<XY>& pin_src, const std::vector<bool>& fixed_rotation) : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    s.mesh = std::move(mesh);
    const Mesh& m = s.mesh;
    const std::size_t nv = m.verts.size();
    s.w.reserve(m.tris.size());
    for (const auto& t : m.tris) {
        const XY &a = m.verts[t[0]], &b = m.verts[t[1]], &c = m.verts[t[2]];
        s.w.push_back({0.5 * cot(a, b, c), 0.5 * cot(b, c, a), 0.5 * cot(c, a, b)});
    }
    for (const XY& p : pin_src) s.bound.push_back(nv == 0 ? Bound{} : bind(m, p));
    // (the mesh's parts: union-find over the triangles' edges)
    std::vector<std::size_t> parent(nv);
    for (std::size_t i = 0; i < nv; ++i) parent[i] = i;
    const auto find = [&parent](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (const auto& t : m.tris) {
        for (std::size_t k = 1; k < 3; ++k) {
            const std::size_t a = find(t[0]), b = find(t[k]);
            if (a != b) parent[a] = b;
        }
    }
    std::vector<bool> pinned_root(nv, false);
    if (nv > 0) {
        for (const Bound& b : s.bound)
            for (const std::size_t v : b.verts) pinned_root[find(v)] = true;
    }
    s.anchored.assign(nv, false);
    for (std::size_t v = 0; v < nv; ++v) s.anchored[v] = !pinned_root[find(v)];
    // (a turned pin: the triangles around its nearest vertex)
    for (std::size_t k = 0; k < s.bound.size(); ++k) {
        std::vector<std::size_t> tris;
        if (k < fixed_rotation.size() && fixed_rotation[k] && nv > 0) {
            const Bound& b = s.bound[k];
            std::size_t v = b.verts[0];
            for (const std::size_t c : b.verts)
                if (dist2(m.verts[c], pin_src[k]) < dist2(m.verts[v], pin_src[k])) v = c;
            for (std::size_t t = 0; t < m.tris.size(); ++t)
                if (std::find(m.tris[t].begin(), m.tris[t].end(), v) != m.tris[t].end()) tris.push_back(t);
        }
        s.pin_tris.push_back(std::move(tris));
    }
    std::vector<std::tuple<std::size_t, std::size_t, double>> trip;
    trip.reserve(m.tris.size() * 12 + nv);
    double diag_sum = 0.0;
    for (std::size_t t = 0; t < m.tris.size(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t i = m.tris[t][(k + 1) % 3], j = m.tris[t][(k + 2) % 3];
            const double wk = s.w[t][k];
            trip.emplace_back(i, i, wk);
            trip.emplace_back(j, j, wk);
            trip.emplace_back(i, j, -wk);
            trip.emplace_back(j, i, -wk);
            diag_sum += 2.0 * wk;
        }
    }
    s.pin_weight = 1e4 * std::max(diag_sum / static_cast<double>(std::max<std::size_t>(nv, 1)), 1.0);
    if (nv > 0) {
        for (const Bound& b : s.bound)
            for (std::size_t a = 0; a < 3; ++a)
                for (std::size_t c = 0; c < 3; ++c) trip.emplace_back(b.verts[a], b.verts[c], s.pin_weight * b.bary[a] * b.bary[c]);
    }
    for (std::size_t v = 0; v < nv; ++v)
        if (s.anchored[v]) trip.emplace_back(v, v, s.pin_weight);
    s.matrix = Csr::from_triplets(nv, std::move(trip));
    s.chol = BandChol::make(s.matrix);
}

Solver::~Solver() = default;

const Mesh& Solver::mesh() const { return impl_->mesh; }

std::pair<std::vector<XY>, std::vector<double>> Solver::solve(const std::vector<XY>& pin_dst, const std::vector<std::optional<double>>& rotations,
                                                              Mode mode, int iterations) const {
    const Impl& s = *impl_;
    const std::size_t nv = s.mesh.verts.size();
    std::vector<XY> v = s.mesh.verts;
    std::vector<double> energies;
    if (nv == 0) return {v, energies};
    // (starting from R = I, or with turned pins each triangle turned by their inverse-distance weighted angle: ARAP alone
    // carries a turn only slowly)
    std::vector<std::pair<XY, double>> turned;
    for (std::size_t k = 0; k < s.bound.size() && k < rotations.size(); ++k) {
        if (rotations[k]) turned.emplace_back(Impl::point(s.bound[k], s.mesh.verts), *rotations[k] * std::numbers::pi / 180.0);
    }
    std::vector<std::array<double, 4>> r(s.mesh.tris.size(), {1.0, 0.0, 0.0, 1.0});
    if (!turned.empty()) {
        for (std::size_t t = 0; t < s.mesh.tris.size(); ++t) {
            XY c{0, 0};
            for (const std::size_t i : s.mesh.tris[t]) {
                c[0] += s.mesh.verts[i][0] / 3.0;
                c[1] += s.mesh.verts[i][1] / 3.0;
            }
            double sw = 0.0, sa = 0.0;
            for (const auto& [p, a] : turned) {
                const double wt = 1.0 / (dist2(p, c) + 1.0);
                sw += wt;
                sa += wt * a;
            }
            const double sn = std::sin(sa / sw), co = std::cos(sa / sw);
            r[t] = {co, -sn, sn, co};
        }
    }
    s.global(r, pin_dst, v);
    for (int it = 0; it < iterations; ++it) {
        const auto rots = s.local(v, mode, rotations);
        s.global(rots, pin_dst, v);
        const auto rigid = s.local(v, Mode::Rigid, rotations);
        energies.push_back(s.energy_with(v, rigid, pin_dst));
    }
    return {v, energies};
}

double Solver::energy(const std::vector<XY>& v, const std::vector<XY>& pin_dst, const std::vector<std::optional<double>>& rotations) const {
    return impl_->energy_with(v, impl_->local(v, Mode::Rigid, rotations), pin_dst);
}

std::vector<XY> Solver::pinned(const std::vector<XY>& v) const {
    std::vector<XY> out;
    for (const Bound& b : impl_->bound) out.push_back(v.empty() ? XY{0, 0} : Impl::point(b, v));
    return out;
}

std::vector<std::size_t> Solver::draw_order(const std::vector<int>& depths, const std::vector<XY>& pin_src) const {
    const Mesh& m = impl_->mesh;
    std::vector<std::size_t> order(m.tris.size());
    for (std::size_t t = 0; t < order.size(); ++t) order[t] = t;
    if (pin_src.empty() || std::all_of(depths.begin(), depths.end(), [](int d) { return d == 0; })) return order;
    std::vector<int> depth(m.tris.size(), 0);
    for (std::size_t t = 0; t < m.tris.size(); ++t) {
        XY c{0, 0};
        for (const std::size_t i : m.tris[t]) {
            c[0] += m.verts[i][0] / 3.0;
            c[1] += m.verts[i][1] / 3.0;
        }
        std::size_t k = 0;
        for (std::size_t p = 1; p < pin_src.size(); ++p)
            if (dist2(pin_src[p], c) < dist2(pin_src[k], c)) k = p;
        depth[t] = k < depths.size() ? depths[k] : 0;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });
    return order;
}

XY Deformed::map(double x, double y) const {
    if (mesh.verts.empty() || verts.size() != mesh.verts.size()) return XY{x, y};
    if (const auto hit = mesh.locate(x, y)) {
        XY out{0, 0};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t i = mesh.tris[hit->first][k];
            out[0] += hit->second[k] * verts[i][0];
            out[1] += hit->second[k] * verts[i][1];
        }
        return out;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < mesh.verts.size(); ++i)
        if (dist2(mesh.verts[i], XY{x, y}) < dist2(mesh.verts[best], XY{x, y})) best = i;
    return XY{x + verts[best][0] - mesh.verts[best][0], y + verts[best][1] - mesh.verts[best][1]};
}

Deformed deform(const Coverage& coverage, const Warp& warp, int iterations) {
    Mesh mesh = build_mesh(coverage, warp.density, warp.expansion);
    std::vector<XY> src, dst;
    std::vector<bool> fixed;
    std::vector<std::optional<double>> rot;
    std::vector<int> depths;
    for (const Pin& p : warp.pins) {
        src.push_back(p.src);
        dst.push_back(p.dst);
        fixed.push_back(p.rotate.has_value());
        rot.push_back(p.rotate);
        depths.push_back(p.depth);
    }
    const Solver solver(std::move(mesh), src, fixed);
    Deformed out;
    out.verts = solver.solve(dst, rot, warp.mode, iterations).first;
    out.order = solver.draw_order(depths, src);
    out.mesh = solver.mesh();
    return out;
}

}  // namespace genko::render::puppet
