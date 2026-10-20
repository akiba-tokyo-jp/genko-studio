#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "core/vector.hpp"

// pen-shapes@1 in a PSD (PC3 C-b, the user's decisions 3 and 7: always read and written; SPEC.md COLOR-07「PSD のシェイプ」):
// a pen layer's shape part as Photoshop's shape layer — PhotoCraft v0.2.0's io/src/vector_map.rs (vscg, vstk, vogk) and
// io/src/blocks.rs (SoCo, GdFl), MIT: native/third_party/photocraft/NOTICE.md. Its fill as a fill layer's block (SoCo:
// a colour; GdFl: a gradient — its angle, scale and offset over the path's box, its kind, its stops and opacities,
// reversed —), the same again in vscg, its path as vsms (render/psd_paths' records), its stroke as vstk (width,
// alignment, caps, joins, dashes, opacity, colour; the fill on or off), its live shape as vogk where Photoshop has an
// origination type for it (a rectangle, rounded or not, an ellipse, a line), the part's opacity as the fill opacity (iOpa).
// Coordinates: the page's mm are fractions of its paper (the canvas), sizes pixels of the canvas at its resolution.

namespace genko::render::psd_shapes {

// What becomes of a pen layer with shape parts in a PSD: one shape layer (one part, no lines, no pictures, no vector mask
// of its own), a group of the layer's (its pictures, its parts under its lines, its lines, its parts over them), or one
// pixel layer of its look — a layer a group would not show the same (clipped to the layer below, screened, shown in a
// colour, with an effect) or a part Photoshop's blocks cannot say (a pattern's fill).
enum class Plan { Shape, Group, Pixels };
Plan plan(const core::Layer& layer);
// Can the blocks say this part (a fill and a stroke of none, a colour, the layer's ink, a gradient)?
bool writable(const core::vector::ShapePart& part);

// The canvas a part is written on: the page's paper in mm and in pixels at the PSD's resolution.
struct Canvas {
    double width_mm = 0;
    double height_mm = 0;
    int width_px = 0;
    int height_px = 0;
    double dpi = 0;
};
// The part's tagged blocks after its name (each "8BIM", its key, its length, its payload padded to 4).
std::string blocks(const core::vector::ShapePart& part, const Canvas& canvas);

// A shape layer read back (its blocks' payloads; `fill_key` SoCo, GdFl or PtFl, or empty when only vscg holds the fill):
// the part on the page (`canvas` as the file's canvas lies on it: mm a canvas pixel across and down, its top left);
// none when its fill or path cannot be read (a pattern's, a block that is not one).
struct Blocks {
    std::string fill_key;
    std::string_view fill;
    std::string_view vscg;
    std::string_view vstk;
    std::string_view vogk;
    std::string_view path;
};
struct Placement {
    double mm_x = 1.0;  // (mm a canvas pixel across)
    double mm_y = 1.0;
    double x0_mm = 0.0;  // (where the canvas's top left lies on the page)
    double y0_mm = 0.0;
    double width_px = 1.0;  // (the canvas, pixels)
    double height_px = 1.0;
    double dpi = 72.0;  // (the file's resolution: a point's pixels)
};
std::optional<core::vector::ShapePart> part_of(const Blocks& blocks, const Placement& placement);

// What a PSD of the book says of its shape parts (the export's notes): layers written as pixels, gradients Photoshop's
// cannot be (an ellipse's, a repeated one), a rule of even and odd baked.
std::vector<std::string> notes(const core::Document& book);

}  // namespace genko::render::psd_shapes
