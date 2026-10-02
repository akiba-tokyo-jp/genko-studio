#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"

// 3D shapes on the page (Python's genko/prim3d.py, drawing guides), turned in space and seen in perspective: boxes,
// cylinders, stairs and floors (a perspective grid on the ground), spheres, cones, props (小物) and whole background
// scenes (a room, a classroom, a corridor, a street) moved, turned and traced as one; and, through mesh3d, the
// figures, heads, hands and models. A box is {"kind": "box", "pos": [x, y, z] (its centre: page mm, z = depth toward
// the back), "size": [w, h, d], "rot": [tip, turn, lean] (radians), "focal_mm": the camera distance (400)}.
//
// Every function takes the camera the prim is seen with (mesh3d::camera_of: the page's, else the prim's own; null for
// none) where Python reads prim["camera"].

namespace genko::core::prim3d {

using la::Vec3;
using mesh3d::Line2;
using mesh3d::Point2;

struct Edge {
    Point2 a;
    Point2 b;
    bool seen = true;
};

using Segment3 = std::pair<Vec3, Vec3>;

// The kinds add_prim3d makes (KINDS without "scene"), the scenes, the props, in Python's order.
extern const std::array<std::string_view, 8> kKinds;
extern const std::array<std::string_view, 4> kScenes;
extern const std::array<std::string_view, 8> kProps;

// PROPS[name]: boxes in a unit box (x, y, z, w, h, d: shares of the size; y from the top, the ground at 1).
const std::vector<std::array<double, 6>>* prop_boxes(std::string_view name);
// SCENE_VIEWS[kind]: the turn the camera looks with and how near the scene starts (× focal); SCENE_SIZES[kind].
std::pair<Json, double> scene_view(std::string_view kind);
Json scene_size(std::string_view kind);

// _size: [w, h, d] of a prim, each at least 0.1 (one number for all three; a short list repeats its last).
Vec3 size_of(const Json& prim);
// _rotate: turn (about the upright axis), then tip, then lean.
Vec3 rotate(const Vec3& p, const Json& rot);
// The eight corners in the prim's own space (turned), x from bit 2, y from bit 1, z from bit 0.
std::vector<Vec3> corners3d(const Json& prim);
// The eight corners on the page (mm).
std::vector<Point2> project(const Json& prim, const Json* camera);

// The lines of a background scene in its own space (centred; the ground is at y = h/2, up is −y).
std::vector<Segment3> scene_parts(std::string_view kind, const Vec3& size);
// The lines of a cylinder, stairs, floor, sphere, cone, prop or scene in its own space.
std::vector<Segment3> segments3d(const Json& prim);
// One point of the prim's own space on the page.
Point2 to_page(const Json& prim, const Vec3& p, const Json* camera);

// (a, b, seen) for each edge: seen is false for the edges at the back of a box; figures, heads, hands and models give
// their pen lines (hidden parts already left out).
std::vector<Edge> edges(const Json& prim, const Json* camera);
// (x, y, w, h) on the page.
std::array<double, 4> bbox(const Json& prim, const Json* camera);
std::array<double, 4> prim_bbox(const Json& prim, const Json* camera);
// Lines to draw the prim with a pen (its seen edges, or the figure's bones and head).
std::vector<Line2> trace(const Json& prim, const Json* camera);
// Segments that meet end to end joined into polylines.
std::vector<Line2> join_lines(const std::vector<Line2>& lines, double eps = 0.05);

}  // namespace genko::core::prim3d
