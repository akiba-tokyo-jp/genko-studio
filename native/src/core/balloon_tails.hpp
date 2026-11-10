#pragma once

#include "core/json.hpp"
#include "core/model.hpp"

namespace genko::core {

// Python's ops.tail_hidden: whether a tail's tip ([x, y] mm) lies inside its line's balloon (an ellipse for the round
// kinds, the box for box, narration, rounded, none and sfx). Python's errors for a tip that is not two numbers
// (core/pyops.hpp). Defined with the line ops (ops_lines.cpp), which move such tips out; the checks report them.
bool tail_hidden(const StoryLine& line, const Json& tip);

}  // namespace genko::core
