#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"

// A posable stick mannequin (Python's genko/mannequin.py): 2D with a little depth, shared by the name and proof renders
// and the pose guide. The figure stands on `pos` (the pelvis, page mm); size[1] is its height (eight heads); every
// joint has `yaw` (a bend in the picture, counter-clockwise +) and `pitch` (toward or away from the viewer, which only
// shortens the segment); `rot` [tip, turn, lean] turns the whole figure.

namespace genko::core::mannequin {

using Point2 = std::array<double, 2>;

extern const std::array<std::string_view, 16> kJoints;  // JOINTS

// PRESETS ({name: {"rot"?: [...], joint: {"yaw": …}}}), in Python's order.
const Json& presets();

// {joint: {"yaw", "pitch"}} with the arms and legs a little apart.
Json default_joints();

// The preset's joints (and turn) on the prim, and prim["preset"]; core::Error("value", "preset must be one of …").
void apply_preset(Json& prim, std::string_view name);

struct Segment {
    Point2 a;
    Point2 b;
    std::string part;  // body, left or right (the figure's own sides)
};

struct Bone {
    std::vector<Segment> segments;
    Point2 head_c{};
    double head_r = 0.0;
    int facing = 1;
    std::vector<std::pair<std::string, Point2>> points;
    std::array<double, 4> bbox{};  // x, y, w, h

    const Point2& point(std::string_view name) const;
};

// Points (page mm) of the figure.
Bone skeleton(const Json& prim);

// The joint change ({"joints": {joint: {"yaw": …}}}) that points the dragged part at `target` (page mm); "pelvis"
// moves the whole figure ({"pos": …}); {} when the target is on the part's base. core::Error("value") for an unknown
// handle (Python's ValueError).
Json pose_to(const Json& prim, std::string_view handle, const Json& target);

// Whether most of the figure is inside a page rect.
bool in_rect(const Json& prim, const std::array<double, 4>& rect);

}  // namespace genko::core::mannequin
