// PC3 節 B (SPEC.md COLOR-05): the selection's and the masks' ops — registered here; each group writes its op's work in
// this file (B1 the Quick Mask and the Magic Eraser, B2 the stroke of a selection and the masks, B3 the matting).

#include <string>

#include "core/pyops.hpp"
#include "render/ops_pc3.hpp"

namespace genko::render {

core::OpFunction not_yet(const char* op, const char* group) {
    const std::string message = std::string(op) + " is not in the C++ build yet (PC3 " + group + ")";
    return [message](core::OpContext&) { core::not_yet_ported(message); };
}

void register_select_ops(core::OpRegistry& registry) {
    // B1: the Quick Mask ({page, on, area?, color?, opacity?, indicates?}: an alpha channel marked quick_mask, masks@1),
    // the Magic Eraser ({page, layer_id, x_mm, y_mm, tolerance, contiguous, antialias, sample, opacity, area?})
    registry.add("quick_mask", not_yet("quick_mask", "B1"));
    registry.add("magic_erase", not_yet("magic_erase", "B1"));
    // B2: Edit › Stroke ({page, layer_id, area?, width_mm, rgb, opacity, location, preserve_transparency?}); Layer › Layer
    // Mask › Apply ({page, id}) and From Transparency ({page, id})
    registry.add("outline_area", not_yet("outline_area", "B2"));
    registry.add("apply_layer_mask", not_yet("apply_layer_mask", "B2"));
    registry.add("mask_from_transparency", not_yet("mask_from_transparency", "B2"));
    // B3: Layer › Matting ({page, id, kind: defringe | remove_black | remove_white | decontaminate, width_mm?, amount?,
    // radius_mm?, area?})
    registry.add("matting", not_yet("matting", "B3"));
}

}  // namespace genko::render
