#pragma once

#include "core/json.hpp"

// An op's area (Python's genko/selection.py and selops.py: the selection language). The bus turns every richer kind
// into {poly} or {mask} first (core::AreaResolver); the ops then take one of those two.

namespace genko::core {

// ops._area: the op's "area" as {"poly": [[x, y], …]} (three corners or more) or {"mask": {"box", "png"}}.
// OpError "an area needs at least three corners" / "area is {poly: [[x, y], …]} or {mask: {box, png}}"; Python's
// AttributeError (PyUncaught) for an area or a mask that is not an object, and len()'s TypeError for a poly that has
// none.
Json op_area(const Json& op);

}  // namespace genko::core
