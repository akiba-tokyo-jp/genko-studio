#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Strokes as saved (Python's stroke_to_packed / coerce_stroke in genko/models.py).

namespace genko::core {

// Little-endian float64 values as base64 (Python's _pack(values, "d")).
std::string pack_doubles(std::span<const double> values);
// The reverse (Python's _unpack(text, "d")): binascii.a2b_base64, then whole 8-byte values. Throws
// core::Error("format") with Python's message for text that is not base64 or a length that is not a multiple of 8.
std::vector<double> unpack_doubles(std::string_view base64);

// A stroke for a .strokes.json blob: {"id", "kind", "width_mm", "xy"} and, when they say something, "p" (pressure),
// "rgb", "opacity" (not 1.0), "r" (rotation) and "po" (pressure opacity, not 0).
Json stroke_to_packed(const Stroke& stroke);

// The bytes of a .strokes.json asset: the canonical JSON of the packed strokes, the same bytes Python writes.
std::string strokes_blob(const StrokeList& strokes);

// A stroke read from JSON in any of its saved forms: packed ({"xy": …}), a dict with "points" (and "pressure",
// v2) or a list of [x, y] / [x, y, pressure] points (v1). A stroke without an id gets a new one. Throws
// core::Error("format") where Python's coerce_stroke raises.
Stroke coerce_stroke(const Json& raw);

// coerce_stroke of a list of points: a new stroke (new id) with these points and, when every point has one, their
// pressure.
Stroke coerce_stroke(const PenPoints& points);

// Python's stroke_points: the points with their pressure when there is one for every point.
PenPoints stroke_points(const Stroke& stroke);
// The same as JSON: [[x, y, p], …] or [[x, y], …].
Json stroke_points_json(const Stroke& stroke);

}  // namespace genko::core
