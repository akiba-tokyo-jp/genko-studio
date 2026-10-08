#pragma once

#include <QLabel>
#include <QTimer>

#include <functional>

#include "core/json.hpp"

class QDoubleSpinBox;
class QSlider;
class QSpinBox;

// Fields as painting tools have them (Python's genko/app/fields.py): a slider beside the number (drag for a feel,
// type for an exact value), and a sample line drawn with the pen as it is set now, so a change is seen before the
// page is touched.

namespace genko::app {

// A row: a slider and the number field it moves (and follows). `log`: even steps over a wide range (a pen from 0.05
// to 50 mm moves as finely at the thin end as at the thick).
QWidget* slider_for(QDoubleSpinBox* spin, bool log = false);
QWidget* slider_for(QSpinBox* spin, bool log = false);

// A slider with its value written beside it.
QWidget* with_value(QSlider* slider, const QString& suffix = QStringLiteral("%"));

// The pen as it is set now, drawn: an S of one line pressed lightly, hard, then lightly, at the real width (to a
// limit), with its taper, pressure, opacity and colour. Drawn again a moment after a setting changes.
class LineSample : public QLabel {
    Q_OBJECT
public:
    // fields: {"kind", "width_mm", "taper", "pressure_gamma"?, "opacity"?, "rgb"?, "taper_in_mm"?, …}
    explicit LineSample(std::function<core::Json()> fields, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {160, 56}; }
    QSize minimumSizeHint() const override { return {80, 56}; }
    void refresh() { timer_.start(); }
    // Draw now (tests).
    void draw();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    std::function<core::Json()> fields_;
    QTimer timer_;
};

}  // namespace genko::app
