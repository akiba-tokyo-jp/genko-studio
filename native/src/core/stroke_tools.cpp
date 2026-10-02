#include "core/stroke_tools.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// Python's round(x) (no digits): the nearest int, ties to even.
std::int64_t round_half_even(double x) { return static_cast<std::int64_t>(py_round(x, 0)); }

// Python's int(x) for a float: towards zero.
std::int64_t trunc_int(double x) { return static_cast<std::int64_t>(std::trunc(x)); }

// max(0.0, min(1.0, v))
double clamp01(double v) {
    const double m = v < 1.0 ? v : 1.0;
    return m > 0.0 ? m : 0.0;
}

// The distance from (x, y) to the segment a–b, as Python's eraser tests compute it.
double segment_distance(double x, double y, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double seg = dx * dx + dy * dy;
    const double t = seg == 0 ? 0.0 : clamp01(((x - ax) * dx + (y - ay) * dy) / seg);
    return py_hypot(x - (ax + t * dx), y - (ay + t * dy));
}

// zip(eraser, eraser[1:] or eraser): the eraser's segments (one point: that point as a segment).
template <class F>
bool any_segment(const PenPoints& eraser, F&& test) {
    if (eraser.size() == 1) return test(eraser[0], eraser[0]);
    for (std::size_t i = 0; i + 1 < eraser.size(); ++i) {
        if (test(eraser[i], eraser[i + 1])) return true;
    }
    return false;
}

// stroke._seg_cross: the parameter t along a→b where it crosses c→d, or nothing.
std::optional<double> seg_cross(const PointF& a, const PointF& b, const PointF& c, const PointF& d) {
    const double rx = b.x - a.x, ry = b.y - a.y;
    const double sx = d.x - c.x, sy = d.y - c.y;
    const double den = rx * sy - ry * sx;
    if (std::fabs(den) < 1e-12) return std::nullopt;
    const double t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
    const double u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    if (0 <= t && t <= 1 && 0 <= u && u <= 1) return t;
    return std::nullopt;
}

double perp(const PenPoint& point, const PenPoint& start, const PenPoint& end) {
    const double x = point.x, y = point.y;
    const double x1 = start.x, y1 = start.y;
    const double x2 = end.x, y2 = end.y;
    const double dx = x2 - x1, dy = y2 - y1;
    double length = py_pow(dx * dx + dy * dy, 0.5);
    if (length == 0.0) length = 1.0;
    return std::fabs((y2 - y1) * x - (x2 - x1) * y + x2 * y1 - y2 * x1) / length;
}

}  // namespace

double py_sum_doubles(std::span<const double> values) {
    if (values.empty()) return 0.0;
    double sum = 0.0 + values[0];
    double c = 0.0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        const double x = values[i];
        const double t = sum + x;
        if (std::fabs(sum) >= std::fabs(x)) {
            c += (sum - t) + x;
        } else {
            c += (x - t) + sum;
        }
        sum = t;
    }
    if (c != 0.0 && std::isfinite(c)) sum += c;
    return sum;
}

PenPoints parse_points(const Json& raw) {
    PenPoints out;
    for (const Json& item : iterate(raw)) {
        const std::size_t n = length(item);
        if (n < 2) throw OpError("points needs [x_mm, y_mm]");
        PenPoint p;
        p.x = to_float(subscript(item, 0));
        p.y = to_float(subscript(item, 1));
        if (n >= 3) p.p = to_float(subscript(item, 2));
        out.push_back(p);
    }
    return out;
}

PenPoints stroke_points(const Stroke& stroke) {
    PenPoints out;
    const bool with_pressure = !stroke.pressure.empty() && stroke.pressure.size() == stroke.points.size();
    for (std::size_t i = 0; i < stroke.points.size(); ++i) {
        PenPoint p{stroke.points[i].x, stroke.points[i].y, std::nullopt};
        if (with_pressure) p.p = stroke.pressure[i];
        out.push_back(p);
    }
    return out;
}

Stroke coerce_stroke(const PenPoints& points) {
    Stroke stroke;
    for (const PenPoint& p : points) {
        stroke.points.push_back(PointF{p.x, p.y});
        if (p.p) stroke.pressure.push_back(*p.p);
    }
    if (stroke.pressure.size() != stroke.points.size()) stroke.pressure.clear();
    stroke.id = new_id();
    return stroke;
}

PenPoints stabilize_points(const PenPoints& points, std::int64_t window, bool by_speed) {
    const auto n = static_cast<std::int64_t>(points.size());
    if (window < 3 || n < 3) return points;
    const std::int64_t half = std::max<std::int64_t>(1, window / 2);
    std::vector<std::int64_t> reach(static_cast<std::size_t>(n), half);
    if (by_speed && n > 3) {
        std::vector<double> steps;
        for (std::int64_t i = 0; i + 1 < n; ++i) {
            const auto& a = points[static_cast<std::size_t>(i)];
            const auto& b = points[static_cast<std::size_t>(i) + 1];
            steps.push_back(py_dist(a.x, a.y, b.x, b.y));
        }
        std::vector<double> sorted = steps;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t m = sorted.size();
        double usual = m % 2 == 1 ? sorted[m / 2] : (sorted[m / 2 - 1] + sorted[m / 2]) / 2;  // statistics.median
        if (usual == 0.0) usual = 1e-6;
        for (std::int64_t i = 0; i < n; ++i) {
            const double step = (steps[static_cast<std::size_t>(std::max<std::int64_t>(0, i - 1))] +
                                 steps[static_cast<std::size_t>(std::min<std::int64_t>(n - 2, i))]) / 2;
            // (half the usual step: slow; twice: quick)
            const double quick = clamp01((step / usual - 0.5) / 1.5);
            reach[static_cast<std::size_t>(i)] =
                std::max<std::int64_t>(1, round_half_even(static_cast<double>(half) * (0.5 + 1.5 * quick)));
        }
    }
    PenPoints out;
    for (std::int64_t i = 0; i < n; ++i) {
        // a window that shrinks evenly at the ends keeps them where they were drawn
        const std::int64_t k = std::min({reach[static_cast<std::size_t>(i)], i, n - 1 - i});
        std::vector<double> xs, ys;
        for (std::int64_t j = i - k; j <= i + k; ++j) {
            xs.push_back(points[static_cast<std::size_t>(j)].x);
            ys.push_back(points[static_cast<std::size_t>(j)].y);
        }
        const double count = static_cast<double>(xs.size());
        out.push_back(PenPoint{py_sum_doubles(xs) / count, py_sum_doubles(ys) / count, points[static_cast<std::size_t>(i)].p});
    }
    return out;
}

PenPoints smoothed(const PenPoints& points, std::int64_t strength) {
    if (strength <= 0 || points.size() < 4) return points;
    const std::int64_t window = std::max<std::int64_t>(1, strength);
    const auto n = static_cast<std::int64_t>(points.size());
    PenPoints out{points.front()};
    for (std::int64_t i = 1; i < n - 1; ++i) {
        const std::int64_t lo = std::max<std::int64_t>(0, i - window);
        const std::int64_t hi = std::min<std::int64_t>(n, i + window + 1);
        std::vector<double> xs, ys;
        for (std::int64_t j = lo; j < hi; ++j) {
            xs.push_back(points[static_cast<std::size_t>(j)].x);
            ys.push_back(points[static_cast<std::size_t>(j)].y);
        }
        const double count = static_cast<double>(xs.size());
        out.push_back(PenPoint{py_sum_doubles(xs) / count, py_sum_doubles(ys) / count, points[static_cast<std::size_t>(i)].p});
    }
    out.push_back(points.back());
    return out;
}

PenPoints fit_curve(const PenPoints& points, double tolerance_mm, double step_mm) {
    struct P3 {
        double x, y, p;
    };
    std::vector<P3> pts;
    for (const PenPoint& p : points) pts.push_back(P3{p.x, p.y, p.p.value_or(1.0)});
    if (pts.size() < 4 || tolerance_mm <= 0) return points;
    const auto dist_seg = [](const P3& p, const P3& a, const P3& b) {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length = dx * dx + dy * dy;
        const double t = length < 1e-12 ? 0.0 : clamp01(((p.x - a.x) * dx + (p.y - a.y) * dy) / length);
        return py_hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
    };
    const std::size_t last = pts.size() - 1;
    std::set<std::size_t> keep{0, last};
    std::vector<std::pair<std::size_t, std::size_t>> todo{{0, last}};
    while (!todo.empty()) {
        const auto [lo, hi] = todo.back();
        todo.pop_back();
        double far = 0.0;
        std::int64_t at = -1;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            const double d = dist_seg(pts[i], pts[lo], pts[hi]);
            if (d > far) {
                far = d;
                at = static_cast<std::int64_t>(i);
            }
        }
        if (far > tolerance_mm && at > 0) {
            keep.insert(static_cast<std::size_t>(at));
            todo.emplace_back(lo, static_cast<std::size_t>(at));
            todo.emplace_back(static_cast<std::size_t>(at), hi);
        }
    }
    std::vector<P3> key;
    for (const std::size_t i : keep) key.push_back(pts[i]);
    if (key.size() < 3) key = {pts[0], pts[pts.size() / 2], pts[last]};
    PenPoints out;
    const auto k = static_cast<std::int64_t>(key.size());
    for (std::int64_t i = 0; i < k - 1; ++i) {
        const P3& p0 = key[static_cast<std::size_t>(std::max<std::int64_t>(0, i - 1))];
        const P3& p1 = key[static_cast<std::size_t>(i)];
        const P3& p2 = key[static_cast<std::size_t>(i + 1)];
        const P3& p3 = key[static_cast<std::size_t>(std::min<std::int64_t>(k - 1, i + 2))];
        const std::int64_t steps = std::max<std::int64_t>(1, loop_count(py_dist(p1.x, p1.y, p2.x, p2.y) / step_mm));
        for (std::int64_t s = 0; s < steps; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(steps);
            const double t2 = t * t, t3 = t * t * t;
            const double x = 0.5 * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                                    (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
            const double y = 0.5 * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                                    (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
            out.push_back(PenPoint{x, y, p1.p + (p2.p - p1.p) * t});
        }
    }
    out.push_back(PenPoint{key.back().x, key.back().y, key.back().p});
    return out;
}

PenPoints taper_points(const PenPoints& points, std::optional<double> in_mm, std::optional<double> out_mm) {
    const std::size_t n = points.size();
    if (n < 2) return points;
    std::vector<double> factors;
    if (!in_mm && !out_mm) {
        // span = max(1, n * 0.25): the int 1, or the float
        const double quarter = static_cast<double>(n) * 0.25;
        const double span = quarter > 1 ? quarter : 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = static_cast<double>(std::min(i, n - 1 - i)) / span;
            factors.push_back(v < 1.0 ? v : 1.0);
        }
    } else {
        std::vector<double> along{0.0};
        for (std::size_t i = 0; i + 1 < n; ++i) {
            along.push_back(along.back() + py_dist(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y));
        }
        double total = along.back();
        if (total == 0.0) total = 1e-6;
        const double in_value = in_mm.value_or(0.0);
        const double out_value = out_mm.value_or(0.0);
        double first = in_value > 0.0 ? in_value : 0.0;
        double last = out_value > 0.0 ? out_value : 0.0;
        if (first + last > total) {  // (a line shorter than both: each end takes its share)
            const double k = total / (first + last);
            first = first * k;
            last = last * k;
        }
        for (const double s : along) {
            double factor = 1.0;
            const double a = first > 0 ? s / first : 1.0;
            if (a < factor) factor = a;
            const double b = last > 0 ? (total - s) / last : 1.0;
            if (b < factor) factor = b;
            factors.push_back(factor);
        }
    }
    PenPoints out;
    for (std::size_t i = 0; i < n; ++i) {
        const double pressure = points[i].p.value_or(1.0);
        const double factor = factors[i] > 0.15 ? factors[i] : 0.15;
        out.push_back(PenPoint{points[i].x, points[i].y, pressure * factor});
    }
    return out;
}

PenPoints apply_pressure_curve(const PenPoints& points, std::string_view curve) {
    if (curve.empty() || curve == "linear") return points;
    PenPoints out;
    for (const PenPoint& point : points) {
        if (!point.p) {
            out.push_back(point);
            continue;
        }
        if (*point.p < 0.0) throw PyTypeError("'<' not supported between instances of 'complex' and 'float'");
        const double v = py_pow(*point.p, 1.8);
        const double m = v < 1.0 ? v : 1.0;
        out.push_back(PenPoint{point.x, point.y, m > 0.05 ? m : 0.05});
    }
    return out;
}

PenPoints rdp(const PenPoints& points, double epsilon) {
    if (points.size() < 3) return points;
    double dmax = 0.0;
    std::size_t index = 0;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const double distance = perp(points[i], points.front(), points.back());
        if (distance > dmax) {
            index = i;
            dmax = distance;
        }
    }
    if (dmax > epsilon) {
        PenPoints left = rdp(PenPoints(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(index) + 1), epsilon);
        const PenPoints right = rdp(PenPoints(points.begin() + static_cast<std::ptrdiff_t>(index), points.end()), epsilon);
        left.pop_back();
        left.insert(left.end(), right.begin(), right.end());
        return left;
    }
    return {points.front(), points.back()};
}

std::vector<double> resampled(const std::vector<double>& values, std::int64_t count) {
    if (values.empty() || count <= 0) return {};
    if (values.size() == 1 || count == 1) return std::vector<double>(static_cast<std::size_t>(count), py_round(values[0], 1));
    std::vector<double> out;
    const auto last = static_cast<std::int64_t>(values.size()) - 1;
    for (std::int64_t i = 0; i < count; ++i) {
        const double at = static_cast<double>(i * last) / static_cast<double>(count - 1);
        const std::int64_t lo = trunc_int(at);
        const std::int64_t hi = std::min(lo + 1, last);
        const double a = values[static_cast<std::size_t>(lo)];
        const double b = values[static_cast<std::size_t>(hi)];
        out.push_back(py_round(a + (b - a) * (at - static_cast<double>(lo)), 1));
    }
    return out;
}

NearestSegment nearest_segment(std::span<const PointF> points, double x, double y) {
    NearestSegment best{0, std::numeric_limits<double>::infinity(), x, y};
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double ax = points[i].x, ay = points[i].y, bx = points[i + 1].x, by = points[i + 1].y;
        const double dx = bx - ax, dy = by - ay;
        const double seg = dx * dx + dy * dy;
        const double t = seg == 0 ? 0.0 : clamp01(((x - ax) * dx + (y - ay) * dy) / seg);
        const double px = ax + t * dx, py = ay + t * dy;
        const double d = py_hypot(x - px, y - py);
        if (d < best.distance) best = NearestSegment{i, d, px, py};
    }
    return best;
}

std::vector<PenPoints> split_by_eraser(const PenPoints& points, const PenPoints& eraser, double radius_mm) {
    const auto near = [&](const PenPoint& p) {
        return any_segment(eraser, [&](const PenPoint& a, const PenPoint& b) {
            return segment_distance(p.x, p.y, a.x, a.y, b.x, b.y) <= radius_mm;
        });
    };
    // densify so a short eraser still cuts a long segment
    PenPoints dense;
    const double half = radius_mm / 2;
    const double spacing = half > 0.2 ? half : 0.2;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const PenPoint& a = points[i];
        const PenPoint& b = points[i + 1];
        const double dist = py_hypot(b.x - a.x, b.y - a.y);
        const std::int64_t steps = std::max<std::int64_t>(1, loop_count(dist / spacing));
        for (std::int64_t k = 0; k < steps; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(steps);
            PenPoint q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, std::nullopt};
            if (a.p) q.p = *a.p + ((b.p ? *b.p : *a.p) - *a.p) * t;
            dense.push_back(q);
        }
    }
    if (!points.empty()) dense.push_back(points.back());
    std::vector<PenPoints> pieces;
    PenPoints current;
    for (const PenPoint& p : dense) {
        if (near(p)) {
            if (current.size() >= 2) pieces.push_back(current);
            current.clear();
        } else {
            current.push_back(p);
        }
    }
    if (current.size() >= 2) pieces.push_back(current);
    return pieces;
}

bool untouched(const Stroke& stroke, const PenPoints& eraser, double radius_mm) {
    for (const PointF& p : stroke.points) {
        const bool hit = any_segment(eraser, [&](const PenPoint& a, const PenPoint& b) {
            return segment_distance(p.x, p.y, a.x, a.y, b.x, b.y) <= radius_mm;
        });
        if (hit) return false;
    }
    return true;
}

std::vector<StrokePtr> erase_to_crossing(const std::vector<StrokePtr>& strokes, const PenPoints& eraser_in,
                                         double radius_mm) {
    const auto arc_positions = [](const std::vector<PointF>& pts) {
        std::vector<double> out{0.0};
        double total = 0.0;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            total += py_dist(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
            out.push_back(total);
        }
        return out;
    };
    // the eraser's path, walked in small steps (a quick drag leaves far-apart points)
    const double half = radius_mm / 2;
    const double step = half > 0.05 ? half : 0.05;
    std::vector<PointF> eraser;
    if (!eraser_in.empty()) eraser.push_back(PointF{eraser_in[0].x, eraser_in[0].y});
    for (std::size_t i = 0; i + 1 < eraser_in.size(); ++i) {
        const PenPoint& a = eraser_in[i];
        const PenPoint& b = eraser_in[i + 1];
        const auto n = std::max<std::int64_t>(1, loop_count(std::ceil(py_dist(a.x, a.y, b.x, b.y) / step)));
        for (std::int64_t k = 1; k <= n; ++k) {
            const double kd = static_cast<double>(k), nd = static_cast<double>(n);
            eraser.push_back(PointF{a.x + (b.x - a.x) * kd / nd, a.y + (b.y - a.y) * kd / nd});
        }
    }
    std::vector<StrokePtr> result;
    for (const StrokePtr& stroke : strokes) {
        const std::vector<PointF>& pts = stroke->points;
        if (pts.size() < 2) {
            result.push_back(stroke);
            continue;
        }
        const std::vector<double> pos = arc_positions(pts);
        // where the eraser touches this line (arc length)
        std::optional<double> touch;
        for (std::size_t i = 0; i + 1 < pts.size() && !touch; ++i) {
            const PointF& a = pts[i];
            const PointF& b = pts[i + 1];
            for (const PointF& e : eraser) {
                const double dx = b.x - a.x, dy = b.y - a.y;
                const double seg = dx * dx + dy * dy;
                const double t = seg == 0 ? 0.0 : clamp01(((e.x - a.x) * dx + (e.y - a.y) * dy) / seg);
                if (py_hypot(e.x - (a.x + t * dx), e.y - (a.y + t * dy)) <= radius_mm) {
                    touch = pos[i] + t * (pos[i + 1] - pos[i]);
                    break;
                }
            }
        }
        if (!touch) {
            result.push_back(stroke);
            continue;
        }
        std::vector<double> crossings;
        for (const StrokePtr& other : strokes) {
            if (other.get() == stroke.get()) continue;
            const std::vector<PointF>& ops = other->points;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                for (std::size_t j = 0; j + 1 < ops.size(); ++j) {
                    if (const auto t = seg_cross(pts[i], pts[i + 1], ops[j], ops[j + 1])) {
                        crossings.push_back(pos[i] + *t * (pos[i + 1] - pos[i]));
                    }
                }
            }
        }
        // max([c for c in crossings if c < touch], default=0.0), min([… > touch], default=pos[-1])
        std::optional<double> before_value, after_value;
        for (const double c : crossings) {
            if (c < *touch && (!before_value || c > *before_value)) before_value = c;
            if (c > *touch && (!after_value || c < *after_value)) after_value = c;
        }
        const double before = before_value.value_or(0.0);
        const double after = after_value.value_or(pos.back());
        const auto piece = [&](double lo, double hi) {
            std::vector<PointF> out;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                const double la = pos[i], lb = pos[i + 1];
                if (lb < lo || la > hi || lb == la) continue;
                const double v0 = (lo - la) / (lb - la);
                const double v1 = (hi - la) / (lb - la);
                const double t0 = v0 > 0.0 ? v0 : 0.0;
                const double t1 = v1 < 1.0 ? v1 : 1.0;
                const PointF& a = pts[i];
                const PointF& b = pts[i + 1];
                const PointF p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0};
                const PointF p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
                if (out.empty()) out.push_back(p0);
                out.push_back(p1);
            }
            return out;
        };
        for (const auto& [lo, hi] : {std::pair<double, double>{0.0, before}, std::pair<double, double>{after, pos.back()}}) {
            const std::vector<PointF> part = piece(lo, hi);
            if (part.size() >= 2 && py_dist(part.front().x, part.front().y, part.back().x, part.back().y) > 0.2) {
                PenPoints as_pen;
                for (const PointF& p : part) as_pen.push_back(PenPoint{p.x, p.y, std::nullopt});
                Stroke fresh = coerce_stroke(as_pen);
                fresh.width_mm = stroke->width_mm;
                fresh.kind = stroke->kind;
                fresh.rgb = stroke->rgb;
                fresh.opacity = stroke->opacity;
                result.push_back(std::make_shared<const Stroke>(std::move(fresh)));
            }
        }
    }
    return result;
}

}  // namespace genko::core
