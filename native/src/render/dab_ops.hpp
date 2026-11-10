#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/geometry.hpp"
#include "core/model.hpp"
#include "core/ops_util.hpp"
#include "render/image.hpp"

// brushes@1 (PC4 E1, SPEC.md COLOR-09「ブラシのダブの描き方」): what the line ops need of the dab engine (render/dabs) —
// add_stroke's check of a dab brush's line and its blend painted into a paint layer's pixels, the eraser of a brush
// (erase / erase_raster's "brush", "block"), define_brush's pictures and upstream's settings.

namespace genko::render::dab_ops {

// add_stroke's hooks (core/ops_util.hpp StrokeHooks): the mixing (render/raster mixed_pieces), the check of a dab brush's
// line (its dabs counted: OpError past their limit, or a line wider than 500 mm) and its blend.
core::StrokeHooks stroke_hooks();
// define_brush's (core/ops_util.hpp DabTakeIn): a picture file as a tip or a texture, upstream's settings.
core::DabTakeIn take_in();

// A line with a blend (not normal: image_ops::composite's) painted into the paint layer `id`'s pixels on its own grid:
// its coverage (the brush's: a dab brush's in float) × the line's opacity × the brush's, in its colour (a dab brush's
// colour dynamics: each pixel's) taken into the page's values; the alpha kept on a layer whose transparency is locked.
// Only the tiles of the layer it reaches are read and the pixels it covers written (every other keeps its bytes).
void blend_line(core::OpContext& c, std::size_t at, const std::string& id, const core::Stroke& stroke, const std::string& blend);

// The eraser of a brush (erase's "brush": a dab brush of the book; "block": Photoshop's ブロック, squares of width_mm):
// the "L" mask of the page (`size` at `dpi`) it takes — its coverage × `opacity`, the flow the eraser is used at.
// OpError for a brush that is not a dab brush.
struct Eraser {
    std::string brush;  // (a dab brush's kind; "" with block)
    bool block = false;
    double opacity = 1.0;
    std::optional<double> flow;
};
Image eraser_mask(Size size, const core::PenPoints& points, double width_mm, int dpi, const Eraser& eraser, const std::string& seed);
// erase's eraser from its keys (none when it has neither "brush" nor "block"): the brush checked against the book
// (OpError for a kind that is not one of its dab brushes), opacity and flow 0..1. Without brush or block, an opacity or
// flow is not used and `notes` says so (the op goes through, as before E1 when they were keys it did not know).
std::optional<Eraser> eraser_of(const core::Json& op, const core::Document& doc, std::vector<std::string>& notes);

}  // namespace genko::render::dab_ops
