#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// The work on a pen line's points before it is kept and when it is cut (Python's genko/stroke.py, brushes.smoothed
// and the helpers of genko/ops.py): every step computes in the same order as Python, so the points kept are the same
// doubles.

namespace genko::core {

// A point of a line as Python passes it along: [x, y] or [x, y, pressure].
struct PenPoint {
    double x = 0.0;
    double y = 0.0;
    std::optional<double> p;
};

using PenPoints = std::vector<PenPoint>;

// Python 3.12's sum() of floats (from int 0: the first added plainly, the rest with Neumaier's compensation).
double py_sum_doubles(std::span<const double> values);

// ops._parse_points: [[x, y], [x, y, p], …] (a third number is the pressure; more are ignored). OpError
// "points needs [x_mm, y_mm]" for a point with fewer than two; Python's errors for values that are not numbers.
PenPoints parse_points(const Json& raw);

// The points of a stroke as stroke_points gives them: with their pressure when every point has one.
PenPoints stroke_points(const Stroke& stroke);

// models.coerce_stroke(points): a new stroke (new id) with these points and, when every point has one, their pressure.
Stroke coerce_stroke(const PenPoints& points);

// stroke.stabilize_points (手ブレ補正; by_speed: 速度による手ブレ補正).
PenPoints stabilize_points(const PenPoints& points, std::int64_t window, bool by_speed);

// brushes.smoothed (後補正): a moving average that keeps the ends.
PenPoints smoothed(const PenPoints& points, std::int64_t strength);

// stroke.fit_curve (後補正（曲線に置き換え）): every point gets a pressure (1.0 where it had none).
PenPoints fit_curve(const PenPoints& points, double tolerance_mm, double step_mm = 0.5);

// stroke.taper_points (入り抜き): nothing for both lengths is the quarter-of-the-points taper.
PenPoints taper_points(const PenPoints& points, std::optional<double> in_mm, std::optional<double> out_mm);

// stroke.apply_pressure_curve: any curve but "" and "linear" is the 1.8 power. PyTypeError for a negative pressure
// (Python's power is complex then, and min() refuses it).
PenPoints apply_pressure_curve(const PenPoints& points, std::string_view curve);

// ops._rdp (Ramer–Douglas–Peucker) with ops._perp.
PenPoints rdp(const PenPoints& points, double epsilon);

// ops._resampled: the numbers stretched or squeezed evenly to `count`, rounded to a tenth.
std::vector<double> resampled(const std::vector<double>& values, std::int64_t count);

// ops._nearest_segment over a polyline: (index of the segment's first point, distance, the nearest point).
struct NearestSegment {
    std::size_t index = 0;
    double distance = 0.0;
    double x = 0.0;
    double y = 0.0;
};
NearestSegment nearest_segment(std::span<const PointF> points, double x, double y);

// stroke.split_by_eraser: the pieces of a line that survive an eraser path.
std::vector<PenPoints> split_by_eraser(const PenPoints& points, const PenPoints& eraser, double radius_mm);

// ops._untouched: no point of the line within the eraser's radius.
bool untouched(const Stroke& stroke, const PenPoints& eraser, double radius_mm);

// stroke.erase_to_crossing (交点まで消す): the strokes after the eraser, the parts left as new strokes (new ids).
std::vector<StrokePtr> erase_to_crossing(const std::vector<StrokePtr>& strokes, const PenPoints& eraser,
                                         double radius_mm);

}  // namespace genko::core
