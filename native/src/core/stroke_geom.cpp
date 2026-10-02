#include "core/stroke_geom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>

#include "core/frames.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"

namespace genko::core {

namespace {

double dist(double ax, double ay, double bx, double by) { return py_dist(ax, ay, bx, by); }

// sum(float(item[0]) for item in part) / len(part), and the same of item[1]
double mean_x(std::span<const PenPoint> part) {
    std::vector<double> xs;
    xs.reserve(part.size());
    for (const PenPoint& p : part) xs.push_back(p.x);
    return py_float_sum(xs) / static_cast<double>(part.size());
}

double mean_y(std::span<const PenPoint> part) {
    std::vector<double> ys;
    ys.reserve(part.size());
    for (const PenPoint& p : part) ys.push_back(p.y);
    return py_float_sum(ys) / static_cast<double>(part.size());
}

}  // namespace

PenPoints stabilize_points(const PenPoints& points, std::int64_t window, bool by_speed) {
    if (window < 3 || points.size() < 3) return points;
    const std::int64_t half = std::max<std::int64_t>(1, window / 2);
    const std::size_t n = points.size();
    std::vector<std::int64_t> reach(n, half);
    if (by_speed && n > 3) {
        std::vector<double> steps;
        steps.reserve(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            steps.push_back(dist(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y));
        }
        double usual = py_median(steps);
        if (usual == 0.0) usual = 1e-6;
        for (std::size_t i = 0; i < n; ++i) {
            const double step = (steps[i == 0 ? 0 : i - 1] + steps[std::min(n - 2, i)]) / 2;
            // (half the usual step: slow; twice: quick)
            const double quick = py_clamp((step / usual - 0.5) / 1.5, 0.0, 1.0);
            reach[i] = std::max<std::int64_t>(1, py_round_int(static_cast<double>(half) * (0.5 + 1.5 * quick)));
        }
    }
    PenPoints out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // (a window that shrinks evenly at the ends keeps them where they were drawn)
        const std::int64_t k = std::min({reach[i], static_cast<std::int64_t>(i), static_cast<std::int64_t>(n - 1 - i)});
        const std::span<const PenPoint> part(points.data() + (i - static_cast<std::size_t>(k)),
                                             static_cast<std::size_t>(2 * k + 1));
        out.push_back(PenPoint{mean_x(part), mean_y(part), points[i].p});
    }
    return out;
}

PenPoints taper_points(const PenPoints& points, std::optional<double> in_mm, std::optional<double> out_mm) {
    const std::size_t n = points.size();
    if (n < 2) return points;
    std::vector<double> factors;
    factors.reserve(n);
    if (!in_mm && !out_mm) {
        const double span = py_max(1.0, static_cast<double>(n) * 0.25);
        for (std::size_t i = 0; i < n; ++i) {
            const double nearest = static_cast<double>(std::min(i, n - 1 - i));
            factors.push_back(py_min(1.0, nearest / span));
        }
    } else {
        std::vector<double> along{0.0};
        for (std::size_t i = 0; i + 1 < n; ++i) {
            along.push_back(along.back() + dist(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y));
        }
        double total = along.back();
        if (total == 0.0) total = 1e-6;
        // float(in_mm or 0): None and 0 are both 0
        double first = py_max(0.0, in_mm.value_or(0.0));
        double last = py_max(0.0, out_mm.value_or(0.0));
        if (first + last > total) {  // (a line shorter than both: each end takes its share)
            const double k = total / (first + last);
            first = first * k;
            last = last * k;
        }
        for (const double s : along) {
            double f = 1.0;
            f = py_min(f, first > 0 ? s / first : 1.0);
            f = py_min(f, last > 0 ? (total - s) / last : 1.0);
            factors.push_back(f);
        }
    }
    PenPoints out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double pressure = points[i].p.value_or(1.0);
        out.push_back(PenPoint{points[i].x, points[i].y, pressure * py_max(0.15, factors[i])});
    }
    return out;
}

PenPoints fit_curve(const PenPoints& points, double tolerance_mm, double step_mm) {
    struct P3 {
        double x, y, p;
    };
    std::vector<P3> pts;
    pts.reserve(points.size());
    for (const PenPoint& p : points) pts.push_back(P3{p.x, p.y, p.p.value_or(1.0)});
    if (pts.size() < 4 || tolerance_mm <= 0) return points;
    const auto dist_seg = [](const P3& p, const P3& a, const P3& b) {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length = dx * dx + dy * dy;
        const double t = length < 1e-12 ? 0.0 : py_clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0, 1.0);
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
        const std::int64_t steps = std::max<std::int64_t>(1, loop_count(dist(p1.x, p1.y, p2.x, p2.y) / step_mm));
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

PenPoints apply_pressure_curve(const PenPoints& points, std::string_view curve) {
    if (curve.empty() || curve == "linear") return points;
    PenPoints out;
    out.reserve(points.size());
    for (const PenPoint& point : points) {
        if (!point.p) {
            out.push_back(point);
            continue;
        }
        // (a negative float to a fractional power is a complex number in Python; -inf ** 1.8 is inf)
        if (*point.p < 0.0 && std::isfinite(*point.p)) {
            throw PyTypeError("'<' not supported between instances of 'complex' and 'float'");
        }
        const double pressure = py_max(0.05, py_min(1.0, py_pow(*point.p, 1.8)));
        out.push_back(PenPoint{point.x, point.y, pressure});
    }
    return out;
}

PenPoint pack_point(double x_mm, double y_mm, std::optional<double> pressure, double tilt) {
    double value = pressure ? py_max(0.05, py_min(1.0, *pressure)) : 0.7;
    if (tilt != 0.0) value = py_max(0.05, py_min(1.0, value * (1.0 + 0.25 * tilt)));
    return PenPoint{x_mm, y_mm, value};
}

PenPoints smoothed(const PenPoints& points, std::int64_t strength) {
    if (strength <= 0 || points.size() < 4) return points;
    const std::int64_t window = std::max<std::int64_t>(1, strength);
    const auto n = static_cast<std::int64_t>(points.size());
    PenPoints out{points.front()};
    for (std::int64_t i = 1; i < n - 1; ++i) {
        const std::int64_t lo = std::max<std::int64_t>(0, i - window);
        const std::int64_t hi = std::min<std::int64_t>(n, i + window + 1);
        const std::span<const PenPoint> part(points.data() + lo, static_cast<std::size_t>(hi - lo));
        out.push_back(PenPoint{mean_x(part), mean_y(part), points[static_cast<std::size_t>(i)].p});
    }
    out.push_back(points.back());
    return out;
}

bool near_eraser(double x, double y, const PenPoints& eraser, double radius_mm) {
    // zip(eraser, eraser[1:] or eraser): one point is a segment of no length
    if (eraser.size() == 1) return distance_to_segment(x, y, eraser[0].x, eraser[0].y, eraser[0].x, eraser[0].y) <= radius_mm;
    for (std::size_t i = 0; i + 1 < eraser.size(); ++i) {
        const PenPoint& a = eraser[i];
        const PenPoint& b = eraser[i + 1];
        if (distance_to_segment(x, y, a.x, a.y, b.x, b.y) <= radius_mm) return true;
    }
    return false;
}

std::vector<PenPoints> split_by_eraser(const PenPoints& points, const PenPoints& eraser, double radius_mm) {
    // densify so a short eraser still cuts a long segment
    PenPoints dense;
    const double spacing = py_max(0.2, radius_mm / 2);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const PenPoint& a = points[i];
        const PenPoint& b = points[i + 1];
        const double d = py_hypot(b.x - a.x, b.y - a.y);
        const std::int64_t steps = std::max<std::int64_t>(1, loop_count(d / spacing));
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
        if (near_eraser(p.x, p.y, eraser, radius_mm)) {
            if (current.size() >= 2) pieces.push_back(current);
            current.clear();
        } else {
            current.push_back(p);
        }
    }
    if (current.size() >= 2) pieces.push_back(current);
    return pieces;
}

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

std::vector<StrokePtr> erase_to_crossing(std::span<const StrokePtr> strokes, const PenPoints& eraser_in,
                                         double radius_mm) {
    const auto arc_positions = [](const std::vector<PointF>& pts) {
        std::vector<double> out{0.0};
        double total = 0.0;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            total += dist(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
            out.push_back(total);
        }
        return out;
    };
    // the eraser's path, walked in small steps (a quick drag leaves far-apart points)
    const double step = py_max(0.05, radius_mm / 2);
    std::vector<PointF> eraser;
    if (!eraser_in.empty()) eraser.push_back(PointF{eraser_in[0].x, eraser_in[0].y});
    for (std::size_t i = 0; i + 1 < eraser_in.size(); ++i) {
        const PenPoint& a = eraser_in[i];
        const PenPoint& b = eraser_in[i + 1];
        const auto n = std::max<std::int64_t>(1, loop_count(std::ceil(dist(a.x, a.y, b.x, b.y) / step)));
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
                const double t = seg == 0 ? 0.0 : py_clamp(((e.x - a.x) * dx + (e.y - a.y) * dy) / seg, 0.0, 1.0);
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
            PenPoints out;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                const double la = pos[i], lb = pos[i + 1];
                if (lb < lo || la > hi || lb == la) continue;
                const double t0 = py_max(0.0, (lo - la) / (lb - la));
                const double t1 = py_min(1.0, (hi - la) / (lb - la));
                const PointF& a = pts[i];
                const PointF& b = pts[i + 1];
                const PenPoint p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0, std::nullopt};
                const PenPoint p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1, std::nullopt};
                if (out.empty()) out.push_back(p0);
                out.push_back(p1);
            }
            return out;
        };
        for (const auto& [lo, hi] : {std::pair<double, double>{0.0, before}, std::pair<double, double>{after, pos.back()}}) {
            const PenPoints part = piece(lo, hi);
            if (part.size() >= 2 && dist(part.front().x, part.front().y, part.back().x, part.back().y) > 0.2) {
                Stroke fresh = coerce_stroke(part);
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
