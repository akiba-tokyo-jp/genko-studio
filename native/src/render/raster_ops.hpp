#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "render/image.hpp"

// The ops of M3-A that draw (Python's ops._apply_one, layerops and selection): convert_layer, merge_down,
// merge_layers, merge_visible, set_layer_mask, paint_mask, put_raster, filter_raster, fill, fill_area, fill_enclosed,
// fill_gaps, flood_fill, gradient_fill, delete_area, transform_area, paste, and erase / erase_raster on a paint
// layer's pixels; lt_convert (the light table picture to pen lines); and the bus's resolver of the richer areas (selops.resolve). Each with Python's arguments,
// defaults, checks, messages and pixels.

namespace genko::render {

// ops._fill_reference(episode, page, layer, reference, dpi): what a fill and auto-select look at — the page as seen
// ("page"), the layer alone ("layer") or the reference layers ("reference") — in grey ("L"). OpError for another
// reference or a layer not on the page.
Image fill_reference_of(const core::Document& doc, const core::Page& page, const std::string& layer_id, const std::string& reference,
                        int dpi);

// The book's own brushes made known to the process (as the ops do before they draw): for the window's own drawing of
// areas (selops with "layer" or "color", auto-select).
void use_brushes_of(const core::Document& doc);

// Adds these ops (erase and erase_raster take over core's, which stop at a paint layer's pixels) and the area
// resolver to `registry`.
void register_raster_ops(core::OpRegistry& registry);

// Adds the paint ops of M3④ (add_shape, smudge, liquify: render/ops_paint.cpp).
void register_paint_ops(core::OpRegistry& registry);

// ops.shape_points: a figure's outline (mm) and whether it is closed — line, polyline, curve (Catmull–Rom through the
// points) from op["points"]; rect (op["radius_mm"] rounds its corners), ellipse, polygon (op["sides"], op["angle"]) from
// op["box"]. The 図形 tool draws its preview with it. Throws core::OpError.
struct ShapeOutline {
    std::vector<std::array<double, 2>> points;
    bool closed = false;
};
ShapeOutline shape_points(const std::string& kind, const core::Json& op);

}  // namespace genko::render
