// The canvas's input: the mouse, the pen (tablet), the wheel, the keys and two-finger gestures (Python's
// PageCanvas mouse*Event, tabletEvent, wheelEvent, keyPressEvent and the panel tool's _frame_press/_move/_release).

#include <QContextMenuEvent>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPinchGesture>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "app/pen.hpp"
#include "app/perf.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"

namespace genko::app {

namespace {

constexpr double kPi = 3.14159265358979323846;

double r2(double v) { return core::py_round(v, 2); }

bool is_drawing_tool(const QString& tool) { return PageCanvas::is_stroke_tool(tool); }

}  // namespace

// --- the line being drawn ------------------------------------------------------------------------------------------

void PageCanvas::begin_stroke(const QPointF& mm, double pressure, double rotation, bool tablet) {
    stroke_ = {core::PenPoint{mm.x(), mm.y(), pressure}};
    turns_ = {rotation};
    stroke_tablet_ = tablet;
    stroke_id_ = core::new_id();  // (the id the line will get: its grain and scatter are drawn with it now)
    live_.reset();
    live_copies_.clear();
    live_snapped_ = false;
    live_sync();
    update();
}

void PageCanvas::extend_stroke(const QPointF& mm, double pressure, double rotation, bool straight) {
    const bool redrawn = straight && tool_ == QLatin1String("pen");
    const QPointF before(stroke_.back().x, stroke_.back().y);
    if (redrawn) {
        stroke_ = {stroke_.front(), core::PenPoint{mm.x(), mm.y(), pressure}};  // Shift: a straight line
        turns_ = {turns_.front(), rotation};
        live_.reset();
    } else {
        stroke_.push_back(core::PenPoint{mm.x(), mm.y(), pressure});
        turns_.push_back(rotation);
    }
    const bool had_live = live_ != nullptr;
    live_sync();
    // only the part of the screen the line changed is painted again (the rest of the page is as it was); a line
    // snapped to a ruler moves as a whole, so it is painted again everywhere
    if (live_snapped_ || (!live_ && (tool_ == QLatin1String("pen") || tool_ == QLatin1String("eraser")) && snapped_preview_applies())) {
        update();
        return;
    }
    if (redrawn || !had_live || !live_) {
        if (live_ || redrawn) {
            update();
        } else {
            // (the plain line of the eraser, or of a pen with no layer to draw on: its newest piece)
            const double pad = (tool_ == QLatin1String("eraser") ? eraser_mm : brush_width_mm) + 1.0;
            const QRectF piece = QRectF(before, mm).normalized().adjusted(-pad, -pad, pad, pad);
            update(mm_transform().mapRect(piece).toAlignedRect().adjusted(-2, -2, 2, 2));
        }
        return;
    }
    const QRect changed = live_->take_changed();
    if (changed.isEmpty()) return;
    const double px = 25.4 / live_->dpi();
    const QRectF piece(changed.x() * px, changed.y() * px, changed.width() * px, changed.height() * px);
    update(mm_transform().mapRect(piece).toAlignedRect().adjusted(-2, -2, 2, 2));
}

void PageCanvas::finish_stroke() {
    if (stroke_.empty()) return;
    core::PenPoints points = std::move(stroke_);
    std::vector<double> turns = std::move(turns_);
    stroke_.clear();
    turns_.clear();
    if (points.size() == 1) {
        points.push_back(core::PenPoint{points[0].x + 0.01, points[0].y + 0.01, points[0].p});  // a tap: a dot with the pen
    }
    if (turns.size() == 1) turns.push_back(turns.back());
    StrokeInput input{std::move(points), std::move(turns), stroke_id_, tool_};
    committing_ = std::move(live_);
    live_copies_.clear();  // (the copies are drawn by their own tiles once the window has added them)
    live_snapped_ = false;
    emit strokeCommitted(input);  // (the window applies it now: stroke_applied or stroke_dropped)
    committing_.reset();
    perf::event("stroke_committed", {{"points", static_cast<std::int64_t>(input.points.size())}});
    back_from_eraser_end();
    emit changed();
    update();
}

// --- the mouse -----------------------------------------------------------------------------------------------------

void PageCanvas::update_cursor(std::optional<QPointF> pos) {
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (space_) {
        setCursor(Qt::OpenHandCursor);
    } else if (is_drawing_tool(tool_)) {
        setCursor(Qt::CrossCursor);  // (the brush's circle is drawn on the page)
    } else if (tool_ == QLatin1String("move")) {
        setCursor(Qt::SizeAllCursor);
    } else if (tool_ == QLatin1String("picker") || tool_ == QLatin1String("fill")) {
        setCursor(Qt::PointingHandCursor);
    } else if (tool_ == QLatin1String("lassofill") || tool_ == QLatin1String("gradient") ||
               tool_ == QLatin1String("reshape") || tool_ == QLatin1String("ruler") || tool_ == QLatin1String("3d") ||
               tool_ == QLatin1String("effect") || tool_ == QLatin1String("stamp")) {
        setCursor(Qt::CrossCursor);
    } else if (tool_ == QLatin1String("text")) {
        setCursor(Qt::IBeamCursor);
    } else if (tool_ == QLatin1String("frame") && pos && page() != nullptr) {
        const QPointF mm = mm_of(*pos);
        if (const auto gutter = hit_gutter(mm.x(), mm.y())) {
            setCursor(gutter->horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }
    } else if (pos && page() != nullptr && tool_ != QLatin1String("marquee") && hit_line(mm_of(*pos).x(), mm_of(*pos).y()) != nullptr) {
        setCursor(Qt::SizeAllCursor);  // (a balloon: dragged with the select tool)
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void PageCanvas::start_pan(const QPointF& pos) {
    stop_coast();
    panning_ = true;
    last_pos_ = pos;
    pan_speed_ = QPointF(0, 0);
    pan_time_ = pan_clock_.elapsed() / 1000.0;
    update_cursor();
}

void PageCanvas::coast() {
    if (reduce_motion() || std::abs(pan_speed_.x()) + std::abs(pan_speed_.y()) < 0.4 || pan_clock_.elapsed() / 1000.0 - pan_time_ > 0.08) {
        emit changed();
        return;
    }
    coast_timer_.start();
}

void PageCanvas::stop_coast() {
    if (coast_timer_.isActive()) {
        coast_timer_.stop();
        emit changed();
    }
}

bool PageCanvas::coasting() const { return coast_timer_.isActive(); }

void PageCanvas::mousePressEvent(QMouseEvent* event) {
    setFocus();
    const QPointF pos = unturned(event->position());
    if (event->button() == Qt::LeftButton && space_ && (event->modifiers() & Qt::ShiftModifier)) {
        const double angle = std::atan2(event->position().y() - height() / 2.0, event->position().x() - width() / 2.0) * 180.0 / kPi;
        turning_ = std::make_pair(angle, view_.rotation);  // Shift+Space: turn the view
        return;
    }
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && space_)) {
        start_pan(pos);
        return;
    }
    if (page() == nullptr || event->button() != Qt::LeftButton) return;
    if (guide_press(event->position())) return;  // (a guide line pulled from a scale: canvas_scale.cpp)
    const QPointF mm = mm_of(pos);
    press_pos_ = pos;
    if (tool_ == QLatin1String("move")) {
        tool_drag_ = std::make_pair(mm, mm);
        emit layerMoveStarted();
        update();
        return;
    }
    if (tool_press(pos, mm, event->modifiers())) return;  // (スポイト・塗りつぶし・囲って塗る・グラデーション・図形)
    if (tool_ == QLatin1String("frame")) {
        modifiers_ = event->modifiers();
        frame_press(pos);
        update();
        return;
    }
    if (tool_ == QLatin1String("zoom")) {  // 虫めがね: a click zooms in (Alt: out), a drag zooms into the area
        zoom_drag_ = std::make_pair(pos, pos);
        zoom_out_ = (event->modifiers() & Qt::AltModifier) != 0;
        return;
    }
    if (tool_ == QLatin1String("select")) {
        if (line_press(pos, mm)) return;  // (a balloon or its handles: canvas_lines.cpp)
        last_pos_ = pos;  // a drag on empty paper moves the view (hand), a click chooses a panel
        return;
    }
    if (tool_ == QLatin1String("marquee")) {
        marquee_press(pos, mm, event->modifiers());
        return;
    }
    if (is_drawing_tool(tool_)) {
        perf::input(event, "mouse_press");
        // (a straight line (Shift) starts on the grid)
        const QPointF start = tool_ == QLatin1String("pen") && (event->modifiers() & Qt::ShiftModifier) ? grid_point(mm.x(), mm.y()) : mm;
        const PenSample sample = mouse_sample(start.x(), start.y());
        begin_stroke(QPointF(sample.x_mm, sample.y_mm), sample.pressure, 0.0, false);
    }
}

void PageCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (turning_) {
        const double angle = std::atan2(event->position().y() - height() / 2.0, event->position().x() - width() / 2.0) * 180.0 / kPi;
        double turn = angle - turning_->first;
        const double before = turning_->second;
        if (event->modifiers() & Qt::ControlModifier) turn = std::nearbyint((before + turn) / 15) * 15 - before;  // Ctrl: in 15° steps
        set_rotation(before + turn);
        return;
    }
    if (guide_move(event->position())) return;
    const QPointF pos = unturned(event->position());
    if (zoom_drag_) {
        zoom_drag_->second = pos;
        update();
        return;
    }
    if (panning_) {
        const QPointF delta = pos - last_pos_;
        view_.pan += delta;
        last_pos_ = pos;
        const double now = pan_clock_.elapsed() / 1000.0;
        const double frames = std::max(1.0, (now - pan_time_) / 0.016);  // (the speed per screen frame, a little smoothed)
        pan_speed_ = pan_speed_ * 0.4 + (delta / frames) * 0.6;
        pan_time_ = now;
        view_.fitted = false;
        rerender_.start();
        update();
        return;
    }
    if (tool_drag_) {
        QPointF mm = mm_of(pos);
        if (event->modifiers() & Qt::ShiftModifier) {  // Shift: straight across, down or at 45°
            const QPointF s = tool_drag_->first;
            const double angle = std::nearbyint(std::atan2(mm.y() - s.y(), mm.x() - s.x()) / (kPi / 4)) * (kPi / 4);
            const double length = std::hypot(mm.x() - s.x(), mm.y() - s.y());
            mm = QPointF(s.x() + length * std::cos(angle), s.y() + length * std::sin(angle));
        }
        tool_drag_->second = mm;
        update();
        return;
    }
    if (frame_drag_) {
        modifiers_ = event->modifiers();
        frame_move(pos);
        return;
    }
    const bool pressed = (event->buttons() & Qt::LeftButton) != 0;
    if (tool_ == QLatin1String("ruler")) {
        modifiers_ = event->modifiers();
        if (ruler_move(pos)) return;
    }
    if (prim_move(pos)) return;
    if (tool_move(mm_of(pos), event->modifiers(), pressed)) return;
    if (tool_ == QLatin1String("marquee") && marquee_move(mm_of(pos), event->modifiers(), pressed)) return;
    if (tool_ == QLatin1String("select") && pressed && press_pos_) {
        if ((pos - *press_pos_).manhattanLength() > 6) {
            start_pan(last_pos_);
            mouseMoveEvent(event);
        }
        return;
    }
    if (stroke_.empty()) {
        hover_ = mm_of(pos);
        update_cursor(pos);
        update();
        return;
    }
    const std::uint64_t seq = perf::input(event, "mouse");
    const bool straight = tool_ == QLatin1String("pen") && (event->modifiers() & Qt::ShiftModifier);
    const QPointF mm = straight ? grid_point(mm_of(pos).x(), mm_of(pos).y()) : mm_of(pos);  // (Shift: a straight line, to the grid)
    const PenSample sample = mouse_sample(mm.x(), mm.y());
    extend_stroke(QPointF(sample.x_mm, sample.y_mm), sample.pressure, 0.0, (event->modifiers() & Qt::ShiftModifier) != 0);
    if (seq != 0) {
        perf::event("live_drawn", {{"seq", seq}});
        perf::event("update_requested", {{"seq", seq}});
    }
}

void PageCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (turning_) {
        turning_.reset();
        return;
    }
    if (guide_release(event->position())) return;
    if (zoom_drag_) {
        const auto [start, end] = *zoom_drag_;
        zoom_drag_.reset();
        if (std::abs(end.x() - start.x()) > 6 && std::abs(end.y() - start.y()) > 6) {
            const QPointF a = mm_of(start);
            const QPointF b = mm_of(end);
            zoom_to_rect(a.x(), a.y(), b.x(), b.y());
        } else {
            const bool out = zoom_out_;
            glide([this, out, start] { zoom_by(out ? 0.5 : 2.0, start); });
        }
        update();
        return;
    }
    if (tool_release()) return;  // (線の編集・つまむ・図形・囲って塗る・効果線: before the others, as Python releases them)
    if (tool_drag_) {
        const auto [s, e] = *tool_drag_;
        tool_drag_.reset();
        if (tool_ == QLatin1String("move")) {
            move_image_ = QImage();
            if (std::abs(e.x() - s.x()) > 0.05 || std::abs(e.y() - s.y()) > 0.05) {
                emit layerMoved(core::py_round(e.x() - s.x(), 3), core::py_round(e.y() - s.y(), 3));
            }
        } else if (std::abs(e.x() - s.x()) + std::abs(e.y() - s.y()) > 0.5) {  // (グラデーション: from, to)
            emit gradientRequested(QPointF(core::py_round(s.x(), 3), core::py_round(s.y(), 3)), QPointF(core::py_round(e.x(), 3), core::py_round(e.y(), 3)));
        }
        update();
        return;
    }
    if (panning_) {
        panning_ = false;
        press_pos_.reset();
        update_cursor(unturned(event->position()));
        coast();
        request_tiles();
        return;
    }
    if (frame_drag_) {
        press_pos_.reset();
        frame_release();
        return;
    }
    if (prim_drag_) {
        prim_release();
        return;
    }
    if (tool_ == QLatin1String("ruler")) {
        ruler_release();
        return;
    }
    if (page() == nullptr || event->button() != Qt::LeftButton) return;
    if (tool_ == QLatin1String("marquee")) {
        press_pos_.reset();
        marquee_release();
        return;
    }
    if (tool_ == QLatin1String("select")) {
        press_pos_.reset();
        const QPointF mm = to_mm(event->position());
        if (const core::Frame* frame = page()->frame_at(core::Num(mm.x()), core::Num(mm.y()))) {
            emit frameSelected(QString::fromStdString(frame->id));
        }
        return;
    }
    if (!stroke_.empty()) finish_stroke();
}

void PageCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        fit_page();
        return;
    }
    if (tool_ == QLatin1String("marquee") && !poly_points_.empty()) {
        finish_points();
        return;
    }
    if (tool_ == QLatin1String("shape") && !shape_pts_.empty()) {
        finish_shape();
        return;
    }
    if (tool_ == QLatin1String("ruler")) {
        finish_curve();
        return;
    }
    if (tool_ == QLatin1String("frame") && !frame_poly_.empty()) {
        if (frame_poly_.size() > 1 && frame_poly_.back() == frame_poly_[frame_poly_.size() - 2]) {
            frame_poly_.pop_back();  // (the double click's second press added the same corner again)
        }
        finish_frame_poly();
        return;
    }
    if (page() != nullptr && event->button() == Qt::LeftButton && tool_ == QLatin1String("select")) {
        line_double_click(to_mm(event->position()));  // (a balloon: type over it)
        return;
    }
    // (otherwise the second press of a double click is swallowed, as in Python: never a second fill, stamp or cut)
}

void PageCanvas::leaveEvent(QEvent*) {
    hover_.reset();
    update();
}

void PageCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (page() == nullptr) return;
    const QPointF mm = to_mm(QPointF(event->pos()));
    if (line_context_menu(mm, event->globalPos())) return;  // (a balloon's own menu)
    const core::Frame* frame = page()->frame_at(core::Num(mm.x()), core::Num(mm.y()));
    const std::string chosen = page()->selected_frame_id.is_string() ? page()->selected_frame_id.get<std::string>() : std::string();
    if (frame != nullptr && frame->id != chosen) emit frameSelected(QString::fromStdString(frame->id));
    emit contextMenuAt(frame != nullptr ? QString::fromStdString(frame->id) : QString(), event->globalPos());
}

void PageCanvas::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
        if (delta != 0) zoom_by(1.0 + std::max(-0.5, std::min(0.5, delta / 600.0)), unturned(event->position()));
        return;
    }
    const QPoint pixel = event->pixelDelta();
    double dx = !pixel.isNull() ? pixel.x() : event->angleDelta().x() / 3.0;
    double dy = !pixel.isNull() ? pixel.y() : event->angleDelta().y() / 3.0;
    if ((event->modifiers() & Qt::ShiftModifier) && dx == 0.0) {
        dx = dy;
        dy = 0;
    }
    const QPointF moved = unturned(QPointF(dx, dy)) - unturned(QPointF(0, 0));  // the page follows the fingers, turned or not
    view_.pan += moved;
    view_.fitted = false;
    rerender_.start();
    update();
}

bool PageCanvas::event(QEvent* e) {
    if (e->type() == QEvent::Gesture) {  // two fingers on a touch screen: zoom, turn, move
        auto* gesture = static_cast<QGestureEvent*>(e);
        if (auto* p = static_cast<QPinchGesture*>(gesture->gesture(Qt::PinchGesture))) {
            pinch(p->scaleFactor(), p->rotationAngle() - p->lastRotationAngle(), mapFromGlobal(p->centerPoint()),
                  p->centerPoint() - p->lastCenterPoint());
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::NativeGesture) {  // a trackpad's pinch
        auto* native = static_cast<QNativeGestureEvent*>(e);
        if (native->gestureType() == Qt::ZoomNativeGesture) {
            zoom_by(1.0 + native->value(), unturned(native->position()));
            return true;
        }
    }
    return QWidget::event(e);
}

// --- the keys ------------------------------------------------------------------------------------------------------

void PageCanvas::hold_modifier(const QString& key, bool down) {
    // Ctrl held: the select tool for a moment, Alt the eyedropper (スポイト); back to the tool before when let go
    // (環境設定's own choice of these comes with the preferences)
    const QString base = held_tool_.value_or(tool_);
    QString tool = key == QLatin1String("ctrl") ? QStringLiteral("select") : key == QLatin1String("alt") ? QStringLiteral("picker") : QString();
    if ((base == QLatin1String("zoom") || base == QLatin1String("marquee")) && key == QLatin1String("alt")) tool.clear();
    if (down) {
        // (not while a line, a selection or its handles are being dragged: the drag ends with the tool it began with)
        const bool dragging = !stroke_.empty() || !marquee_stroke_.empty() || sel_drag_ || ellipse_drag_ || !lasso_fill_.empty() || shape_drag_ ||
                              vector_drag_ || vector_trace_ || reshape_ || tool_drag_ || zoom_drag_ || frame_drag_ || prim_drag_ ||
                              ruler_drag_ || effect_drag_ || line_drag_ || handle_drag_ || !balloon_stroke_.empty() || panning_;
        if (!tool.isEmpty() && !held_tool_ && !dragging && tool != tool_) {
            held_tool_ = tool_;
            held_key_ = key;
            tool_ = tool;
            update_cursor();
            update();
            emit toolHeld(tool_);
        }
    } else if (held_tool_ && held_key_ == key) {
        tool_ = *held_tool_;
        held_tool_.reset();
        update_cursor();
        update();
        emit toolHeld(tool_);
    }
}

void PageCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_ = true;
        update_cursor();
        return;
    }
    if ((event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift) && !event->isAutoRepeat()) {
        hold_modifier(event->key() == Qt::Key_Alt ? QStringLiteral("alt") : event->key() == Qt::Key_Control ? QStringLiteral("ctrl")
                                                                                                               : QStringLiteral("shift"),
                      true);
    }
    if (!shape_pts_.empty() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finish_shape((event->modifiers() & Qt::ShiftModifier) != 0);  // (Shift+Enter: closed)
        return;
    }
    if (event->key() == Qt::Key_Escape && cancel_shape()) return;
    if (tool_ == QLatin1String("vector") && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && vector_delete()) return;
    if (tool_ == QLatin1String("ruler") && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finish_curve();
        return;
    }
    if (tool_ == QLatin1String("ruler") && event->key() == Qt::Key_Escape && cancel_ruler()) return;
    if (!frame_poly_.empty() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finish_frame_poly();
        return;
    }
    if (!frame_poly_.empty() && event->key() == Qt::Key_Escape) {
        frame_poly_.clear();
        update();
        return;
    }
    if (marquee_key(event)) return;
    if (event->key() == Qt::Key_Escape && selection_) {
        set_selection(std::nullopt);
        return;
    }
    if (event->key() == Qt::Key_Escape && line_drag_) {  // (a balloon being dragged stays where it was)
        line_drag_.reset();
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PageCanvas::keyReleaseEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift) && !event->isAutoRepeat()) {
        hold_modifier(event->key() == Qt::Key_Alt ? QStringLiteral("alt") : event->key() == Qt::Key_Control ? QStringLiteral("ctrl")
                                                                                                               : QStringLiteral("shift"),
                      false);
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_ = false;
        panning_ = false;
        update_cursor(unturned(QPointF(mapFromGlobal(QCursor::pos()))));
        return;
    }
    QWidget::keyReleaseEvent(event);
}

// --- the pen -------------------------------------------------------------------------------------------------------

void PageCanvas::back_from_eraser_end() {
    if (eraser_end_) {
        tool_ = *eraser_end_;
        eraser_end_.reset();
        update_cursor();
    }
}

void PageCanvas::tabletEvent(QTabletEvent* event) {
    const auto type = event->type();
    last_pressure_ = event->pressure();  // (the vector tool's traces take the pen's pressure)
    const bool eraser_end = event->pointerType() == QPointingDevice::PointerType::Eraser;
    if (type == QEvent::TabletPress && eraser_end && page() != nullptr && tool_ != QLatin1String("eraser") && !space_) {
        eraser_end_ = tool_;  // the pen turned over: erase for this stroke, then back
        tool_ = QStringLiteral("eraser");
        update_cursor();
    }
    if (page() == nullptr || !is_drawing_tool(tool_) || space_) {
        event->ignore();  // (the other tools work with the pen as a mouse)
        return;
    }
    if (type == QEvent::TabletPress && event->button() != Qt::LeftButton) {
        event->ignore();  // (a side button: the right click's menu)
        return;
    }
    const QPointF mm = to_mm(event->position());
    const PenSample sample = tablet_sample(mm.x(), mm.y(), *event);
    if (type == QEvent::TabletPress) {
        perf::input(event, "tablet_press");
        begin_stroke(QPointF(sample.x_mm, sample.y_mm), sample.pressure, sample.rotation, true);
        event->accept();
        return;
    }
    if (type == QEvent::TabletMove && !stroke_.empty()) {
        const std::uint64_t seq = perf::input(event, "tablet");
        extend_stroke(QPointF(sample.x_mm, sample.y_mm), sample.pressure, sample.rotation, (event->modifiers() & Qt::ShiftModifier) != 0);
        if (seq != 0) {
            perf::event("live_drawn", {{"seq", seq}});
            perf::event("update_requested", {{"seq", seq}});
        }
        event->accept();
        return;
    }
    if (type == QEvent::TabletRelease && !stroke_.empty()) {
        finish_stroke();
        event->accept();
        return;
    }
    if (type == QEvent::TabletRelease) {
        back_from_eraser_end();
        event->accept();
        return;
    }
    if (type == QEvent::TabletMove) {
        hover_ = mm;
        update();
    }
    event->accept();
}

// --- the panel tool: drag gutters, cut panels (any angle), move corners, bow edges, draw panels ----------------------

std::optional<Gutter> PageCanvas::hit_gutter(double x_mm, double y_mm) const {
    const core::Page* p = page();
    if (p == nullptr || p->frames.empty()) return std::nullopt;
    const double tolerance = 6 / view_.scale;
    std::optional<std::pair<double, Gutter>> best;
    for (const Gutter& gutter : gutters(p->frames[0])) {
        const double d = distance_to_segment(QPointF(x_mm, y_mm), gutter.p0, gutter.p1);
        if (d <= gutter.width / 2 + tolerance && (!best || d < best->first)) best = std::make_pair(d, gutter);
    }
    if (!best) return std::nullopt;
    return best->second;
}

std::vector<std::pair<int, QPointF>> PageCanvas::vertex_handles() const {
    const core::Page* p = page();
    if (tool_ != QLatin1String("frame") || p == nullptr || !p->selected_frame_id.is_string()) return {};
    const core::Frame* frame = p->find_frame(p->selected_frame_id.get<std::string>());
    if (frame == nullptr) return {};
    const std::vector<QPointF> pts = frame_drag_ && frame_drag_->kind == QLatin1String("vertex") ? frame_drag_->poly : shape_of(*frame);
    std::vector<std::pair<int, QPointF>> out;
    for (std::size_t i = 0; i < pts.size(); ++i) out.emplace_back(static_cast<int>(i), pts[i]);
    return out;
}

std::vector<std::pair<int, QPointF>> PageCanvas::bow_handles() const {
    // the ◇ in the middle of each edge of the chosen panel: drag it out or in to bow the edge (曲線の枠)
    const core::Page* p = page();
    if (tool_ != QLatin1String("frame") || p == nullptr || !p->selected_frame_id.is_string()) return {};
    const core::Frame* frame = p->find_frame(p->selected_frame_id.get<std::string>());
    if (frame == nullptr || !frame->children.empty()) return {};
    const std::vector<QPointF> pts = shape_of(*frame);
    const std::vector<double> curves = curves_of(*frame, pts.size());
    std::vector<std::pair<int, QPointF>> out;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const EdgeNormal e = edge_normal(pts, i);
        const bool held = frame_drag_ && frame_drag_->kind == QLatin1String("bow") && frame_drag_->edge == static_cast<int>(i);
        const double bow = held ? frame_drag_->mm : (i < curves.size() ? curves[i] : 0.0);
        out.emplace_back(static_cast<int>(i), e.mid + e.normal * bow);
    }
    return out;
}

void PageCanvas::frame_press(const QPointF& pos) {
    const core::Page* p = page();
    const QPointF mm = mm_of(pos);
    const auto near = [&](const QPointF& at, double reach) {
        const QPointF q = pt(at.x(), at.y());
        return std::abs(q.x() - pos.x()) <= reach && std::abs(q.y() - pos.y()) <= reach;
    };
    for (const auto& [index, at] : vertex_handles()) {
        if (near(at, kHandlePx + 2)) {
            const core::Frame* frame = p->find_frame(p->selected_frame_id.get<std::string>());
            FrameDrag drag;
            drag.kind = QStringLiteral("vertex");
            drag.index = index;
            drag.frame = QString::fromStdString(frame->id);
            drag.poly = shape_of(*frame);
            frame_drag_ = drag;
            return;
        }
    }
    for (const auto& [index, at] : bow_handles()) {
        if (near(at, kHandlePx + 2)) {
            const core::Frame* frame = p->find_frame(p->selected_frame_id.get<std::string>());
            FrameDrag drag;
            drag.kind = QStringLiteral("bow");
            drag.edge = index;
            drag.frame = QString::fromStdString(frame->id);
            drag.curves = curves_of(*frame, shape_of(*frame).size());
            drag.mm = drag.curves[static_cast<std::size_t>(index)];
            frame_drag_ = drag;
            return;
        }
    }
    if (frame_mode == QLatin1String("poly")) {  // (corner by corner; a click on the first corner, Enter or a double click closes it)
        if (frame_poly_.size() >= 3 && near(frame_poly_.front(), kHandlePx + 3)) {
            finish_frame_poly();
            return;
        }
        frame_poly_.push_back(QPointF(r2(mm.x()), r2(mm.y())));
        return;
    }
    if (frame_mode == QLatin1String("rect") || frame_mode == QLatin1String("free")) {
        FrameDrag drag;
        drag.kind = frame_mode;
        drag.p0 = drag.p1 = mm;
        drag.points = {mm};
        frame_drag_ = drag;
        return;
    }
    if (const auto gutter = hit_gutter(mm.x(), mm.y())) {
        FrameDrag drag;
        drag.kind = QStringLiteral("gutter");
        drag.gutter = *gutter;
        drag.start = mm;
        frame_drag_ = drag;
        return;
    }
    // a cut may start outside the panels (from the margin across): the panel is found on release
    const core::Frame* frame = p->frame_at(core::Num(mm.x()), core::Num(mm.y()));
    FrameDrag drag;
    drag.kind = QStringLiteral("cut");
    drag.frame = frame != nullptr ? QString::fromStdString(frame->id) : QString();
    drag.p0 = drag.p1 = mm;
    frame_drag_ = drag;
}

void PageCanvas::frame_move(const QPointF& pos) {
    FrameDrag& drag = *frame_drag_;
    QPointF mm = mm_of(pos);
    if (drag.kind == QLatin1String("rect") || drag.kind == QLatin1String("free")) {
        drag.p1 = mm;
        if (drag.kind == QLatin1String("free") && std::hypot(drag.points.back().x() - mm.x(), drag.points.back().y() - mm.y()) >= 0.5) {
            drag.points.push_back(mm);
        }
        update();
        return;
    }
    if (drag.kind == QLatin1String("vertex")) {
        drag.poly[static_cast<std::size_t>(drag.index)] = QPointF(r2(mm.x()), r2(mm.y()));
    } else if (drag.kind == QLatin1String("bow")) {
        if (const core::Frame* frame = page()->find_frame(drag.frame.toStdString())) {
            const std::vector<QPointF> pts = shape_of(*frame);
            const EdgeNormal e = edge_normal(pts, static_cast<std::size_t>(drag.edge));
            const QPointF a = pts[static_cast<std::size_t>(drag.edge)];
            const QPointF b = pts[(static_cast<std::size_t>(drag.edge) + 1) % pts.size()];
            const double edge = std::hypot(b.x() - a.x(), b.y() - a.y());
            const double bow = (mm.x() - e.mid.x()) * e.normal.x() + (mm.y() - e.mid.y()) * e.normal.y();
            drag.mm = r2(std::max(-edge / 2 + 0.01, std::min(edge / 2 - 0.01, bow)));
        }
    } else if (drag.kind == QLatin1String("gutter")) {
        const Gutter& g = drag.gutter;
        double length = std::hypot(g.p1.x() - g.p0.x(), g.p1.y() - g.p0.y());
        if (length == 0.0) length = 1.0;
        double nx = -(g.p1.y() - g.p0.y()) / length;
        double ny = (g.p1.x() - g.p0.x()) / length;
        if ((g.horizontal && ny < 0) || (!g.horizontal && nx < 0)) {
            nx = -nx;
            ny = -ny;
        }
        const double delta = (mm.x() - drag.start.x()) * nx + (mm.y() - drag.start.y()) * ny;
        drag.delta = delta;
        drag.offset = QPointF(nx * delta, ny * delta);
    } else {
        // nearly level or upright cuts snap straight (hold Alt for a free angle)
        const double dx = mm.x() - drag.p0.x();
        const double dy = mm.y() - drag.p0.y();
        const bool free = (modifiers_ & Qt::AltModifier) != 0;
        if (!free && std::abs(dy) <= std::abs(dx) * 0.07) {
            mm.setY(drag.p0.y());
        } else if (!free && std::abs(dx) <= std::abs(dy) * 0.07) {
            mm.setX(drag.p0.x());
        }
        drag.p1 = mm;
    }
    update();
}

bool PageCanvas::finish_frame_poly() {
    // the panel drawn corner by corner is closed: it becomes a panel (three corners or more)
    std::vector<QPointF> pts = std::move(frame_poly_);
    frame_poly_.clear();
    update();
    if (pts.size() < 3) return false;
    emit frameDrawn(QVector<QPointF>(pts.begin(), pts.end()));
    return true;
}

void PageCanvas::frame_release() {
    const FrameDrag drag = *frame_drag_;
    frame_drag_.reset();
    if (drag.kind == QLatin1String("rect")) {
        const double x0 = drag.p0.x(), y0 = drag.p0.y(), x1 = drag.p1.x(), y1 = drag.p1.y();
        if (std::abs(x1 - x0) >= 5 && std::abs(y1 - y0) >= 5) {
            const double xa = std::min(x0, x1), xb = std::max(x0, x1), ya = std::min(y0, y1), yb = std::max(y0, y1);
            emit frameDrawn({QPointF(r2(xa), r2(ya)), QPointF(r2(xb), r2(ya)), QPointF(r2(xb), r2(yb)), QPointF(r2(xa), r2(yb))});
        }
    } else if (drag.kind == QLatin1String("free")) {
        if (drag.points.size() >= 8) {
            QVector<QPointF> pts;
            for (const QPointF& q : drag.points) pts.push_back(QPointF(r2(q.x()), r2(q.y())));
            emit frameDrawn(pts);
        }
    } else if (drag.kind == QLatin1String("vertex")) {
        emit frameShaped(drag.frame, QVector<QPointF>(drag.poly.begin(), drag.poly.end()));
    } else if (drag.kind == QLatin1String("bow")) {
        if (std::abs(drag.mm - drag.curves[static_cast<std::size_t>(drag.edge)]) > 0.05) {
            emit frameBowed(drag.frame, drag.edge, std::abs(drag.mm) > 0.5 ? drag.mm : 0.0);
        }
    } else if (drag.kind == QLatin1String("gutter")) {
        if (std::abs(drag.delta) > 0.2) emit gutterMoved(QString::fromStdString(drag.gutter.node), drag.gutter.index, r2(drag.delta));
    } else if (std::hypot(drag.p1.x() - drag.p0.x(), drag.p1.y() - drag.p0.y()) >= 5) {
        const QPointF mid = (drag.p0 + drag.p1) / 2;
        const core::Frame* target = page()->frame_at(core::Num(mid.x()), core::Num(mid.y()));
        const QString frame_id = target != nullptr ? QString::fromStdString(target->id) : drag.frame;
        if (!frame_id.isEmpty()) emit cutRequested(frame_id, drag.p0, drag.p1);
    } else if (!drag.frame.isEmpty()) {
        emit frameSelected(drag.frame);
    }
    update();
}

}  // namespace genko::app
