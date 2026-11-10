#pragma once

// The 3D guides on a page (Python's render._draw_prims, _draw_surfaces, _draw_mannequin): figures, heads, hands and
// models with their lightly shaded surfaces and pen lines; boxes, cylinders, stairs, floors, spheres, cones, props and
// scenes as their edges (the hidden ones paler); the stick mannequin. Drawn in the name and proof renders only, each
// inside its panel when it has one, the same pixels as the Python baseline.

#include "render/image.hpp"
#include "render/page_internal.hpp"

namespace genko::render::detail {

// render._draw_prims onto the RGB picture `part` of the box `area` of the page.
void draw_prims(Image& part, const Box& area, const Ctx& ctx);

}  // namespace genko::render::detail
