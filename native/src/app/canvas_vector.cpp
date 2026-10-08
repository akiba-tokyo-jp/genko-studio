// The canvas's line editing (M3④-3, Python's canvas_vector.VectorMixin and PageCanvas._reshape_*): 線の編集 — choose a
// line of the layer drawn on (Shift: a second one), see its control points, drag one, Alt+click on it to add one,
// Delete to take the point (or the chosen lines) away, cut a line where it is clicked, or trace over lines to mend
// them (widen, narrow, redraw, join, simplify: trace_edit) — and 線の修正（つまむ）: pinch a line anywhere along it and
// the part around bends smoothly.

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"

namespace genko::app {

namespace {

constexpr double kPickPx = 8.0;

double r3(double v) { return core::py_round(v, 3); }

// ops._nearest_segment: (the segment's first point, the distance, the nearest point on it).
std::tuple<std::size_t, double, QPointF> nearest_segment(const std::vector<core::PointF>& points, double x, double y) {
    std::tuple<std::size_t, double, QPointF> best{0, std::numeric_limits<double>::infinity(), QPointF(x, y)};
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double ax = points[i].x, ay = points[i].y, bx = points[i + 1].x, by = points[i + 1].y;
        const double dx = bx - ax, dy = by - ay;
        const double seg = dx * dx + dy * dy;
        const double t = seg == 0 ? 0.0 : core::py_clamp(((x - ax) * dx + (y - ay) * dy) / seg, 0.0, 1.0);
        const double px = ax + t * dx, py = ay + t * dy;
        const double d = std::hypot(x - px, y - py);
        if (d < std::get<1>(best)) best = {i, d, QPointF(px, py)};
    }
    return best;
}

}  // namespace

std::vector<core::StrokePtr> PageCanvas::vector_strokes() const {
    const core::Layer* layer = drawing_layer ? drawing_layer() : nullptr;
    if (layer == nullptr || !layer->strokes) return {};
    return layer->strokes->items;
}

core::StrokePtr PageCanvas::vector_hit(double x, double y) const {
    const double reach = kPickPx / std::max(0.01, view_.scale);
    core::StrokePtr best;
    double best_d = reach;
    for (const core::StrokePtr& stroke : vector_strokes()) {
        if (stroke->points.size() < 2) continue;
        const double d = std::get<1>(nearest_segment(stroke->points, x, y));
        if (d < best_d) {
            best = stroke;
            best_d = d;
        }
    }
    return best;
}

std::optional<int> PageCanvas::vector_point_hit(double x, double y) const {
    if (vector_ids.empty()) return std::nullopt;
    const double reach = kPickPx / std::max(0.01, view_.scale);
    for (const core::StrokePtr& stroke : vector_strokes()) {
        if (stroke->id != vector_ids.front()) continue;
        for (std::size_t k = 0; k < stroke->points.size(); ++k) {
            if (std::hypot(stroke->points[k].x - x, stroke->points[k].y - y) <= reach) return static_cast<int>(k);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void PageCanvas::vector_press(double x, double y, Qt::KeyboardModifiers modifiers) {
    if (vector_mode != QLatin1String("edit")) {  // (a trace: the lines near it are mended when it ends)
        vector_trace_ = {{x, y, last_pressure_ > 0 ? last_pressure_ : 0.7}};
        update();
        return;
    }
    const auto point = vector_point_hit(x, y);
    if (point && !vector_cut) {
        vector_point = point;
        vector_drag_ = VectorDrag{vector_ids.front(), *point, QPointF(x, y)};
        update();
        return;
    }
    const core::StrokePtr hit = vector_hit(x, y);
    if (!hit) {
        if (!(modifiers & Qt::ShiftModifier)) {
            vector_ids.clear();
            vector_point.reset();
        }
        update();
        return;
    }
    if (vector_cut) {
        emit vectorEdited(core::Json{{"action", "cut"}, {"stroke_id", hit->id}, {"at", core::Json::array({r3(x), r3(y)})}});
        vector_ids.clear();
        vector_point.reset();
        return;
    }
    if ((modifiers & Qt::AltModifier) && !vector_ids.empty() && hit->id == vector_ids.front()) {
        emit vectorEdited(core::Json{{"action", "add_point"}, {"stroke_id", hit->id}, {"at", core::Json::array({r3(x), r3(y)})}});
        return;
    }
    if (modifiers & Qt::ShiftModifier) {
        const auto found = std::find(vector_ids.begin(), vector_ids.end(), hit->id);
        if (found != vector_ids.end()) {
            vector_ids.erase(found);
        } else {
            vector_ids.push_back(hit->id);
        }
    } else {
        vector_ids = {hit->id};
    }
    vector_point.reset();
    update();
}

bool PageCanvas::vector_move(double x, double y) {
    if (vector_trace_) {
        const auto& last = vector_trace_->back();
        if (std::hypot(last[0] - x, last[1] - y) >= 0.2) vector_trace_->push_back({x, y, last_pressure_ > 0 ? last_pressure_ : 0.7});
        update();
        return true;
    }
    if (!vector_drag_) return false;
    vector_drag_->to = QPointF(x, y);
    update();
    return true;
}

bool PageCanvas::vector_release() {
    if (vector_trace_) {
        std::vector<std::array<double, 3>> trace;
        std::swap(trace, *vector_trace_);
        vector_trace_.reset();
        if (trace.size() >= 2) {
            core::Json points = core::Json::array();
            for (const auto& [x, y, p] : trace) points.push_back(core::Json::array({r3(x), r3(y), r3(p)}));
            emit vectorTraced(points, vector_mode);
        }
        update();
        return true;
    }
    if (!vector_drag_) return false;
    const VectorDrag drag = *vector_drag_;
    vector_drag_.reset();
    emit vectorEdited(core::Json{{"action", "move_point"}, {"stroke_id", drag.id}, {"index", drag.index},
                                 {"to", core::Json::array({r3(drag.to.x()), r3(drag.to.y())})}});
    return true;
}

bool PageCanvas::vector_delete() {
    // Delete: the chosen point, else the chosen lines
    if (vector_ids.empty()) return false;
    if (vector_point) {
        emit vectorEdited(core::Json{{"action", "delete_point"}, {"stroke_id", vector_ids.front()}, {"index", *vector_point}});
        vector_point.reset();
    } else {
        core::Json ids = core::Json::array();
        for (const std::string& id : vector_ids) ids.push_back(id);
        emit vectorEdited(core::Json{{"action", "delete"}, {"ids", ids}});
        vector_ids.clear();
    }
    update();
    return true;
}

void PageCanvas::draw_vector(QPainter& painter) const {
    if (tool_ == QLatin1String("vector") && vector_trace_ && !vector_trace_->empty()) {
        const double reach = std::max(2.0, vector_radius_mm * view_.scale);
        painter.setPen(QPen(QColor(28, 126, 214, 90), reach * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF line;
        for (const auto& p : *vector_trace_) line << pt(p[0], p[1]);
        painter.drawPolyline(line);
    }
    if (reshape_) {
        painter.setPen(QPen(theme::accent(), 2));
        painter.setBrush(Qt::NoBrush);
        QPainterPath path(pt(reshape_->points.front()[0], reshape_->points.front()[1]));
        for (std::size_t i = 1; i < reshape_->points.size(); ++i) path.lineTo(pt(reshape_->points[i][0], reshape_->points[i][1]));
        painter.drawPath(path);
    }
    if (tool_ != QLatin1String("vector") || vector_ids.empty()) return;
    const auto strokes = vector_strokes();
    for (std::size_t n = 0; n < vector_ids.size(); ++n) {
        const auto found = std::find_if(strokes.begin(), strokes.end(), [&](const core::StrokePtr& s) { return s->id == vector_ids[n]; });
        if (found == strokes.end()) continue;
        std::vector<QPointF> points;
        for (const core::PointF& p : (*found)->points) points.emplace_back(p.x, p.y);
        if (vector_drag_ && vector_drag_->id == vector_ids[n] && vector_drag_->index >= 0 &&
            static_cast<std::size_t>(vector_drag_->index) < points.size())
            points[static_cast<std::size_t>(vector_drag_->index)] = vector_drag_->to;
        painter.setPen(QPen(QColor(QStringLiteral("#1c7ed6")), 1.5));
        painter.setBrush(Qt::NoBrush);
        QPolygonF line;
        for (const QPointF& p : points) line << pt(p.x(), p.y());
        painter.drawPolyline(line);
        if (n == 0) {  // the first chosen line shows its control points
            for (std::size_t k = 0; k < points.size(); ++k) {
                const QPointF q = pt(points[k].x(), points[k].y());
                painter.setBrush(vector_point && static_cast<std::size_t>(*vector_point) == k ? theme::accent() : QColor(Qt::white));
                painter.setPen(QPen(QColor(QStringLiteral("#1c7ed6")), 1));
                painter.drawRect(static_cast<int>(q.x()) - 3, static_cast<int>(q.y()) - 3, 6, 6);
            }
            painter.setBrush(Qt::NoBrush);
        }
    }
}

// --- 線の修正（つまむ） ---------------------------------------------------------------------------------------------

void PageCanvas::reshape_press(double x, double y) {
    // grab the nearest line (anywhere along it); it is walked in 1 mm steps so the pinch bends smoothly
    const double reach = std::max(1.0, 10 / view_.scale);  // (about 10 px on screen, at least 1 mm)
    core::StrokePtr best;
    double best_d = 0;
    for (const core::StrokePtr& stroke : vector_strokes()) {
        const auto& pts = stroke->points;
        if (pts.empty()) continue;
        const std::size_t count = pts.size() > 1 ? pts.size() - 1 : 1;
        for (std::size_t i = 0; i < count; ++i) {
            const core::PointF& a = pts[i];
            const core::PointF& b = pts.size() > 1 ? pts[i + 1] : pts[i];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double seg = dx * dx + dy * dy;
            const double t = seg == 0 ? 0.0 : core::py_clamp(((x - a.x) * dx + (y - a.y) * dy) / seg, 0.0, 1.0);
            const double d = std::hypot(x - (a.x + t * dx), y - (a.y + t * dy));
            if (d <= reach && (!best || d < best_d)) {
                best = stroke;
                best_d = d;
            }
        }
    }
    if (!best) return;
    const bool pressed = best->pressure.size() == best->points.size();
    std::vector<std::vector<double>> src;
    for (std::size_t i = 0; i < best->points.size(); ++i) {
        std::vector<double> p{best->points[i].x, best->points[i].y};
        if (pressed) p.push_back(best->pressure[i]);
        src.push_back(std::move(p));
    }
    Reshape r;
    r.id = best->id;
    r.grab = QPointF(x, y);
    r.points.push_back(src.front());
    for (std::size_t i = 0; i + 1 < src.size(); ++i) {
        const auto& a = src[i];
        const auto& b = src[i + 1];
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(b[0] - a[0], b[1] - a[1]) / 1.0)));
        for (int k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / n;
            std::vector<double> p{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
            if (a.size() > 2 && b.size() > 2) p.push_back(a[2] + (b[2] - a[2]) * t);
            r.points.push_back(std::move(p));
        }
    }
    r.along.push_back(0.0);
    for (std::size_t i = 0; i + 1 < r.points.size(); ++i)
        r.along.push_back(r.along.back() + std::hypot(r.points[i + 1][0] - r.points[i][0], r.points[i + 1][1] - r.points[i][1]));
    r.orig = r.points;
    reshape_ = std::move(r);
}

void PageCanvas::reshape_move(double x, double y) {
    Reshape& r = *reshape_;
    const double dx = x - r.grab.x(), dy = y - r.grab.y();
    const double radius = std::max(0.5, reshape_radius_mm);
    const double total = r.along.empty() ? 0.0 : r.along.back();
    for (std::size_t k = 0; k < r.orig.size(); ++k) {
        const auto& p = r.orig[k];
        double w = std::pow(std::max(0.0, 1 - std::hypot(p[0] - r.grab.x(), p[1] - r.grab.y()) / radius), 2);
        if (reshape_pin_ends && total > 0) {  // (fixed ends: the pull fades to nothing at each end)
            const double s = r.along[k], fade = std::min(radius, total / 2);
            w *= std::min({1.0, s / fade, (total - s) / fade});
        }
        r.points[k] = p;
        r.points[k][0] = p[0] + dx * w;
        r.points[k][1] = p[1] + dy * w;
    }
    update();
}

bool PageCanvas::reshape_release() {
    if (!reshape_) return false;
    const Reshape r = std::move(*reshape_);
    reshape_.reset();
    if (r.points != r.orig) {
        core::Json points = core::Json::array();
        for (const auto& p : r.points) {
            core::Json q = core::Json::array();
            for (const double v : p) q.push_back(r3(v));
            points.push_back(q);
        }
        emit strokeReshaped(QString::fromStdString(r.id), points);
    }
    update();
    return true;
}

}  // namespace genko::app
