#pragma once

#include <string>
#include <string_view>

#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/selection.hpp"
#include "render/warp.hpp"

// The eraser and the selection on a layer's high-precision colour pixels (native.color_raster_v1). Each returns the
// layer's new raster in its own precision (u16 or f32): what the tool does not reach keeps its bytes, nothing goes
// through an 8-bit picture, and colours are mixed in linear light as the page composites them (ColorCanvas).
//
// The raster lies stretched over the whole page. A page mask (the eraser's track, an area) is drawn at the raster's
// own resolution when a whole dpi gives the page exactly its size — a raster made at the working 200 dpi is drawn as
// an 8-bit paint layer is — and otherwise at the working dpi and resized onto the raster.

namespace genko::render::color_edit {

// raster.erase_raster on precise pixels: the alpha loses what the 8-bit eraser's mask takes (a hard eraser clears the
// pixel); the colour of what stays is kept.
std::string erase(const core::Page& page, const core::Layer& layer, const core::PenPoints& points, double width_mm,
                  std::string_view texture, const std::string& seed);

// delete_area: the area's pixels cleared (its soft edge partly).
std::string delete_area(const core::Page& page, const core::Layer& layer, const core::Json& area);

// transform_area: the area's pixels lifted, resampled through m (mm) and laid over what stays. Pixels moved off the
// page are gone, as in any paint layer that keeps only the page.
std::string move_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const selection::Matrix& m,
                      Resample resample);

// transform_area with a warp: the area's box through `go` (mm → mm), as warp.warp_image does it, in pieces of two
// triangles.
std::string warp_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const warp::Go& go,
                      Resample resample);

// paste: the copied lines and pictures, placed on a clear layer like this one, drawn over its pixels (with the
// layer's alpha kept, 透明保護, when it is locked: as render clips a paint layer's lines to its pixels).
std::string paste(const core::Page& page, const core::Layer& layer, const core::Layer& pasted, const core::Document* episode);

// The lines and pictures an op left on precise paint layers (the pen, a fill, a material) drawn into their pixels, as
// a paint program does: a precise layer keeps no lines or fills of its own. Pages not read yet are not looked at.
void bake_marks(core::Document& doc);

}  // namespace genko::render::color_edit
