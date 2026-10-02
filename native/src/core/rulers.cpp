// Python's genko/rulers.py. Every expression keeps Python's order of operations (a + b * c / d is a + ((b * c) / d)),
// sums of floats are Python 3.12's sum() (core::py_float_sum) and the math functions are CPython's (core::py_*), so the
// numbers are Python's to the last bit.

#include "core/rulers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyvalue.hpp"

namespace genko::core::rulers {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kDegToRad = kPi / 180.0;  // mathmodule.c degToRad
constexpr double kTau = 6.283185307179586;  // math.tau

const std::vector<std::string> kShapes{"rect", "ellipse", "polygon"};

std::size_t points_needed(const std::string& kind) {
    if (kind == "parallel" || kind == "guide") return 0;
    if (kind == "concentric" || kind == "radial" || kind == "perspective") return 1;
    if (kind == "polygon") return 3;
    return 2;
}

bool is_shape(const std::string& kind) { return std::find(kShapes.begin(), kShapes.end(), kind) != kShapes.end(); }

// ruler["kind"] (KeyError when it has none), as a str ("" for anything else: no kind compares equal to it).
std::string kind_of(const Json& ruler) {
    if (!ruler.is_object()) throw PyTypeError("'" + py_type_name(ruler) + "' object is not subscriptable");
    const auto it = ruler.find("kind");
    if (it == ruler.end()) throw OpKeyError("'kind'");
    return it->is_string() ? it->get<std::string>() : std::string("\x01");
}

// ruler[key] (KeyError when it is missing).
const Json& at_key(const Json& ruler, const char* key) {
    if (!ruler.is_object()) throw PyTypeError("'" + py_type_name(ruler) + "' object is not subscriptable");
    const auto it = ruler.find(key);
    if (it == ruler.end()) throw OpKeyError(py_repr_str(key));
    return *it;
}

// Python's min(a, b) and max(a, b) for floats: the first unless the second is smaller (larger).
double pmin(double a, double b) { return b < a ? b : a; }
double pmax(double a, double b) { return b > a ? b : a; }

double radians(double degrees) { return degrees * kDegToRad; }

double dist(const XY& a, const XY& b) { return py_dist(a.x, a.y, b.x, b.y); }

std::vector<XY> xy_list(const Json& points) {
    std::vector<XY> out;
    for (const Json& p : py_iter(points)) out.push_back(xy_of(p));
    return out;
}

// _pressure(p): the point's pressure, or nothing.
std::optional<double> pressure_of(const PenPoint& p) { return p.p; }

// _with(points_xy, source): new positions with the pressures of the stroke they came from, spread along them.
PenPoints with_pressures(const std::vector<XY>& points_xy, const PenPoints& source) {
    PenPoints out;
    out.reserve(points_xy.size());
    const bool none = std::all_of(source.begin(), source.end(), [](const PenPoint& p) { return !pressure_of(p); });
    if (none) {
        for (const XY& q : points_xy) out.push_back(PenPoint{py_round(q.x, 4), py_round(q.y, 4), std::nullopt});
        return out;
    }
    std::vector<double> pressures;
    pressures.reserve(source.size());
    for (const PenPoint& p : source) pressures.push_back(p.p ? *p.p : 0.7);
    const auto n = static_cast<std::int64_t>(points_xy.size());
    const auto m = static_cast<std::int64_t>(pressures.size());
    for (std::int64_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(std::max<std::int64_t>(1, n - 1)) *
                         static_cast<double>(m - 1);
        const std::int64_t k = m > 1 ? std::min<std::int64_t>(m - 2, py_int_of(t)) : 0;
        const double f = t - static_cast<double>(k);
        const double p = pressures[static_cast<std::size_t>(k)] * (1 - f) +
                         pressures[static_cast<std::size_t>(std::min<std::int64_t>(m - 1, k + 1))] * f;
        const XY& q = points_xy[static_cast<std::size_t>(i)];
        out.push_back(PenPoint{py_round(q.x, 4), py_round(q.y, 4), py_round(p, 4)});
    }
    return out;
}

// _segment(a, b, source): about a point per mm from a to b.
PenPoints segment(const XY& a, const XY& b, const PenPoints& source) {
    const auto n = std::max<std::int64_t>({2, static_cast<std::int64_t>(source.size()),
                                           py_int_of(dist(a, b) / 1.0) + 2});
    checked_count(n);
    std::vector<XY> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const double di = static_cast<double>(i);
        const double dn = static_cast<double>(n - 1);
        pts.push_back(XY{a.x + (b.x - a.x) * di / dn, a.y + (b.y - a.y) * di / dn});
    }
    return with_pressures(pts, source);
}

// _project(p, a, d): (t, point) of p on the line a + t·d.
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
    if (!d) return with_pressures({start, end}, source);
    const double t = project(end, start, *d).first;
    return segment(start, XY{start.x + t * d->x, start.y + t * d->y}, source);
}

struct Nearest {
    double distance = kInf;
    double arc = 0.0;
    XY point;
};

// _nearest_on_polyline(p, poly): (distance, arc position, point) of the nearest place on a polyline.
Nearest nearest_on_polyline(const XY& p, const std::vector<XY>& poly) {
    if (poly.empty()) throw PyIndexError("list index out of range");
    Nearest best{kInf, 0.0, poly[0]};
    double pos = 0.0;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const XY& a = poly[i];
        const XY& b = poly[i + 1];
        const double seg = dist(a, b);
        if (seg > 1e-9) {
            const double t = pmax(0.0, pmin(1.0, ((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / (seg * seg)));
            const XY q{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
            const double d = dist(p, q);
            if (d < best.distance) best = Nearest{d, pos + t * seg, q};
        }
        pos += seg;
    }
    return best;
}

// _at_arc(poly, s): the point s mm along a polyline.
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
    if (poly.empty()) throw PyIndexError("list index out of range");
    return poly.back();
}

// sum(math.dist(a, b) for a, b in zip(poly, poly[1:]))
double polyline_length(const std::vector<XY>& poly) {
    std::vector<double> parts;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) parts.push_back(dist(poly[i], poly[i + 1]));
    return py_float_sum(parts);
}

// Python's float % float (the sign of the divisor).
double py_mod(double x, double y) { return py_fmod(x, y); }

// _around(poly, points): the stroke carried round a closed outline, the way round followed as it went; corners kept.
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
        double s = nearest_on_polyline(XY{p.x, p.y}, poly).arc;
        if (last) s = *last + std::remainder(s - *last, total);  // (math.remainder: the exact IEEE remainder)
        arcs.push_back(s);
        last = s;
    }
    std::vector<XY> out;
    for (std::size_t i = 0; i + 1 < arcs.size(); ++i) {
        const double a = arcs[i];
        const double b = arcs[i + 1];
        const double lo = pmin(a, b);
        const double hi = pmax(a, b);
        std::set<double> between;  // (a set: the same position once)
        const auto k0 = py_int_of(std::floor(lo / total)) - 1;
        const auto k1 = py_int_of(std::ceil(hi / total)) + 1;
        checked_count(static_cast<std::int64_t>(
            std::min(9e15, static_cast<double>(corners.size()) * (static_cast<double>(k1) - static_cast<double>(k0)))));
        for (const double c : corners) {
            for (std::int64_t k = k0; k < k1; ++k) {
                const double v = c + static_cast<double>(k) * total;
                if (lo < v && v < hi) between.insert(v);
            }
        }
        out.push_back(at_arc(poly, py_mod(a, total)));
        if (b < a) {
            for (auto it = between.rbegin(); it != between.rend(); ++it) out.push_back(at_arc(poly, py_mod(*it, total)));
        } else {
            for (const double s : between) out.push_back(at_arc(poly, py_mod(s, total)));
        }
    }
    if (arcs.empty()) throw PyIndexError("list index out of range");
    out.push_back(at_arc(poly, py_mod(arcs.back(), total)));
    return with_pressures(out, points);
}

std::vector<XY> resampled(const std::vector<XY>& poly, std::int64_t n) {
    double total = polyline_length(poly);
    if (total == 0.0) total = 1.0;
    std::vector<XY> out;
    out.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) out.push_back(at_arc(poly, total * static_cast<double>(i) / static_cast<double>(n - 1)));
    return out;
}

// _curve_through(ruler, start): the curve of a parallel / multi / radial curve ruler that passes `start`.
std::optional<std::vector<XY>> curve_through(const Json& ruler, const XY& start) {
    const std::string kind = kind_of(ruler);
    std::vector<XY> poly = smooth_curve(at_key(ruler, "points"));
    if (poly.size() < 2) return std::nullopt;
    if (kind == "parallel_curve") {
        const XY near = nearest_on_polyline(start, poly).point;
        const double dx = start.x - near.x;
        const double dy = start.y - near.y;
        for (XY& p : poly) p = XY{p.x + dx, p.y + dy};
        return poly;
    }
    if (kind == "radial_curve") {
        const XY c = xy_of(at_key(ruler, "center"));
        const double angle = py_atan2(start.y - c.y, start.x - c.x);
        const XY* best = nullptr;
        double best_off = kInf;
        for (const XY& p : poly) {
            const double off = std::fabs(std::remainder(py_atan2(p.y - c.y, p.x - c.x) - angle, 2 * kPi));
            if (off < best_off) {
                best = &p;
                best_off = off;
            }
        }
        if (best == nullptr) throw PyTypeError("'NoneType' object is not iterable");
        const double base = dist(*best, c);
        if (base < 0.1) return std::nullopt;
        const double k = dist(start, c) / base;
        std::vector<XY> out;
        out.reserve(poly.size());
        for (const XY& p : poly) out.push_back(XY{c.x + (p.x - c.x) * k, c.y + (p.y - c.y) * k});
        return out;
    }
    const std::vector<XY> other = smooth_curve(at_key(ruler, "points2"));
    const std::int64_t n = 120;
    const std::vector<XY> a = resampled(poly, n);
    const std::vector<XY> b = resampled(other, n);
    std::optional<std::vector<XY>> best;
    double best_d = kInf;
    for (int i = 0; i < 41; ++i) {  // the blend of the two curves that passes nearest the start
        const double w = i / 40.0;
        std::vector<XY> blend;
        blend.reserve(std::min(a.size(), b.size()));
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

// The value of `ruler.get(key, fallback) or fallback` as float (Python's float(ruler.get(...) or ...)).
double float_or(const Json& ruler, const char* key, const Json& fallback) {
    return to_float(py_or(py_get(ruler, key, fallback), fallback));
}

// _snap_one(ruler, points): the stroke along this ruler, or nothing when the ruler does not take it.
std::optional<PenPoints> snap_one(const Json& ruler_in, const PenPoints& points) {
    std::string kind = kind_of(ruler_in);
    Json guide_line;
    const Json* ruler = &ruler_in;
    if (kind == "guide") {
        const double at = to_float(at_key(ruler_in, "at"));
        const bool across = py_equal(py_get(ruler_in, "axis"), Json("h"));
        guide_line = Json::object();
        guide_line["kind"] = "line";
        guide_line["points"] = across ? Json::array({Json::array({-1e4, at}), Json::array({1e4, at})})
                                      : Json::array({Json::array({at, -1e4}), Json::array({at, 1e4})});
        guide_line["reach_mm"] = py_get(ruler_in, "reach_mm", Json(kGuideReachMm));
        ruler = &guide_line;
        kind = "line";
    }
    if (points.empty()) throw PyIndexError("list index out of range");
    const XY start{points.front().x, points.front().y};
    const XY end{points.back().x, points.back().y};
    const double reach = float_or(*ruler, "reach_mm", Json(kReachMm));
    if (kind == "line") {
        const Json& pts = at_key(*ruler, "points");
        const XY a = xy_of(py_item(pts, 0));
        const XY b = xy_of(py_item(pts, 1));
        const auto d = unit(b.x - a.x, b.y - a.y);
        if (!d) return std::nullopt;
        const XY p0 = project(start, a, *d).second;
        if (dist(start, p0) > reach) return std::nullopt;
        const XY p1 = project(end, a, *d).second;
        return segment(p0, p1, points);
    }
    if (kind == "curve") {
        const std::vector<XY> poly = smooth_curve(at_key(*ruler, "points"));
        const Nearest n0 = nearest_on_polyline(start, poly);
        if (n0.distance > reach) return std::nullopt;
        const double s0 = n0.arc;
        const double s1 = nearest_on_polyline(end, poly).arc;
        const auto n = std::max<std::int64_t>({2, static_cast<std::int64_t>(points.size()),
                                               py_int_of(std::fabs(s1 - s0)) + 2});
        checked_count(n);
        std::vector<XY> out;
        for (std::int64_t i = 0; i < n; ++i) {
            out.push_back(at_arc(poly, s0 + (s1 - s0) * static_cast<double>(i) / static_cast<double>(n - 1)));
        }
        return with_pressures(out, points);
    }
    if (kind == "concentric") {
        const XY c = xy_of(py_item(at_key(*ruler, "points"), 0));
        const double ratio = float_or(*ruler, "ratio", Json(1));
        const double rot = radians(float_or(*ruler, "angle", Json(0)));
        const double cos_r = math_cos(rot);
        const double sin_r = math_sin(rot);
        const auto to_local = [&](const XY& p) {
            const double x = p.x - c.x;
            const double y = p.y - c.y;
            return XY{x * cos_r + y * sin_r, (-x * sin_r + y * cos_r) / ratio};
        };
        const auto to_page = [&](double x, double y) {
            y *= ratio;
            return XY{c.x + x * cos_r - y * sin_r, c.y + x * sin_r + y * cos_r};
        };
        const XY l = to_local(start);
        const double radius = py_hypot(l.x, l.y);
        if (radius < 0.2) return std::nullopt;
        std::vector<double> angles;
        std::optional<double> last;
        for (const PenPoint& p : points) {
            const XY q = to_local(XY{p.x, p.y});
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
            const double a = angles[i];
            const double b = angles[i + 1];
            const auto n = checked_count(std::max<std::int64_t>(1, py_int_of(std::fabs(b - a) * radius)));
            for (std::int64_t k = 0; k < n; ++k) {
                const double angle = a + (b - a) * static_cast<double>(k) / static_cast<double>(n);
                out.push_back(to_page(radius * math_cos(angle), radius * math_sin(angle)));
            }
        }
        out.push_back(to_page(radius * math_cos(angles.back()), radius * math_sin(angles.back())));
        return with_pressures(out, points);
    }
    if (is_shape(kind)) {
        const std::vector<XY> poly = shape_outline(*ruler);
        if (nearest_on_polyline(start, poly).distance > reach) return std::nullopt;
        return around(poly, points);
    }
    if (kind == "parallel_curve" || kind == "multi_curve" || kind == "radial_curve") {
        const auto poly = curve_through(*ruler, start);
        if (!poly) return std::nullopt;
        const double s0 = nearest_on_polyline(start, *poly).arc;
        const double s1 = nearest_on_polyline(end, *poly).arc;
        const auto n = std::max<std::int64_t>({2, static_cast<std::int64_t>(points.size()),
                                               py_int_of(std::fabs(s1 - s0)) + 2});
        checked_count(n);
        std::vector<XY> out;
        for (std::int64_t i = 0; i < n; ++i) {
            out.push_back(at_arc(*poly, s0 + (s1 - s0) * static_cast<double>(i) / static_cast<double>(n - 1)));
        }
        return with_pressures(out, points);
    }
    if (kind == "parallel" || kind == "radial" || kind == "perspective") {
        const std::vector<XY> options = directions(*ruler, start);
        if (options.empty()) return std::nullopt;
        const auto drawn = unit(end.x - start.x, end.y - start.y);
        if (!drawn) return std::nullopt;
        const XY* best = &options.front();
        double best_value = std::fabs(best->x * drawn->x + best->y * drawn->y);
        for (const XY& d : options) {  // (max(): the first of the largest)
            const double v = std::fabs(d.x * drawn->x + d.y * drawn->y);
            if (v > best_value) {
                best = &d;
                best_value = v;
            }
        }
        return straight_from(start, end, *best, points);
    }
    return std::nullopt;
}

bool applies(const Json& ruler, const XY& start, const FrameContains& frame_contains, const std::optional<std::string>& layer_id) {
    if (!py_truthy(py_get(ruler, "active", Json(true))) || py_equal(py_get(ruler, "kind"), Json("symmetry"))) return false;
    if (py_truthy(py_get(ruler, "layer_id")) && layer_id && !py_equal(at_key(ruler, "layer_id"), Json(*layer_id))) return false;
    if (py_truthy(py_get(ruler, "frame_id")) && frame_contains) return frame_contains(at_key(ruler, "frame_id"), start.x, start.y);
    return true;
}

}  // namespace

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> k{"line",          "curve",       "parallel",     "concentric", "radial",
                                            "perspective",   "symmetry",    "guide",        "parallel_curve",
                                            "multi_curve",   "radial_curve", "rect",        "ellipse",    "polygon"};
    return k;
}

XY xy_of(const Json& point) {
    const double x = to_float(py_item(point, 0));
    const double y = to_float(py_item(point, 1));
    return XY{x, y};
}

void validate(const Json& ruler) {
    const Json kind_value = py_get(ruler, "kind");
    const auto& all = kinds();
    if (!kind_value.is_string() || std::find(all.begin(), all.end(), kind_value.get<std::string>()) == all.end()) {
        std::string names;
        for (const auto& k : all) names += (names.empty() ? "" : ", ") + k;
        throw PyValueError("kind must be one of " + names);
    }
    const std::string kind = kind_value.get<std::string>();
    const Json points = py_or(py_get(ruler, "points"), Json::array());
    const std::size_t needed = points_needed(kind);
    if (py_len(points) < needed) throw PyValueError("a " + kind + " ruler needs " + std::to_string(needed) + " point(s)");
    if (kind == "perspective" && py_len(points) > 3) throw PyValueError("a perspective ruler has 1 to 3 vanishing points");
    if (kind == "concentric" && float_or(ruler, "ratio", Json(1)) <= 0) throw PyValueError("ratio must be above 0");
    if (kind == "guide") {
        const Json axis = py_get(ruler, "axis");
        if (!py_equal(axis, Json("h")) && !py_equal(axis, Json("v"))) throw PyValueError("a guide's axis is h or v");
        (void)to_float(py_get(ruler, "at"));
    }
    if (kind == "symmetry") {
        const std::int64_t copies = to_int(py_or(py_get(ruler, "copies", Json(2)), Json(2)));
        if (!(2 <= copies && copies <= 32)) throw PyValueError("copies is 2 to 32");
    }
    if (kind == "multi_curve" && py_len(py_or(py_get(ruler, "points2"), Json::array())) < 2) {
        throw PyValueError("a multi_curve ruler needs points2 (a second curve)");
    }
    if (kind == "radial_curve" && py_len(py_or(py_get(ruler, "center"), Json::array())) < 2) {
        throw PyValueError("a radial_curve ruler needs its center");
    }
    if (kind == "rect" || kind == "ellipse") {
        if (!points.is_array() && !points.is_string()) throw PyTypeError("unhashable type: 'slice'");
        const XY a = xy_of(py_item(points, 0));
        const XY b = xy_of(py_item(points, 1));
        if (std::fabs(b.x - a.x) < 0.5 || std::fabs(b.y - a.y) < 0.5) {
            throw PyValueError("a " + kind + " ruler needs a box with some size (points: two opposite corners)");
        }
    }
    const Json grid = py_get(ruler, "grid");
    if (!grid.is_null()) {
        const std::int64_t n = to_int(grid);
        if (!(0 <= n && n <= 60)) throw PyValueError("grid is 0 (none) to 60 lines");
    }
}

Polyline smooth_curve(const Json& points, double per_mm) {
    const std::vector<XY> pts = xy_list(points);
    if (pts.size() < 3) return pts;
    Polyline out{pts.front()};
    std::vector<XY> ext;
    ext.reserve(pts.size() + 2);
    ext.push_back(pts.front());
    ext.insert(ext.end(), pts.begin(), pts.end());
    ext.push_back(pts.back());
    for (std::size_t i = 1; i + 2 < ext.size(); ++i) {
        const XY& p0 = ext[i - 1];
        const XY& p1 = ext[i];
        const XY& p2 = ext[i + 1];
        const XY& p3 = ext[i + 2];
        const auto n = checked_count(std::max<std::int64_t>(2, py_int_of(dist(p1, p2) * per_mm)));
        for (std::int64_t k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            const double t2 = t * t;
            const double t3 = t * t * t;
            const auto coord = [&](double q0, double q1, double q2, double q3) {
                return 0.5 * (2 * q1 + (-q0 + q2) * t + (2 * q0 - 5 * q1 + 4 * q2 - q3) * t2 + (-q0 + 3 * q1 - 3 * q2 + q3) * t3);
            };
            out.push_back(XY{coord(p0.x, p1.x, p2.x, p3.x), coord(p0.y, p1.y, p2.y, p3.y)});
        }
    }
    return out;
}

Polyline shape_outline(const Json& ruler) {
    const std::string kind = kind_of(ruler);
    const std::vector<XY> pts = xy_list(py_or(py_get(ruler, "points"), Json::array()));
    if (kind == "polygon") {
        Polyline out = pts;
        if (!pts.empty()) out.push_back(pts.front());
        return out;
    }
    if (pts.size() < 2) throw PyIndexError("list index out of range");
    const XY a = pts[0];
    const XY b = pts[1];
    const double cx = (a.x + b.x) / 2;
    const double cy = (a.y + b.y) / 2;
    const double w = std::fabs(b.x - a.x);
    const double h = std::fabs(b.y - a.y);
    std::vector<XY> local;
    if (kind == "rect") {
        local = {XY{-w / 2, -h / 2}, XY{w / 2, -h / 2}, XY{w / 2, h / 2}, XY{-w / 2, h / 2}};
    } else {
        const double span = kPi * (w + h) / 2;
        const std::int64_t n = std::max<std::int64_t>(
            48, std::min<std::int64_t>(360, py_int_of(span)));
        for (std::int64_t k = 0; k < n; ++k) {
            const double angle = kTau * static_cast<double>(k) / static_cast<double>(n);
            local.push_back(XY{w / 2 * math_cos(angle), h / 2 * math_sin(angle)});
        }
    }
    const double rot = radians(float_or(ruler, "angle", Json(0)));
    const double c = math_cos(rot);
    const double s = math_sin(rot);
    Polyline out;
    out.reserve(local.size() + 1);
    for (const XY& p : local) out.push_back(XY{cx + p.x * c - p.y * s, cy + p.x * s + p.y * c});
    out.push_back(out.front());
    return out;
}

std::vector<XY> directions(const Json& ruler, XY at) {
    const std::string kind = kind_of(ruler);
    if (kind == "parallel") {
        const double a = radians(float_or(ruler, "angle", Json(0)));
        return {XY{math_cos(a), math_sin(a)}};
    }
    if (kind == "radial") {
        const XY c = xy_of(py_item(at_key(ruler, "points"), 0));
        const auto d = unit(c.x - at.x, c.y - at.y);
        if (d) return {*d};
        return {};
    }
    if (kind == "perspective") {
        const std::vector<XY> vps = xy_list(at_key(ruler, "points"));
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

std::optional<std::pair<XY, XY>> horizon(const Json& ruler) {
    const std::vector<XY> vps = xy_list(py_or(py_get(ruler, "points"), Json::array()));
    if (vps.empty()) return std::nullopt;
    if (vps.size() == 1) return std::make_pair(vps[0], XY{1.0, 0.0});
    return std::make_pair(vps[0], unit(vps[1].x - vps[0].x, vps[1].y - vps[0].y).value_or(XY{1.0, 0.0}));
}

PenPoints snap(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains,
               const std::optional<std::string>& only, const std::optional<std::string>& layer_id) {
    if (points.size() < 2) return points;
    const XY start{points.front().x, points.front().y};
    std::optional<PenPoints> best;
    double best_cost = kInf;
    for (const Json& ruler : py_iter(rulers)) {
        if (only && !only->empty() && !py_equal(py_get(ruler, "id"), Json(*only))) continue;
        if (!applies(ruler, start, frame_contains, layer_id)) continue;
        const auto snapped = snap_one(ruler, points);
        if (!snapped || snapped->empty()) continue;
        std::vector<XY> line;
        line.reserve(snapped->size());
        for (const PenPoint& q : *snapped) line.push_back(XY{q.x, q.y});
        std::vector<double> costs;
        const std::size_t step = std::max<std::size_t>(1, points.size() / 12);
        for (std::size_t i = 0; i < points.size(); i += step) {
            costs.push_back(nearest_on_polyline(XY{points[i].x, points[i].y}, line).distance);
        }
        const double cost = py_float_sum(costs);
        if (cost < best_cost) {
            best = *snapped;
            best_cost = cost;
        }
    }
    return best ? *best : points;
}

std::vector<PenPoints> symmetry_copies(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains,
                                       const std::optional<std::string>& layer_id) {
    std::vector<PenPoints> out;
    const XY start = points.empty() ? XY{0, 0} : XY{points.front().x, points.front().y};
    for (const Json& ruler : py_iter(rulers)) {
        if (!py_equal(py_get(ruler, "kind"), Json("symmetry")) || !py_truthy(py_get(ruler, "active", Json(true)))) continue;
        if (py_truthy(py_get(ruler, "layer_id")) && layer_id && !py_equal(at_key(ruler, "layer_id"), Json(*layer_id))) continue;
        if (py_truthy(py_get(ruler, "frame_id")) && frame_contains &&
            !frame_contains(at_key(ruler, "frame_id"), start.x, start.y)) {
            continue;
        }
        const Json& pts = at_key(ruler, "points");
        const XY a = xy_of(py_item(pts, 0));
        const XY b = xy_of(py_item(pts, 1));
        const double axis = py_atan2(b.y - a.y, b.x - a.x);
        const std::int64_t copies = to_int(py_or(py_get(ruler, "copies", Json(2)), Json(2)));
        std::vector<std::pair<bool, double>> transforms;  // (turn, angle)
        if (copies == 2) {
            transforms.emplace_back(false, axis);
        } else {
            const double step = 2 * kPi / static_cast<double>(copies);
            for (std::int64_t k = 1; k < copies; ++k) transforms.emplace_back(true, static_cast<double>(k) * step);
            if (py_truthy(py_get(ruler, "mirror"))) {
                for (std::int64_t k = 0; k < copies; ++k) transforms.emplace_back(false, axis + static_cast<double>(k) * step / 2);
            }
        }
        for (const auto& [turn, angle] : transforms) {
            const double c = math_cos(angle);
            const double s = math_sin(angle);
            PenPoints moved;
            moved.reserve(points.size());
            for (const PenPoint& p : points) {
                const double x = p.x - a.x;
                const double y = p.y - a.y;
                double nx = 0;
                double ny = 0;
                if (turn) {
                    nx = x * c - y * s;
                    ny = x * s + y * c;
                } else {  // reflect across the line through a at `angle`
                    const double c2 = math_cos(2 * angle);
                    const double s2 = math_sin(2 * angle);
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

std::vector<std::vector<OutlinePoint>> outline(const Json& ruler, const PageSize& page_size) {
    const std::string kind = kind_of(ruler);
    const auto floats = [](const std::vector<XY>& pts) {
        std::vector<OutlinePoint> out;
        out.reserve(pts.size());
        for (const XY& p : pts) out.push_back(OutlinePoint{Num(p.x), Num(p.y)});
        return out;
    };
    if (kind == "line") {
        const Json& pts = at_key(ruler, "points");
        if (!pts.is_array() && !pts.is_string()) throw PyTypeError("unhashable type: 'slice'");
        const Json all = py_iter(pts);
        std::vector<XY> two;
        for (std::size_t i = 0; i < all.size() && i < 2; ++i) two.push_back(xy_of(all[i]));
        return {floats(two)};
    }
    if (kind == "curve" || kind == "parallel_curve" || kind == "radial_curve") return {floats(smooth_curve(at_key(ruler, "points")))};
    if (kind == "multi_curve") {
        return {floats(smooth_curve(at_key(ruler, "points"))), floats(smooth_curve(at_key(ruler, "points2")))};
    }
    if (is_shape(kind)) return {floats(shape_outline(ruler))};
    if (kind == "guide") {
        const double at = to_float(at_key(ruler, "at"));
        if (py_equal(py_get(ruler, "axis"), Json("h"))) return {{OutlinePoint{Num(0.0), Num(at)}, OutlinePoint{page_size.width, Num(at)}}};
        return {{OutlinePoint{Num(at), Num(0.0)}, OutlinePoint{Num(at), page_size.height}}};
    }
    if (kind == "concentric" && py_len(py_or(py_get(ruler, "points"), Json::array())) > 1) {
        const Json& pts = at_key(ruler, "points");
        const XY c = xy_of(py_item(pts, 0));
        const XY edge = xy_of(py_item(pts, 1));
        const double ratio = float_or(ruler, "ratio", Json(1));
        const double rot = radians(float_or(ruler, "angle", Json(0)));
        const double x = edge.x - c.x;
        const double y = edge.y - c.y;
        const double r = py_hypot(x * math_cos(rot) + y * math_sin(rot), (-x * math_sin(rot) + y * math_cos(rot)) / ratio);
        std::vector<XY> out;
        for (int i = 0; i < 97; ++i) {
            const double t = 2 * kPi * i / 96;
            const double lx = r * math_cos(t);
            const double ly = r * math_sin(t) * ratio;
            out.push_back(XY{c.x + lx * math_cos(rot) - ly * math_sin(rot), c.y + lx * math_sin(rot) + ly * math_cos(rot)});
        }
        return {floats(out)};
    }
    if (kind == "perspective") {
        const auto eye = horizon(ruler);
        if (!eye) return {};
        const auto [p, d] = *eye;
        return {floats({XY{p.x - d.x * 1000, p.y - d.y * 1000}, XY{p.x + d.x * 1000, p.y + d.y * 1000}})};
    }
    return {};
}

std::vector<std::array<XY, 2>> perspective_grid(const Json& ruler, const PageSize& page_size, std::optional<std::int64_t> lines) {
    const std::vector<XY> vps = xy_list(py_or(py_get(ruler, "points"), Json::array()));
    const std::int64_t n = lines ? *lines : to_int(py_or(py_get(ruler, "grid"), Json(0)));
    if (vps.empty() || n <= 0) return {};
    const Num& w = page_size.width;
    const Num& h = page_size.height;
    // base_y = max(h, vps[0][1] + 10) + h * 0.5
    const Num lifted(vps[0].y + 10);
    const Num tallest = lifted > h ? lifted : h;
    const double base_y = (tallest + h * Num(0.5)).value();
    // spread = max(w, 1.0) * 3 (an int when the width is an int above 1)
    const Num widest = Num(1.0) > w ? Num(1.0) : w;
    const Num spread = widest * Num(3);
    std::vector<double> xs;
    for (std::int64_t k = 0; k <= n; ++k) {
        xs.push_back((Num(vps[0].x) - spread / Num(2) + spread * Num(k) / Num(n)).value());
    }
    std::vector<std::array<XY, 2>> out;
    if (vps.size() == 1) {
        const double vx = vps[0].x;
        const double vy = vps[0].y;
        for (const double x : xs) out.push_back({XY{vx, vy}, XY{x, base_y}});
        // the depths: a diagonal from the left end to a measuring point on the eye level
        const double mx = (Num(vx) + spread).value();
        const double lx = xs.front();
        const double ly = base_y;
        for (std::size_t i = 1; i < xs.size(); ++i) {
            const double x = xs[i];
            // where the line toward the vanishing point from (x, base_y) meets the diagonal (lx, ly)→(mx, vy): _cross
            const double ax = vx, ay = vy, bx = x, by = base_y, cx = lx, cy = ly, dx = mx, dy = vy;
            const double den = (ax - bx) * (cy - dy) - (ay - by) * (cx - dx);
            if (std::fabs(den) < 1e-9) continue;
            const double t = ((ax - cx) * (cy - dy) - (ay - cy) * (cx - dx)) / den;
            const XY p{ax + t * (bx - ax), ay + t * (by - ay)};
            if (vy < p.y && p.y <= base_y) {
                out.push_back({XY{xs.front() + (p.y - base_y) * (vx - xs.front()) / (vy - base_y), p.y},
                               XY{xs.back() + (p.y - base_y) * (vx - xs.back()) / (vy - base_y), p.y}});
            }
        }
        return out;
    }
    for (std::size_t v = 0; v < vps.size() && v < 2; ++v) {
        for (const double x : xs) out.push_back({vps[v], XY{x, base_y}});
    }
    return out;
}

XY snap_to_grid(XY point, double spacing_mm, XY origin) {
    if (spacing_mm <= 0) return point;
    return XY{origin.x + std::nearbyint((point.x - origin.x) / spacing_mm) * spacing_mm,
              origin.y + std::nearbyint((point.y - origin.y) / spacing_mm) * spacing_mm};
}

}  // namespace genko::core::rulers
