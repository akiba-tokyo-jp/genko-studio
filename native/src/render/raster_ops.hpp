#pragma once

#include "core/command_bus.hpp"

// The ops of M3-A that draw (Python's ops._apply_one, layerops and selection): convert_layer, merge_down,
// merge_layers, merge_visible, set_layer_mask, paint_mask, put_raster, filter_raster, fill, fill_area, fill_enclosed,
// fill_gaps, flood_fill, gradient_fill, delete_area, transform_area, paste, and erase / erase_raster on a paint
// layer's pixels; and the bus's resolver of the richer areas (selops.resolve). Each with Python's arguments,
// defaults, checks, messages and pixels.

namespace genko::render {

// Adds these ops (erase and erase_raster take over core's, which stop at a paint layer's pixels) and the area
// resolver to `registry`.
void register_raster_ops(core::OpRegistry& registry);

}  // namespace genko::render
