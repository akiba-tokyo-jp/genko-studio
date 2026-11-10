#pragma once

#include "core/command_bus.hpp"

// PC3 C-c (SPEC.md COLOR-07): Edit › Content-Aware Scale (render/seam) and Filter › Liquify's strokes (render/
// liquify_field) on a layer.

namespace genko::render {

// content_aware_scale {page, id, area?, scale_x, scale_y (1..400 %), amount (0..100), protect? (an alpha channel's id or
// name), protect_skin?}: a paint layer's pixels within the area (or all it shows) made scale_x × scale_y as large by
// seam carving, laid at the same top left.
void content_aware_scale(core::OpContext& c);
// liquify's strokes ({page, layer_id | layer, strokes: [{tool, size_mm, density, pressure, rate, points, amount?}…],
// mesh_mm?, area?}): the strokes replayed on a fresh field; the layer's pixels and patches drawn through it once, its
// pen lines' and shape parts' points moved by it. A smart object (PC3 A5) keeps them as its smart filter (its contents
// as they are; the area its filter mask when it has none: PhotoCraft's run_on_layer).
void liquify_strokes(core::OpContext& c);

}  // namespace genko::render
