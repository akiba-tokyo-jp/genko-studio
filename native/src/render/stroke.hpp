#pragma once

#include <optional>

#include "core/geometry.hpp"
#include "render/draw.hpp"

// The drawing half of Python's genko/stroke.py: a pen line from its points (mm, with pressure) at any resolution.

namespace genko::render {

// stroke._mm_to_px: max(1, round(mm / 25.4 * dpi)).
int stroke_mm_to_px(double mm, int dpi);

// stamp_polyline(draw, points, dpi, width_mm, fill, coords): round dabs every pixel along each segment ("px": the
// points are pixels already; "mm": page mm).
void stamp_polyline(Draw& draw, const core::PenPoints& points, int dpi, double width_mm, const Ink& fill,
                    bool coords_mm = false);

// draw_stroke_mm(draw, points, dpi, width_mm, fill, pressure_scale=True, floor=0.15): each segment a quad between two
// round caps whose radii follow the pressure.
void draw_stroke_mm(Draw& draw, const core::PenPoints& points, int dpi, double width_mm, const Ink& fill,
                    bool pressure_scale = true, double floor = 0.15);

}  // namespace genko::render
