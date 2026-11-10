#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "render/image.hpp"

// PC3 節 B (B2, SPEC.md COLOR-06「マスク」「範囲の中身の編集」): the ops of a layer's mask and of an area's edge, on the three
// ways' pixels (render/image_ops Pixels: 8 bits, precise, inks) — registered by register_select_ops (render/ops_select).

namespace genko::render {

// Layer › Layer Mask › Apply ({page, id}): the layer's alpha × its mask as it shows (its density and feather laid on),
// then the mask taken off. A pen layer's lines are drawn into its pixels first (it is a paint layer after); any other
// kind of layer is refused (rasterize it first).
void apply_layer_mask(core::OpContext& c);

// Layer › Layer Mask › From Transparency ({page, id, linked?, density?, feather_mm?}): the layer's alpha, grey as it is,
// becomes its mask (at the page's mask grid, layer_masks::grid_dpi) and its pixels opaque where it held any. A mask it
// had is replaced, its settings kept.
void mask_from_transparency(core::OpContext& c);

// Edit › Stroke ({page, layer_id, area?, width_mm, rgb?, opacity?, location: inside | center | outside,
// preserve_transparency?}): a band along the area's edge (none given: the edge of what the layer holds — its alpha),
// inside, straddling or outside it, made by the distance transform (render/selection_ext), filled in the colour as
// fill_area fills an area.
void outline_area(core::OpContext& c);

// The colour `rgb` (sRGB 0..255) laid by `coverage` × opacity over the paint layer `id`'s pixels with their alpha kept
// (Photoshop's 透明部分の保持): `coverage` an "L" picture lying at `offset` (pixels) on the page at `dpi`. Each pixel's
// colour moves toward the colour by its share — 8 bits in its values, precise in linear light, inks through the colours
// they mean and back by the page's rules (a pixel not reached keeps its bytes, its inks) —; a clear pixel stays clear.
void fill_keeping_alpha(core::OpContext& c, std::size_t at, const std::string& id, const Image& coverage, Point offset, int dpi,
                        const std::vector<std::int64_t>& rgb, double opacity);

}  // namespace genko::render
