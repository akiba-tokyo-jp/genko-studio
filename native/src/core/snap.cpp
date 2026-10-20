#include "core/snap.hpp"

#include <algorithm>
#include <cmath>

// PhotoCraft v0.2.0 crates/engine/src/snap.rs (SnapTargets: add_rect, nearest, snap_point, snap_rect, union), MIT:
// native/third_party/photocraft/NOTICE.md. In the page's mm instead of the document's pixels.

namespace genko::core::snap {

namespace {

Line line(bool vertical, const Target& t, double a, double b) {
    return Line{vertical, t.pos, std::min({t.span.first, a, b}), std::max({t.span.second, a, b}), t.kind};
}

}  // namespace

bool is_smart(Kind kind) { return kind == Kind::LayerEdge || kind == Kind::LayerMiddle || kind == Kind::PaperMiddle; }

void Targets::add_box(const Box& r, Kind edge, Kind middle) {
    const std::pair<double, double> down{r[1], r[3]}, across{r[0], r[2]};
    x.push_back(Target{r[0], edge, down});
    x.push_back(Target{r[2], edge, down});
    x.push_back(Target{(r[0] + r[2]) / 2, middle, down});
    y.push_back(Target{r[1], edge, across});
    y.push_back(Target{r[3], edge, across});
    y.push_back(Target{(r[1] + r[3]) / 2, middle, across});
}

std::optional<Target> Targets::nearest(bool vertical, double v, double reach) const {
    const std::vector<Target>& list = vertical ? x : y;
    std::optional<std::pair<double, Target>> best;
    const auto consider = [&](const Target& t) {
        const double d = std::fabs(t.pos - v);
        if (d > reach) return;
        const bool better = !best || d < best->first - 1e-9 ||
                            (std::fabs(d - best->first) <= 1e-9 && t.kind == Kind::Guide && best->second.kind != Kind::Guide);
        if (better) best = std::make_pair(d, t);
    };
    for (const Target& t : list) consider(t);
    if (grid && *grid > 0) {
        const double g = std::round(v / *grid) * *grid;
        const std::pair<double, double> span = vertical ? std::make_pair(extent[1], extent[3]) : std::make_pair(extent[0], extent[2]);
        consider(Target{g, Kind::Grid, span});
    }
    if (!best) return std::nullopt;
    return best->second;
}

std::pair<std::array<double, 2>, std::vector<Line>> Targets::point(const std::array<double, 2>& p, double reach) const {
    std::array<double, 2> out = p;
    std::vector<Line> lines;
    if (const auto t = nearest(true, p[0], reach)) {
        out[0] = t->pos;
        lines.push_back(line(true, *t, p[1], p[1]));
    }
    if (const auto t = nearest(false, p[1], reach)) {
        out[1] = t->pos;
        lines.push_back(line(false, *t, out[0], out[0]));
    }
    return {out, lines};
}

std::pair<std::array<double, 2>, std::vector<Line>> Targets::box(const Box& r, double reach) const {
    std::array<double, 2> delta{0.0, 0.0};
    std::vector<Line> lines;
    for (const auto& [axis, vertical] : {std::pair<std::size_t, bool>{0, true}, std::pair<std::size_t, bool>{1, false}}) {
        const double a = r[axis], b = r[axis + 2];
        const std::array<double, 3> anchors{a, (a + b) / 2, b};
        std::optional<std::pair<double, double>> best;  // (|d|, d)
        for (const double v : anchors) {
            if (const auto t = nearest(vertical, v, reach)) {
                const double d = t->pos - v;
                if (!best || std::fabs(d) < best->first) best = std::make_pair(std::fabs(d), d);
            }
        }
        if (!best) continue;
        const double d = best->second;
        delta[axis] = d;
        const std::pair<double, double> other = vertical ? std::make_pair(r[1], r[3]) : std::make_pair(r[0], r[2]);
        const std::vector<Target>& list = vertical ? x : y;
        for (const double v : anchors) {
            const double moved = v + d;
            std::optional<Target> hit;
            for (const Target& t : list) {
                if (std::fabs(t.pos - moved) >= 1e-6) continue;
                if (!hit || (t.kind == Kind::Guide && hit->kind != Kind::Guide)) hit = t;  // (a guide first)
            }
            if (!hit && grid && *grid > 0 && std::fabs(std::round(moved / *grid) * *grid - moved) < 1e-6) hit = Target{moved, Kind::Grid, other};
            if (hit) lines.push_back(line(vertical, *hit, other.first, other.second));
        }
    }
    return {delta, lines};
}

std::optional<Box> united(const std::vector<Box>& boxes) {
    std::optional<Box> out;
    for (const Box& b : boxes) {
        out = out ? Box{std::min((*out)[0], b[0]), std::min((*out)[1], b[1]), std::max((*out)[2], b[2]), std::max((*out)[3], b[3])} : b;
    }
    return out;
}

}  // namespace genko::core::snap
