// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): the count marks over the page (the extra "count": each visible group's
// marks, a dot and its number in the group's colour and sizes) and the ものさしツール's line (the extra "ruler", while the
// tool is in hand), and the two tools — the ものさしツール ("measure": a drag draws the line, Shift by 45°; a drag of an end
// moves it, of the line moves it whole; Alt+drag from an end draws the protractor's arm) and the カウントツール ("count": a
// click puts a mark, Alt+click takes one away, a drag moves one). The window makes the ops (app/main_window_measure.cpp).

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/canvas.hpp"
#include "core/measure.hpp"

namespace genko::app {

namespace {

constexpr double kGrab = 6.0;  // (screen pixels: how near the pointer finds an end, the line or a mark)

QPointF qp(const std::array<double, 2>& p) { return QPointF(p[0], p[1]); }
std::array<double, 2> ap(const QPointF& p) { return {std::round(p.x() * 10000.0) / 10000.0, std::round(p.y() * 10000.0) / 10000.0}; }

// The point from `from` toward `to` at a multiple of 45° (Shift).
QPointF by_45(const QPointF& from, const QPointF& to) {
    const QPointF d = to - from;
    const double length = std::hypot(d.x(), d.y());
    if (length <= 0) return to;
    const double step = std::numbers::pi / 4;
    const double a = std::round(std::atan2(d.y(), d.x()) / step) * step;
    return from + QPointF(std::cos(a) * length, std::sin(a) * length);
}

double to_segment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = l2 > 0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / l2, 0.0, 1.0) : 0.0;
    const QPointF q = a + ab * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

}  // namespace

std::optional<core::RulerLine> PageCanvas::ruler_in_hand() const {
    if (measure_drag_ && measure_drag_->moved) return measure_drag_->line;
    const core::Page* p = page();
    if (p == nullptr || !p->measure || !p->measure->ruler) return std::nullopt;
    return *p->measure->ruler;
}

void PageCanvas::paint_count(QPainter& painter, const core::Page& page, const ExtraView& view) const {
    if (!page.measure || page.measure->count_groups.empty()) return;
    const QRectF seen = view.seen_mm.adjusted(-20, -20, 20, 20);
    const QTransform screen = painter.transform();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t g = 0; g < page.measure->count_groups.size(); ++g) {
        const core::CountGroup& group = page.measure->count_groups[g];
        if (!group.visible) continue;
        const QColor colour(group.color[0], group.color[1], group.color[2]);
        QFont font = painter.font();
        font.setPixelSize(group.label_size);
        painter.setFont(font);
        const double r = 1.5 + group.marker_size;  // (its dot's radius, screen pixels)
        for (std::size_t i = 0; i < group.points.size(); ++i) {
            QPointF at = qp(group.points[i]);
            if (count_drag_ && count_drag_->moved && count_drag_->group == g && count_drag_->index == i) at += count_drag_->to - count_drag_->grabbed;
            if (!seen.contains(at)) continue;
            const QPointF c = screen.map(view.at.map(at));  // (upright on the screen)
            painter.setPen(QPen(QColor(255, 255, 255, 200), 1));
            painter.setBrush(colour);
            painter.drawEllipse(c, r, r);
            painter.setPen(colour);
            painter.drawText(QPointF(c.x() + r + 2, c.y() - r - 1), QString::number(i + 1));
        }
    }
}

void PageCanvas::paint_ruler(QPainter& painter, const core::Page& page, const ExtraView& view) const {
    (void)page;
    if (tool_ != QLatin1String("measure")) return;  // (Photoshop's: the line shows while the tool is in hand)
    const std::optional<core::RulerLine> line = ruler_in_hand();
    if (!line) return;
    const QTransform screen = painter.transform();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPointF a = screen.map(view.at.map(qp(line->start))), b = screen.map(view.at.map(qp(line->end)));
    const auto stroke = [&](const QPointF& p, const QPointF& q) {
        painter.setPen(QPen(QColor(255, 255, 255, 220), 3));
        painter.drawLine(p, q);
        painter.setPen(QPen(QColor(20, 20, 20), 1));
        painter.drawLine(p, q);
    };
    const auto end = [&](const QPointF& p) {
        stroke(p + QPointF(-5, 0), p + QPointF(5, 0));
        stroke(p + QPointF(0, -5), p + QPointF(0, 5));
    };
    stroke(a, b);
    end(a);
    end(b);
    if (line->protractor) {
        const QPointF c = screen.map(view.at.map(qp(*line->protractor)));
        stroke(a, c);
        end(c);
        // (the angle between the arms: an arc at the vertex)
        const double a1 = std::atan2(-(b.y() - a.y()), b.x() - a.x()) * 180.0 / std::numbers::pi;
        const double a2 = std::atan2(-(c.y() - a.y()), c.x() - a.x()) * 180.0 / std::numbers::pi;
        double span = a2 - a1;
        while (span > 180) span -= 360;
        while (span < -180) span += 360;
        painter.setPen(QPen(QColor(20, 20, 20), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(a - QPointF(18, 18), QSizeF(36, 36)), static_cast<int>(std::lround(a1 * 16)), static_cast<int>(std::lround(span * 16)));
    }
}

std::optional<std::pair<std::size_t, std::size_t>> PageCanvas::count_at(const QPointF& widget) const {
    const core::Page* p = page();
    if (p == nullptr || !p->measure || !extras_shown || !extra_shown(QStringLiteral("count"))) return std::nullopt;
    std::optional<std::pair<std::size_t, std::size_t>> best;
    double nearest = kGrab + 1;
    for (std::size_t g = 0; g < p->measure->count_groups.size(); ++g) {
        const core::CountGroup& group = p->measure->count_groups[g];
        if (!group.visible) continue;
        for (std::size_t i = 0; i < group.points.size(); ++i) {
            const QPointF at = to_widget(qp(group.points[i]));
            const double d = std::hypot(at.x() - widget.x(), at.y() - widget.y());
            if (d <= std::max(kGrab, 1.5 + group.marker_size) && d < nearest) {
                nearest = d;
                best = std::make_pair(g, i);
            }
        }
    }
    return best;
}

bool PageCanvas::measure_press(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    modifiers_ = modifiers;
    MeasureDrag drag;
    drag.grabbed = mm;
    const double grab = kGrab / std::max(view_.scale, 1e-9);
    const std::optional<core::RulerLine> now = ruler_in_hand();
    const auto near = [&](const std::array<double, 2>& p) { return std::hypot(p[0] - mm.x(), p[1] - mm.y()) <= grab; };
    if (now) {
        drag.was = *now;
        drag.line = *now;
        const bool alt = (modifiers & Qt::AltModifier) != 0;
        if (alt && (near(now->start) || near(now->end))) {
            // (the protractor's arm from the end grabbed: that end the vertex — the line's start)
            if (!near(now->start)) std::swap(drag.line.start, drag.line.end);
            drag.part = MeasureDrag::Part::Arm;
            drag.line.protractor = ap(mm);
        } else if (near(now->start)) {
            drag.part = MeasureDrag::Part::Start;
        } else if (near(now->end)) {
            drag.part = MeasureDrag::Part::End;
        } else if (now->protractor && near(*now->protractor)) {
            drag.part = MeasureDrag::Part::Arm;
        } else if (to_segment(mm, qp(now->start), qp(now->end)) <= grab) {
            drag.part = MeasureDrag::Part::Line;
        }
    }
    if (drag.part == MeasureDrag::Part::New) {
        drag.line = core::RulerLine{};
        drag.line.start = ap(mm);
        drag.line.end = ap(mm);
    }
    measure_drag_ = drag;
    return true;
}

bool PageCanvas::measure_move(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!measure_drag_) return false;
    MeasureDrag& d = *measure_drag_;
    const bool shift = (modifiers & Qt::ShiftModifier) != 0;
    if (!d.moved && std::hypot(mm.x() - d.grabbed.x(), mm.y() - d.grabbed.y()) * view_.scale < 3.0) return true;
    d.moved = true;
    switch (d.part) {
        case MeasureDrag::Part::New:
            d.line.end = ap(shift ? by_45(qp(d.line.start), mm) : mm);
            break;
        case MeasureDrag::Part::Start:
            d.line.start = ap(shift ? by_45(qp(d.was.end), mm) : mm);
            break;
        case MeasureDrag::Part::End:
            d.line.end = ap(shift ? by_45(qp(d.was.start), mm) : mm);
            break;
        case MeasureDrag::Part::Arm:
            d.line.protractor = ap(shift ? by_45(qp(d.line.start), mm) : mm);
            break;
        case MeasureDrag::Part::Line: {
            const QPointF by = mm - d.grabbed;
            d.line.start = ap(qp(d.was.start) + by);
            d.line.end = ap(qp(d.was.end) + by);
            if (d.was.protractor) d.line.protractor = ap(qp(*d.was.protractor) + by);
            break;
        }
    }
    const core::RulerLine& l = d.line;
    emit rulerMoving(qp(l.start), qp(l.end), l.protractor.has_value(), l.protractor ? qp(*l.protractor) : QPointF());
    update();
    return true;
}

bool PageCanvas::measure_release() {
    if (!measure_drag_) return false;
    const MeasureDrag d = *measure_drag_;
    measure_drag_.reset();
    if (d.moved) {
        const core::RulerLine& l = d.line;
        emit rulerDrawn(qp(l.start), qp(l.end), l.protractor.has_value(), l.protractor ? qp(*l.protractor) : QPointF());
    }
    update();
    return true;
}

bool PageCanvas::count_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    const core::Page* p = page();
    if (p == nullptr) return true;
    const std::optional<std::pair<std::size_t, std::size_t>> hit = count_at(view_transform().map(pos));
    if ((modifiers & Qt::AltModifier) != 0) {  // (Alt+click: the mark there taken away)
        if (hit) emit countRemoved(static_cast<int>(hit->first), static_cast<int>(hit->second));
        return true;
    }
    if (hit) {
        count_drag_ = CountDrag{hit->first, hit->second, mm, mm, false};
        return true;
    }
    if (mm.x() < 0 || mm.y() < 0 || mm.x() > p->spec.width_mm.value() || mm.y() > p->spec.height_mm.value()) return true;  // (on the paper)
    emit countRequested(mm.x(), mm.y());
    return true;
}

bool PageCanvas::count_move(const QPointF& mm) {
    if (!count_drag_) return false;
    count_drag_->to = mm;
    if (!count_drag_->moved && std::hypot(mm.x() - count_drag_->grabbed.x(), mm.y() - count_drag_->grabbed.y()) * view_.scale >= 3.0) count_drag_->moved = true;
    update();
    return true;
}

bool PageCanvas::count_release() {
    if (!count_drag_) return false;
    const CountDrag drag = *count_drag_;
    count_drag_.reset();
    const core::Page* p = page();
    if (drag.moved && p != nullptr && p->measure && drag.group < p->measure->count_groups.size() &&
        drag.index < p->measure->count_groups[drag.group].points.size()) {
        const QPointF to = qp(p->measure->count_groups[drag.group].points[drag.index]) + drag.to - drag.grabbed;
        emit countMoved(static_cast<int>(drag.group), static_cast<int>(drag.index), to.x(), to.y());
    }
    update();
    return true;
}

}  // namespace genko::app
