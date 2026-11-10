// PC3 節 C (SPEC.md COLOR-07): the transform, align and arrange ops — registered here; C-c and C-d write their work in
// this file (the map of each element: core/page_geometry; the pixels drawn again: render/resample). C-c's work is
// render/layer_transform (transform_layers) and render/distort_ops (content_aware_scale), C-d's render/layer_align (the
// boxes: render/layer_bounds).

#include "render/distort_ops.hpp"
#include "render/layer_align.hpp"
#include "render/layer_transform.hpp"
#include "render/ops_pc3.hpp"

namespace genko::render {

void register_transform_ops(core::OpRegistry& registry) {
    // C-c: layers moved, scaled, turned, warped ({page, ids, parts?, area?, box?, move | matrix | quad | turn | warp |
    // perspective | puppet, interp, with?}), Content-Aware Scale ({page, id, area?, scale_x, scale_y, amount, protect?})
    registry.add("transform_layers", layer_transform::transform_layers);
    registry.add("content_aware_scale", content_aware_scale);
    // C-d: align and distribute ({page, ids, lines?, edge, to?, area?, frame_id?} | {page, ids, lines?, by}), the stacking
    // order among siblings ({page, ids, to}), links ({page, ids, link?})
    registry.add("align_layers", layer_align::align_layers);
    registry.add("distribute_layers", layer_align::distribute_layers);
    registry.add("arrange_layers", layer_align::arrange_layers);
    registry.add("link_layers", layer_align::link_layers);
}

}  // namespace genko::render
