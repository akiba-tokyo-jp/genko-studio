#pragma once

#include <string>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"
#include "render/area_kinds.hpp"

// vector@1 (PC3 C-a, SPEC.md COLOR-07「パスの区域」): the area kinds of a path — {"path": PATH}, {"path_name": "work" | a
// saved path's name}, {"shape_part": {"layer_id", "id"}} (a pen layer's shape part's path), {"vector_mask": a layer's
// id} (its path, as Photoshop loads a vector mask as a selection: neither its density nor its feather) — each its path's
// exact coverage on the area's grid (render/path_raster), grey at the edge unless the area says "antialias": false
// (then two-valued: half covered or more is inside). render/area_kinds' table points at them; the modifiers
// (feather_mm, grow_mm, invert, …) are laid on them as on any kind.

namespace genko::render::path_areas {

Image path(const core::Json& value, const core::Json& area, const area_kinds::Context& context);
Image path_name(const core::Json& value, const core::Json& area, const area_kinds::Context& context);
Image shape_part(const core::Json& value, const core::Json& area, const area_kinds::Context& context);
Image vector_mask(const core::Json& value, const core::Json& area, const area_kinds::Context& context);

// The path a name means on a page: none, null or "work" its work path, else the saved path of that name. core::OpError
// ("no work path", "no path named …") when there is none.
const core::vector::Path& named_path(const core::Page& page, const core::Json& name);
// Is the name the work path's (none, null or "work")?
bool is_work(const core::Json* name);

}  // namespace genko::render::path_areas
