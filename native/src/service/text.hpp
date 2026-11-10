#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Python's str operations the studio's words use on UTF-8 text.

namespace genko::service::text {

// len(s): the code points.
std::size_t length(std::string_view utf8);
// s[:n]: the first n code points.
std::string head(std::string_view utf8, std::size_t n);

}  // namespace genko::service::text

namespace genko::service {

// Python's a // b for floats (CPython's float_floor_div: fmod, then the quotient snapped to its integer).
double py_floordiv(double a, double b);

}  // namespace genko::service
