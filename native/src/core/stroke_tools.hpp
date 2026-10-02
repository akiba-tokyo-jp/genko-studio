#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/stroke_geom.hpp"
#include "core/strokes.hpp"

// The helpers of genko/ops.py that work on a pen line's points (the geometry itself is stroke.py's:
// core/stroke_geom.hpp): every step computes in the same order as Python, so the points kept are the same doubles.

namespace genko::core {

// ops._parse_points: [[x, y], [x, y, p], …] (a third number is the pressure; more are ignored). OpError
// "points needs [x_mm, y_mm]" for a point with fewer than two; Python's errors for values that are not numbers.
PenPoints parse_points(const Json& raw);

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

// ops._untouched: no point of the line within the eraser's radius.
bool untouched(const Stroke& stroke, const PenPoints& eraser, double radius_mm);

}  // namespace genko::core
