#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"

// The smooth body of the 3D figure, its head and its hands (SPEC FIGURE-01, this build's own: Python's mesh3d.py put
// ellipsoids, capsules and boxes side by side). The shape is a field: each part of the body (the pelvis, the chest, an
// upper arm, a finger…) is the distance to a few rounded pieces — round cones, ellipsoids and rounded boxes set in the
// skeleton's bones —; the parts meet by the plain minimum, and, near the joint between two parts that the skeleton
// joins, by a smooth minimum whose width falls off with the distance from the joint (so an arm laid on the belly shows
// a crease, never a web). core/surface_nets makes the triangles.
//
// Every number is made with + − × ÷, the square root, min and max, in a fixed order (the bones' turns keep their
// sines and cosines, as core/mesh3d makes them): the same bytes on every run and every thread.

namespace genko::core::figure_shape {

using la::Mat3;
using la::Vec3;

// One rounded piece. Every distance is the true distance or less, and changes by at most the step taken.
struct Piece {
    enum class Kind : std::uint8_t { Cone, Ellipsoid, Box };
    Kind kind = Kind::Cone;
    Vec3 a{};       // cone: one end; ellipsoid, box: the centre
    Vec3 b{};       // cone: the other end
    double ra = 0;  // cone: the radius at a
    double rb = 0;  // cone: the radius at b
    Mat3 axes{};    // ellipsoid, box: its axes as columns (a turn)
    Vec3 size{};    // ellipsoid: its radii; box: its half sizes
    double round = 0;  // box: the rounding
    Vec3 centre{};  // a ball that holds the piece
    double reach = 0;
    bool paired = false;  // (this piece and the next are a left and right pair: joined first)

    double at(const Vec3& x) const;
};

Piece cone(const Vec3& a, double ra, const Vec3& b, double rb);
Piece ellipsoid(const Vec3& centre, const Mat3& axes, const Vec3& radii);
Piece box(const Vec3& centre, const Mat3& axes, const Vec3& half, double round);

// A part: its pieces joined by a smooth minimum of width k (0: the plain minimum), its details by one of detail_k, its
// hollows taken away; then, when `cut` is set, only the side of the plane where n·x − d ≤ 0 is kept (with the edge rounded by `cut_k`: a sole flattened, a stub cut off).
struct Part {
    std::vector<Piece> pieces;
    double k = 0;
    std::vector<Piece> details;  // (small pieces added after, by a narrower smooth minimum: a nose, the lips, the ears)
    double detail_k = 0;
    std::vector<Piece> hollows;  // (pieces taken away by a smooth maximum: the eyes' hollows)
    double hollow_k = 0;
    bool cut = false;
    Vec3 cut_n{};
    double cut_d = 0;
    double cut_k = 0;
    bool no_contour = false;  // (its points draw no outline: a hand's stub of the forearm, seen through the body's own)

    double at(const Vec3& x) const;
};

// A joint: near `at` (within `radius`), parts p and c meet by a smooth minimum of width up to k.
struct Joint {
    int p = 0;
    int c = 0;
    Vec3 at{};
    double k = 0;
    double radius = 1;
};

struct Field {
    std::vector<Part> parts;
    std::vector<Joint> joints;

    // The field at x from the parts marked in `use` (null: all). `d` has room for one value per part; `nearest` (when
    // given) is the part nearest to x.
    double at(const Vec3& x, const std::uint8_t* use, double* d, int* nearest = nullptr) const;
    // The widest blend a part takes part in.
    double blend_of(int part) const;
    // A box that holds every part, and the widest blend.
    void bounds(Vec3& lo, Vec3& hi) const;
};

// The smooth minimum of a and b over a width k (polynomial: + − × ÷ only): a and b themselves where they are k apart.
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
// A head on its own (u = its height / 1.1), centred on 0, and its face's guide lines. The face has a neutral relief
// (brow, the eyes' hollows, nose, lips, chin, ears) and no drawn features: a person turned anywhere reads as one.
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
