// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」): the slices over the page (the extra "slices": each slice's outline in
// 環境設定 → ガイド・グリッド・スライス's colour — the auto ones dotted and paler, while a slice tool is in hand — and its
// number in a badge at its top left, ▪ after a layer-based one's; the chosen one's outline orange, with its 8 handles when it
// is a user slice) and the two tools — the スライスツール ("slice": a drag draws a user slice, Shift square; 縦横比を固定 and
// 固定サイズ as its style says; a click without a drag chooses the slice there) and the スライス選択ツール ("slice_select": a
// click chooses the slice there — a stored one before an auto one, the later on top —, a drag of the chosen one moves it, of
// a handle sizes it — an auto or layer-based one made a user slice by it —, a double click asks its options) —, after
// PhotoCraft v0.2.0's crates/ui-egui/src/slice_ui.rs (MIT, native/third_party/photocraft/NOTICE.md). スライスをロック: no
// gesture changes them (slicesLocked). The window makes the ops (app/main_window_slices.cpp).

#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"

namespace genko::app {

namespace {

const QColor kChosenSlice(0xff, 0xb0, 0x00);
constexpr double kHandle = 6.0;  // (screen pixels: how near the pointer finds a handle)

}  // namespace

bool PageCanvas::is_slice_tool(const QString& tool) { return tool == QLatin1String("slice") || tool == QLatin1String("slice_select"); }

QRectF PageCanvas::slice_mm(const core::ResolvedSlice& r) const {
    const SliceView* v = slice_view ? slice_view() : nullptr;
    if (v == nullptr) return {};
    const auto mm = core::slice_rect_mm(r.box, v->grid);
    return QRectF(mm[0], mm[1], mm[2], mm[3]);
}

std::optional<core::ResolvedSlice> PageCanvas::chosen_slice_now() const {
    const SliceView* v = slice_view ? slice_view() : nullptr;
    if (v == nullptr || !chosen_slice) return std::nullopt;
    for (const core::ResolvedSlice& r : v->slices)
        if (r.number == *chosen_slice) return r;
    return std::nullopt;
}

std::optional<core::ResolvedSlice> PageCanvas::slice_at(const QPointF& widget) const {
    const SliceView* v = slice_view ? slice_view() : nullptr;
    if (v == nullptr) return std::nullopt;
    const QPointF mm = to_mm(widget);
    const double x = mm.x() * v->grid.px_x, y = mm.y() * v->grid.px_y;
    const auto order = [&](const core::ResolvedSlice& r) -> std::ptrdiff_t {
        if (!r.id) return 0;
        const auto it = std::find(v->order.begin(), v->order.end(), *r.id);
        return it == v->order.end() ? 0 : (it - v->order.begin()) + 1;
    };
    std::optional<core::ResolvedSlice> best;
    for (const core::ResolvedSlice& r : v->slices) {
        if (x < static_cast<double>(r.box.x0) || x >= static_cast<double>(r.box.x1) || y < static_cast<double>(r.box.y0) || y >= static_cast<double>(r.box.y1)) continue;
        if (!best || order(r) > order(*best)) best = r;
    }
    return best;
}

void PageCanvas::paint_slices(QPainter& painter, const core::Page& page, const ExtraView& view) const {
    (void)page;
    const SliceView* v = slice_view ? slice_view() : nullptr;
    if (v == nullptr) return;
    const bool tool_on = is_slice_tool(tool_);
    if (!tool_on && v->order.empty()) return;  // (a page without slices: nothing, unless a slice tool is in hand)
    const QTransform screen = painter.transform();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QFont font = painter.font();
    font.setPixelSize(10);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF seen = view.seen_mm;
    const auto on_screen = [&](const core::ResolvedSlice& r) {
        const QRectF mm = slice_mm(r);
        const QPolygonF corners = screen.map(view.at.map(QPolygonF(mm)));
        return corners.boundingRect();  // (upright on the screen: a turned view shows its box)
    };
    for (const bool automatic : {true, false}) {
        for (const core::ResolvedSlice& r : v->slices) {
            if ((r.origin == "auto") != automatic) continue;
            if (automatic && (!tool_on || hide_auto_slices)) continue;
            if (!slice_mm(r).intersects(seen)) continue;
            const QRectF box = on_screen(r).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool chosen = tool_on && chosen_slice == r.number;
            QColor line = slice_colour;
            if (automatic) line.setAlphaF(0.7);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(line, 1, automatic ? Qt::DotLine : Qt::SolidLine));
            painter.drawRect(box);
            if (chosen) {
                painter.setPen(QPen(kChosenSlice, 2));
                painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
                if (r.origin == "user" && !lock_slices) {
                    painter.setBrush(kChosenSlice);
                    painter.setPen(Qt::NoPen);
                    for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft(), QPointF(box.center().x(), box.top()),
                                             QPointF(box.right(), box.center().y()), QPointF(box.center().x(), box.bottom()), QPointF(box.left(), box.center().y())})
                        painter.drawRect(QRectF(c.x() - 2.5, c.y() - 2.5, 5, 5));
                }
            }
            if (show_slice_numbers && box.width() > 18 && box.height() > 12) {
                QString label = QStringLiteral("%1").arg(r.number, 2, 10, QLatin1Char('0'));
                if (r.origin == "layer") label += QStringLiteral(" ▪");
                if (r.kind == "noImage") label += QStringLiteral(" ×");  // (no picture written)
                const QRectF badge(box.left() + 2, box.top() + 2, metrics.horizontalAdvance(label) + 6, metrics.height() + 2);
                painter.setPen(Qt::NoPen);
                painter.setBrush(automatic ? QColor(110, 110, 110) : chosen ? kChosenSlice : slice_colour);
                painter.drawRect(badge);
                painter.setPen(Qt::white);
                painter.drawText(badge, Qt::AlignCenter, label);
            }
        }
    }
}

void PageCanvas::draw_slice_drag(QPainter& painter) const {
    if (!slice_drag_ || !slice_drag_->moved) return;
    const QRectF r = slice_drag_->now.normalized();
    painter.save();
    QPolygonF shape;
    for (const QPointF& c : {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}) shape << pt(c.x(), c.y());
    painter.setBrush(Qt::NoBrush);
    if (slice_drag_->part == SliceDrag::Part::New) {
        painter.setPen(QPen(Qt::white, 1));
        painter.drawPolygon(shape);
        painter.setPen(QPen(slice_colour, 1, Qt::DashLine));
    } else {
        painter.setPen(QPen(kChosenSlice, 2));
    }
    painter.drawPolygon(shape);
    painter.restore();
}

QRectF PageCanvas::slice_drawn(const QPointF& from, const QPointF& to, Qt::KeyboardModifiers modifiers) const {
    double w = to.x() - from.x(), h = to.y() - from.y();
    if (slice_style == 1 && slice_style_w > 0 && slice_style_h > 0) {  // (縦横比を固定)
        const double k = slice_style_h / slice_style_w;
        if (std::abs(w) * k >= std::abs(h)) {
            h = std::copysign(std::abs(w) * k, h == 0 ? 1.0 : h);
        } else {
            w = std::copysign(std::abs(h) / k, w == 0 ? 1.0 : w);
        }
    } else if ((modifiers & Qt::ShiftModifier) != 0) {  // (Shift: a square)
        const double side = std::max(std::abs(w), std::abs(h));
        w = std::copysign(side, w == 0 ? 1.0 : w);
        h = std::copysign(side, h == 0 ? 1.0 : h);
    }
    return QRectF(from, QSizeF(w, h)).normalized();
}

bool PageCanvas::slice_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    const core::Page* p = page();
    if (p == nullptr) return true;
    const QPointF widget = view_transform().map(pos);
    if (tool_ == QLatin1String("slice")) {
        if (lock_slices) {
            emit slicesLocked();
            return true;
        }
        SliceDrag drag;
        drag.part = SliceDrag::Part::New;
        drag.grabbed = snap_point(mm, modifiers);
        drag.now = QRectF(drag.grabbed, QSizeF(0, 0));
        // (固定サイズ: the box at the press, the page's pixels it says)
        const SliceView* v = slice_view ? slice_view() : nullptr;
        if (slice_style == 2 && v != nullptr && slice_style_w > 0 && slice_style_h > 0) {
            drag.now = QRectF(drag.grabbed, QSizeF(slice_style_w / v->grid.px_x, slice_style_h / v->grid.px_y));
            drag.moved = true;
        }
        slice_drag_ = drag;
        update();
        return true;
    }
    // the スライス選択ツール: a handle of the chosen user slice, else the slice under the press
    if (const auto chosen = chosen_slice_now(); chosen && chosen->origin == "user" && !lock_slices) {
        const QRectF r = slice_mm(*chosen);
        const std::array<QPointF, 8> at{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), QPointF(r.center().x(), r.top()),
                                        QPointF(r.right(), r.center().y()), QPointF(r.center().x(), r.bottom()), QPointF(r.left(), r.center().y())};
        for (std::size_t i = 0; i < at.size(); ++i) {
            const QPointF c = to_widget(at[i]);
            if (std::abs(c.x() - widget.x()) <= kHandle && std::abs(c.y() - widget.y()) <= kHandle) {
                slice_drag_ = SliceDrag{SliceDrag::Part::Handle, static_cast<int>(i), chosen->number, mm, r, r, false};
                return true;
            }
        }
    }
    const std::optional<core::ResolvedSlice> hit = slice_at(widget);
    chosen_slice = hit ? std::optional<int>(hit->number) : std::nullopt;
    emit sliceChosen(hit ? hit->number : 0);
    if (hit && !lock_slices) {
        const QRectF r = slice_mm(*hit);
        slice_drag_ = SliceDrag{SliceDrag::Part::Move, -1, hit->number, mm, r, r, false};
    } else if (hit && lock_slices) {
        slice_drag_.reset();
    }
    update();
    return true;
}

bool PageCanvas::slice_move(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!slice_drag_) return false;
    SliceDrag& d = *slice_drag_;
    snap_slice_moving_ = d.part == SliceDrag::Part::New ? 0 : d.number;  // (its own edges no target)
    if (d.part == SliceDrag::Part::New && slice_style == 2) {  // (固定サイズ: the box follows the pointer)
        d.now.moveTopLeft(snap_point(mm, modifiers));
        update();
        return true;
    }
    if (!d.moved && std::hypot(mm.x() - d.grabbed.x(), mm.y() - d.grabbed.y()) * view_.scale < 3.0) return true;
    d.moved = true;
    switch (d.part) {
        case SliceDrag::Part::New:
            d.now = slice_drawn(d.grabbed, snap_point(mm, modifiers), modifiers);
            break;
        case SliceDrag::Part::Move: {
            const QPointF by = mm - d.grabbed;
            const QRectF moved = d.was.translated(by);
            const QPointF snapped = snap_box({moved.left(), moved.top(), moved.right(), moved.bottom()}, modifiers);
            d.now = moved.translated(snapped);
            break;
        }
        case SliceDrag::Part::Handle: {
            const QPointF at = snap_point(mm, modifiers);
            QRectF r = d.was;
            const int h = d.handle;
            if (h == 0 || h == 3 || h == 7) r.setLeft(at.x());
            if (h == 1 || h == 2 || h == 5) r.setRight(at.x());
            if (h == 0 || h == 1 || h == 4) r.setTop(at.y());
            if (h == 2 || h == 3 || h == 6) r.setBottom(at.y());
            d.now = r.normalized();
            break;
        }
    }
    update();
    return true;
}

bool PageCanvas::slice_release() {
    if (!slice_drag_) return false;
    const SliceDrag d = *slice_drag_;
    slice_drag_.reset();
    snap_slice_moving_ = 0;
    forget_snap();
    if (d.part == SliceDrag::Part::New) {
        const SliceView* v = slice_view ? slice_view() : nullptr;
        const QRectF r = d.now.normalized();
        // (a box of less than a pixel or two of the page: a click — the slice there chosen)
        const bool small = v == nullptr || r.width() * v->grid.px_x < 2 || r.height() * v->grid.px_y < 2;
        if (d.moved && !small) {
            emit sliceDrawn(r);
        } else if (v != nullptr) {
            const std::optional<core::ResolvedSlice> hit = slice_at(to_widget(d.grabbed));
            chosen_slice = hit ? std::optional<int>(hit->number) : std::nullopt;
            emit sliceChosen(hit ? hit->number : 0);
        }
    } else if (d.moved && d.now != d.was) {
        emit sliceMoved(d.number, d.now.normalized());
    }
    update();
    return true;
}

bool PageCanvas::slice_double(const QPointF& widget) {
    if (tool_ != QLatin1String("slice_select")) return false;
    const std::optional<core::ResolvedSlice> hit = slice_at(widget);
    if (!hit) return false;
    slice_drag_.reset();
    chosen_slice = hit->number;
    emit sliceOptionsAsked(hit->number);
    return true;
}

}  // namespace genko::app
