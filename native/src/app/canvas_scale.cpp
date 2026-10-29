// The canvas's scales and the guide lines pulled from them (Python's canvas_shapes.py: _scale_hit, _guide_press,
// _guide_move, _guide_release, _draw_guide_drag, _draw_scale), the phone screens over a vertical-scroll book (canvas.py
// phone_screens, _draw_phone) and the mark of a problem the checks found (canvas_guides.py _draw_highlight).

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

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
    hover_ = mm;  // (PC5 H-b: where its words are drawn)
    update();
    return true;
}

bool PageCanvas::guide_release(const QPointF& widget) {
    if (!guide_drag_) return false;
    const GuideDrag drag = *guide_drag_;
    guide_drag_.reset();
    if (drag.id) {  // (PC5 H-b: a guide placed, grabbed — canvas_guide_lines.cpp)
        std::optional<double> was;
        for (const Json& r : page_rulers()) {
            if (r.is_object() && r.value("id", Json()) == Json(*drag.id) && r.value("at", Json()).is_number()) was = r["at"].get<double>();
        }
        const bool moved = drag.at && was && std::abs(*drag.at - *was) > 1e-9;
        const bool off = scale_hit(widget).has_value() || !QRectF(rect()).contains(widget);
        const QString id = QString::fromStdString(*drag.id);
        if (!moved && drag.turn) {  // (Alt+click: turned about the point clicked)
            const QPointF mm = to_mm(widget);
            const bool across = drag.axis != QLatin1String("h");
            emit rulerEdited(id, Json{{"axis", across ? "h" : "v"}, {"at", core::py_round(across ? mm.y() : mm.x(), 2)}});
        } else if (moved && off) {
            emit guideRemoved(id);  // (let go on a scale or out of the view: taken away)
        } else if (moved) {
            emit rulerEdited(id, Json{{"at", *drag.at}});
        }
        update();
        return true;
    }
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
    // (PC5 H-b: where it is, in the ruler's unit, by the pointer's end of it)
    const QString words = length_words(at, !across);
    const QPointF near = across ? pt(std::clamp(hover_ ? hover_->x() : 0.0, 0.0, p->spec.width_mm.value()), at)
                                : pt(at, std::clamp(hover_ ? hover_->y() : 0.0, 0.0, p->spec.height_mm.value()));
    const QRectF label = painter.fontMetrics().boundingRect(words).adjusted(-3, -1, 3, 1);
    const QRectF placed(near.x() + 8, near.y() + 8, label.width(), label.height());
    painter.fillRect(placed, QColor(0, 0, 0, 170));
    painter.setPen(Qt::white);
    painter.drawText(placed, Qt::AlignCenter, words);
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
    if (std::abs(core::py_fmod(view_.rotation, 360.0)) < 0.01 && ruler_unit != core::units::Unit::Mm) {
        draw_scale_in_unit(painter);  // (PC5 H-b: 環境設定 → 単位・定規's unit)
    } else if (std::abs(core::py_fmod(view_.rotation, 360.0)) < 0.01) {
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

void PageCanvas::draw_scale_in_unit(QPainter& painter) const {
    // (PC5 H-b; PhotoCraft v0.2.0's rulers.rs draw_rulers and nice_step, MIT: a labelled mark every step — about 60 screen
    // pixels apart, a step of 1, 2, 2.5 or 5 times a power of ten (whole pixels: no 2.5) — and ten small marks between, the
    // fifth longer; the page's pixels, inches, cm, points, picas or percent of its side)
    const QTransform view = view_transform();
    const bool whole = ruler_unit == core::units::Unit::Px;
    const auto nice_step = [whole](double least) {
        double step = whole ? 1.0 : 0.001;
        for (int round = 0; round < 40; ++round, step *= 10.0) {
            for (const double m : {1.0, 2.0, 2.5, 5.0}) {
                if ((m != 2.5 || !whole) && step * m >= least) return step * m;
            }
        }
        return step;
    };
    const double w = width(), h = height();
    for (const bool along_x : {true, false}) {
        const double mm_per_unit = core::units::to_mm(1.0, ruler_unit, unit_basis(along_x));
        if (!(mm_per_unit > 0) || !std::isfinite(mm_per_unit)) continue;
        const double px_per_unit = mm_per_unit * view_.scale;
        const double step = nice_step(60.0 / std::max(px_per_unit, 1e-6));
        const auto screen = [&](double unit_value) {
            const double mm = unit_value * mm_per_unit;
            return along_x ? view.map(pt(mm, 0)).x() : view.map(pt(0, mm)).y();
        };
        const double span = along_x ? w : h;
        // (the part of the side in sight, in the unit; the screen's view of it may be flipped: either end first)
        const double a = (along_x ? to_mm(QPointF(kScalePx, 0)).x() : to_mm(QPointF(0, kScalePx)).y()) / mm_per_unit;
        const double b = (along_x ? to_mm(QPointF(w, 0)).x() : to_mm(QPointF(0, h)).y()) / mm_per_unit;
        const double first = std::floor(std::min(a, b) / step) * step, last = std::max(a, b);
        for (double v = first; v <= last + step * 1e-9; v += step) {
            const double c = screen(v);
            const auto mark = [&](double at, double length) {
                if (at < kScalePx || at > span) return;
                if (along_x) {
                    painter.drawLine(QPointF(at, kScalePx), QPointF(at, kScalePx - length));
                } else {
                    painter.drawLine(QPointF(kScalePx, at), QPointF(kScalePx - length, at));
                }
            };
            mark(c, 8);
            for (int k = 1; k < 10; ++k) mark(screen(v + step * k / 10.0), k == 5 ? 6 : 3);
            if (c < kScalePx || c > span) continue;
            const double shown = std::abs(v) < step * 1e-6 ? 0.0 : v;
            const QString words = step >= 1.0 || whole ? QString::number(std::llround(shown)) : QString::number(shown, 'g', 6);
            if (along_x) {
                painter.drawText(QPointF(c + 2, 9), words);
            } else {
                painter.drawText(QPointF(1, c - 2), words);
            }
        }
    }
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
