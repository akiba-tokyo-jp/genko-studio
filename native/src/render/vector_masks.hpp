#pragma once

#include <optional>
#include <vector>

#include "core/model.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"

// vector@1 (PC3 C-a, SPEC.md COLOR-07「ベクトルマスク」): a layer's vector mask as the drawing lays it on its layer — in
// the three ways (8 bits, precise, inks) and the merges, with its layer mask (render/layer_masks): the layer shows by
// the layer mask × the vector mask — and drawn into its layer mask (Layer › Rasterize › Vector Mask).

namespace genko::render::vector_masks {

// Does the layer's vector mask lie on it as it is drawn (it has one and it is on)?
bool shows(const core::Layer& layer);
// The vector mask's values over `area` of the page drawn at `size` (0..1, the area's pixels row by row): its path's
// coverage (a path of no subpath: everything, or nothing when inverted), its density laid on (1 − density·(1 − c)),
// feathered by a Gaussian of σ = feather_mm / 2 on the page (worked over the area widened by 3σ, so the blur at its
// edge sees what lies past it). All 1 when it is off.
std::vector<float> values(const core::Page& page, const core::vector::VectorMask& mask, Size size, const Box& area);
// The same as an "L" picture of the area.
Image shown(const core::Page& page, const core::vector::VectorMask& mask, Size size, const Box& area);
// `mask` (the layer mask as it shows over the area, or none) times the layer's vector mask when it shows; `mask` as it
// was when it does not (a layer without one is drawn as before).
std::optional<Image> with_vector_mask(const core::Page& page, const core::Layer& layer, Size size, const Box& area, std::optional<Image> mask);
// Layer › Rasterize › Vector Mask: the layer's vector mask (as it shows: density, feather; nothing when it is off)
// multiplied into its layer mask at the mask's own 150 dpi (a layer without one: a mask that reveals all first), and
// taken off. The layer mask's own settings (enabled, linked, density, feather) are kept.
void rasterize(const core::Page& page, core::Layer& layer);

}  // namespace genko::render::vector_masks
