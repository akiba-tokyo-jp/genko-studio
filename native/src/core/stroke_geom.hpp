#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/model.hpp"

// The pen-line geometry of Python's genko/stroke.py (and brushes.smoothed) that needs no picture: steadying,
// tapering, curve fitting, pressure curves and the vector eraser. Every function returns the values Python returns,
// to the last bit (the same operations in the same order, Python 3.12's sum(), libm called as CPython calls it), and
// fails where Python fails, with the exceptions of core/pyops.hpp (as the ops report them). The arguments' defaults
// are Python's.

namespace genko::core {

// 手ブレ補正: each point the average of its neighbours (by_speed: further where the hand moved quickly).
PenPoints stabilize_points(const PenPoints& points, std::int64_t window = 5, bool by_speed = false);

// 入り抜き: the line thins towards its ends (a quarter of the points at each end, or in_mm / out_mm along the line;
// nothing for both is the quarter-of-the-points taper). Every point comes back with a pressure.
PenPoints taper_points(const PenPoints& points, std::optional<double> in_mm = std::nullopt,
                       std::optional<double> out_mm = std::nullopt);

// 後補正（曲線に置き換え）: the points that shape the line (within tolerance_mm), joined again by a Catmull–Rom curve
// with a point every step_mm; every point gets a pressure (1.0 where it had none). Python's int() of the steps between
// two points: PyValueError for NaN, PyUncaught for an infinity (OverflowError) or a walk too long (MemoryError).
PenPoints fit_curve(const PenPoints& points, double tolerance_mm = 0.3, double step_mm = 0.5);

// The G pen's pressure curve (p ** 1.8, kept in 0.05..1); "" and "linear" leave the points as they are. PyTypeError
// for a negative pressure (Python's power is complex then, and min() refuses it); -inf ** 1.8 is inf (so 1.0).
PenPoints apply_pressure_curve(const PenPoints& points, std::string_view curve = "gpen");

// A pen point from the device: no pressure is 0.7; a tilt adds up to a quarter.
PenPoint pack_point(double x_mm, double y_mm, std::optional<double> pressure = std::nullopt, double tilt = 0.0);

// brushes.smoothed (後補正): a moving average of `strength` points either side; the ends stay put.
PenPoints smoothed(const PenPoints& points, std::int64_t strength);

// Whether (x, y) is within radius_mm of the eraser's path (split_by_eraser's near; one point is a segment of no
// length, no points touch nothing).
bool near_eraser(double x, double y, const PenPoints& eraser, double radius_mm);

// The pieces of a line that survive an eraser path (the line is cut, not painted over).
std::vector<PenPoints> split_by_eraser(const PenPoints& points, const PenPoints& eraser, double radius_mm);

// stroke._seg_cross: the parameter t along a→b where it crosses c→d, or nothing.
std::optional<double> seg_cross(const PointF& a, const PointF& b, const PointF& c, const PointF& d);

// 交点まで消す: the part of each touched line between the crossings (with the other lines) around the touch is
// erased. Untouched lines come back as they are (the same pointers); the pieces kept are new strokes with new ids.
std::vector<StrokePtr> erase_to_crossing(std::span<const StrokePtr> strokes, const PenPoints& eraser,
                                         double radius_mm);

}  // namespace genko::core
