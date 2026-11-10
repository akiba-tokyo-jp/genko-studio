#include "core/stroke_tools.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/command_bus.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

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
        const std::int64_t lo = py_trunc_int(at);
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
        const double t = seg == 0 ? 0.0 : py_clamp(((x - ax) * dx + (y - ay) * dy) / seg, 0.0, 1.0);
        const double px = ax + t * dx, py = ay + t * dy;
        const double d = py_hypot(x - px, y - py);
        if (d < best.distance) best = NearestSegment{i, d, px, py};
    }
    return best;
}

bool untouched(const Stroke& stroke, const PenPoints& eraser, double radius_mm) {
    for (const PointF& p : stroke.points) {
        if (near_eraser(p.x, p.y, eraser, radius_mm)) return false;
    }
    return true;
}

}  // namespace genko::core
