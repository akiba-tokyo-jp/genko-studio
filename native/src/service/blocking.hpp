#pragma once

#include <array>
#include <vector>

#include "core/json.hpp"
#include "core/pynum.hpp"

// Where characters sit in a panel, derived from the panel brief (Python's genko/studio/blocking.py): a rough heuristic,
// enough to keep balloons off faces and to point tails.

namespace genko::service {

using core::Json;

using Box = std::array<double, 4>;      // x, y, w, h in mm
using NumBox = std::array<core::Num, 4>;  // the same as Python holds them (min and max keep a panel's int as it is)

Box as_box(const NumBox& box);
Json box_json(const NumBox& box, int places);  // [round(v, places) for v in box]

struct Figure {
    Json char_id;  // the character's "id" as the brief gives it ("" when it has none)
    NumBox head;
    NumBox body;
};

// blocking.figures(panel, rect): a figure for each character of the brief, sized by the shot (none for a shot it does
// not know). Python's errors for a brief that is not what it takes (AttributeError for a character that is not an
// object, TypeError for an unhashable shot or position, ValueError/TypeError for a scale that is not a number).
std::vector<Figure> figures(const Json& panel, const NumBox& rect);

}  // namespace genko::service
