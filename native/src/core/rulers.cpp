#include "core/rulers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

constexpr double kReachMm = 10.0;
constexpr double kGuideReachMm = 3.0;
constexpr double kTau = 6.283185307179586;  // math.tau
constexpr double kDegToRad = kPi / 180.0;   // math.radians

struct XY {
    double x = 0.0;
    double y = 0.0;
};

// min(hi, int(v)) without overflowing (Python's int is unbounded)
std::int64_t trunc_at_most(double v, std::int64_t hi) {
    if (!(v < static_cast<double>(hi))) return hi;
    return std::max<std::int64_t>(-hi, py_trunc_int(std::max(v, -static_cast<double>(hi))));
}

double dist(const XY& a, const XY& b) { return py_dist(a.x, a.y, b.x, b.y); }

// _xy(p) for a ruler's JSON point
XY xy(const Json& p) {
    const double x = to_float(subscript(p, 0));
    const double y = to_float(subscript(p, 1));
    return XY{x, y};
}

XY xy(const PenPoint& p) { return XY{p.x, p.y}; }

const Json& as_object(const Json& ruler) {
    if (!ruler.is_object()) {
        throw PyUncaught("AttributeError", "'" + py_type_name(ruler) + "' object has no attribute 'get'");
    }
    return ruler;
}

// [_xy(p) for p in value]
std::vector<XY> xy_list(const Json& value) {
    std::vector<XY> out;
    for (const Json& p : iterate(value)) out.push_back(xy(p));
    return out;
}

// rulers._with: new positions, with the pressures of the stroke they came from (spread along the new points)
PenPoints with_pressure(const std::vector<XY>& points_xy, const PenPoints& source) {
    PenPoints out;
    const bool any = std::any_of(source.begin(), source.end(), [](const PenPoint& p) { return p.p.has_value(); });
    if (!any) {
        for (const XY& p : points_xy) out.push_back(PenPoint{py_round(p.x, 4), py_round(p.y, 4), std::nullopt});
        return out;
    }
    std::vector<double> pressures;
    for (const PenPoint& p : source) pressures.push_back(p.p.value_or(0.7));
    const auto n = static_cast<std::int64_t>(points_xy.size());
    const auto m = static_cast<std::int64_t>(pressures.size());
    for (std::int64_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(std::max<std::int64_t>(1, n - 1)) *
                         static_cast<double>(m - 1);
        const std::int64_t k = m > 1 ? std::min<std::int64_t>(m - 2, py_trunc_int(t)) : 0;
        const double f = t - static_cast<double>(k);
        const double p = pressures[static_cast<std::size_t>(k)] * (1 - f) +
                         pressures[static_cast<std::size_t>(std::min<std::int64_t>(m - 1, k + 1))] * f;
        const XY& q = points_xy[static_cast<std::size_t>(i)];
        out.push_back(PenPoint{py_round(q.x, 4), py_round(q.y, 4), py_round(p, 4)});
    }
    return out;
}

PenPoints segment(const XY& a, const XY& b, const PenPoints& source) {
    const std::int64_t n = std::max<std::int64_t>({2, static_cast<std::int64_t>(source.size()), loop_count(dist(a, b) / 1.0) + 2});
    std::vector<XY> pts;
    for (std::int64_t i = 0; i < n; ++i) {
        const double id = static_cast<double>(i), nd = static_cast<double>(n - 1);
        pts.push_back(XY{a.x + (b.x - a.x) * id / nd, a.y + (b.y - a.y) * id / nd});
    }
    return with_pressure(pts, source);
}

std::pair<double, XY> project(const XY& p, const XY& a, const XY& d) {
    const double t = (p.x - a.x) * d.x + (p.y - a.y) * d.y;
    return {t, XY{a.x + t * d.x, a.y + t * d.y}};
}

std::optional<XY> unit(double dx, double dy) {
    const double n = py_hypot(dx, dy);
    if (n > 1e-9) return XY{dx / n, dy / n};
    return std::nullopt;
}

PenPoints straight_from(const XY& start, const XY& end, const XY& direction, const PenPoints& source) {
    const auto d = unit(direction.x, direction.y);
    if (!d) return with_pressure({start, end}, source);
    const double t = project(end, start, *d).first;
    return segment(start, XY{start.x + t * d->x, start.y + t * d->y}, source);
}

// rulers.smooth_curve: a Catmull-Rom curve through the points, about one point per mm.
std::vector<XY> smooth_curve(const Json& points, double per_mm = 1.0) {
    const std::vector<XY> pts = xy_list(points);
    if (pts.size() < 3) return pts;
    std::vector<XY> out{pts.front()};
    std::vector<XY> ext{pts.front()};
    ext.insert(ext.end(), pts.begin(), pts.end());
    ext.push_back(pts.back());
    for (std::size_t i = 1; i + 2 < ext.size(); ++i) {
        const XY& p0 = ext[i - 1];
        const XY& p1 = ext[i];
        const XY& p2 = ext[i + 1];
        const XY& p3 = ext[i + 2];
        const std::int64_t n = std::max<std::int64_t>(2, loop_count(dist(p1, p2) * per_mm));
        for (std::int64_t k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            const double t2 = t * t, t3 = t * t * t;
            const double x = 0.5 * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                                    (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
            const double y = 0.5 * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                                    (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
            out.push_back(XY{x, y});
        }
    }
    return out;
}

struct Nearest {
    double distance;
    double arc;
    XY point;
};

// rulers._nearest_on_polyline: (distance, arc position, point) of the nearest place on a polyline.
Nearest nearest_on_polyline(const XY& p, const std::vector<XY>& poly) {
    if (poly.empty()) throw PyUncaught("IndexError", "list index out of range");
    Nearest best{std::numeric_limits<double>::infinity(), 0.0, poly.front()};
    double pos = 0.0;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const XY& a = poly[i];
        const XY& b = poly[i + 1];
        const double seg = dist(a, b);
        if (seg > 1e-9) {
            const double t = py_clamp(((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / (seg * seg), 0.0, 1.0);
            const XY q{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
            const double d = dist(p, q);
            if (d < best.distance) best = Nearest{d, pos + t * seg, q};
        }
        pos += seg;
    }
    return best;
}

XY at_arc(const std::vector<XY>& poly, double s) {
    double pos = 0.0;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const XY& a = poly[i];
        const XY& b = poly[i + 1];
        const double seg = dist(a, b);
        if (pos + seg >= s && seg > 1e-9) {
            const double t = (s - pos) / seg;
            return XY{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
        }
        pos += seg;
    }
    return poly.back();
}

// sum(math.dist(a, b) for a, b in zip(poly, poly[1:]))
double polyline_length(const std::vector<XY>& poly) {
    std::vector<double> parts;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) parts.push_back(dist(poly[i], poly[i + 1]));
    return py_float_sum(parts);
}

bool is_kind(const Json& kind, const char* name) { return kind.is_string() && kind.get_ref<const std::string&>() == name; }

// rulers.shape_outline
std::vector<XY> shape_outline(const Json& ruler) {
    const Json kind = subscript(ruler, "kind");
    const Json points_value = get_or(ruler, "points", Json(nullptr));
    const std::vector<XY> pts = py_truthy(points_value) ? xy_list(points_value) : std::vector<XY>{};
    if (is_kind(kind, "polygon")) {
        std::vector<XY> out = pts;
        if (!pts.empty()) out.push_back(pts.front());
        return out;
    }
    if (pts.size() < 2) throw PyUncaught("IndexError", "list index out of range");
    const XY a = pts[0], b = pts[1];
    const double cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, w = std::fabs(b.x - a.x), h = std::fabs(b.y - a.y);
    std::vector<XY> local;
    if (is_kind(kind, "rect")) {
        local = {XY{-w / 2, -h / 2}, XY{w / 2, -h / 2}, XY{w / 2, h / 2}, XY{-w / 2, h / 2}};
    } else {
        const std::int64_t n = std::max<std::int64_t>(48, trunc_at_most(kPi * (w + h) / 2, 360));
        for (std::int64_t k = 0; k < n; ++k) {
            const double angle = kTau * static_cast<double>(k) / static_cast<double>(n);
            local.push_back(XY{w / 2 * py_cos(angle), h / 2 * py_sin(angle)});
        }
    }
    const Json angle_value = get_or(ruler, "angle", Json(0));
    const double rot = (py_truthy(angle_value) ? to_float(angle_value) : 0.0) * kDegToRad;
    const double c = py_cos(rot), s = py_sin(rot);
    std::vector<XY> out;
    for (const XY& p : local) out.push_back(XY{cx + p.x * c - p.y * s, cy + p.x * s + p.y * c});
    out.push_back(out.front());
    return out;
}

// rulers._around: the stroke carried round a closed outline (it may go past the start, round and round; corners kept)
PenPoints around(const std::vector<XY>& poly, const PenPoints& points) {
    double total = polyline_length(poly);
    if (total == 0.0) total = 1.0;
    std::vector<double> corners;
    double pos = 0.0;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        corners.push_back(pos);
        pos += dist(poly[i], poly[i + 1]);
    }
    std::vector<double> arcs;
    std::optional<double> last;
    for (const PenPoint& p : points) {
        double s = nearest_on_polyline(xy(p), poly).arc;
        if (last) s = *last + std::remainder(s - *last, total);
        arcs.push_back(s);
        last = s;
    }
    std::vector<XY> out;
    for (std::size_t i = 0; i + 1 < arcs.size(); ++i) {
        const double a = arcs[i], b = arcs[i + 1];
        const double lo = b < a ? b : a;
        const double hi = b > a ? b : a;
        std::vector<double> between;
        const std::int64_t k_from = static_cast<std::int64_t>(std::floor(lo / total)) - 1;
        const std::int64_t k_to = static_cast<std::int64_t>(std::ceil(hi / total)) + 1;
        for (const double c : corners) {
            for (std::int64_t k = k_from; k < k_to; ++k) {
                const double v = c + static_cast<double>(k) * total;
                if (lo < v && v < hi && std::find(between.begin(), between.end(), v) == between.end()) between.push_back(v);
            }
        }
        std::sort(between.begin(), between.end());
        if (b < a) std::reverse(between.begin(), between.end());
        out.push_back(at_arc(poly, py_fmod(a, total)));
        for (const double s : between) out.push_back(at_arc(poly, py_fmod(s, total)));
    }
    out.push_back(at_arc(poly, py_fmod(arcs.back(), total)));
    return with_pressure(out, points);
}

// rulers._resampled
std::vector<XY> resampled_poly(const std::vector<XY>& poly, std::int64_t n) {
    double total = polyline_length(poly);
    if (total == 0.0) total = 1.0;
    std::vector<XY> out;
    for (std::int64_t i = 0; i < n; ++i) {
        out.push_back(at_arc(poly, total * static_cast<double>(i) / static_cast<double>(n - 1)));
    }
    return out;
}

// rulers._curve_through: the curve of a parallel / multi / radial curve ruler that passes `start`
std::optional<std::vector<XY>> curve_through(const Json& ruler, const XY& start) {
    const Json kind = subscript(ruler, "kind");
    const std::vector<XY> poly = smooth_curve(subscript(ruler, "points"));
    if (poly.size() < 2) return std::nullopt;
    if (is_kind(kind, "parallel_curve")) {
        const XY near = nearest_on_polyline(start, poly).point;
        const double dx = start.x - near.x, dy = start.y - near.y;
        std::vector<XY> out;
        for (const XY& p : poly) out.push_back(XY{p.x + dx, p.y + dy});
        return out;
    }
    if (is_kind(kind, "radial_curve")) {
        const XY c = xy(subscript(ruler, "center"));
        const double angle = py_atan2(start.y - c.y, start.x - c.x);
        std::optional<XY> best;
        double best_off = std::numeric_limits<double>::infinity();
        for (const XY& p : poly) {
            const double off = std::fabs(std::remainder(py_atan2(p.y - c.y, p.x - c.x) - angle, 2 * kPi));
            if (off < best_off) {
                best = p;
                best_off = off;
            }
        }
        if (!best) throw PyTypeError("'NoneType' object is not subscriptable");
        const double base = dist(*best, c);
        if (base < 0.1) return std::nullopt;
        const double k = dist(start, c) / base;
        std::vector<XY> out;
        for (const XY& p : poly) out.push_back(XY{c.x + (p.x - c.x) * k, c.y + (p.y - c.y) * k});
        return out;
    }
    const std::vector<XY> other = smooth_curve(subscript(ruler, "points2"));
    const std::int64_t n = 120;
    const std::vector<XY> a = resampled_poly(poly, n);
    const std::vector<XY> b = resampled_poly(other, n);
    std::optional<std::vector<XY>> best;
    double best_d = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 41; ++i) {  // the blend of the two curves that passes nearest the start
        const double w = static_cast<double>(i) / 40.0;
        std::vector<XY> blend;
        for (std::size_t j = 0; j < a.size() && j < b.size(); ++j) {
            blend.push_back(XY{a[j].x * (1 - w) + b[j].x * w, a[j].y * (1 - w) + b[j].y * w});
        }
        const double d = nearest_on_polyline(start, blend).distance;
        if (d < best_d) {
            best = std::move(blend);
            best_d = d;
        }
    }
    return best;
}

// rulers.directions: the straight directions a ruler offers at a point
std::vector<XY> directions(const Json& ruler, const XY& at) {
    const Json kind = subscript(ruler, "kind");
    if (is_kind(kind, "parallel")) {
        const Json angle_value = get_or(ruler, "angle", Json(0));
        const double a = (py_truthy(angle_value) ? to_float(angle_value) : 0.0) * kDegToRad;
        return {XY{py_cos(a), py_sin(a)}};
    }
    if (is_kind(kind, "radial")) {
        const XY c = xy(subscript(subscript(ruler, "points"), 0));
        if (const auto d = unit(c.x - at.x, c.y - at.y)) return {*d};
        return {};
    }
    if (is_kind(kind, "perspective")) {
        const std::vector<XY> vps = xy_list(subscript(ruler, "points"));
        std::vector<XY> out;
        for (const XY& v : vps) {
            if (const auto d = unit(v.x - at.x, v.y - at.y)) out.push_back(*d);
        }
        if (vps.size() == 1) {
            out.push_back(XY{1.0, 0.0});
            out.push_back(XY{0.0, 1.0});
        } else if (vps.size() == 2) {
            const XY h = unit(vps[1].x - vps[0].x, vps[1].y - vps[0].y).value_or(XY{1.0, 0.0});
            out.push_back(XY{-h.y, h.x});  // upright: square to the horizon
        }
        return out;
    }
    return {};
}

// rulers._snap_one: the stroke along this ruler, or nothing when the ruler does not take it.
std::optional<PenPoints> snap_one(const Json& original, const PenPoints& points) {
    Json ruler = original;
    Json kind = subscript(ruler, "kind");
    if (is_kind(kind, "guide")) {
        const double at = to_float(subscript(ruler, "at"));
        Json line = Json::array();
        if (get_or(ruler, "axis", Json(nullptr)) == Json("h")) {
            line = Json::array({Json::array({-1e4, at}), Json::array({1e4, at})});
        } else {
            line = Json::array({Json::array({at, -1e4}), Json::array({at, 1e4})});
        }
        Json replaced = Json::object();
        replaced["kind"] = "line";
        replaced["points"] = std::move(line);
        replaced["reach_mm"] = get_or(ruler, "reach_mm", Json(kGuideReachMm));
        ruler = std::move(replaced);
        kind = Json("line");
    }
    const XY start = xy(points.front());
    const XY end = xy(points.back());
    const Json reach_value = get_or(ruler, "reach_mm", Json(kReachMm));
    const double reach = py_truthy(reach_value) ? to_float(reach_value) : kReachMm;
    if (is_kind(kind, "line")) {
        const Json pts = subscript(ruler, "points");
        const XY a = xy(subscript(pts, 0));
        const XY b = xy(subscript(subscript(ruler, "points"), 1));
        const auto d = unit(b.x - a.x, b.y - a.y);
        if (!d) return std::nullopt;
        const XY p0 = project(start, a, *d).second;
        if (dist(start, p0) > reach) return std::nullopt;
        const XY p1 = project(end, a, *d).second;
        return segment(p0, p1, points);
    }
    if (is_kind(kind, "curve")) {
        const std::vector<XY> poly = smooth_curve(subscript(ruler, "points"));
        const Nearest n0 = nearest_on_polyline(start, poly);
        if (n0.distance > reach) return std::nullopt;
        const double s0 = n0.arc;
        const double s1 = nearest_on_polyline(end, poly).arc;
        const std::int64_t n =
            std::max<std::int64_t>({2, static_cast<std::int64_t>(points.size()), loop_count(std::fabs(s1 - s0)) + 2});
        std::vector<XY> pts;
        for (std::int64_t i = 0; i < n; ++i) {
            pts.push_back(at_arc(poly, s0 + (s1 - s0) * static_cast<double>(i) / static_cast<double>(n - 1)));
        }
        return with_pressure(pts, points);
    }
    if (is_kind(kind, "concentric")) {
        const XY c = xy(subscript(subscript(ruler, "points"), 0));
        const Json ratio_value = get_or(ruler, "ratio", Json(1));
        const double ratio = py_truthy(ratio_value) ? to_float(ratio_value) : 1.0;
        const Json angle_value = get_or(ruler, "angle", Json(0));
        const double rot = (py_truthy(angle_value) ? to_float(angle_value) : 0.0) * kDegToRad;
        const double cos_r = py_cos(rot), sin_r = py_sin(rot);
        if (ratio == 0.0) throw PyUncaught("ZeroDivisionError", "float division by zero");
        const auto to_local = [&](const XY& p) {
            const double x = p.x - c.x, y = p.y - c.y;
            return XY{x * cos_r + y * sin_r, (-x * sin_r + y * cos_r) / ratio};
        };
        const auto to_page = [&](double x, double y) {
            y *= ratio;
            return XY{c.x + x * cos_r - y * sin_r, c.y + x * sin_r + y * cos_r};
        };
        const XY local = to_local(start);
        const double radius = py_hypot(local.x, local.y);
        if (radius < 0.2) return std::nullopt;
        std::vector<double> angles;
        std::optional<double> last;
        for (const PenPoint& p : points) {
            const XY q = to_local(xy(p));
            double a = py_atan2(q.y, q.x);
            if (last) {
                while (a - *last > kPi) a -= 2 * kPi;
                while (a - *last < -kPi) a += 2 * kPi;
            }
            angles.push_back(a);
            last = a;
        }
        std::vector<XY> out;
        for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
            const double a = angles[i], b = angles[i + 1];
            const std::int64_t n = std::max<std::int64_t>(1, loop_count(std::fabs(b - a) * radius));
            for (std::int64_t k = 0; k < n; ++k) {
                const double angle = a + (b - a) * static_cast<double>(k) / static_cast<double>(n);
                out.push_back(to_page(radius * py_cos(angle), radius * py_sin(angle)));
            }
        }
        out.push_back(to_page(radius * py_cos(angles.back()), radius * py_sin(angles.back())));
        return with_pressure(out, points);
    }
    if (is_kind(kind, "rect") || is_kind(kind, "ellipse") || is_kind(kind, "polygon")) {
        const std::vector<XY> poly = shape_outline(ruler);
        if (nearest_on_polyline(start, poly).distance > reach) return std::nullopt;
        return around(poly, points);
    }
    if (is_kind(kind, "parallel_curve") || is_kind(kind, "multi_curve") || is_kind(kind, "radial_curve")) {
        const auto poly = curve_through(ruler, start);
        if (!poly) return std::nullopt;
        const double s0 = nearest_on_polyline(start, *poly).arc;
        const double s1 = nearest_on_polyline(end, *poly).arc;
        const std::int64_t n =
            std::max<std::int64_t>({2, static_cast<std::int64_t>(points.size()), loop_count(std::fabs(s1 - s0)) + 2});
        std::vector<XY> pts;
        for (std::int64_t i = 0; i < n; ++i) {
            pts.push_back(at_arc(*poly, s0 + (s1 - s0) * static_cast<double>(i) / static_cast<double>(n - 1)));
        }
        return with_pressure(pts, points);
    }
    if (is_kind(kind, "parallel") || is_kind(kind, "radial") || is_kind(kind, "perspective")) {
        const std::vector<XY> options = directions(ruler, start);
        if (options.empty()) return std::nullopt;
        const auto drawn = unit(end.x - start.x, end.y - start.y);
        if (!drawn) return std::nullopt;
        std::size_t best = 0;
        double best_key = 0.0;
        for (std::size_t i = 0; i < options.size(); ++i) {
            const double key = std::fabs(options[i].x * drawn->x + options[i].y * drawn->y);
            if (i == 0 || key > best_key) {
                best = i;
                best_key = key;
            }
        }
        return straight_from(start, end, options[best], points);
    }
    return std::nullopt;
}

// ruler.get(key) != value for a layer or panel the ruler belongs to
bool other_layer(const Json& ruler, const Json* layer_id) {
    const Json* own = get(ruler, "layer_id");
    return own != nullptr && py_truthy(*own) && layer_id != nullptr && !py_equals(*own, *layer_id);
}

// rulers._applies
bool applies(const Json& ruler, const XY& start, const FrameContains& frame_contains, const Json* layer_id) {
    if (!py_truthy(get_or(ruler, "active", Json(true))) || get_or(ruler, "kind", Json(nullptr)) == Json("symmetry")) {
        return false;
    }
    if (other_layer(ruler, layer_id)) return false;
    const Json* frame_id = get(ruler, "frame_id");
    if (frame_id != nullptr && py_truthy(*frame_id) && frame_contains) return frame_contains(*frame_id, start.x, start.y);
    return true;
}

}  // namespace

PenPoints ruler_snap(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains, const Json& only,
                     const Json* layer_id) {
    if (points.size() < 2) return points;
    const XY start = xy(points.front());
    std::optional<PenPoints> best;
    double best_cost = std::numeric_limits<double>::infinity();
    const std::size_t step = std::max<std::size_t>(1, points.size() / 12);
    for (const Json& item : iterate(rulers)) {
        const Json& ruler = as_object(item);
        if (py_truthy(only) && !py_equals(get_or(ruler, "id", Json(nullptr)), only)) continue;
        if (!applies(ruler, start, frame_contains, layer_id)) continue;
        std::optional<PenPoints> snapped = snap_one(ruler, points);
        if (!snapped || snapped->empty()) continue;
        std::vector<XY> line;
        for (const PenPoint& q : *snapped) line.push_back(xy(q));
        std::vector<double> parts;
        for (std::size_t i = 0; i < points.size(); i += step) parts.push_back(nearest_on_polyline(xy(points[i]), line).distance);
        const double cost = py_float_sum(parts);
        if (cost < best_cost) {
            best = std::move(snapped);
            best_cost = cost;
        }
    }
    return best ? *best : points;
}

std::vector<PenPoints> symmetry_copies(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains,
                                       const Json* layer_id) {
    std::vector<PenPoints> out;
    const XY start = points.empty() ? XY{0, 0} : xy(points.front());
    for (const Json& item : iterate(rulers)) {
        const Json& ruler = as_object(item);
        if (get_or(ruler, "kind", Json(nullptr)) != Json("symmetry") || !py_truthy(get_or(ruler, "active", Json(true)))) {
            continue;
        }
        if (other_layer(ruler, layer_id)) continue;
        const Json* frame_id = get(ruler, "frame_id");
        if (frame_id != nullptr && py_truthy(*frame_id) && frame_contains && !frame_contains(*frame_id, start.x, start.y)) {
            continue;
        }
        const XY a = xy(subscript(subscript(ruler, "points"), 0));
        const XY b = xy(subscript(subscript(ruler, "points"), 1));
        const double axis = py_atan2(b.y - a.y, b.x - a.x);
        const Json copies_value = get_or(ruler, "copies", Json(2));
        const std::int64_t copies = py_truthy(copies_value) ? to_int(copies_value) : 2;
        std::vector<std::pair<bool, double>> transforms;  // (mirror, angle)
        if (copies == 2) {
            transforms.emplace_back(true, axis);
        } else {
            if (copies == 0) throw PyUncaught("ZeroDivisionError", "float division by zero");
            const double step = 2 * kPi / static_cast<double>(copies);
            for (std::int64_t k = 1; k < copies; ++k) transforms.emplace_back(false, static_cast<double>(k) * step);
            if (py_truthy(get_or(ruler, "mirror", Json(nullptr)))) {
                for (std::int64_t k = 0; k < copies; ++k) transforms.emplace_back(true, axis + static_cast<double>(k) * step / 2);
            }
        }
        for (const auto& [mirror, angle] : transforms) {
            const double c = py_cos(angle), s = py_sin(angle);
            PenPoints moved;
            for (const PenPoint& p : points) {
                const double x = p.x - a.x, y = p.y - a.y;
                double nx = 0.0, ny = 0.0;
                if (!mirror) {
                    nx = x * c - y * s;
                    ny = x * s + y * c;
                } else {  // reflect across the line through a at `angle`
                    const double c2 = py_cos(2 * angle), s2 = py_sin(2 * angle);
                    nx = x * c2 + y * s2;
                    ny = x * s2 - y * c2;
                }
                moved.push_back(PenPoint{py_round(a.x + nx, 4), py_round(a.y + ny, 4), p.p});
            }
            out.push_back(std::move(moved));
        }
    }
    return out;
}

}  // namespace genko::core
