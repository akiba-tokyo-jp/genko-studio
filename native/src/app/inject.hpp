#pragma once

#include <QPointF>

#include <vector>

class QWidget;

// Input for tests and the test script: pen lines and clicks given to a widget as the window system would give them
// (QMouseEvent, QTabletEvent with pressure, tilt and the barrel's turn), so they go through the canvas's own handlers.

namespace genko::app {
class PageCanvas;
}

namespace genko::app::inject {

// One point of a pen line: where on the page (mm) and what the pen reports there.
struct PenInput {
    QPointF mm;
    double pressure = 1.0;
    double x_tilt = 0.0;  // degrees, as QTabletEvent::xTilt
    double y_tilt = 0.0;
    double rotation = 0.0;
};

enum class Phase { Press, Move, Release };

// One event of a line: the left mouse button, or the pen (eraser_end: the pen turned over). timestamp: the event's
// time in ms as the window system would stamp it (0: now, on the injector's own clock).
void mouse(PageCanvas* canvas, Phase phase, const QPointF& mm, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
void tablet(PageCanvas* canvas, Phase phase, const PenInput& point, bool eraser_end = false, quint64 timestamp = 0);

// A line drawn with the left mouse button (press, moves, release). pump: let the event loop run between the moves
// (the screen is painted as with a real mouse).
void mouse_stroke(PageCanvas* canvas, const std::vector<QPointF>& mm, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                  bool pump = false);
// A line drawn with a pen (TabletPress, TabletMove…, TabletRelease); eraser_end: the pen turned over.
void tablet_stroke(PageCanvas* canvas, const std::vector<PenInput>& points, bool eraser_end = false, bool pump = false);
// A left click at a point of the page.
void click_mm(PageCanvas* canvas, const QPointF& mm, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
// A left click at a point of a widget.
void click(QWidget* widget, const QPointF& pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

}  // namespace genko::app::inject
