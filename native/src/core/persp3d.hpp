#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "core/json.hpp"

// 3D and the perspective ruler together (Python's genko/persp3d.py, 3D カメラとパース定規の連動): the vanishing points
// a 3D object (or the page's camera) makes, as a perspective ruler; and the camera that puts the 3D where a
// perspective ruler says.
//
// A direction u in a prim's own space is seen as r = view · turn · u (view: the page camera's turn, when there is one).
// Lines along it meet on the page at c + focal · (r_x, r_y) / r_z, c being the camera's target (or, without a camera,
// the prim's own centre). Axes seen straight across (r_z ≈ 0) have no vanishing point: they stay parallel.

namespace genko::core::persp3d {

inline constexpr double kFarMm = 20000.0;  // vanishing points further out than this count as none

// {axis: [x, y] (mm, rounded to 2 places)} for the prim's axes that meet on the page, in the order x, z, y.
// `camera` is the page's (null or not truthy: none).
Json vanishing_points(const Json& prim, const Json* camera);

// A perspective ruler through the vanishing points (1 to 3), in the order x, z, y: {"kind", "points",
// "lock_horizon"}; nothing when every axis is seen straight on.
std::optional<Json> ruler_from(const Json& prim, const Json* camera);

// The page camera (tip, turn, roll, focal) whose vanishing points for this prim sit on the ruler's: one point is the
// depth axis, two are across and depth, three add the upright. The target stays. `camera` is the page's camera (or
// the default one with only a target). Throws core::OpError("the perspective ruler has no vanishing points") and
// core::OpError with the message of what Python raises as ValueError; core::Error for its TypeErrors.
Json camera_for(const Json& ruler, const Json& prim, const Json& camera);

// Nelder–Mead as persp3d._minimize: numpy's float64 steps, its stable argsort of the values.
using Vector = std::vector<double>;
Vector minimize(const std::function<double(const Vector&)>& f, const Vector& x0, int steps = 400);

}  // namespace genko::core::persp3d
