#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"

// The checks every pose of the library passes (SPEC FIGURE-02; test_figure_poses runs them on each pose, and on the
// mirror of each): the file's form, each joint within the range a person's joint turns, the body resting on what it
// says it rests on (the floor, a chair's seat) and balanced over it, no limb laid through the body or the head, no
// limb through another, the mirror exact, the surface within its budget and drawn. Every number is u, the figure's
// unit (its height / its heads), so the checks hold for any size.

namespace genko::core::pose_checks {

using la::Vec3;

// --- the thresholds (u) -------------------------------------------------------------------------------------------

// A contact: the lowest point of the part named is this close to the floor (the lowest point of the body).
inline constexpr double kContactGap = 0.03;
// A part not named as a contact this close to the floor counts as touching it (the contact list leaves it out).
inline constexpr double kUnlistedGap = 0.008;
// A chair's seat: the seat measured within this of the pose's seat_h, and seat_h a chair's (stools and steps too).
inline constexpr double kSeatGap = 0.05;
inline constexpr double kSeatLow = 1.2;
inline constexpr double kSeatHigh = 3.6;
// Balance: the centre of mass over the place the body rests on (the floor's or the seat's touching points, as seen
// from above), within this of it.
inline constexpr double kBalance = 0.25;
inline constexpr double kSeatBand = 0.1;  // (the seat's part of it: what is this close to the seat's lowest point)
// A limb's axis no deeper inside the torso or the head than this part of its radius; two limbs' (tapered) capsules
// overlapping by no more than this part of the thinner one's radius.
inline constexpr double kInside = 0.5;
inline constexpr double kOverlap = 0.4;
// Drawn: the whole body within this of the pelvis on every axis (in the picture whatever the view).
inline constexpr double kFrame = 7.5;

// A joint's range (radians) for the joints of the middle and the left side (but the shoulders and hips: below); the
// right side's is its mirror (x the same, y and z negated and swapped). Each value checked is the pose's own
// (add_figure's `joints`).
struct JointRange {
    std::string_view joint;
    std::array<double, 2> x;
    std::array<double, 2> y;
    std::array<double, 2> z;
};
const std::vector<JointRange>& joint_ranges();
std::array<double, 2> range_of(std::string_view joint, char axis);

// The shoulders and the hips (l_arm, l_leg; the right ones mirrored): how far the bone may rise from hanging down —
// toward the front, outward, the back and inward, straight between —, and its twist about itself (outward +). The
// arm's twist is against the shortest swing from hanging down (measured only up to twist_rise: the swing to an arm
// raised high depends on the way it went up, so its twist is no measure there); the thigh's is the hip's own rotation
// about the femur in the joint's coordinate system (flexion, then abduction, then rotation — the clinical one: the
// shortest swing to a thigh both flexed and spread reads up to 0.8 rad of twist the hip does not make, so it refused
// a cross-legged sit at no outward rotation at all). The arm's rest (±0.12 about z) counts.
struct BallRange {
    std::string_view joint;
    double forward;
    double out;
    double back;
    double in;
    std::array<double, 2> twist;
    double twist_rise;
};
const std::vector<BallRange>& ball_ranges();
struct Ball {
    double rise = 0;    // from hanging down (radians)
    double toward = 0;  // 0 forward, π/2 outward, ±π back, −π/2 inward
    double twist = 0;
};
Ball ball_of(std::string_view joint, const Json& axes);

// --- the checks ---------------------------------------------------------------------------------------------------

struct Issue {
    std::string check;  // file, range, support, balance, inside, overlap, mirror, render
    std::string what;
};

// (1) The file (genko.poses@1): its keys, its categories, each pose's keys and values (see core/pose_library.hpp), ids
// "<category>/<name>" each once, numbers finite and as the ops keep them (joints to 4 places, curls, spread and thumb
// to 3), names and tags in Japanese and English.
std::vector<Issue> check_file(const Json& file);

// (2) Each joint within its range.
std::vector<Issue> check_ranges(const Json& joints);

// The figure a pose makes as pose_figure gives it (pos 0, no turn), with the body given (null: the usual one).
Json figure_of(const Json& pose, double height_mm = 120.0, const Json& body = Json());

// What the checks measure on a pose's surface (the mesh core::mesh3d::model_of makes), its own space (y down).
struct Measure {
    double unit = 0;
    double floor = 0;                    // the lowest point (mm)
    std::map<std::string, double> gap;   // each part's (l_foot … pelvis) lowest point above the floor (u)
    std::map<std::string, Vec3> lowest;  // and that point (mm); "torso" the chest's and back's (the ribcage)
    std::string down;                    // the torso's face nearest the floor: back, front or side
    double seat = 0;                     // the seat's lowest point above the floor (u)
    double balance = 0;                  // the centre of mass beyond the place it rests on (u; 0 inside)
    Vec3 mass{};                         // the centre of mass (mm)
    std::size_t triangles = 0;
    double reach = 0;                    // the farthest point from the pelvis on any axis (u)
};
Measure measure(const Json& prim, const mesh3d::Model& model, const Json& pose);

// (3) Resting on what it says: floor — each contact on the floor, no other part touching it, the body balanced over
// them (not for walking, running, or a body that leans on something: back, front, side); chair — the seat on it at
// seat_h and the feet named on the floor, balanced; none — nothing. A body lying on the floor (its ribcage on it) names
// the face it lies on — back, front or side — as a contact, and that face is the one toward the floor; it is balanced
// as any body on the floor is (back, front and side off the floor are a lean on a wall, as before).
std::vector<Issue> check_support(const Json& pose, const Measure& m);

// (4) No limb through the body: each limb's axis (the upper arm and thigh beyond their roots) outside the torso and the
// head by kInside of its radius at most; the limbs of different arms and legs overlapping by kOverlap at most (but the
// two hands, which clasp and interlace).
struct Inside {
    double torso = 0;    // the deepest limb inside the torso (part of its radius)
    double head = 0;     // inside the head
    double overlap = 0;  // the largest overlap of two limbs (part of the thinner radius)
    std::string torso_limb;
    std::string head_limb;
    std::string overlap_limbs;
};
Inside inside(const Json& prim);
std::vector<Issue> check_inside(const Json& prim);

// (5) The mirror: twice is the pose itself, its skeleton the reflected skeleton (within 1e-9 u), a symmetric pose its
// own mirror.
std::vector<Issue> check_mirror(const Json& pose);

// (6) Drawn: a surface, within the budget (figure_shape::kMaxTriangles), within kFrame of the pelvis, with lines.
std::vector<Issue> check_render(const Measure& m, std::size_t lines);

// Every check of one pose (the file's form aside), its surface made once.
struct Report {
    std::string id;
    std::vector<Issue> issues;
    Measure measured;
    Inside inside;
    bool ok() const { return issues.empty(); }
};
Report check_pose(const Json& pose, const Json& body = Json());

// --- for the ops and the pictures ---------------------------------------------------------------------------------

// The lowest point of a figure on the page's y (its own turn applied; mm below its pelvis): what keep: ground holds.
double lowest(const Json& prim);

// A small grey picture of a pose's figure (255 white where there is no surface), turned `turn` about the upright axis,
// the floor near the foot of the picture, 13 u tall: the same bytes on every run and every thread.
std::string thumbnail(const Json& prim, double turn, int width, int height);

}  // namespace genko::core::pose_checks
