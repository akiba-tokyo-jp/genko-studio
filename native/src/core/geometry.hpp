#pragma once

#include "core/json.hpp"
#include "core/pynum.hpp"

namespace genko::core {

// A point on the page in mm, as Python keeps it (each coordinate an int or a float).
struct Point {
    Num x;
    Num y;
};

// A pen point in mm (always a float).
struct PointF {
    double x = 0.0;
    double y = 0.0;
};

// A box on the page in mm (Python's models.Rect).
struct Rect {
    Num x;
    Num y;
    Num width;
    Num height;

    // Python's Rect.contains: the edges count as inside (compared exactly, as Python compares ints and floats).
    bool contains(const Num& px, const Num& py) const;
};

// {"x", "y", "width", "height"} (io._rect_to_dict).
Json rect_to_json(const Rect& rect);

}  // namespace genko::core
