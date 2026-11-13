#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// fukidashi@1 (SPEC BALLOON-01): the balloons' geometry — every outline as a closed polygon of real pixels, and the
// coverage of a group of them: the fill (their union) and the line (what lies within the line's width of the union's
// edge). The outlines are made from + − × ÷ and sqrt alone (the regular polygon's corners and a wave's phase from
// render/libm_float's sinf and cosf, the same bits everywhere), a quarter or a half made and mirrored, so a shape is
// its own mirror image exactly; the coverage is the exact-area rasterizer's (render/path_raster), so a line has its
// width at any angle and no edge is stepped. Nothing here reads a book: balloons.cpp turns a line into these.
//
// Coordinates are pixels of the grid drawn on, y down; a pixel (x, y) covers [x, x + 1) × [y, y + 1). Outlines run
// clockwise on the page (top → right → bottom → left).

namespace genko::render::text::clean {

struct P {
    double x = 0.0;
    double y = 0.0;
};
using Contour = std::vector<P>;

// --- measuring an outline ---------------------------------------------------------------------------------------------

double distance(P a, P b);
// The length of the closed outline up to each of its points (lengths[i]: from points[0] to points[i]; one more entry
// than the outline has points: its whole length).
std::vector<double> lengths_of(const Contour& closed);
// The point `s` along the closed outline (s taken modulo its length), and the unit direction it runs in there.
P point_at(const Contour& closed, const std::vector<double>& lengths, double s);
P direction_at(const Contour& closed, const std::vector<double>& lengths, double s);
// Whether the point lies inside the closed outline (non-zero winding).
bool inside(const Contour& closed, P point);
// Twice the signed area: positive for an outline that runs clockwise on the page (y down).
double shoelace(const Contour& closed);
// How far along the closed outline a ray from `from` toward `toward` leaves it (its farthest crossing; 0 when it
// meets none).
double facing(const Contour& closed, const std::vector<double>& lengths, P from, P toward);

// --- the bodies -------------------------------------------------------------------------------------------------------

// Outlines inside the box (x0, y0, x1, y1), touching its four sides.
struct Box {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (y0 + y1) / 2; }
    double rx() const { return (x1 - x0) / 2; }
    double ry() const { return (y1 - y0) / 2; }
};

// A true ellipse.
Contour ellipse(const Box& box);
// A rectangle whose corners are quarter circles of `radius` (0: square corners; at most half the short side: a
// capsule).
Contour rounded(const Box& box, double radius);
// A superellipse of power 4 (a squircle: flat sides, round corners).
Contour squircle(const Box& box);
// A regular polygon of `sides` (5..12) stretched to the box, a flat side up, its corners rounded by `radius`.
Contour polygon(const Box& box, int sides, double radius);
// A cloud: `bumps` scallops of equal length round an ellipse.
Contour cloud(const Box& box, int bumps);
// A burst: `spikes` points at equal distances round the ellipse, the valleys `depth` (a share of the radius) in;
// every other point shorter by `alt` of the depth; the points' reach changed by `jitter[i]` (shares of the radius, one
// a spike; empty: none).
Contour burst(const Box& box, int spikes, double depth, double alt, const std::vector<double>& jitter);
// A sawtooth round a squircle: `teeth` teeth of equal length, their inner corners `depth` in and lagging.
Contour sawtooth(const Box& box, int teeth, double depth);
// A closed Catmull–Rom curve through the points (a balloon drawn by hand, smoothed), within `tolerance` of the curve.
Contour smooth_closed(const Contour& points, double tolerance = 0.05);
// The outline trembling: pushed out and in along its normal by `amplitude`, a whole number of waves of about
// `wavelength` round it.
Contour waved(const Contour& closed, double amplitude, double wavelength);
// The outline made `factor` times as big about `centre`.
Contour scaled_about(const Contour& closed, P centre, double factor);

// --- shapes and their tails ------------------------------------------------------------------------------------------

// One closed piece of a group: added to it, or cut out of it (an inward tail). `stroke[i]` says whether the segment
// from points[i] to the next is part of the outline where it lies on the group's edge (a tail's base inside its
// balloon is not).
struct Shape {
    Contour points;
    std::vector<unsigned char> stroke;
    bool cut = false;
    int owner = -1;  // (a cut: the index of the shape it is cut out of)
};
Shape whole(Contour points, bool cut = false, int owner = -1);

struct TailSpec {
    P tip;
    bool has_via = false;
    P via;
    std::vector<P> bends;      // (折れ線のしっぽ: it bends at each)
    double base = 0.0;         // how wide it leaves the balloon (px along the outline)
    double fillet = 0.0;       // how far its join with the balloon is rounded (px; 0: a corner)
    double depth = 0.0;        // how far its base reaches inside the balloon (px)
    std::string_view kind = "wedge";  // wedge | straight | zigzag | fade | inward (bubbles are circles: bubble_trail)
    bool bolt = false;         // (an electric balloon's: a lightning bolt)
};
// A tail leaving the outline `guide` (whose middle is `centre`) toward its tip: its shape, the two points where its
// sides leave the outline (on it), and the rounded joins there (shapes of their own; none when fillet is 0).
struct Tail {
    Shape shape;
    P left;
    P right;
    std::vector<Shape> joins;
};
Tail tail(const Contour& guide, P centre, const TailSpec& spec);
// The three bubbles of a thought toward `tip`, from the outline's edge, the first `radius` big.
std::vector<Contour> bubble_trail(const Contour& guide, P centre, P tip, double radius);
// The rounded joins where two outlines cross (joined balloons): a patch in each notch, `reach` along both.
std::vector<Shape> joins_between(const Contour& a, const Contour& b, double reach);
// A neck between two outlines that do not meet (joined balloons a little apart): from one middle to the other,
// `width` wide.
Shape neck(P from, P to, double width);

// --- the coverage -----------------------------------------------------------------------------------------------------

// Coverage (0..1) of `width` × `height` pixels, row by row.
struct Plane {
    int width = 0;
    int height = 0;
    std::vector<float> values;
    float at(int x, int y) const { return values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
};

// The union of the shapes (the cuts taken out).
Plane fill_plane(const std::vector<Shape>& shapes, int width, int height);
// The pieces of the shapes' outlines that lie on the group's edge, cut where one dips into another.
struct Run {
    Contour points;
    bool closed = false;
    bool cut_first = false;  // (it begins, or ends, where another shape covers the outline: a round end there)
    bool cut_last = false;
};
std::vector<Run> edge_runs(const std::vector<Shape>& shapes);
// The same outlines dashed: `on` then `off` along each shape (a whole number of dashes round it, one centred where
// the outline begins).
std::vector<Shape> dashed(const std::vector<Shape>& shapes, double on, double off);
// What lies within `reach` of the runs (a ribbon `2 × reach` wide along them, mitred at their corners).
Plane ribbon_plane(const std::vector<Run>& runs, double reach, int width, int height, bool round_ends = true);
// Closed outlines filled (dots, diamonds, spokes), as one union.
Plane union_plane(const std::vector<Contour>& outlines, int width, int height);

// A circle (a dot, a bubble) as an outline.
Contour circle(P centre, double radius);

}  // namespace genko::render::text::clean
