// PC3 節 B (SPEC.md COLOR-05): the retouching ops (B4) — registered here; B4 writes their work in this file (the dabs and
// coverage of render/retouch_dabs, the Poisson blend of render/poisson, PatchMatch of render/inpaint).

#include "render/ops_pc3.hpp"

namespace genko::render {

void register_retouch_ops(core::OpRegistry& registry) {
    // the brushes' common keys: points, width_mm, hardness, opacity, flow, spacing, layer_id, area?, blend
    registry.add("clone_stamp", not_yet("clone_stamp", "B4"));        // (+ offset_mm | source_mm, aligned, transform?, sample)
    registry.add("healing_brush", not_yet("healing_brush", "B4"));    // (as clone_stamp, blended by Poisson)
    registry.add("spot_healing", not_yet("spot_healing", "B4"));      // (+ type: content_aware | create_texture | proximity_match)
    registry.add("history_brush", not_yet("history_brush", "B4"));    // (+ from: the layer's pixels of a state, as fade_layer's)
    registry.add("content_aware_fill", not_yet("content_aware_fill", "B4"));  // ({page, layer_id, area, sampling, …, output, seed})
}

}  // namespace genko::render
