#include "core/frames.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

constexpr double kEps = 1e-6;

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

// Python's min(a, b): the first unless the second is smaller.
const Num& min_of(const Num& a, const Num& b) { return b < a ? b : a; }

// min(values) and max(values): the first of the smallest (largest).
Num min_all(const std::vector<Num>& values) {
    if (values.empty()) throw PyValueError("min() iterable argument is empty");
    Num out = values.front();
    for (const Num& v : values) {
        if (v < out) out = v;
    }
    return out;
}

Num max_all(const std::vector<Num>& values) {
    if (values.empty()) throw PyValueError("max() iterable argument is empty");
    Num out = values.front();
    for (const Num& v : values) {
        if (v > out) out = v;
    }
    return out;
}

double dist(const Point& a, const Point& b) { return py_dist(a.x.value(), a.y.value(), b.x.value(), b.y.value()); }

// Each corner cut back along both edges and joined by a round (frames._round_corners, with no corners kept).
std::vector<Point> round_corners(std::span<const Point> pts, double radius) {
    std::vector<Point> out;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point& p = pts[i];
        const Point& a = pts[(i + n - 1) % n];
        const Point& b = pts[(i + 1) % n];
        const double la = dist(p, a);
        const double lb = dist(p, b);
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
        const std::int64_t steps = std::max<std::int64_t>(3, loop_count(std::fabs(sweep) * r / 0.6));
        for (std::int64_t k = 0; k <= steps; ++k) {
            const double angle = a0 + sweep * static_cast<double>(k) / static_cast<double>(steps);
            out.push_back(Point{Num(cx + r * py_cos(angle)), Num(cy + r * py_sin(angle))});
        }
    }
    return out;
}

// frames._norm: a point in its box's 0..1 coordinates, rounded to 6 places.
Json norm(const Rect& rect, const Point& p) {
    const Num w = rect.width.truthy() ? rect.width : Num(1);
    const Num h = rect.height.truthy() ? rect.height : Num(1);
    return Json::array({py_round((p.x - rect.x) / w, 6).json(), py_round((p.y - rect.y) / h, 6).json()});
}

// frames._denorm: rect.x + float(q[0]) * rect.width, …
Point denorm(const Rect& rect, const Json& q) {
    const double qx = to_float(subscript(q, 0));
    const double qy = to_float(subscript(q, 1));
    return Point{rect.x + Num(qx) * rect.width, rect.y + Num(qy) * rect.height};
}

// frames._extend: the line through p0 and p1 from one side of the box to the other.
std::pair<Point, Point> extend(const Rect& rect, const Point& p0, const Point& p1) {
    const Num dx = p1.x - p0.x;
    const Num dy = p1.y - p0.y;
    if (py_abs(dx) >= py_abs(dy)) {
        const Num k = dy / (dx.truthy() ? dx : Num(kEps));
        const Num x0 = rect.x;
        const Num x1 = rect.x + rect.width;
        return {Point{x0, p0.y + (x0 - p0.x) * k}, Point{x1, p0.y + (x1 - p0.x) * k}};
    }
    const Num k = dx / (dy.truthy() ? dy : Num(kEps));
    const Num y0 = rect.y;
    const Num y1 = rect.y + rect.height;
    return {Point{p0.x + (y0 - p0.y) * k, y0}, Point{p0.x + (y1 - p0.y) * k, y1}};
}

// frames._map: a point of the box `old` at the same place in the box `fresh`.
Point map_point(const Rect& old, const Rect& fresh, const Point& p) {
    const Num sx = fresh.width / (old.width.truthy() ? old.width : Num(1));
    const Num sy = fresh.height / (old.height.truthy() ? old.height : Num(1));
    return Point{fresh.x + (p.x - old.x) * sx, fresh.y + (p.y - old.y) * sy};
}

// frames._relayout_stack: children side by side (or stacked): keep the gutters, share the new span in proportion.
void relayout_stack(Frame& node) {
    const Rect fresh = node.rect;
    const bool horizontal = node.split_axis && *node.split_axis == "horizontal";
    std::vector<Frame*> kids;
    for (Frame& child : node.children) kids.push_back(&child);
    std::stable_sort(kids.begin(), kids.end(), [horizontal](const Frame* a, const Frame* b) {
        return horizontal ? a->rect.y < b->rect.y : a->rect.x < b->rect.x;
    });
    std::vector<Num> spans, starts, gaps;
    for (const Frame* kid : kids) {
        spans.push_back(horizontal ? kid->rect.height : kid->rect.width);
        starts.push_back(horizontal ? kid->rect.y : kid->rect.x);
    }
    for (std::size_t i = 0; i + 1 < kids.size(); ++i) gaps.push_back(starts[i + 1] - (starts[i] + spans[i]));
    Num total_old = py_sum(spans);
    if (!total_old.truthy()) total_old = Num(1.0);
    const Num total_new = (horizontal ? fresh.height : fresh.width) - py_sum(gaps);
    Num pos = horizontal ? fresh.y : fresh.x;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const Num span = spans[i] * total_new / total_old;
        const Rect rect = horizontal ? Rect{fresh.x, pos, fresh.width, span} : Rect{pos, fresh.y, span, fresh.height};
        const std::vector<Point> pts = corners(rect);
        relayout(*kids[i], &pts);
        pos = pos + (span + (i < gaps.size() ? gaps[i] : Num(0)));
    }
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

Rect bbox(std::span<const Point> points) {
    std::vector<Num> xs, ys;
    for (const Point& p : points) {
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    const Num x0 = min_all(xs), y0 = min_all(ys);
    return Rect{py_round(x0, 3), py_round(y0, 3), py_round(max_all(xs) - x0, 3), py_round(max_all(ys) - y0, 3)};
}

std::vector<Point> dedupe(std::span<const Point> points) {
    std::vector<Point> out;
    for (const Point& p : points) {
        if (out.empty() || dist(p, out.back()) > 1e-4) out.push_back(p);
    }
    if (out.size() > 1 && dist(out.front(), out.back()) <= 1e-4) out.pop_back();
    return out;
}

std::optional<Rect> as_rect(std::span<const Point> points) {
    std::vector<Point> pts(points.begin(), points.end());
    if (pts.size() != 4) {
        pts = dedupe(points);
        if (pts.size() != 4) return std::nullopt;
    }
    const Rect box = bbox(pts);
    const Num tolerance(1e-3);
    for (const Point& p : pts) {
        const bool on_x = py_abs(p.x - box.x) < tolerance || py_abs(p.x - (box.x + box.width)) < tolerance;
        const bool on_y = py_abs(p.y - box.y) < tolerance || py_abs(p.y - (box.y + box.height)) < tolerance;
        if (!(on_x && on_y)) return std::nullopt;
    }
    return box;
}

void set_shape(Frame& frame, std::span<const Point> points) {
    if (const auto rect = as_rect(points)) {
        frame.rect = *rect;
        frame.poly.reset();
        return;
    }
    std::vector<Point> pts;
    for (const Point& p : dedupe(points)) pts.push_back(Point{py_round(p.x, 3), py_round(p.y, 3)});
    frame.rect = bbox(pts);
    frame.poly = std::move(pts);
}

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

double area(std::span<const Point> pts) {
    std::vector<Num> terms;
    terms.reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % pts.size()];
        terms.push_back(a.x * b.y - b.x * a.y);
    }
    return (py_abs(py_sum(terms)) / Num(2)).value();
}

std::pair<double, double> centroid(std::span<const Point> points) {
    if (points.empty()) throw PyUncaught("ZeroDivisionError", "division by zero");
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
        const double length = dist(a, b);
        const std::int64_t steps = std::max<std::int64_t>(4, loop_count(length / step_mm));
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

bool is_free(const Frame& node) { return node.split_axis && *node.split_axis == kFreeSplit; }

std::vector<Point> clip_half(std::span<const Point> points, const Point& p0, const Point& p1, bool keep_left,
                             double offset) {
    const Num dx = p1.x - p0.x;
    const Num dy = p1.y - p0.y;
    double length = py_hypot(dx.value(), dy.value());
    if (length == 0.0) length = 1.0;
    const double nx = (-dy).value() / length;  // left normal (in page coordinates, y down)
    const double ny = dx.value() / length;
    const double sign = keep_left ? 1.0 : -1.0;
    const auto side = [&](const Point& p) {
        return sign * ((p.x - p0.x).value() * nx + (p.y - p0.y).value() * ny) - offset;
    };
    const auto between = [](const Point& prev, const Point& cur, double t) {
        return Point{prev.x + Num((cur.x - prev.x).value() * t), prev.y + Num((cur.y - prev.y).value() * t)};
    };
    std::vector<Point> out;
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point& cur = points[i];
        const Point& prev = points[(i + n - 1) % n];
        const double sc = side(cur);
        const double sp = side(prev);
        if (sc >= 0) {
            if (sp < 0) out.push_back(between(prev, cur, sp / (sp - sc)));
            out.push_back(cur);
        } else if (sp >= 0) {
            out.push_back(between(prev, cur, sp / (sp - sc)));
        }
    }
    return dedupe(out);
}

std::pair<std::vector<Point>, std::vector<Point>> cut(std::span<const Point> points, const Point& p0, const Point& p1,
                                                      double gutter) {
    std::vector<Point> left = clip_half(points, p0, p1, true, gutter / 2);
    std::vector<Point> right = clip_half(points, p0, p1, false, gutter / 2);
    if (left.size() < 3 || right.size() < 3 || area(left) < 1 || area(right) < 1) {
        throw PyValueError("the cut does not cross the panel");
    }
    const bool horizontal = py_abs(p1.x - p0.x) >= py_abs(p1.y - p0.y);
    const auto ca = centroid(left);
    const auto cb = centroid(right);
    const bool first_is_left = horizontal ? ca.second < cb.second : ca.first < cb.first;
    if (first_is_left) return {std::move(left), std::move(right)};
    return {std::move(right), std::move(left)};
}

std::optional<CutLine> cut_line(const Frame& node) {
    if (!node.split || !py_truthy(*node.split) || !node.split->is_object() || !node.split->contains("a")) {
        return std::nullopt;
    }
    const Json& split = *node.split;
    CutLine line;
    line.p0 = denorm(node.rect, split["a"]);
    line.p1 = denorm(node.rect, subscript(split, "b"));
    const Json* gutter = get(split, "gutter_mm");
    line.gutter = gutter != nullptr ? to_float(*gutter) : 4.0;
    return line;
}

std::pair<Frame*, Frame*> cut_frame(Frame& node, const Point& p0, const Point& p1, double gutter) {
    auto [first, second] = cut(shape(node), p0, p1, gutter);
    // extend the line to the node's box so the stored cut does not depend on where the drag started
    const auto [a, b] = extend(node.rect, p0, p1);
    Json split = Json::object();
    split["a"] = norm(node.rect, a);
    split["b"] = norm(node.rect, b);
    split["gutter_mm"] = py_round(gutter, 3);
    node.split = std::move(split);
    const bool horizontal = py_abs(p1.x - p0.x) >= py_abs(p1.y - p0.y);
    node.split_axis = horizontal ? "horizontal" : "vertical";
    std::vector<Frame> children;
    for (const auto* pts : {&first, &second}) {
        Frame child;
        child.id = new_id();
        child.rect = bbox(*pts);
        set_shape(child, *pts);
        children.push_back(std::move(child));
    }
    node.children = std::move(children);
    return {&node.children[0], &node.children[1]};
}

std::pair<Point, Point> axis_line(const Frame& node, std::string_view axis, double ratio, double gutter, double tilt) {
    const Rect& r = node.rect;
    const Num g(gutter), q(ratio), half_tilt = Num(tilt) / Num(2);
    if (axis == "horizontal") {
        const Num y = r.y + (r.height - g) * q + g / Num(2);
        return {Point{r.x, y - half_tilt}, Point{r.x + r.width, y + half_tilt}};
    }
    const Num x = r.x + (r.width - g) * q + g / Num(2);
    return {Point{x - half_tilt, r.y}, Point{x + half_tilt, r.y + r.height}};
}

void remember_split(Frame& node) {
    if ((node.split && py_truthy(*node.split)) || node.children.size() != 2 || is_free(node)) return;
    const Frame& a = node.children[0];
    const Frame& b = node.children[1];
    const Rect r = node.rect;
    Json split = Json::object();
    if (node.split_axis && *node.split_axis == "horizontal") {
        const bool swap = b.rect.y < a.rect.y;  // (sorted by y, stable)
        const Frame& top = swap ? b : a;
        const Frame& bottom = swap ? a : b;
        const Num gutter = bottom.rect.y - (top.rect.y + top.rect.height);
        const Num y = top.rect.y + top.rect.height + gutter / Num(2);
        split["a"] = norm(r, Point{r.x, y});
        split["b"] = norm(r, Point{r.x + r.width, y});
        split["gutter_mm"] = py_round(gutter, 3).json();
    } else {
        const bool swap = b.rect.x < a.rect.x;
        const Frame& left = swap ? b : a;
        const Frame& right = swap ? a : b;
        const Num gutter = right.rect.x - (left.rect.x + left.rect.width);
        const Num x = left.rect.x + left.rect.width + gutter / Num(2);
        split["a"] = norm(r, Point{x, r.y});
        split["b"] = norm(r, Point{x, r.y + r.height});
        split["gutter_mm"] = py_round(gutter, 3).json();
    }
    node.split = std::move(split);
}

void relayout(Frame& node, const std::vector<Point>* points) {
    const Rect old = node.rect;
    if (points != nullptr) {
        if (node.children.empty() && node.custom && node.poly && !node.poly->empty()) {
            // a free-form panel keeps its form
            const Rect box = bbox(*points);
            std::vector<Point> mapped;
            for (const Point& p : *node.poly) mapped.push_back(map_point(old, box, p));
            set_shape(node, mapped);
        } else {
            set_shape(node, *points);
        }
    }
    if (node.children.empty()) return;
    if (is_free(node)) {  // drawn panels: each moves and stretches with the node, keeping its form
        for (Frame& child : node.children) {
            std::vector<Point> mapped;
            for (const Point& p : shape(child)) mapped.push_back(map_point(old, node.rect, p));
            relayout(child, &mapped);
        }
        return;
    }
    const auto line = cut_line(node);
    if (line && node.children.size() == 2) {
        auto [first, second] = cut(shape(node), line->p0, line->p1, line->gutter);
        Frame& a = node.children[0];
        Frame& b = node.children[1];
        // keep each child on its side (the order of children does not change)
        const auto ca = centroid(shape(a));
        const auto c1 = centroid(first);
        const auto c2 = centroid(second);
        if (py_dist(ca.first, ca.second, c1.first, c1.second) > py_dist(ca.first, ca.second, c2.first, c2.second)) {
            std::swap(first, second);
        }
        relayout(a, &first);
        relayout(b, &second);
        return;
    }
    relayout_stack(node);
}

void move_gutter(Frame& node, std::int64_t index, double delta, std::optional<double> gutter, double min_span) {
    if (is_free(node)) throw PyValueError("drawn panels have no gutter to move: move or reshape the panel itself");
    const auto line = cut_line(node);
    if (line && node.children.size() == 2) {
        const Point& p0 = line->p0;
        const Point& p1 = line->p1;
        const Num dx = p1.x - p0.x;
        const Num dy = p1.y - p0.y;
        double length = py_hypot(dx.value(), dy.value());
        if (length == 0.0) length = 1.0;
        const bool horizontal = py_abs(dx) >= py_abs(dy);
        // move across the line: down for a horizontal cut, right for a vertical one
        double nx = (-dy).value() / length;
        double ny = dx.value() / length;
        if ((horizontal && ny < 0) || (!horizontal && nx < 0)) {
            nx = -nx;
            ny = -ny;
        }
        const Point q0{p0.x + Num(nx * delta), p0.y + Num(ny * delta)};
        const Point q1{p1.x + Num(nx * delta), p1.y + Num(ny * delta)};
        const double new_gutter = gutter ? *gutter : line->gutter;
        std::pair<std::vector<Point>, std::vector<Point>> parts;
        try {
            parts = cut(shape(node), q0, q1, new_gutter);
        } catch (const PyValueError&) {
            throw PyValueError("the gutter would leave a panel too small");
        }
        for (const auto* pts : {&parts.first, &parts.second}) {
            const Rect box = bbox(*pts);
            if (min_of(box.width, box.height) < Num(min_span)) throw PyValueError("the gutter would leave a panel too small");
        }
        Json split = Json::object();
        split["a"] = norm(node.rect, q0);
        split["b"] = norm(node.rect, q1);
        split["gutter_mm"] = py_round(new_gutter, 3);
        node.split = std::move(split);
        relayout(node);
        return;
    }
    const bool horizontal = node.split_axis && *node.split_axis == "horizontal";
    std::vector<Frame*> kids;
    for (Frame& child : node.children) kids.push_back(&child);
    std::stable_sort(kids.begin(), kids.end(), [horizontal](const Frame* a, const Frame* b) {
        return horizontal ? a->rect.y < b->rect.y : a->rect.x < b->rect.x;
    });
    if (!(0 <= index && index < static_cast<std::int64_t>(kids.size()) - 1)) throw PyValueError("no gutter there");
    Frame& a = *kids[static_cast<std::size_t>(index)];
    Frame& b = *kids[static_cast<std::size_t>(index) + 1];
    const Rect ra = a.rect;
    const Rect rb = b.rect;
    const Num half(2);
    Rect a_rect, b_rect;
    Num small;
    if (horizontal) {
        const Num old_gap = rb.y - (ra.y + ra.height);
        const Num gap = gutter ? Num(*gutter) : old_gap;
        const Num top = ra.y, bottom = rb.y + rb.height;
        const Num cut_at = ra.y + ra.height + old_gap / half + Num(delta);
        a_rect = Rect{ra.x, top, ra.width, cut_at - gap / half - top};
        b_rect = Rect{rb.x, cut_at + gap / half, rb.width, bottom - (cut_at + gap / half)};
        small = min_of(a_rect.height, b_rect.height);
    } else {
        const Num old_gap = rb.x - (ra.x + ra.width);
        const Num gap = gutter ? Num(*gutter) : old_gap;
        const Num left = ra.x, right = rb.x + rb.width;
        const Num cut_at = ra.x + ra.width + old_gap / half + Num(delta);
        a_rect = Rect{left, ra.y, cut_at - gap / half - left, ra.height};
        b_rect = Rect{cut_at + gap / half, rb.y, right - (cut_at + gap / half), rb.height};
        small = min_of(a_rect.width, b_rect.width);
    }
    if (small < Num(min_span)) throw PyValueError("the gutter would leave a panel too small");
    const std::vector<Point> a_pts = corners(a_rect);
    const std::vector<Point> b_pts = corners(b_rect);
    relayout(a, &a_pts);
    relayout(b, &b_pts);
}

double distance_to_segment(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double seg = dx * dx + dy * dy;
    double t = 0.0;
    if (seg != 0) {
        const double v = ((px - ax) * dx + (py - ay) * dy) / seg;
        const double m = v < 1.0 ? v : 1.0;
        t = m > 0.0 ? m : 0.0;
    }
    return py_hypot(px - (ax + t * dx), py - (ay + t * dy));
}

}  // namespace genko::core
