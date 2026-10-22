#pragma once

#include <optional>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/area_kinds.hpp"
#include "render/image.hpp"
#include "render/proof.hpp"

// PC3 B1 (SPEC.md COLOR-06「色で選ぶ」): choosing by colour — the magic wand's colour criterion on a layer or on the page
// (render/selection's {"color"}, with this build's own "layer" and "antialias", and on a precise or ink page by its
// values), Color Range ({"range"}), Grow and Similar ({"grow"}, {"similar"}): PhotoCraft v0.2.0's
// engine/src/selection_cmds.rs (magic_wand, color_range, grow_similar) over algo/src/selection.rs (render/selection_ext),
// MIT (native/third_party/photocraft/NOTICE.md). Each reads the pixels it compares — a layer's own (`layer`:
// image_ops::shown_layer_pixels) or the page as it shows (image_ops::page_pixels, the working 200 dpi) — as RGB 0..1
// (a precise page's values, HDR clipped to 0..1; an ink page's colours its inks mean), works on that grid and gives the
// area kind's mask on the grid it is asked for (render/area_kinds::Context).

namespace genko::render::select_color {

// Does a {"color"} area go this build's way: it names a layer or anti-aliasing, or the page is not one of 8-bit RGB
// (a precise or ink page: compared by its values)? Otherwise it is render/selection's own (the page drawn at 8 bits, as
// always).
bool own_colour(const core::Json& spec, const core::Page& page);
// {"color": {"x_mm", "y_mm", "tolerance" = 24 (levels, 0..255), "contiguous", "layer"?, "antialias"?}} that way: the
// pixels within the tolerance of the one at the point (each channel, alpha too), only those joined to it when
// contiguous; anti-aliased, the edge grey (selection_ext::magic_wand). A mask ("L") of `size` over the page.
Image colour(const core::Json& spec, const core::Page& page, const core::Document* episode, Size size);

// The area kinds (render/area_kinds' table):
// {"range": {"select" = "sampled" | reds | yellows | greens | cyans | blues | magentas | highlights | midtones | shadows |
//  out_of_gamut, "colors"?: [[r, g, b] 0..255, …], "points_mm"?: [[x, y], …], "subtract_points_mm"?, "fuzziness" (0..200 =
//  40; tones 0..100 % = 20), "localized", "range" (0..100 % of the longer side = 100), "tonal_range" (a level, or
//  [lo, hi] for midtones), "invert", "layer"?, "profile"? and "threshold"? (out_of_gamut: the proof profile — a built-in
//  id or one of the book's by its sha256 ref; none: the plain conversion for print — and the ΔE)}}
Image range(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"grow": {"area", "tolerance" = 32, "layer"?}}: the area (each pixel half covered or more) and the pixels joined to it
// whose colour lies in the area's range of colours widened by the tolerance (each channel, alpha too). {"similar": …}:
// those anywhere on the page.
Image grow(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
Image similar(const core::Json& value, const core::Json& area, const area_kinds::Context& c);

// Color Range's Out of Gamut: the page's colours (its own values: an RGB page's of its profile, a grey or CMYK page's
// inks; else as shown) the proof setup's profile cannot hold — what 色域外警告 marks (render/proof::Gamut, ΔE past
// `threshold`) —, as a mask of `size` over the page. The window passes its own setup (表示 → 校正の設定).
Image out_of_gamut(const core::Page& page, const core::Document& doc, const proof::Setup& setup, double threshold, Size size);

}  // namespace genko::render::select_color
