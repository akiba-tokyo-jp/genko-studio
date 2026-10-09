// The canvas's scales and the guide lines pulled from them (Python's canvas_shapes.py: _scale_hit, _guide_press,
// _guide_move, _guide_release, _draw_guide_drag, _draw_scale), the phone screens over a vertical-scroll book (canvas.py
// phone_screens, _draw_phone) and the mark of a problem the checks found (canvas_guides.py _draw_highlight).

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"

namespace genko::app {

using core::Json;

// --- the scales and guide lines ------------------------------------------------------------------------------------

std::optional<QString> PageCanvas::scale_hit(const QPointF& widget) const {
    // which scale a press on the screen falls on: "h" (the top: a guide across) or "v" (the left)
    if (!show_scale) return std::nullopt;
    if (widget.y() < kScalePx && widget.x() >= kScalePx) return QStringLiteral("h");
    if (widget.x() < kScalePx && widget.y() >= kScalePx) return QStringLiteral("v");
    return std::nullopt;
}

bool PageCanvas::guide_press(const QPointF& widget) {
    const auto axis = scale_hit(widget);
    if (!axis) return false;
    guide_drag_ = GuideDrag{*axis, std::nullopt};
    return true;
}

bool PageCanvas::guide_move(const QPointF& widget) {
    if (!guide_drag_) return false;
    const QPointF mm = to_mm(widget);
    guide_drag_->at = core::py_round(guide_drag_->axis == QLatin1String("h") ? mm.y() : mm.x(), 2);
    update();
    return true;
}

bool PageCanvas::guide_release(const QPointF& widget) {
    if (!guide_drag_) return false;
    const GuideDrag drag = *guide_drag_;
    guide_drag_.reset();
    if (drag.at && !scale_hit(widget) && widget.x() >= kScalePx && widget.y() >= kScalePx) {
        emit rulerPlaced(Json{{"kind", "guide"}, {"axis", drag.axis.toStdString()}, {"at", *drag.at}});
    }
    update();
    return true;
}

void PageCanvas::draw_guide_drag(QPainter& painter) const {
    const core::Page* p = page();
    if (!guide_drag_ || !guide_drag_->at || p == nullptr) return;
    const double at = *guide_drag_->at;
    const bool across = guide_drag_->axis == QLatin1String("h");
    const QPointF a = across ? QPointF(-50, at) : QPointF(at, -50);
    const QPointF b = across ? QPointF(p->spec.width_mm.value() + 50, at) : QPointF(at, p->spec.height_mm.value() + 50);
    painter.setPen(QPen(QColor(0, 170, 200), 1, Qt::DashLine));
    painter.drawLine(pt(a.x(), a.y()), pt(b.x(), b.y()));
}

void PageCanvas::draw_scale(QPainter& painter) const {
    // mm scales along the top and the left, following the view (zoom and pan; turned views show none)
    const core::Page* p = page();
    if (!show_scale || p == nullptr) return;
    painter.save();
    painter.resetTransform();
    const double w = width();
    const double h = height();
    const theme::Tokens t = theme::tokens();
    painter.fillRect(QRectF(0, 0, w, kScalePx), QColor(t.panel));
    painter.fillRect(QRectF(0, 0, kScalePx, h), QColor(t.panel));
    painter.setPen(QColor(t.muted));
    QFont font(painter.font());
    font.setPointSizeF(std::max(7.0, 9.0 - 1.5));  // (theme.MIN_PT - 1.5: follows the screen's scale; the strip is narrow)
    painter.setFont(font);
    if (std::abs(core::py_fmod(view_.rotation, 360.0)) < 0.01) {
        const QTransform view = view_transform();
        double step = 1.0;
        while (step * view_.scale < 5) step *= core::py_float_repr(step).front() == '1' ? 5 : 2;
        const double width_mm = p->spec.width_mm.value();
        const double height_mm = p->spec.height_mm.value();
        for (std::int64_t k = 0; static_cast<double>(k) * step <= width_mm + 1e-6; ++k) {
            const double x = view.map(pt(static_cast<double>(k) * step, 0)).x();
            const std::int64_t mark = core::py_round_int(static_cast<double>(k) * step);
            const bool major = mark % (step <= 2 ? 10 : 50) == 0;
            if (kScalePx <= x && x <= w) {
                painter.drawLine(QPointF(x, kScalePx), QPointF(x, kScalePx - (major ? 8 : 4)));
                if (major) painter.drawText(QPointF(x + 2, 9), QString::number(mark));
            }
        }
        for (std::int64_t k = 0; static_cast<double>(k) * step <= height_mm + 1e-6; ++k) {
            const double y = view.map(pt(0, static_cast<double>(k) * step)).y();
            const std::int64_t mark = core::py_round_int(static_cast<double>(k) * step);
            const bool major = mark % (step <= 2 ? 10 : 50) == 0;
            if (kScalePx <= y && y <= h) {
                painter.drawLine(QPointF(kScalePx, y), QPointF(kScalePx - (major ? 8 : 4), y));
                if (major) painter.drawText(QPointF(1, y - 2), QString::number(mark));
            }
        }
    }
    painter.fillRect(QRectF(0, 0, kScalePx, kScalePx), QColor(t.divider));
    painter.restore();
}

// --- the phone screens -----------------------------------------------------------------------------------------------

std::vector<double> PageCanvas::phone_screens() const {
    // where each phone screen ends down the page (mm from the page's top), the page's width filling the screen
    std::vector<double> ends;
    const core::Page* p = page();
    if (p == nullptr) return ends;
    const core::Rect trim = p->trim_rect_mm();
    const double step = trim.width.value() * kPhoneAspect;
    if (!(step > 0)) return ends;
    for (double y = trim.y.value() + step; y < trim.y.value() + trim.height.value() - 1e-6; y += step) ends.push_back(y);
    return ends;
}

void PageCanvas::draw_phone(QPainter& painter) const {
    // the screens' breaks down the page, and the screen under the cursor (the rest a little dimmed)
    const core::Page* p = page();
    if (p == nullptr) return;
    const core::Rect r = p->trim_rect_mm();
    const double tx = r.x.value(), ty = r.y.value(), tw = r.width.value(), th = r.height.value();
    const double step = tw * kPhoneAspect;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 150, 136, 170), 1, Qt::DashDotLine));
    int n = 0;
    for (const double y : phone_screens()) {
        const QPointF a = pt(tx, y);
        const QPointF b = pt(tx + tw, y);
        painter.drawLine(a, b);
        painter.drawText(QPointF(b.x() + 4, b.y() + 4), QString::number(++n));
    }
    if (!hover_) return;
    const double top = core::py_min(core::py_max(hover_->y() - step / 2, ty), core::py_max(ty, ty + th - step));
    const QPointF a = pt(tx, top);
    const QRectF screen(a.x(), a.y(), tw * view_.scale, core::py_min(step, th) * view_.scale);
    QPainterPath whole;
    whole.addRect(QRectF(pt(tx, ty), pt(tx + tw, ty + th)));
    QPainterPath hole;
    hole.addRect(screen);
    painter.fillPath(whole.subtracted(hole), QColor(0, 0, 0, 22));
    painter.setPen(QPen(QColor(0, 150, 136, 220), 2));
    painter.drawRoundedRect(screen, 6, 6);
}

// --- the checks' mark ----------------------------------------------------------------------------------------------

void PageCanvas::draw_highlight(QPainter& painter) const {
    if (!highlight_box || page() == nullptr) return;
    const auto [x, y, w, h] = *highlight_box;
    const double pad = 2.0;
    const QPointF a = pt(x - pad, y - pad);
    const QPointF b = pt(x + w + pad, y + h + pad);
    painter.save();
    painter.setPen(QPen(QColor(QStringLiteral("#e03131")), 2.5, Qt::DashLine));
    painter.setBrush(QColor(224, 49, 49, 40));
    painter.drawRect(QRectF(a, b));
    painter.restore();
}

}  // namespace genko::app
