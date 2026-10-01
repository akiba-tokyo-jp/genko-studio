#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "core/geometry.hpp"
#include "core/model.hpp"

// Panel geometry (Python's genko/frames.py): the parts that reading order and Page::frame_at need.

namespace genko::core {

// The four corners of a box: top left, top right, bottom right, bottom left.
std::vector<Point> corners(const Rect& rect);

// A panel's outline corners: its poly (as floats) or the corners of its rect.
std::vector<Point> shape(const Frame& frame);

// The mean of the points (Python 3.12's sum, then / len).
std::pair<double, double> centroid(std::span<const Point> points);

// The panel's bows (mm, outward +) when they fit its corners, else nothing.
std::optional<std::vector<double>> curves_of(const Frame& frame, std::span<const Point> points);

// Whether the panel's outline is more than its corners: bowed edges or rounded corners.
bool rounded(const Frame& frame);

// The panel's outline: its corners, each bowed edge walked as a curve, its corners rounded (角の丸み) when it has
// no bowed edges.
std::vector<Point> outline(const Frame& frame, double step_mm = 1.0);

// Whether (x, y) is inside the panel (its box, or its outline when it is slanted, bowed or rounded).
bool contains(const Frame& frame, const Num& x, const Num& y);

// Drawn panels in reading order: rows from the top (panels whose middles are within the smaller one's half height
// share a row), right to left in a row for a right-bound book.
std::vector<const Frame*> reading_order(std::vector<const Frame*> children, bool right_to_left);

}  // namespace genko::core
