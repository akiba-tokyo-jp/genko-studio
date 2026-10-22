#include "render/perspective_warp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

// PhotoCraft v0.2.0 crates/algo/src/perspective.rs (1-322 and its tests), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::perspective {

namespace {

std::optional<resample::Homography> unit_to_quad(const Quad& q) {
    return resample::Homography::rect_to_quad({0.0, 0.0, 1.0, 1.0}, {{{q[0][0], q[0][1]}, {q[1][0], q[1][1]}, {q[2][0], q[2][1]}, {q[3][0], q[3][1]}}});
}

bool near(const XY& a, const XY& b, double link) { return std::fabs(a[0] - b[0]) <= link && std::fabs(a[1] - b[1]) <= link; }

// Edge k of a plane runs from corner k to corner k + 1: the point on it at (u, v)'s place, and the weight of its
// correction there.
std::pair<XY, double> edge_uv(std::size_t k, double u, double v) {
    switch (k) {
        case 0: return {XY{u, 0.0}, 1.0 - v};
        case 1: return {XY{1.0, v}, u};
        case 2: return {XY{u, 1.0}, v};
        default: return {XY{0.0, v}, 1.0 - u};
    }
}

double dist_to_quad(const Quad& q, const XY& p) {
    double best = std::numeric_limits<double>::max();
    for (std::size_t k = 0; k < 4; ++k) {
        const XY a = q[k], b = q[(k + 1) % 4];
        const double dx = b[0] - a[0], dy = b[1] - a[1];
        const double l2 = dx * dx + dy * dy;
        const double t = l2 > 0.0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(a[0] + t * dx - p[0], a[1] + t * dy - p[1]));
    }
    return best;
}

}  // namespace

std::optional<Straighten> straighten_from(std::string_view text) {
    if (text == "horizontal") return Straighten::Horizontal;
    if (text == "vertical") return Straighten::Vertical;
    if (text == "auto") return Straighten::Auto;
    return std::nullopt;
}

std::vector<std::vector<std::pair<std::size_t, std::size_t>>> linked_corners(const std::vector<Plane>& planes, double link) {
    std::vector<std::vector<std::pair<std::size_t, std::size_t>>> groups;
    for (std::size_t pi = 0; pi < planes.size(); ++pi) {
        for (std::size_t ci = 0; ci < 4; ++ci) {
            const XY& c = planes[pi].src[ci];
            auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                const auto [a, b] = group.front();
                return near(planes[a].src[b], c, link);
            });
            if (g != groups.end()) {
                g->emplace_back(pi, ci);
            } else {
                groups.push_back({{pi, ci}});
            }
        }
    }
    return groups;
}

void unify(std::vector<Plane>& planes, double link) {
    for (const auto& g : linked_corners(planes, link)) {
        if (g.size() < 2) continue;
        const double n = static_cast<double>(g.size());
        XY s{0, 0}, d{0, 0};
        for (const auto& [p, c] : g) {
            s[0] += planes[p].src[c][0] / n;
            s[1] += planes[p].src[c][1] / n;
            d[0] += planes[p].dst[c][0] / n;
            d[1] += planes[p].dst[c][1] / n;
        }
        for (const auto& [p, c] : g) {
            planes[p].src[c] = s;
            planes[p].dst[c] = d;
        }
    }
}

void straighten(std::vector<Plane>& planes, Straighten mode, double link) {
    unify(planes, link);
    const auto groups = linked_corners(planes, link);
    const auto group_of = [&](std::size_t p, std::size_t c) {
        for (std::size_t g = 0; g < groups.size(); ++g)
            if (std::find(groups[g].begin(), groups[g].end(), std::make_pair(p, c)) != groups[g].end()) return g;
        return std::size_t{0};
    };
    std::vector<XY> pos;
    for (const auto& g : groups) pos.push_back(planes[g[0].first].dst[g[0].second]);
    for (std::size_t p = 0; p < planes.size(); ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            const std::size_t a = group_of(p, k), b = group_of(p, (k + 1) % 4);
            const XY pa = pos[a], pb = pos[b];
            const double dx = std::fabs(pb[0] - pa[0]), dy = std::fabs(pb[1] - pa[1]);
            if (dy <= dx && (mode == Straighten::Horizontal || mode == Straighten::Auto)) {
                const double y = (pa[1] + pb[1]) / 2.0;
                pos[a][1] = y;
                pos[b][1] = y;
            } else if (dx < dy && (mode == Straighten::Vertical || mode == Straighten::Auto)) {
                const double x = (pa[0] + pb[0]) / 2.0;
                pos[a][0] = x;
                pos[b][0] = x;
            }
        }
    }
    for (std::size_t g = 0; g < groups.size(); ++g)
        for (const auto& [pi, ci] : groups[g]) planes[pi].dst[ci] = pos[g];
}

bool is_identity(const std::vector<Plane>& planes) {
    for (const Plane& p : planes)
        for (std::size_t k = 0; k < 4; ++k)
            if (std::fabs(p.src[k][0] - p.dst[k][0]) >= 1e-9 || std::fabs(p.src[k][1] - p.dst[k][1]) >= 1e-9) return false;
    return true;
}

std::optional<Map> Map::make(const std::vector<Plane>& given, double link) {
    if (given.empty()) return std::nullopt;
    std::vector<Plane> planes = given;
    unify(planes, link);
    Map out;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const Plane& p = planes[i];
        const auto src = unit_to_quad(p.src);
        if (!src) return std::nullopt;
        const auto src_inv = src->inverse();
        const auto dst = unit_to_quad(p.dst);
        if (!src_inv || !dst) return std::nullopt;
        PlaneMap pm{*src, *src_inv, *dst, p.src, {}};
        for (std::size_t k = 0; k < 4; ++k) {
            const XY a = p.src[k], b = p.src[(k + 1) % 4];
            for (std::size_t j = 0; j < planes.size() && !pm.shared[k]; ++j) {
                if (j == i) continue;
                for (std::size_t m = 0; m < 4; ++m) {
                    const XY c = planes[j].src[m], d = planes[j].src[(m + 1) % 4];
                    if ((near(a, c, link) && near(b, d, link)) || (near(a, d, link) && near(b, c, link))) {
                        pm.shared[k] = j;
                        break;
                    }
                }
            }
        }
        out.planes_.push_back(pm);
    }
    return out;
}

std::tuple<std::size_t, double, double> Map::locate(double x, double y) const {
    std::size_t best_i = 0;
    double best_d = std::numeric_limits<double>::max(), best_u = 0.0, best_v = 0.0;
    for (std::size_t i = 0; i < planes_.size(); ++i) {
        const auto [u, v] = planes_[i].src_inv.apply(x, y);
        if (u >= -1e-9 && u <= 1.0 + 1e-9 && v >= -1e-9 && v <= 1.0 + 1e-9) return {i, u, v};
        const double d = dist_to_quad(planes_[i].quad, XY{x, y});
        if (d < best_d) {
            best_d = d;
            best_i = i;
            best_u = u;
            best_v = v;
        }
    }
    return {best_i, best_u, best_v};
}

XY Map::map(double x, double y) const {
    const auto [i, u, v] = locate(x, y);
    const PlaneMap& pm = planes_[i];
    auto [ox, oy] = pm.dst.apply(u, v);
    if (!std::isfinite(ox) || !std::isfinite(oy)) return XY{x, y};
    const double cu = std::clamp(u, 0.0, 1.0), cv = std::clamp(v, 0.0, 1.0);
    for (std::size_t k = 0; k < 4; ++k) {
        if (!pm.shared[k]) continue;
        const auto [e, w] = edge_uv(k, cu, cv);
        if (w <= 0.0) continue;
        const auto mine = pm.dst.apply(e[0], e[1]);
        const auto [qx, qy] = pm.src.apply(e[0], e[1]);
        const PlaneMap& other = planes_[*pm.shared[k]];
        const auto [ou, ov] = other.src_inv.apply(qx, qy);
        const auto theirs = other.dst.apply(ou, ov);
        ox += w * ((mine.first + theirs.first) / 2.0 - mine.first);
        oy += w * ((mine.second + theirs.second) / 2.0 - mine.second);
    }
    return XY{ox, oy};
}

}  // namespace genko::render::perspective
