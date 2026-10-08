#pragma once

#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// filter_raster on a layer's high-precision colour pixels (native.color_raster_v1): every kind of filters.KINDS but a
// plugin, as the 8-bit filter does it (Python's genko/filters.py: the same settings, read and refused as it reads
// them, the same steps in the same order and colour space) with the samples kept in the raster's own precision —
// nothing through 8 bits, no rounding between the steps. Where the 8-bit filter clips to 0..255, an in-range sample is
// held to 0..1 and an HDR one goes on. Line extraction alone reads an 8-bit picture of the layer: what it makes is a
// line drawing, not the layer's colours.
//
// Sizes in pixels (a blur's radius, a mosaic's block …) are the 8-bit layer's, at the working 200 dpi: on a raster of
// another resolution they are scaled to the same size on the page.

namespace genko::render::color_filters {

// The settings read and refused as the 8-bit filter reads them (on a picture of a few pixels): before the op's area,
// as the 8-bit op reads them.
void check(std::string_view kind, const core::Json& params);

// The layer's new raster (its own precision). `area`: only inside it (filters.within), on the page.
std::string apply(const core::Page& page, const core::Layer& layer, std::string_view kind, const core::Json& params,
                  const core::Json* area);

}  // namespace genko::render::color_filters
