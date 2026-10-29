// 切り抜きツール (PC3 C-e, SPEC.md COLOR-07「判型」; PhotoCraft's Crop tool, crates/ui-egui/src/state.rs): a box drawn on the
// page (past its paper too), moved by a drag inside it, sized by its corners (its shape kept at crop_ratio when one is set),
// turned by a drag outside it (Shift: 15° steps); the paper outside it dimmed and its thirds drawn. Enter crops the page to
// it (cropRequested: the box upright and its turn), Esc lets it go.

#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/canvas.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

constexpr double kPi = std::numbers::pi;
const QColor kCrop(QStringLiteral("#1c7ed6"));

QPointF turned(const QPointF& v, double degrees) {
    const double a = degrees * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    return QPointF(c * v.x() - s * v.y(), s * v.x() + c * v.y());
}

// The box's corners on the page (mm): top left, top right, bottom right, bottom left.
std::array<QPointF, 4> corners(const PageCanvas::CropBox& box) {
    const double w = box.size.width() / 2, h = box.size.height() / 2;
    return {box.centre + turned(QPointF(-w, -h), box.angle), box.centre + turned(QPointF(w, -h), box.angle),
            box.centre + turned(QPointF(w, h), box.angle), box.centre + turned(QPointF(-w, h), box.angle)};
}

// A point in the box's own (unturned) frame, about its middle.
QPointF local(const PageCanvas::CropBox& box, const QPointF& mm) { return turned(mm - box.centre, -box.angle); }

}  // namespace

void PageCanvas::set_crop(std::optional<CropBox> box) {
    crop_ = std::move(box);
    crop_drag_.reset();
    emit cropChanged();
    update();
}

bool PageCanvas::crop_press(const QPointF& pos, const QPointF& pointer, Qt::KeyboardModifiers modifiers) {
    // (PC3 C-d, 表示 → スナップ: a new box's corner snaps — canvas_snap.cpp)
    const QPointF mm = crop_ ? pointer : snap_point(pointer, modifiers);
    if (crop_) {
        const std::array<QPointF, 4> at = corners(*crop_);
        for (int i = 0; i < 4; ++i) {
            const QPointF p = pt(at[std::size_t(i)].x(), at[std::size_t(i)].y());
            if (std::abs(p.x() - pos.x()) <= 8 && std::abs(p.y() - pos.y()) <= 8) {
                crop_drag_ = CropDrag{QStringLiteral("corner"), mm, *crop_, i};
                return true;
            }
        }
        const QPointF in = local(*crop_, mm);
        const bool inside = std::abs(in.x()) <= crop_->size.width() / 2 && std::abs(in.y()) <= crop_->size.height() / 2;
        crop_drag_ = CropDrag{inside ? QStringLiteral("move") : QStringLiteral("turn"), mm, *crop_, 0};
        return true;
    }
    crop_ = CropBox{mm, QSizeF(0, 0), 0};
    crop_drag_ = CropDrag{QStringLiteral("new"), mm, *crop_, 2};
    update();
    return true;
}

bool PageCanvas::crop_move(const QPointF& pointer, Qt::KeyboardModifiers modifiers, bool pressed) {
    if (!pressed || !crop_drag_ || !crop_) return false;
    const CropDrag& d = *crop_drag_;
    CropBox box = d.was;
    // (PC3 C-d, 表示 → スナップ: an upright box moved snaps by its edges and middle, the corner dragged where it would be;
    // a turned box and a turn do not — canvas_snap.cpp)
    QPointF mm = pointer;
    if (d.kind == QLatin1String("new")) {
        mm = snap_point(pointer, modifiers);
    } else if (d.kind == QLatin1String("move") && d.was.angle == 0) {
        const QRectF r = d.was.rect().translated(pointer - d.start);
        mm = pointer + snap_box({r.left(), r.top(), r.right(), r.bottom()}, modifiers);
    } else if (d.kind == QLatin1String("corner") && d.was.angle == 0) {
        const QPointF grip = corners(d.was)[std::size_t(d.corner)];
        mm = snap_point(grip + (pointer - d.start), modifiers) - grip + d.start;
    } else {
        forget_snap();
    }
    if (d.kind == QLatin1String("move")) {
        box.centre = d.was.centre + (mm - d.start);
    } else if (d.kind == QLatin1String("turn")) {
        const QPointF a = d.start - d.was.centre, b = mm - d.was.centre;
        double angle = d.was.angle + (std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x())) * 180.0 / kPi;
        if (modifiers & Qt::ShiftModifier) angle = std::round(angle / 15.0) * 15.0;
        angle = std::remainder(angle, 360.0);
        box.angle = std::abs(angle) < 1e-9 ? 0.0 : angle;
    } else {
        // (a corner: the one across from it stays; a new box: from where the drag began)
        const std::array<QPointF, 4> at = corners(d.was);
        const QPointF fixed = d.kind == QLatin1String("new") ? d.start : at[std::size_t((d.corner + 2) % 4)];
        const QPointF o = turned(fixed - d.was.centre, -d.was.angle), p = turned(mm - d.was.centre, -d.was.angle);
        double w = std::abs(p.x() - o.x()), h = std::abs(p.y() - o.y());
        const double ratio = (modifiers & Qt::ShiftModifier) && d.was.size.height() > 0 ? d.was.size.width() / d.was.size.height() : crop_ratio;
        if (ratio > 0) h = w / ratio;
        const QPointF to(o.x() + (p.x() >= o.x() ? w : -w), o.y() + (p.y() >= o.y() ? h : -h));
        box.size = QSizeF(w, h);
        box.centre = d.was.centre + turned((o + to) / 2.0, d.was.angle);
    }
    crop_ = box;
    emit cropChanged();
    update();
    return true;
}

bool PageCanvas::crop_release() {
    if (!crop_drag_) return false;
    crop_drag_.reset();
    if (crop_ && (crop_->size.width() < 1.0 || crop_->size.height() < 1.0)) crop_.reset();  // (a click: no box)
    emit cropChanged();
    update();
    return true;
}

bool PageCanvas::crop_key(QKeyEvent* event) {
    if (tool_ != QLatin1String("crop") || !crop_) return false;
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const CropBox box = *crop_;
        set_crop(std::nullopt);
        emit cropRequested(box.rect(), box.angle);
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        set_crop(std::nullopt);
        return true;
    }
    return false;
}

void PageCanvas::draw_crop(QPainter& painter) const {
    if (tool_ != QLatin1String("crop") || !crop_) return;
    const std::array<QPointF, 4> at = corners(*crop_);
    QPolygonF box;
    for (const QPointF& p : at) box << pt(p.x(), p.y());
    // (outside the box, dimmed)
    QPainterPath outside;
    outside.addRect(QRectF(-1e5, -1e5, 2e5, 2e5));
    QPainterPath inside;
    inside.addPolygon(box);
    inside.closeSubpath();
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 110));
    painter.drawPath(outside.subtracted(inside));
    // (its thirds)
    painter.setPen(QPen(QColor(255, 255, 255, 160), 1));
    for (const double t : {1.0 / 3.0, 2.0 / 3.0}) {
        const QPointF a = at[0] + (at[1] - at[0]) * t, b = at[3] + (at[2] - at[3]) * t;
        const QPointF c = at[0] + (at[3] - at[0]) * t, d = at[1] + (at[2] - at[1]) * t;
        painter.drawLine(pt(a.x(), a.y()), pt(b.x(), b.y()));
        painter.drawLine(pt(c.x(), c.y()), pt(d.x(), d.y()));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawPolygon(box);
    painter.setPen(QPen(kCrop, 1.5, Qt::DashLine));
    painter.drawPolygon(box);
    painter.setPen(QPen(kCrop, 1.5));
    painter.setBrush(Qt::white);
    for (const QPointF& p : box) painter.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8));
    painter.restore();
}

}  // namespace genko::app
