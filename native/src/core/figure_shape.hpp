#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"

// The smooth body of the 3D figure, its head and its hands (SPEC FIGURE-01, this build's own: Python's mesh3d.py put
// ellipsoids, capsules and boxes side by side). The shape is a field: each part of the body (the torso, an upper arm,
// a finger…) is the distance to a few rounded pieces set in the skeleton's bones — round cones, ellipsoids, rounded
// boxes and lofts (a form along a bone whose oval section changes smoothly along it: the ribcage, the waist, a thigh
// with its taper) —, with its low relief as swells of that surface (a shoulder blade, an eyelid); the parts meet by
// the plain minimum, and, near the joint between two parts that the skeleton joins, by a smooth minimum whose width
// falls off with the distance from the joint (so an arm laid on the belly shows a crease, never a web).
// core/surface_nets makes the triangles.
//
// Every number is made with + − × ÷, the square root, min and max, in a fixed order (the bones' turns keep their
// sines and cosines, as core/mesh3d makes them): the same bytes on every run and every thread.

namespace genko::core::figure_shape {

using la::Mat3;
using la::Vec3;

// A loft's section at y along its axis: half the width (across, x), half the depth (z) and where its middle is in z.
struct Station {
    double y = 0;
    double rx = 0;
    double rz = 0;
    double zc = 0;
};

// One rounded piece. A cone's and a box's distance is the true one; an ellipsoid's and a loft's is the first-order
// one (the true distance at the surface, never more than it elsewhere). A swell is no solid of its own: it lifts (or,
// with a negative amount, sinks) the surface of what is before it in the part, by its amount at its middle, falling to
// nothing at the rim of its ellipsoid as (1 − r²)³ does (r 0 at the middle, 1 at the rim) — a low relief with no
// edge anywhere: a shoulder blade, the furrow of the spine, a collarbone, an eyelid.
struct Piece {
    enum class Kind : std::uint8_t { Cone, Ellipsoid, Box, Loft, Swell };
    Vec3 centre{};  // a ball that holds the piece
    double reach = 0;
    double steep = 1;     // (past that ball the piece's value is at least the distance from the ball / steep)
    double blend = -1;    // the width it is joined by in its part (< 0: the part's own k)
    Kind kind = Kind::Cone;
    bool carve = false;   // taken away from what is before it in the part (a hollow), by a smooth maximum
    bool paired = false;  // (this piece and the next are a left and right pair: joined first)
    bool rest = false;    // set in the part's rest space (Part::bend): the trunk, which bends with the spine
    Vec3 a{};       // cone: one end; ellipsoid, box: the centre; loft: where y = 0 is
    Vec3 b{};       // cone: the other end; slab: its radii on the far side of its middle
    double ra = 0;  // cone: the radius at a; loft: the height of the dome that closes its first end; swell: its amount
    double rb = 0;  // cone: the radius at b; loft: of its last end
    Mat3 axes{};    // ellipsoid, box, loft, swell: its axes as columns (a turn; a loft runs along y)
    Vec3 size{};    // ellipsoid, swell: its radii; box: its half sizes; loft: how full its two domes are
    double round = 0;  // box: the rounding; swell: 1 when it is flat, 2 when it is a slab
    double arch = 0;        // slab: how fast its second axis' line falls away along its first
    double arch_round = 0;  // (and over what length it is round at the middle)
    std::vector<double> loft;  // loft: its stations and the cubics between them (see loft())

    double at(const Vec3& x) const;
};

Piece cone(const Vec3& a, double ra, const Vec3& b, double rb);
Piece ellipsoid(const Vec3& centre, const Mat3& axes, const Vec3& radii);
Piece box(const Vec3& centre, const Mat3& axes, const Vec3& half, double round);
// (`flat`: the swell keeps most of its amount over its middle and falls off nearer the rim, as (1 − r⁴)³ does — a
// broad even form with a soft edge: a pectoral)
Piece swell(const Vec3& centre, const Mat3& axes, const Vec3& radii, double amount, bool flat = false);
// A slab: a flat swell whose rim is a rounded box, not an ellipsoid — amount × (1 − (x⁴ + y⁴ + z⁴))³, x, y and z the
// point's places along the three axes over the radii. Its edges run straight between rounded corners, so a broad
// muscle can end along a line (a pectoral's lower edge, rising to the armpit) with no round outline. The axes need
// not be square to each other (each is a direction the place is measured along): the rim is then a slanted box.
// `back` (when given) are the radii on the far side of the middle along each axis: an edge may be a soft step on
// one side and a long fade on the other (the lowest ribs: a step over the belly, nothing to see toward the chest).
// `arch` bends the slab along its first axis: its place along the second is taken from a line that falls away by
// `arch` for each unit out from the middle (round over `arch_round` at the middle: arch × (√(x² + round²) − round)) —
// an edge that is an arch, as the lowest ribs' is over the belly.
Piece slab(const Vec3& centre, const Mat3& axes, const Vec3& radii, double amount, const Vec3* back = nullptr, double arch = 0.0, double arch_round = 0.0);
// A loft from the first station to the last (y rising; two or more) in the frame `axes` at `origin`: between the
// stations the section's numbers run as cubics through them, and each end is closed by half an ellipsoid as high as
// cap0 and cap1 (its middle going on as zc slopes at the end: a dome that leans). A run is best begun and ended where
// the form is widest (flat there, narrowing after): the dome then carries the form on with no turn in its curve — a
// run that ends narrowing, flat at its end, has one there, and a wide blend with another part shows it as a line.
// full0 and full1 (0..1) make a dome fuller: 0 is the half ellipsoid, toward 1 it leaves its base more slowly (its
// profile √(1 − (1 − f) t² − f t⁴), t the height over the cap's) — set so the dome curves at its base as the run does. The cubics' slopes at the stations are `slopes` (each number's change per unit of y, in its place in
// a Station), or without them those of the smoothest run through the stations that is flat at both ends (run_slopes).
Piece loft(const Vec3& origin, const Mat3& axes, const std::vector<Station>& stations, double cap0, double cap1, const std::vector<Station>* slopes = nullptr,
           double full0 = 0.0, double full1 = 0.0);
// The slopes at each station of the cubic spline through them (its curvature unbroken; flat at both ends, or with the
// slopes `first` and `last` there).
std::vector<Station> run_slopes(const std::vector<Station>& stations, const Station* first = nullptr, const Station* last = nullptr);

// The bend of a trunk: its rest space is the body standing straight (the pelvis at 0, the waist `waist` above it on
// −y); a point of the figure is carried there by the pelvis's turn below the pelvis, the spine's between the two
// joints and the chest's above the waist, and over a stretch above each joint (from `from` to `from` + `over`, along
// the trunk) by the two turns mixed, more and more of the upper one — so the trunk's one run of sections bends as a
// spine does, with no step where a turn changes. (The mix makes the field steeper there: its value is divided by how
// much, so it stays a distance or less.)
struct Bend {
    bool on = false;
    Vec3 pelvis{};
    Vec3 waist_at{};
    Mat3 hip{};
    Mat3 spine{};
    Mat3 chest{};
    double waist = 0;    // (the waist joint's height above the pelvis in the rest space)
    double from[2]{};    // (where each stretch begins, as a height above the pelvis)
    double over[2]{};    // (and how long it is)
};

// A part: its pieces joined in their order by a smooth minimum (each by its own width, or the part's k; 0: the plain
// minimum), a carved piece taken away from what is before it, a swell lifting it; then, when `cut` is set, only the
// side of the plane where n·x − d ≤ 0 is kept (with the edge rounded by `cut_k`: a sole flattened, a stub cut off);
// and when `fade` is set the part thins by fade × (n·x − d0) past the plane n·x = d0 (a stub narrowing toward its cut
// end, under the other mesh of the same limb; the neck's two meshes crossing at their seam).
struct Part {
    std::vector<Piece> pieces;
    double k = 0;
    bool cut = false;
    Vec3 cut_n{};
    double cut_d = 0;
    double cut_k = 0;
    double fade = 0;
    Vec3 fade_n{};
    double fade_d = 0;
    Bend bend;  // (the pieces marked `rest` are worked out in its rest space)
    bool no_contour = false;  // (its points draw no outline: a hand's stub of the forearm, seen through the body's own)

    double at(const Vec3& x) const;
};

// A joint: near `at` (within `radius`), parts p and c meet by a smooth minimum of width up to k: the whole k within
// flat × radius of it, falling to nothing at the radius. A joint may be wider about a second place too (`more`, by up
// to `more_k`, the same way): the two widths are added — one smooth width, where two joints of the same pair would
// leave a crease where one takes over from the other. (The fold of the armpit, under the shoulder.)
struct Joint {
    int p = 0;
    int c = 0;
    Vec3 at{};
    double k = 0;
    double radius = 1;
    double flat = 0;
    Vec3 more{};
    double more_k = 0;
    double more_radius = 1;
    double more_flat = 0;
};

struct Field {
    std::vector<Part> parts;
    std::vector<Joint> joints;

    // The field at x from the parts marked in `use` (null: all). `d` has room for one value per part (a part's own, or
    // for a cut part too far past its plane to matter, the plane's distance: less than its own); `nearest` (when
    // given) is the part nearest to x.
    double at(const Vec3& x, const std::uint8_t* use, double* d, int* nearest = nullptr) const;
    // The widest blend a part takes part in.
    double blend_of(int part) const;
    // A box that holds every part, and the widest blend.
    void bounds(Vec3& lo, Vec3& hi) const;
};

// The smooth minimum of a and b over a width k (a cubic: + − × ÷ only; its curvature has no step where the blend
// begins, so no band shows in the light): a and b themselves where they are k apart, k / 6 less where they are equal.
double smin(double a, double b, double k);

// The shape of a hand: each finger's curl (thumb, index, middle, ring, little: 0 straight .. 1 closed), how far the
// fingers fan out (0 together .. 1 spread) and how far the thumb turns to face them (0 beside the palm .. 1 across it).
using HandShape = mesh3d::HandShape;

// A mesh of triangles with a unit normal at each point (the field's direction of growth there).
struct Smooth {
    std::vector<Vec3> v;
    std::vector<Vec3> n;
    std::vector<std::array<std::int32_t, 3>> t;
    std::vector<std::uint8_t> no_contour;  // per point
    std::vector<std::uint8_t> part;        // per point: the part nearest to it
    // per point, or empty (then n): the direction the outline is found by. A head's is that of the plain head under
    // its face's relief, so its outline stays the clean oval (and, from the side, the face's own profile) whatever the
    // nose, the lids and the ears do to the light.
    std::vector<Vec3> outline;
};

// The figure's body (head, torso, limbs, feet) and its two hands, in the figure's own space (as figure_skeleton has
// it), and the guide lines of its face laid on its head.
struct FigureMesh {
    Smooth mesh;
    std::vector<std::vector<Vec3>> marks;
};
FigureMesh figure(const mesh3d::Skeleton& sk, const HandShape& left, const HandShape& right);
// The figure's torso and its head as the body has them (the same fields figure() meshes): for the pose checks
// (core/pose_checks: a limb laid through the body, a hand inside the head).
Part torso_part(const mesh3d::Skeleton& sk);
Part head_part(const mesh3d::Skeleton& sk);
// A head on its own (u = its height / 1.1), centred on 0, and its face's guide lines. The face is a mannequin's: a
// low brow over each eye, the eyes as smooth shallow oval hollows, the nose's bridge and tip, closed lips,
// the chin and the jaw's line over a flat underside, small flat ears; no drawn features: a person turned anywhere
// reads as one.
FigureMesh head(double u);
// A hand on its own, `length` long, its wrist at y = −length / 2, the back of the hand toward the viewer.
Smooth hand(double length, const HandShape& shape, int side);

// The body's hand size (the sex's: 1 for the usual body) and the figure's hand length (0.8 u × that).
double hand_scale(const Json& body);
double hand_length(const mesh3d::Skeleton& sk);

// What the hands hold (SPEC FIGURE-02, `hold`; core/figure_props.cpp): small solids of flat faces in each hand's frame,
// in the figure's own space (as figure() puts the hands). Each face has its own points and one normal (flat), its
// triangles fanned; its points draw no outline (no_contour). `edges` are the solids' edges for the pen's lines: {point a,
// point b (of the first face), a point of that face, a point of the other face (−1: none), 1 when both faces are of a
// rounded piece (drawn only where it turns away from the eye)}. A name the hand does not know (or empty) holds nothing.
struct Props {
    Smooth mesh;
    std::vector<std::array<std::int32_t, 5>> edges;
};
Props props(const mesh3d::Skeleton& sk, std::string_view left, std::string_view right);

// The grid steps (per figure unit u; per hand length; per head height).
inline constexpr double kBodyCells = 16.0;
inline constexpr double kHandCells = 52.0;
inline constexpr double kHeadCells = 30.0;  // (per u, for a figure's head and a head on its own)
inline constexpr std::size_t kMaxTriangles = 64000;  // (a figure's budget; the tests hold every preset to it)

}  // namespace genko::core::figure_shape
