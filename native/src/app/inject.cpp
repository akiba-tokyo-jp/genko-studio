#include "app/inject.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>

#include "app/canvas.hpp"

namespace genko::app::inject {

namespace {

// The events' timestamps: milliseconds on one clock, as the window system stamps them.
quint64 stamp() {
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    return static_cast<quint64>(clock.elapsed()) + 1;
}

const QPointingDevice* stylus(bool eraser) {
    using Cap = QInputDevice::Capability;
    const QInputDevice::Capabilities caps = Cap::Position | Cap::Pressure | Cap::XTilt | Cap::YTilt | Cap::Rotation | Cap::Hover;
    static const QPointingDevice pen(QStringLiteral("Genko test pen"), 0x6e6b01, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen, caps, 1, 3);
    static const QPointingDevice rubber(QStringLiteral("Genko test pen (eraser end)"), 0x6e6b01, QInputDevice::DeviceType::Stylus,
                                        QPointingDevice::PointerType::Eraser, caps, 1, 3);
    return eraser ? &rubber : &pen;
}

void send_mouse(QWidget* widget, QEvent::Type type, const QPointF& pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(type, pos, widget->mapToGlobal(pos), button, buttons, modifiers);
    event.setTimestamp(stamp());
    QApplication::sendEvent(widget, &event);
}

void send_tablet(QWidget* widget, QEvent::Type type, const PenInput& p, const QPointF& pos, bool eraser, Qt::MouseButton button,
                 Qt::MouseButtons buttons, quint64 timestamp = 0) {
    QTabletEvent event(type, stylus(eraser), pos, widget->mapToGlobal(pos), p.pressure, static_cast<float>(p.x_tilt),
                       static_cast<float>(p.y_tilt), 0.0F, p.rotation, 0.0F, Qt::NoModifier, button, buttons);
    event.setTimestamp(timestamp != 0 ? timestamp : stamp());
    QApplication::sendEvent(widget, &event);
}

}  // namespace

void mouse(PageCanvas* canvas, Phase phase, const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    const QPointF at = canvas->to_widget(mm);
    switch (phase) {
    case Phase::Press:
        send_mouse(canvas, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, modifiers);
        break;
    case Phase::Move:
        send_mouse(canvas, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton, modifiers);
        break;
    case Phase::Release:
        send_mouse(canvas, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, modifiers);
        break;
    }
}

void tablet(PageCanvas* canvas, Phase phase, const PenInput& point, bool eraser_end, quint64 timestamp) {
    const QPointF at = canvas->to_widget(point.mm);
    switch (phase) {
    case Phase::Press:
        send_tablet(canvas, QEvent::TabletPress, point, at, eraser_end, Qt::LeftButton, Qt::LeftButton, timestamp);
        break;
    case Phase::Move:
        send_tablet(canvas, QEvent::TabletMove, point, at, eraser_end, Qt::NoButton, Qt::LeftButton, timestamp);
        break;
    case Phase::Release:
        send_tablet(canvas, QEvent::TabletRelease, point, at, eraser_end, Qt::LeftButton, Qt::NoButton, timestamp);
        break;
    }
}

void mouse_stroke(PageCanvas* canvas, const std::vector<QPointF>& mm, Qt::KeyboardModifiers modifiers, bool pump) {
    if (mm.empty()) return;
    send_mouse(canvas, QEvent::MouseButtonPress, canvas->to_widget(mm.front()), Qt::LeftButton, Qt::LeftButton, modifiers);
    for (std::size_t i = 1; i < mm.size(); ++i) {
        send_mouse(canvas, QEvent::MouseMove, canvas->to_widget(mm[i]), Qt::NoButton, Qt::LeftButton, modifiers);
        if (pump) QCoreApplication::processEvents();
    }
    send_mouse(canvas, QEvent::MouseButtonRelease, canvas->to_widget(mm.back()), Qt::LeftButton, Qt::NoButton, modifiers);
}

void tablet_stroke(PageCanvas* canvas, const std::vector<PenInput>& points, bool eraser_end, bool pump) {
    if (points.empty()) return;
    send_tablet(canvas, QEvent::TabletPress, points.front(), canvas->to_widget(points.front().mm), eraser_end, Qt::LeftButton, Qt::LeftButton);
    for (std::size_t i = 1; i < points.size(); ++i) {
        send_tablet(canvas, QEvent::TabletMove, points[i], canvas->to_widget(points[i].mm), eraser_end, Qt::NoButton, Qt::LeftButton);
        if (pump) QCoreApplication::processEvents();
    }
    PenInput last = points.back();
    last.pressure = 0.0;  // (the pen leaves the paper)
    send_tablet(canvas, QEvent::TabletRelease, last, canvas->to_widget(last.mm), eraser_end, Qt::LeftButton, Qt::NoButton);
}

void click_mm(PageCanvas* canvas, const QPointF& mm, Qt::KeyboardModifiers modifiers) { click(canvas, canvas->to_widget(mm), modifiers); }

void click(QWidget* widget, const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    send_mouse(widget, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, modifiers);
    send_mouse(widget, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, modifiers);
}

}  // namespace genko::app::inject
