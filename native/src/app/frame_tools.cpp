#include "app/frame_tools.hpp"

#include <algorithm>
#include <cmath>

#include "core/frames.hpp"
#include "core/pynum.hpp"

namespace genko::app {

namespace {

constexpr double kEps = 1e-6;

QPointF to_q(const core::Point& p) { return QPointF(p.x.value(), p.y.value()); }

double signed_area(const std::vector<QPointF>& pts) {
    double total = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const QPointF& a = pts[i];
        const QPointF& b = pts[(i + 1) % pts.size()];
        total += a.x() * b.y() - b.x() * a.y();
    }
    return total / 2;
}

// frames._extend: the part of the (infinite) line p0 → p1 inside the box.
std::pair<QPointF, QPointF> extend(const core::Rect& rect, const QPointF& p0, const QPointF& p1) {
    const double dx = p1.x() - p0.x();
    const double dy = p1.y() - p0.y();
    if (std::abs(dx) >= std::abs(dy)) {
        const double k = dy / (dx != 0.0 ? dx : kEps);
        const double x0 = rect.x.value();
        const double x1 = (rect.x + rect.width).value();
        return {QPointF(x0, p0.y() + (x0 - p0.x()) * k), QPointF(x1, p0.y() + (x1 - p0.x()) * k)};
    }
    const double k = dx / (dy != 0.0 ? dy : kEps);
    const double y0 = rect.y.value();
    const double y1 = (rect.y + rect.height).value();
    return {QPointF(p0.x() + (y0 - p0.y()) * k, y0), QPointF(p0.x() + (y1 - p0.y()) * k, y1)};
}

void walk(const core::Frame& node, std::vector<Gutter>& out) {
    if (node.children.empty()) return;
    if (core::is_free(node)) {
        for (const core::Frame& child : node.children) walk(child, out);
        return;
    }
    const auto line = core::cut_line(node);
    if (line && node.children.size() == 2) {
        const std::vector<core::Point> pts = core::shape(node);
        const QPointF p0 = to_q(line->p0);
        const QPointF p1 = to_q(line->p1);
        const auto [a, b] = extend(core::bbox(pts), p0, p1);
        out.push_back(Gutter{node.id, 0, a, b, line->gutter, std::abs(p1.x() - p0.x()) >= std::abs(p1.y() - p0.y())});
    } else {
        const bool horizontal = node.split_axis && *node.split_axis == "horizontal";
        std::vector<const core::Frame*> kids;
        for (const core::Frame& child : node.children) kids.push_back(&child);
        std::stable_sort(kids.begin(), kids.end(), [&](const core::Frame* a, const core::Frame* b) {
            return horizontal ? a->rect.y.value() < b->rect.y.value() : a->rect.x.value() < b->rect.x.value();
        });
        for (std::size_t i = 0; i + 1 < kids.size(); ++i) {
            const core::Rect& ra = kids[i]->rect;
            const core::Rect& rb = kids[i + 1]->rect;
            if (horizontal) {
                const double g = rb.y.value() - (ra.y + ra.height).value();
                const double y = (ra.y + ra.height).value() + g / 2;
                const double x0 = std::min(ra.x.value(), rb.x.value());
                const double x1 = std::max((ra.x + ra.width).value(), (rb.x + rb.width).value());
                out.push_back(Gutter{node.id, static_cast<int>(i), QPointF(x0, y), QPointF(x1, y), g, true});
            } else {
                const double g = rb.x.value() - (ra.x + ra.width).value();
                const double x = (ra.x + ra.width).value() + g / 2;
                const double y0 = std::min(ra.y.value(), rb.y.value());
                const double y1 = std::max((ra.y + ra.height).value(), (rb.y + rb.height).value());
                out.push_back(Gutter{node.id, static_cast<int>(i), QPointF(x, y0), QPointF(x, y1), g, false});
            }
        }
    }
    for (const core::Frame& child : node.children) walk(child, out);
}

void leaves(const core::Frame& frame, core::Binding binding, std::vector<const core::Frame*>& out) {
    if (frame.children.empty()) {
        out.push_back(&frame);
        return;
    }
    std::vector<const core::Frame*> children;
    for (const core::Frame& child : frame.children) children.push_back(&child);
    if (frame.split_axis && *frame.split_axis == "free") {
        children = core::reading_order(std::move(children), binding == core::Binding::Right);
    } else if (frame.split_axis && *frame.split_axis == "vertical" && binding == core::Binding::Right) {
        std::reverse(children.begin(), children.end());
    }
    for (const core::Frame* child : children) leaves(*child, binding, out);
}

}  // namespace

std::vector<Gutter> gutters(const core::Frame& root) {
    std::vector<Gutter> out;
    walk(root, out);
    return out;
}

EdgeNormal edge_normal(const std::vector<QPointF>& points, std::size_t i) {
    const QPointF& a = points[i];
    const QPointF& b = points[(i + 1) % points.size()];
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    double length = std::hypot(dx, dy);
    if (length == 0.0) length = 1.0;
    QPointF n(dy / length, -dx / length);
    if (signed_area(points) < 0) n = -n;
    return EdgeNormal{QPointF((a.x() + b.x()) / 2, (a.y() + b.y()) / 2), n, b};
}

std::vector<QPointF> shape_of(const core::Frame& frame) {
    std::vector<QPointF> out;
    for (const core::Point& p : core::shape(frame)) out.push_back(to_q(p));
    return out;
}

std::vector<QPointF> outline_of(const core::Frame& frame) {
    std::vector<QPointF> out;
    for (const core::Point& p : core::outline(frame)) out.push_back(to_q(p));
    return out;
}

std::vector<double> curves_of(const core::Frame& frame, std::size_t corners) {
    const std::vector<core::Point> pts = core::shape(frame);
    const auto curves = core::curves_of(frame, pts);
    if (!curves) return std::vector<double>(corners, 0.0);
    return *curves;
}

std::vector<const core::Frame*> leaves_in_reading_order(const core::Frame& root, core::Binding binding) {
    std::vector<const core::Frame*> out;
    leaves(root, binding, out);
    return out;
}

double distance_to_segment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double seg = dx * dx + dy * dy;
    const double t = seg == 0.0 ? 0.0 : std::max(0.0, std::min(1.0, ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / seg));
    return std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy));
}

core::Json frame_tree(const core::Frame& frame) {
    using core::Json;
    // round(v, 3): an int stays an int, as in Python
    const auto r3 = [](const core::Num& v) {
        const Json raw = v.json();
        return raw.is_number_integer() ? raw : Json(core::py_round(v.value(), 3));
    };
    Json node = Json::object();
    node["rect_mm"] = Json::array({r3(frame.rect.x), r3(frame.rect.y), r3(frame.rect.width), r3(frame.rect.height)});
    if (!frame.children.empty() || (frame.split_axis && *frame.split_axis == "free")) {
        node["axis"] = frame.split_axis ? Json(*frame.split_axis) : Json(nullptr);
        Json children = Json::array();
        for (const core::Frame& child : frame.children) children.push_back(frame_tree(child));
        node["children"] = std::move(children);
        if (frame.split && core::Json(*frame.split).is_object() && !frame.split->empty()) node["split"] = *frame.split;
    }
    if (frame.poly && !frame.poly->empty()) {
        Json poly = Json::array();
        for (const core::Point& p : *frame.poly) poly.push_back(Json::array({core::py_round(p.x.value(), 3), core::py_round(p.y.value(), 3)}));
        node["poly"] = std::move(poly);
    }
    if (frame.bleed) node["bleed"] = true;
    if (!frame.clip) node["clip"] = false;
    if (frame.custom) node["custom"] = true;
    if (std::abs(frame.border_mm - 0.8) > 1e-6) node["border_mm"] = frame.border_mm;
    if (frame.curves && !frame.curves->empty()) node["curves"] = Json(*frame.curves);
    if (frame.line && !frame.line->is_null() && !frame.line->empty()) node["line"] = *frame.line;
    if (frame.corner_mm != 0.0) node["corner_mm"] = frame.corner_mm;
    return node;
}

}  // namespace genko::app
