// The canvas's other drawing tools (M3④-2, Python's PageCanvas and ShapeSelectMixin): スポイト (the colour under the
// pointer — as seen, or the drawing layer's own), 塗りつぶし (a click), 囲って塗る (a drag around the area),
// グラデーション (a drag from where to where; Shift keeps it straight or at 45°: the drag shared with レイヤー移動),
// 図形 (a line, rectangle, ellipse or polygon dragged — Shift: 45° steps and squares —, a polyline or curve clicked
// point by point and ended by a double click or Enter, Shift+Enter closing it) and 色混ぜ / ゆがみ (lines drawn as with
// the pen; the window makes them smudge and liquify ops).

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/raster_ops.hpp"

namespace genko::app {

namespace {

constexpr double kPi = 3.14159265358979323846;

double r3(double v) { return core::py_round(v, 3); }

core::Json point_json(const QPointF& p) { return core::Json::array({r3(p.x()), r3(p.y())}); }

}  // namespace

bool PageCanvas::is_stroke_tool(const QString& tool) {
    return tool == QLatin1String("pen") || tool == QLatin1String("eraser") || tool == QLatin1String("blend") || tool == QLatin1String("liquify");
}

bool PageCanvas::tool_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (tool_ == QLatin1String("text")) return text_press(mm);  // (テキスト: canvas_lines.cpp)
    if (tool_ == QLatin1String("vector")) {
        vector_press(mm.x(), mm.y(), modifiers);
        return true;
    }
    if (tool_ == QLatin1String("reshape")) {
        reshape_press(mm.x(), mm.y());
        return true;
    }
    if (tool_ == QLatin1String("ruler")) {
        modifiers_ = modifiers;
        ruler_press(pos);
        return true;
    }
    if (tool_ == QLatin1String("3d")) {
        prim_press(pos);
        return true;
    }
    if (tool_ == QLatin1String("effect")) {
        effect_press(pos);
        return true;
    }
    if (tool_ == QLatin1String("stamp")) {
        emit stampRequested(mm.x(), mm.y());
        return true;
    }
    if (tool_ == QLatin1String("shape")) {
        modifiers_ = modifiers;
        if (shape_kind == QLatin1String("polyline") || shape_kind == QLatin1String("curve")) {
            shape_pts_.push_back(mm);
        } else {
            shape_drag_ = std::make_pair(mm, mm);
        }
        update();
        return true;
    }
    if (tool_ == QLatin1String("gradient")) {
        tool_drag_ = std::make_pair(mm, mm);
        update();
        return true;
    }
    if (tool_ == QLatin1String("picker")) {
        pick_colour(pos);
        return true;
    }
    if (tool_ == QLatin1String("fill")) {
        emit fillRequested(mm.x(), mm.y());
        return true;
    }
    if (tool_ == QLatin1String("lassofill")) {
        lasso_fill_ = {mm};
        update();
        return true;
    }
    return false;
}

bool PageCanvas::tool_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed) {
    if (line_move(mm, modifiers)) return true;  // (a balloon or one of its handles dragged: canvas_lines.cpp)
    if (text_move(mm, pressed)) return true;
    if (effect_move(mm)) return true;
    if ((vector_drag_ || vector_trace_) && vector_move(mm.x(), mm.y())) return true;
    if (reshape_) {
        reshape_move(mm.x(), mm.y());
        return true;
    }
    if (shape_drag_) {
        modifiers_ = modifiers;
        shape_drag_->second = constrained(shape_drag_->first, mm, modifiers);
        update();
        return true;
    }
    if (!lasso_fill_.empty() && pressed) {
        lasso_fill_.push_back(mm);
        update();
        return true;
    }
    return false;
}

bool PageCanvas::tool_release() {
    if (text_release()) return true;
    if (line_release()) return true;
    if (effect_release()) return true;
    if (vector_release()) return true;
    if (reshape_release()) return true;
    if (shape_drag_) {
        const auto [a, b] = *shape_drag_;
        shape_drag_.reset();
        if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 0.5) {
            update();
            return true;
        }
        core::Json shape;
        if (shape_kind == QLatin1String("line")) {
            shape = core::Json{{"shape", "line"}, {"points", core::Json::array({point_json(a), point_json(b)})}};
        } else {
            const core::Json box = core::Json::array({r3(std::min(a.x(), b.x())), r3(std::min(a.y(), b.y())), r3(std::abs(b.x() - a.x())),
                                                      r3(std::abs(b.y() - a.y()))});
            shape = core::Json{{"shape", shape_kind.toStdString()}, {"box", box}};
            if (shape_kind == QLatin1String("polygon")) shape["sides"] = shape_sides;
        }
        emit shapeDrawn(shape);
        update();
        return true;
    }
    if (!lasso_fill_.empty()) {
        std::vector<QPointF> points;
        std::swap(points, lasso_fill_);
        double reach = 0;
        for (const QPointF& p : points) reach = std::max(reach, std::hypot(p.x() - points[0].x(), p.y() - points[0].y()));
        if (points.size() >= 3 && reach > 1.0) emit areaFilled(QVector<QPointF>(points.begin(), points.end()));
        update();
        return true;
    }
    return false;
}

QPointF PageCanvas::constrained(const QPointF& a, const QPointF& b, Qt::KeyboardModifiers modifiers) const {
    // Shift: lines at steps of 45°, figures square
    if (!(modifiers & Qt::ShiftModifier)) return b;
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    if (shape_kind == QLatin1String("line") && tool_ != QLatin1String("marquee")) {
        const double angle = core::py_round_whole(std::atan2(dy, dx) / (kPi / 4)) * (kPi / 4);
        const double length = std::hypot(dx, dy);
        return QPointF(a.x() + length * std::cos(angle), a.y() + length * std::sin(angle));
    }
    const double side = std::max(std::abs(dx), std::abs(dy));
    return QPointF(a.x() + std::copysign(side, dx != 0.0 ? dx : 1.0), a.y() + std::copysign(side, dy != 0.0 ? dy : 1.0));
}

bool PageCanvas::finish_shape(bool closed) {
    std::vector<QPointF> points;
    std::swap(points, shape_pts_);
    update();
    if (points.size() < 2) return false;
    core::Json list = core::Json::array();
    for (const QPointF& p : points) list.push_back(point_json(p));
    core::Json shape{{"shape", shape_kind.toStdString()}, {"points", list}};
    if (closed) shape["closed"] = true;
    emit shapeDrawn(shape);
    return true;
}

bool PageCanvas::cancel_shape() {
    if (shape_pts_.empty() && !shape_drag_) return false;
    shape_pts_.clear();
    shape_drag_.reset();
    update();
    return true;
}

void PageCanvas::pick_colour(const QPointF& pos) {
    const core::Page* p = page();
    if (p == nullptr) return;
    const QPointF mm = mm_of(pos);
    if (pick_source == QLatin1String("layer") && layer_colour_at) {
        if (const auto rgb = layer_colour_at(mm.x(), mm.y())) emit colourPicked(*rgb);
        return;
    }
    if (const auto colour = renderer_->pixel_at(mm.x(), mm.y())) emit colourPicked({colour->red(), colour->green(), colour->blue()});
}

std::vector<QPointF> PageCanvas::shape_preview(bool& closed) const {
    // the figure being drawn, as the points of its outline (mm)
    closed = false;
    if (shape_drag_) {
        const auto [a, b] = *shape_drag_;
        if (shape_kind == QLatin1String("line")) return {a, b};
        try {
            const auto outline = render::shape_points(
                shape_kind.toStdString(),
                core::Json{{"box", core::Json::array({std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::abs(b.x() - a.x()) != 0.0 ? std::abs(b.x() - a.x()) : 0.01,
                                                      std::abs(b.y() - a.y()) != 0.0 ? std::abs(b.y() - a.y()) : 0.01})},
                           {"sides", shape_sides}});
            std::vector<QPointF> out;
            for (const auto& q : outline.points) out.emplace_back(q[0], q[1]);
            if (outline.closed && !out.empty()) out.push_back(out.front());
            return out;
        } catch (const core::Error&) {
            return {};
        }
    }
    if (!shape_pts_.empty()) {
        std::vector<QPointF> points = shape_pts_;
        if (hover_) points.push_back(*hover_);
        if (shape_kind == QLatin1String("curve") && points.size() >= 3) {
            core::Json list = core::Json::array();
            for (const QPointF& p : points) list.push_back(core::Json::array({p.x(), p.y()}));
            try {
                const auto outline = render::shape_points("curve", core::Json{{"points", list}});
                std::vector<QPointF> out;
                for (const auto& q : outline.points) out.emplace_back(q[0], q[1]);
                return out;
            } catch (const core::Error&) {
                return points;
            }
        }
        return points;
    }
    return {};
}

void PageCanvas::draw_tools(QPainter& painter) const {
    draw_vector(painter);  // (線の編集 and 線の修正: canvas_vector.cpp)
    // the area being drawn around to fill (囲って塗る)
    if (!lasso_fill_.empty()) {
        painter.setPen(QPen(theme::accent(), 1.5, Qt::DashLine));
        painter.setBrush(QColor(232, 89, 12, 40));
        draw_polygon_mm(painter, lasso_fill_);
        painter.setBrush(Qt::NoBrush);
    }
    // the gradient's direction
    if (tool_drag_ && tool_ == QLatin1String("gradient")) {
        const QPointF s = pt(tool_drag_->first.x(), tool_drag_->first.y());
        const QPointF e = pt(tool_drag_->second.x(), tool_drag_->second.y());
        painter.setPen(QPen(theme::accent(), 2));
        painter.drawLine(s, e);
        painter.setBrush(theme::accent());
        painter.drawEllipse(s, 4, 4);
        painter.setBrush(QColor(Qt::white));
        painter.drawEllipse(e, 4, 4);
        painter.setBrush(Qt::NoBrush);
    }
    // the figure being drawn
    bool closed = false;
    const std::vector<QPointF> outline = shape_preview(closed);  // (whatever the tool now: a figure still pending shows)
    if (!outline.empty()) {
        painter.setPen(QPen(theme::accent(), 1.5));
        painter.setBrush(Qt::NoBrush);
        QPolygonF line;
        for (const QPointF& p : outline) line << pt(p.x(), p.y());
        painter.drawPolyline(line);
        for (const QPointF& p : shape_pts_) painter.drawEllipse(pt(p.x(), p.y()), 3, 3);
    }
}

}  // namespace genko::app
