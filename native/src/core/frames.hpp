#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/geometry.hpp"
#include "core/model.hpp"

// Panel geometry (Python's genko/frames.py). Points keep Python's numbers: a corner of a box of ints is ints, a point
// on a cut is floats, and every result (rounded boxes, cuts, re-laid panels) has the type Python's has.
//
// Errors are Python's: PyValueError("the cut does not cross the panel"), PyValueError("no gutter there"), ….

namespace genko::core {

inline constexpr std::string_view kFreeSplit = "free";  // drawn panels (not cut): no gutters, each keeps its shape

// The four corners of a box: top left, top right, bottom right, bottom left.
std::vector<Point> corners(const Rect& rect);

// A panel's outline corners: its poly (as floats) or the corners of its rect.
std::vector<Point> shape(const Frame& frame);

// The points' box, each number rounded to 3 places (frames.bbox; ints stay ints). PyValueError for no points.
Rect bbox(std::span<const Point> points);

// The rectangle these points are, if they are one (axis-aligned).
std::optional<Rect> as_rect(std::span<const Point> points);

// The points without repeats (within 1e-4 mm), nor the last when it closes back onto the first.
std::vector<Point> dedupe(std::span<const Point> points);

// The panel's shape: its box when the points are a rectangle, else its poly (rounded to 3 places) and that box.
void set_shape(Frame& frame, std::span<const Point> points);

// sum(a[0] * b[1] - b[0] * a[1]) / 2 over the edges (Python 3.12's sum).
double signed_area(std::span<const Point> points);

// The area of a polygon (frames.area).
double area(std::span<const Point> points);

// The mean of the points (Python 3.12's sum, then / len).
std::pair<double, double> centroid(std::span<const Point> points);

// The panel's bows (mm, outward +) when they fit its corners, else nothing.
std::optional<std::vector<double>> curves_of(const Frame& frame, std::span<const Point> points);

// Whether the panel's outline is more than its corners: bowed edges or rounded corners.
bool rounded(const Frame& frame);

// frames._round_corners: each corner cut back along both edges and joined by a round (a quarter circle at a right
// angle); the corners whose `keep` is true stay sharp (a bleed panel's corners off the paper).
std::vector<Point> round_corners(std::span<const Point> pts, double radius, const std::vector<bool>& keep = {});

// The panel's outline: its corners, each bowed edge walked as a curve, its corners rounded (角の丸み) when it has
// no bowed edges.
std::vector<Point> outline(const Frame& frame, double step_mm = 1.0);

// frames.offset: the outline moved inward by d mm (outward when d < 0), corner by corner along the mitre.
std::vector<Point> offset(std::span<const Point> points, double d);

// Whether (x, y) is inside the panel (its box, or its outline when it is slanted, bowed or rounded).
bool contains(const Frame& frame, const Num& x, const Num& y);

// Drawn panels in reading order: rows from the top (panels whose middles are within the smaller one's half height
// share a row), right to left in a row for a right-bound book.
std::vector<const Frame*> reading_order(std::vector<const Frame*> children, bool right_to_left);

bool is_free(const Frame& node);

// Sutherland-Hodgman against the line p0→p1 moved `offset` mm to the kept side.
std::vector<Point> clip_half(std::span<const Point> points, const Point& p0, const Point& p1, bool keep_left,
                             double offset);

// The two sides of a shape cut along p0→p1 with a gutter: for a mostly horizontal cut the upper part first, for a
// mostly vertical cut the left part first. PyValueError("the cut does not cross the panel").
std::pair<std::vector<Point>, std::vector<Point>> cut(std::span<const Point> points, const Point& p0, const Point& p1,
                                                      double gutter);

// The cut of a split node in page mm, or nothing for a stack.
struct CutLine {
    Point p0;
    Point p1;
    double gutter = 4.0;
};
std::optional<CutLine> cut_line(const Frame& node);

// Cut a leaf in two along p0→p1 (page mm); the leaf becomes their parent and keeps the cut. The children's ids come
// from new_id(), the first child's first.
std::pair<Frame*, Frame*> cut_frame(Frame& node, const Point& p0, const Point& p1, double gutter);

// The cut line split_frame means: `ratio` of the space left after the gutter, tilted by `tilt` mm between its ends.
std::pair<Point, Point> axis_line(const Frame& node, std::string_view axis, double ratio, double gutter, double tilt);

// Give a two-child axis split (made before cuts were stored) its cut, from its children.
void remember_split(Frame& node);

// Give `node` a new shape (or keep its own, with no points) and lay its children out again inside it.
void relayout(Frame& node, const std::vector<Point>* points = nullptr);

// Move the gutter after child `index` (in position order) by `delta` mm; optionally set its width.
void move_gutter(Frame& node, std::int64_t index, double delta, std::optional<double> gutter, double min_span = 8.0);

// The distance from p to the segment a–b (floats).
double distance_to_segment(double px, double py, double ax, double ay, double bx, double by);

}  // namespace genko::core
