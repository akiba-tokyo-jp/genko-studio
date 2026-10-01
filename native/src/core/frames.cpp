#include "core/frames.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/error.hpp"

namespace genko::core {

namespace {

constexpr double kEps = 1e-6;

// Python 3.12's sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:] + points[:1])) / 2
double signed_area(std::span<const Point> pts) {
    std::vector<Num> terms;
    terms.reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % pts.size()];
        terms.push_back(a.x * b.y - b.x * a.y);
    }
    return (py_sum(terms) / Num(2)).value();
}

struct EdgeNormal {
    Point mid;
    double nx = 0.0, ny = 0.0;
    Point end;
};

// Edge i (corner i → i+1): its middle, its outward normal and its end.
EdgeNormal edge_normal(std::span<const Point> pts, std::size_t i) {
    const Point& a = pts[i];
    const Point& b = pts[(i + 1) % pts.size()];
    const Num dx = b.x - a.x;
    const Num dy = b.y - a.y;
    double length = py_hypot(dx.value(), dy.value());
    if (length == 0.0) length = 1.0;
    Num nx = dy / Num(length);
    Num ny = -dx / Num(length);
    if (signed_area(pts) < 0) {
        nx = -nx;
        ny = -ny;
    }
    EdgeNormal out;
    out.mid = Point{(a.x + b.x) / Num(2), (a.y + b.y) / Num(2)};
    out.nx = nx.value();
    out.ny = ny.value();
    out.end = b;
    return out;
}

std::int64_t trunc_to_int(double v) { return static_cast<std::int64_t>(std::trunc(v)); }

// Each corner cut back along both edges and joined by a round (frames._round_corners, with no corners kept).
std::vector<Point> round_corners(std::span<const Point> pts, double radius) {
    std::vector<Point> out;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point& p = pts[i];
        const Point& a = pts[(i + n - 1) % n];
        const Point& b = pts[(i + 1) % n];
        const double la = py_dist(p.x.value(), p.y.value(), a.x.value(), a.y.value());
        const double lb = py_dist(p.x.value(), p.y.value(), b.x.value(), b.y.value());
        if (la < kEps || lb < kEps) {
            out.push_back(p);
            continue;
        }
        const double ux = ((a.x - p.x) / Num(la)).value();
        const double uy = ((a.y - p.y) / Num(la)).value();
        const double vx = ((b.x - p.x) / Num(lb)).value();
        const double vy = ((b.y - p.y) / Num(lb)).value();
        const double cos = std::max(-1.0, std::min(1.0, ux * vx + uy * vy));
        const double half = py_acos(cos) / 2;
        if (half < 1e-3 || half > kPi / 2 - 1e-3) {
            out.push_back(p);
            continue;
        }
        const double t = std::min(std::min(radius / py_tan(half), la / 2), lb / 2);  // never past an edge's middle
        const double r = t * py_tan(half);
        const double s0x = (p.x + Num(ux * t)).value(), s0y = (p.y + Num(uy * t)).value();
        const double s1x = (p.x + Num(vx * t)).value(), s1y = (p.y + Num(vy * t)).value();
        const double bisx = ux + vx, bisy = uy + vy;
        double bl = py_hypot(bisx, bisy);
        if (bl == 0.0) bl = 1.0;
        const double d = r / py_sin(half);
        const double cx = (p.x + Num(bisx / bl * d)).value();
        const double cy = (p.y + Num(bisy / bl * d)).value();
        const double a0 = py_atan2(s0y - cy, s0x - cx);
        const double a1 = py_atan2(s1y - cy, s1x - cx);
        const double sweep = py_fmod(a1 - a0 + kPi, 2 * kPi) - kPi;
        const std::int64_t steps = std::max<std::int64_t>(3, trunc_to_int(std::fabs(sweep) * r / 0.6));
        for (std::int64_t k = 0; k <= steps; ++k) {
            const double angle = a0 + sweep * static_cast<double>(k) / static_cast<double>(steps);
            out.push_back(Point{Num(cx + r * py_cos(angle)), Num(cy + r * py_sin(angle))});
        }
    }
    return out;
}

}  // namespace

std::vector<Point> corners(const Rect& rect) {
    return {Point{rect.x, rect.y}, Point{rect.x + rect.width, rect.y}, Point{rect.x + rect.width, rect.y + rect.height},
            Point{rect.x, rect.y + rect.height}};
}

std::vector<Point> shape(const Frame& frame) {
    if (frame.poly && !frame.poly->empty()) {
        std::vector<Point> out;
        out.reserve(frame.poly->size());
        for (const Point& p : *frame.poly) out.push_back(Point{Num(p.x.value()), Num(p.y.value())});
        return out;
    }
    return corners(frame.rect);
}

std::pair<double, double> centroid(std::span<const Point> points) {
    if (points.empty()) throw Error("value", "division by zero");
    std::vector<Num> xs, ys;
    xs.reserve(points.size());
    ys.reserve(points.size());
    for (const Point& p : points) {
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    const Num n(static_cast<std::int64_t>(points.size()));
    return {(py_sum(xs) / n).value(), (py_sum(ys) / n).value()};
}

std::optional<std::vector<double>> curves_of(const Frame& frame, std::span<const Point> points) {
    if (!frame.curves || frame.curves->empty() || frame.curves->size() != points.size()) return std::nullopt;
    const bool bows = std::any_of(frame.curves->begin(), frame.curves->end(),
                                  [](double c) { return std::fabs(c) > 1e-6; });
    if (!bows) return std::nullopt;
    return *frame.curves;
}

bool rounded(const Frame& frame) {
    const std::vector<Point> pts = shape(frame);
    return curves_of(frame, pts).has_value() || frame.corner_mm > 0;
}

std::vector<Point> outline(const Frame& frame, double step_mm) {
    const std::vector<Point> pts = shape(frame);
    const auto curves = curves_of(frame, pts);
    if (!curves) {
        const double radius = frame.corner_mm;
        return radius > 0 ? round_corners(pts, radius) : pts;
    }
    std::vector<Point> out;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point& a = pts[i];
        out.push_back(a);
        const double bow = (*curves)[i];
        if (std::fabs(bow) < 1e-6) continue;
        const EdgeNormal e = edge_normal(pts, i);
        const Point& b = e.end;
        // (a quadratic curve's middle is half way to its control point)
        const double cx = (e.mid.x + Num(e.nx * 2 * bow)).value();
        const double cy = (e.mid.y + Num(e.ny * 2 * bow)).value();
        const double length = py_dist(a.x.value(), a.y.value(), b.x.value(), b.y.value());
        const std::int64_t steps = std::max<std::int64_t>(4, trunc_to_int(length / step_mm));
        for (std::int64_t k = 1; k < steps; ++k) {
            const double t = (Num(k) / Num(steps)).value();
            const double w0 = py_pow(1 - t, 2);
            const double x = w0 * a.x.value() + 2 * (1 - t) * t * cx + t * t * b.x.value();
            const double y = w0 * a.y.value() + 2 * (1 - t) * t * cy + t * t * b.y.value();
            out.push_back(Point{Num(x), Num(y)});
        }
    }
    return out;
}

bool contains(const Frame& frame, const Num& x, const Num& y) {
    if (!(frame.poly && !frame.poly->empty()) && !rounded(frame)) return frame.rect.contains(x, y);
    bool inside = false;
    const std::vector<Point> pts = outline(frame);
    if (pts.empty()) return false;
    std::size_t j = pts.size() - 1;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point& pi = pts[i];
        const Point& pj = pts[j];
        if ((pi.y > y) != (pj.y > y)) {
            Num dy = pj.y - pi.y;
            if (!dy.truthy()) dy = Num(kEps);
            if (x < (pj.x - pi.x) * (y - pi.y) / dy + pi.x) inside = !inside;
        }
        j = i;
    }
    return inside;
}

std::vector<const Frame*> reading_order(std::vector<const Frame*> children, bool right_to_left) {
    struct Item {
        const Frame* frame;
        double cx;
        double cy;
    };
    std::vector<Item> rest;
    rest.reserve(children.size());
    for (const Frame* f : children) {
        const auto pts = shape(*f);
        const auto [x, y] = centroid(pts);
        rest.push_back(Item{f, x, y});
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Item& a, const Item& b) { return a.cy < b.cy; });
    std::vector<const Frame*> out;
    while (!rest.empty()) {
        const Item first = rest.front();
        const Num fh = first.frame->rect.height;
        std::vector<Item> row;
        std::vector<Item> left;
        for (const Item& c : rest) {
            const Num smaller = c.frame->rect.height < fh ? c.frame->rect.height : fh;  // Python's min(fh, h)
            if (std::fabs(c.cy - first.cy) < (smaller / Num(2)).value()) {
                row.push_back(c);
            } else {
                left.push_back(c);
            }
        }
        // (Python loops for ever when the first panel is not in its own row, which only a panel with no height
        // can cause; it is taken alone instead.)
        if (row.empty()) {
            row.push_back(first);
            left.erase(left.begin());
        }
        std::stable_sort(row.begin(), row.end(), [right_to_left](const Item& a, const Item& b) {
            return right_to_left ? a.cx > b.cx : a.cx < b.cx;
        });
        for (const Item& c : row) out.push_back(c.frame);
        rest = std::move(left);
    }
    return out;
}

}  // namespace genko::core
