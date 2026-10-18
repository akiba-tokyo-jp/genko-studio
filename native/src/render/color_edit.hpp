#pragma once

#include <string>
#include <string_view>

#include "core/color_raster.hpp"
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

// An area of the page as a mask ("L") on the raster's own pixels.
Image area_on(const core::Page& page, const core::ColorRasterView& raster, const core::Json& area);

// How many of the raster's pixels span one pixel of the working 200 dpi (for sizes given in an 8-bit layer's pixels).
double pixel_scale(const core::Page& page, const core::ColorRasterView& raster);

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

// set_page_spec (PC3 C-e): the raster on the new paper — the old basic frame onto the new one as set_page_spec moves a
// point —, at the resolution it had (its pixels a mm across and down), bilinear and premultiplied (outside: clear).
std::string relayout(const std::string& gkcr, const core::PageSpec& old_paper, const core::PageSpec& paper, const core::Rect& old_frame,
                     const core::Rect& new_frame);

// The lines and pictures an op left on precise paint layers (the pen, a fill, a material) drawn into their pixels, as
// a paint program does: a precise layer keeps no lines or fills of its own. Pages not read yet are not looked at.
void bake_marks(core::Document& doc);

// color-model@1: a picture lifted off an RGB page of a profile (selection::lift: its paint pixels, the page's values)
// carries that profile ("rgb_profile": its ref — the C++ build's own key, never saved).
inline constexpr std::string_view kPatchProfileKey = "rgb_profile";
// Such a picture as the sRGB its values show (its profile's, a built-in one or one the book holds; another taken as
// sRGB), the key dropped; any other picture as it is.
void patch_to_srgb(core::Patch& patch, const core::Document& doc);
// After an op, those pictures where it left them: on an RGB page of the same profile laid into the paint layer's pixels
// as the values they are (a layer with none gets them at the working 200 dpi), anywhere else brought to sRGB. No
// picture keeps the key. Pages not read yet are not looked at.
void bake_page_values(core::Document& doc);

}  // namespace genko::render::color_edit
