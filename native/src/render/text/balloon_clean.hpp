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

// Outlines inside the box (x0, y0, x1, y1), touching its four sides. `corners` (where a builder takes it; fukidashi@2):
// the outline's corners as the pen sees them — where one stroke ends and the next begins —, each a point of the outline:
// a sharp corner itself, a rounded one the middle of its rounding. They are the shape's own, whatever the resolution
// cuts its curves into.
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
Contour rounded(const Box& box, double radius, Contour* corners = nullptr);
// A superellipse of power 4 (a squircle: flat sides, round corners).
Contour squircle(const Box& box);
// A regular polygon of `sides` (5..12) stretched to the box, a flat side up, its corners rounded by `radius`.
Contour polygon(const Box& box, int sides, double radius, Contour* corners = nullptr);
// A cloud: `bumps` scallops of equal length round an ellipse.
Contour cloud(const Box& box, int bumps, Contour* corners = nullptr);
// fukidashi@2 (SPEC BALLOON-02) — the symbols. Each is its own mirror image left to right and touches the box's four
// sides.
// A diamond: its corners on the middles of the box's sides, rounded by `radius`.
Contour diamond(const Box& box, double radius, Contour* corners = nullptr);
// A star of `points` (4..12) points, the first straight up: its valleys at `inner` (0.2..0.95; a line's: 0.4..0.95) of
// its points' reach;
// its points rounded by `radius` and its valleys by half of it (a negative radius: its own — a point cut short by
// about 0.035 of the box's narrow half).
Contour star(const Box& box, int points, double inner, double radius, Contour* corners = nullptr);
// A flower of `petals` (4..12) round petals, one straight up.
Contour flower(const Box& box, int petals, Contour* corners = nullptr);
// A heart: two round lobes over a point, the point rounded by `radius` (negative: 0.05 of the box's narrow half).
Contour heart(const Box& box, double radius, Contour* corners = nullptr);
// The middle a star's points stand round (an odd star's is below its box's middle), and the oval that lies inside the
// valleys of a star or a flower, about that middle: what a tail leaves from.
P star_centre(const Box& box, int points, double inner);
Contour star_guide(const Box& box, int points, double inner);
Contour flower_guide(const Box& box, int petals);
// The room a symbol leaves its letters: a convex outline inside it — the diamond itself, a star's and a flower's
// valleys, a heart below its cleft. `kind` is heart, star, flower or diamond (anything else: nothing).
Contour symbol_room(std::string_view kind, const Box& box, int points, double inner);

// A burst: `spikes` points at equal distances round the ellipse, the valleys `depth` (a share of the radius) in;
// every other point shorter by `alt` of the depth; the points' reach changed by `jitter[i]` (shares of the radius, one
// a spike; empty: none).
Contour burst(const Box& box, int spikes, double depth, double alt, const std::vector<double>& jitter);
// A sawtooth round a squircle: `teeth` teeth of equal length, their inner corners `depth` in and lagging.
Contour sawtooth(const Box& box, int teeth, double depth);
// A closed Catmull–Rom curve through the points (a balloon drawn by hand, smoothed), within `tolerance` of the curve.
Contour smooth_closed(const Contour& points, double tolerance = 0.05);
// The outline trembling: pushed out and in along its normal by `amplitude`, a whole number of waves of about
// `wavelength` round it. Within a wave of each of `corners` that turns in (fukidashi@2: a heart's cleft) the push calms
// to nothing, so that the line does not cross itself there; the corners that turn out keep their wave.
Contour waved(const Contour& closed, double amplitude, double wavelength, const Contour& corners = {});
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
    bool join = false;  // (a rounded join: its curve leaves both outlines it joins along their own way — no corner where it meets them)
};
Shape whole(Contour points, bool cut = false, int owner = -1);

struct TailSpec {
    P tip;
    bool has_via = false;
    P via;
    std::vector<P> bends;      // (折れ線のしっぽ: it bends at each)
    double base = 0.0;         // how wide it is where it leaves the balloon (px, across its middle line)
    double fillet = 0.0;       // how far its join with the balloon is rounded, along the outline and along its side (px; 0: a corner)
    double depth = 0.0;        // how far its base reaches inside the balloon (px)
    double line = 0.0;         // the outline's width (px; 0: not said) — a zigzag stops stepping out where it is slimmer than its line
    std::string_view kind = "wedge";  // wedge | straight | zigzag | fade | inward (bubbles are circles: bubble_trail)
    bool bolt = false;         // (an electric balloon's: a lightning bolt)
    bool pen = false;          // (fukidashi@2: the line is the pen's — its strokes thin toward their ends)
};
// A tail of the balloon whose outline is `outline` (and `guide` the smooth outline it leaves from — the outline itself,
// or a burst's valleys — whose middle is `centre`). Its root is where the line from the middle to what it points at
// leaves the guide; its middle line leaves the balloon straight across the outline there and bends to the tip (no bend
// when the tip lies straight out); its two sides lie either side of that line, slimming to the tip. `left` and `right`
// are where its sides leave the outline (on it), and `joins` the small rounded joins there (shapes of their own; none
// when fillet is 0): the outline itself is untouched everywhere else.
struct Tail {
    Shape shape;
    P left;
    P right;
    std::vector<Shape> joins;
    Contour corners;  // (fukidashi@2: the corners the pen's strokes end at — its tip, its bends, a zigzag's steps)
};
// Where an end of its base falls outside the outline and that leaves the line open (a box's corner, a narrow outline
// under a wide tail), that end is drawn in to just inside the outline when that closes it; every other tail is made as
// before 2026-11-14, point for point (a pen's line: such an end is always drawn in).
Tail tail(const Contour& outline, const Contour& guide, P centre, const TailSpec& spec);
// (tests) While one lives, every tail's base is made as it falls, as before 2026-11-14 (nothing drawn in).
class TailBasesAsBeforeForTest {
public:
    TailBasesAsBeforeForTest();
    ~TailBasesAsBeforeForTest();
    TailBasesAsBeforeForTest(const TailBasesAsBeforeForTest&) = delete;
    TailBasesAsBeforeForTest& operator=(const TailBasesAsBeforeForTest&) = delete;
};
// The three bubbles of a thought toward `tip`, from the outline's edge, the first `radius` big.
std::vector<Contour> bubble_trail(const Contour& guide, P centre, P tip, double radius);
// The rounded joins where two outlines cross (joined balloons): a patch in each notch, `reach` along both.
std::vector<Shape> joins_between(const Contour& a, const Contour& b, double reach);
// fukidashi@2 (SPEC BALLOON-02) — a drip hanging from a balloon: a finger about the upright line x = `x`, `half` its
// half width where it leaves the outline (at `root_y`), `length` long from there down to its tip. A long one (over 3.2
// of its half width) slims to a neck and ends in a round drop; a short one ends round. Its two sides begin inside the
// balloon, at `left_top` and `right_top` (y); the stretch between them is not part of the outline. Its own mirror image
// about its line.
Shape drip(double x, double root_y, double left_top, double right_top, double half, double length);
// The rounded joins of a drip's finger with the balloon's outline it hangs from: as joins_between, but each stops short
// of the outline's next corner (and is left out where a corner is right there).
std::vector<Shape> drip_joins(const Contour& body, const Contour& finger, double reach);
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
    int shape = -1;          // (the shape it is a piece of: its place in the shapes given)
};
std::vector<Run> edge_runs(const std::vector<Shape>& shapes);
// The same outlines dashed: `on` then `off` along each shape (a whole number of dashes round it, one centred where
// the outline begins).
std::vector<Shape> dashed(const std::vector<Shape>& shapes, double on, double off);
// What lies within `reach` of the runs (a ribbon `2 × reach` wide along them, mitred at their corners).
Plane ribbon_plane(const std::vector<Run>& runs, double reach, int width, int height, bool round_ends = true);
// fukidashi@2 (SPEC BALLOON-02) — the pen's line: an outline whose width varies along it, as a pen's or a brush's does
// (強弱・入り抜き). The group's edge is followed as whole outlines — the balloon, its tails, their joins, its drips, the
// balloons joined to it: one line where they are one outline —, and drawn in strokes: an outline with corners from
// corner to corner, a smooth one in `strokes` equal parts beginning where its line is lightest. The corners are the
// shapes' own (`corners`: a box's, a polygon's, a star's points and valleys, a burst's, a cloud's valleys, a heart's
// cleft and point, a tail's tip and bends — given by what builds them, so they are the same at every resolution) and
// the places where two pieces of outline meet at a turn of more than 30°.
struct Pen {
    double width = 0.0;   // the line's mean width (px): what the even line's is
    double swell = 0.0;   // 0..0.9: the line is width × (1 + swell × n·heavy), n the outline's outward normal
    P heavy{0.0, 1.0};    // the way the heavy side faces (a unit vector)
    double taper = 0.0;   // 0..1: the share of each stroke over which it thins toward its two ends
    int strokes = 2;      // 1..8: the strokes of an outline without corners
    double step = 4.0;    // no stretch between two points of the line is longer (px)
    bool chain = true;    // false (a dashed outline): each run by itself, the swell alone
    double nominal = 0.0; // the width the strokes' rules count in (px; 0: `width`): the line's own before it is made whole pixels
    Contour corners;      // the corners the shapes say are theirs (their builders' `corners`, a tail's): where strokes end
};
// A stretch of the pen's line: its points and how far the line reaches into the group at each (its width there).
struct PenRun {
    Contour points;
    std::vector<double> reach;
    std::vector<unsigned char> stroke_start;  // (1 where a stroke begins — and the one before it ends)
    bool closed = false;
    bool round_first = false;  // (an open stretch's end where another shape covers the outline: round)
    bool round_last = false;
};
// The runs of a group's edge (edge_runs of the same shapes) as the pen draws them.
std::vector<PenRun> pen_runs(const std::vector<Run>& runs, const std::vector<Shape>& shapes, const Pen& pen);
// What lies within each point's reach of the pen's runs (ribbon_plane with a reach of its own at every point).
Plane pen_plane(const std::vector<PenRun>& runs, int width, int height);
// Closed outlines filled (dots, diamonds, spokes), as one union.
Plane union_plane(const std::vector<Contour>& outlines, int width, int height);

// A circle (a dot, a bubble) as an outline.
Contour circle(P centre, double radius);

}  // namespace genko::render::text::clean
