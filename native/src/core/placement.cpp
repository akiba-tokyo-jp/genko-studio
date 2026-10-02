#include "core/placement.hpp"

#include <cmath>
#include <utility>

#include "core/frames.hpp"

namespace genko::core {

OuterEdges outer_edges(const Page& page, const Frame& frame) {
    const Rect inner = page.inner_rect_mm();
    const Rect& r = frame.rect;
    const Num eps(kEdgeEpsMm);
    OuterEdges out;
    out.left = py_abs(r.x - inner.x) < eps;
    out.top = py_abs(r.y - inner.y) < eps;
    out.right = py_abs((r.x + r.width) - (inner.x + inner.width)) < eps;
    out.bottom = py_abs((r.y + r.height) - (inner.y + inner.height)) < eps;
    return out;
}

std::optional<std::vector<PointF>> bleed_poly(const Page& page, const Frame* frame) {
    if (frame == nullptr || !frame->bleed || !frame->poly || frame->poly->empty()) return std::nullopt;
    const Rect inner = page.inner_rect_mm();
    const Rect bleed = page.bleed_rect_mm();
    const auto near = [](double v, const Num& edge) { return std::fabs(v - edge.value()) < kEdgeEpsMm; };
    std::vector<PointF> out;
    for (const Point& point : *frame->poly) {
        double x = point.x.value();
        double y = point.y.value();
        if (near(x, inner.x)) {
            x = bleed.x.value();
        } else if (near(x, inner.x + inner.width)) {
            x = (bleed.x + bleed.width).value();
        }
        if (near(y, inner.y)) {
            y = bleed.y.value();
        } else if (near(y, inner.y + inner.height)) {
            y = (bleed.y + bleed.height).value();
        }
        out.push_back(PointF{x, y});
    }
    return out;
}

std::optional<std::vector<Point>> bleed_outline(const Page& page, const Frame* frame, std::optional<double> beyond_mm) {
    if (frame == nullptr || !frame->bleed) return std::nullopt;
    const Rect inner = page.inner_rect_mm();
    Num far_left, far_top, far_right, far_bottom;
    if (!beyond_mm) {  // (to the bleed's edge: the panel's area)
        const Rect b = page.bleed_rect_mm();
        far_left = b.x;
        far_top = b.y;
        far_right = b.x + b.width;
        far_bottom = b.y + b.height;
    } else {  // (past the paper: its border, so the sides off the paper draw nothing)
        far_left = Num(-*beyond_mm);
        far_top = Num(-*beyond_mm);
        far_right = page.spec.width_mm + Num(*beyond_mm);
        far_bottom = page.spec.height_mm + Num(*beyond_mm);
    }
    const Num eps(kEdgeEpsMm);
    std::vector<Point> out;
    std::vector<bool> moved;
    for (const Point& p : shape(*frame)) {
        Num nx = p.x;
        Num ny = p.y;
        if (py_abs(p.x - inner.x) < eps) {
            nx = far_left;
        } else if (py_abs(p.x - (inner.x + inner.width)) < eps) {
            nx = far_right;
        }
        if (py_abs(p.y - inner.y) < eps) {
            ny = far_top;
        } else if (py_abs(p.y - (inner.y + inner.height)) < eps) {
            ny = far_bottom;
        }
        moved.push_back(!(nx == p.x && ny == p.y));
        out.push_back(Point{nx, ny});
    }
    const double radius = frame->corner_mm;
    if (radius > 0) return round_corners(out, radius, moved);
    return out;
}

bool on_bleed_edge(const Page& page, const PointF& a, const PointF& b) {
    const Rect bleed = page.bleed_rect_mm();
    const std::pair<Num, bool> sides[] = {{bleed.x, true}, {bleed.x + bleed.width, true}, {bleed.y, false},
                                          {bleed.y + bleed.height, false}};
    for (const auto& [fixed, along_x] : sides) {
        const double ca = along_x ? a.x : a.y;
        const double cb = along_x ? b.x : b.y;
        if (std::fabs(ca - fixed.value()) < kEdgeEpsMm && std::fabs(cb - fixed.value()) < kEdgeEpsMm) return true;
    }
    return false;
}

bool in_poly(std::span<const PointF> points, double x, double y) {
    bool inside = false;
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const PointF& a = points[i];
        const PointF& b = points[(i + 1) % n];
        if ((a.y > y) != (b.y > y) && x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y)) inside = !inside;
    }
    return inside;
}

Rect clip_box(const Page& page, const Frame* frame, std::string_view clip_to) {
    const Rect full{Num(0), Num(0), page.spec.width_mm, page.spec.height_mm};
    if (frame == nullptr || clip_to == "none") return full;
    const Rect& r = frame->rect;
    if (clip_to != "bleed") return r;
    const OuterEdges edges = outer_edges(page, *frame);
    const Rect bleed = page.bleed_rect_mm();  // a bleed panel runs out to the bleed (the part that is cut off)
    const Num x0 = edges.left ? bleed.x : r.x;
    const Num y0 = edges.top ? bleed.y : r.y;
    const Num x1 = edges.right ? bleed.x + bleed.width : r.x + r.width;
    const Num y1 = edges.bottom ? bleed.y + bleed.height : r.y + r.height;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace genko::core
