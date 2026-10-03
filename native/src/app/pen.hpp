#pragma once

#include <QString>

#include <optional>
#include <string>
#include <vector>

#include "core/brushes.hpp"
#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

class QTabletEvent;

// The pen in hand: how a device's input becomes a pen point (Python's stroke.pack_point: no pressure — the mouse —
// is 0.7; a tilt adds up to a quarter) and what a new line carries (Python's BrushPanel.stroke_fields). The brush
// panel itself (choosing kinds, sizes, colours) comes with M3; the settings live in the app's settings under the same
// keys as Python's (brush/kind, brush/<kind>/size, brush/rgb …).

namespace genko::app {

struct PenSample {
    double x_mm = 0.0;
    double y_mm = 0.0;
    double pressure = 0.7;  // as packed (0.05 … 1)
    double rotation = 0.0;  // the barrel's turn (degrees; pens that report it)
};

// A tablet event's point (Python's PageCanvas.tabletEvent): pressure from the pen, the tilt's share
// (|xTilt| / 60), the rotation as the pen gives it.
PenSample tablet_sample(double x_mm, double y_mm, const QTabletEvent& event);
// The mouse's point: pressure 0.7.
PenSample mouse_sample(double x_mm, double y_mm);

struct PenSettings {
    std::string kind = std::string(core::kDefaultBrush);
    double width_mm = 0.5;
    double opacity = 1.0;          // 0.05 … 1 (the slider's share)
    std::int64_t stabilize = 3;
    bool taper = true;
    double taper_in_mm = -1;       // -1: 自動
    double taper_out_mm = -1;
    double pressure_gamma = 1.0;   // 0.7 やわらかい | 1.0 ふつう | 1.6 かたい
    std::vector<std::int64_t> rgb{20, 20, 20};
    double ink_pressure = 0;       // 0 … 100 %
    bool speed_steady = false;
    double post_fit = 0;
    double snap_lines = 0;

    // The kind's own defaults (BrushPanel._apply_kind_defaults without stored values).
    static PenSettings for_kind(const std::string& kind);
    // The settings kept in the app's settings (BrushPanel._load).
    static PenSettings load();
    void save() const;

    // What a new line carries (add_stroke fields, in Python's order).
    core::Json stroke_fields() const;
};

}  // namespace genko::app
