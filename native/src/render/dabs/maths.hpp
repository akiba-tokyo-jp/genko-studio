#pragma once

#include <utility>

// The angles of the dab engine, the same bits on every computer (docs/cpp-migration/ARCHITECTURE.md §4): sin and cos
// are the paper's fixed polynomial (render/paper.hpp sin_cos_degrees); the arc tangent is computed here with only +,
// -, *, / and the square root (correctly rounded in IEEE arithmetic), never the C library's.

namespace genko::render::dabs {

// sin and cos of an angle in degrees.
std::pair<double, double> sin_cos(double degrees);
// atan2(y, x) in degrees, -180..180 (0 for 0, 0).
double atan2_degrees(double y, double x);
// x mod m, in [0, m) (Rust's rem_euclid for floats).
double rem_euclid(double x, double m);

}  // namespace genko::render::dabs
