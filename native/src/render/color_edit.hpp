#pragma once

#include <array>
#include <optional>
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

// --- pieces of precise pixels (PC3 B2: カット・コピー・ペースト on a precise page) ---------------------------------------

// A piece copied from a precise paint layer: its samples (GKCR, the layer's precision: alpha × the area's coverage, cut
// to what shows), the box it covers (mm) and the space of its values ("srgb", or the page's RGB profile's ref).
struct Piece {
    std::array<double, 4> box_mm{};  // x, y, w, h
    std::string samples;             // (GKCR bytes)
    std::string space = "srgb";
};
// The precise pixels of the layer an area covers ({poly} or {mask}); none when nothing shows there or the layer holds no
// precise pixels.
std::optional<Piece> lift_piece(const core::Page& page, const core::Layer& layer, const core::Json& area);
// The piece as a picture to copy (selection::Items' patch): its picture as the screen shows it (8-bit sRGB), its box, and
// its samples ("gkcr": GKCR in base64, "gkcr_space": its space — the C++ build's own keys, never saved: paste lays them
// or drops the keys).
inline constexpr std::string_view kPieceKey = "gkcr";
inline constexpr std::string_view kPieceSpaceKey = "gkcr_space";
core::Patch piece_patch(const Piece& piece);
// A patch's own samples (GKCR) in the page's space (converted from another RGB space through sRGB, HDR kept); none when it
// carries none, or they cannot be read.
std::optional<std::string> patch_samples(const core::Patch& patch, const core::Page& page, const core::Document* episode);
// The samples laid over the layer's precise pixels at `box_mm` through `m` (mm → mm): moved and scaled, each pixel
// whole where it lands on a pixel's centre (its samples as they were), else mixed in linear light as move_area does.
// A layer with no precise pixels yet gets a clear raster at the piece's resolution (the working 200 dpi at least, 600
// dpi at most) in the page's precision.
std::string lay_piece(const core::Page& page, const core::Layer& layer, const std::string& samples, const std::array<double, 4>& box_mm,
                      const selection::Matrix& m, Resample resample = Resample::Bilinear);

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
