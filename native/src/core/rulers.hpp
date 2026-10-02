#pragma once

#include <functional>
#include <vector>

#include "core/json.hpp"
#include "core/stroke_tools.hpp"

// Rulers a pen line snaps to (Python's genko/rulers.py): snap() and symmetry_copies(), the parts the drawing ops
// use. The rulers are the page's JSON as Python keeps it; a ruler that lacks what its kind needs fails as it does in
// Python (OpKeyError "not found: 'points'", …).

namespace genko::core {

// frame_contains(frame_id, x, y): whether the point is in the page's panel with that id (false when there is none).
using FrameContains = std::function<bool(const Json& frame_id, double x, double y)>;

// rulers.snap: the stroke snapped to the ruler that suits it best (the one whose line stays nearest to what was
// drawn); the points as they are when no ruler takes it. `only`: one ruler by id (when truthy); `layer_id`: the layer
// drawn on (nullptr: Python's None).
PenPoints ruler_snap(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains, const Json& only,
                     const Json* layer_id);

// rulers.symmetry_copies: the extra strokes the active symmetry rulers make from one stroke.
std::vector<PenPoints> symmetry_copies(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains,
                                       const Json* layer_id);

}  // namespace genko::core
