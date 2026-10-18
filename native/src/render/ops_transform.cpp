// PC3 節 C (SPEC.md COLOR-07): the transform, align and arrange ops — registered here; C-c and C-d write their work in
// this file (the map of each element: core/page_geometry; the pixels drawn again: render/resample).

#include "render/ops_pc3.hpp"

namespace genko::render {

void register_transform_ops(core::OpRegistry& registry) {
    // C-c: layers moved, scaled, turned, warped ({page, ids, parts?, area?, box?, move | matrix | quad | turn | warp |
    // perspective | puppet, interp, with?}), Content-Aware Scale ({page, id, area?, scale_x, scale_y, amount, protect?})
    registry.add("transform_layers", not_yet("transform_layers", "C-c"));
    registry.add("content_aware_scale", not_yet("content_aware_scale", "C-c"));
    // C-d: align and distribute ({page, ids, lines?, edge | by, to?}), the stacking order among siblings ({page, ids, to}),
    // links ({page, ids, link?})
    registry.add("align_layers", not_yet("align_layers", "C-d"));
    registry.add("distribute_layers", not_yet("distribute_layers", "C-d"));
    registry.add("arrange_layers", not_yet("arrange_layers", "C-d"));
    registry.add("link_layers", not_yet("link_layers", "C-d"));
}

}  // namespace genko::render
