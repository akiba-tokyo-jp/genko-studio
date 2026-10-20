#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"

// The boxes the align and distribute ops, the Move tool's snapping and Auto-Select go by (PC3 C-d, SPEC.md COLOR-07
// 「移動・整列」). A layer's box is the box of what it shows drawn alone at the working 200 dpi — its pixels, patches, pen
// lines and shape parts, a placed picture or a smart object where it lies, a tone's screen, each as its panels cut it —
// without its mask and vector mask (what the layer holds, as PhotoCraft's layer_bounds: layer_multi_cmds.rs:254); a
// folder's is the box of its layers'; a fill or an adjustment layer has none (it covers the page, as PhotoCraft's). A
// line of dialogue's box is its balloon's frame (x, y, w, h: its tails not in it).

namespace genko::render::layer_bounds {

using Box = std::array<double, 4>;  // x0, y0, x1, y1 (mm); a layer's on the page's working grid (whole working pixels)

constexpr int kDpi = 200;
constexpr double kPx = kDpi / 25.4;  // (working pixels a mm, nominally)

// The page's working grid: its working pixels a mm across and down — the page drawn at 200 dpi, its width and height
// whole pixels, as an 8-bit paint layer holds its pixels over the page (kPx exactly on a page of whole 0.127 mm).
std::array<double, 2> grid(const core::Page& page);

// The layer's box (none: it shows nothing, or is a fill or adjustment layer). core::OpKeyError for a layer the page
// does not have.
std::optional<Box> of_layer(const core::Document& doc, const core::Page& page, const std::string& id);
// A line of dialogue's box.
Box of_line(const core::StoryLine& line);

// Auto-Select (layer.pickAt): the layers that show something at the point (as drawn: their masks and panels cut them;
// a fill layer where it shows), the topmost first — the hidden ones (or in a hidden folder), folders and adjustment
// layers left out. The point is the working 200 dpi pixel it falls in.
std::vector<std::string> layers_at(const core::Document& doc, const core::Page& page, double x_mm, double y_mm);
// Does the layer show something at the point (as layers_at asks it)?
bool shows_at(const core::Document& doc, const core::Page& page, const core::Layer& layer, double x_mm, double y_mm);
// Auto-Select's グループ: the outermost folder the layer is in (the layer itself when it is in none).
std::string outermost(const core::Page& page, const std::string& id);
// The layers inside a folder, every depth (page order).
std::vector<std::string> inside(const core::Page& page, const std::string& folder);

}  // namespace genko::render::layer_bounds
