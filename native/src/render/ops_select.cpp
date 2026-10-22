// PC3 節 B (SPEC.md COLOR-06): the selection's and the masks' ops — registered here; each group writes its op's work in
// this file (B1 the Quick Mask and the Magic Eraser, B2 the stroke of a selection and the masks, B3 the matting).

#include <string>

#include "core/pyops.hpp"
#include "render/ops_masks.hpp"
#include "render/ops_pc3.hpp"

namespace genko::render {

core::OpFunction not_yet(const char* op, const char* group) {
    const std::string message = std::string(op) + " is not in the C++ build yet (PC3 " + group + ")";
    return [message](core::OpContext&) { core::not_yet_ported(message); };
}

void register_select_ops(core::OpRegistry& registry) {
    // B1: the Quick Mask ({page, on, area?, color?, opacity?, indicates?}: an alpha channel marked quick_mask, masks@1),
    // the Magic Eraser ({page, layer_id, x_mm, y_mm, tolerance, contiguous, antialias, sample, opacity, area?})
    registry.add("quick_mask", quick_mask);    // (render/ops_quick_mask.cpp)
    registry.add("magic_erase", magic_erase);  // (render/erase_magic.cpp)
    // B2: Edit › Stroke ({page, layer_id, area?, width_mm, rgb, opacity, location, preserve_transparency?}); Layer › Layer
    // Mask › Apply ({page, id}) and From Transparency ({page, id})
    // (render/ops_masks.cpp)
    registry.add("outline_area", outline_area);
    registry.add("apply_layer_mask", apply_layer_mask);
    registry.add("mask_from_transparency", mask_from_transparency);
    // B3: Layer › Matting ({page, id, kind: defringe | remove_black | remove_white | decontaminate, width_mm?, amount?,
    // radius_mm?, area?})
    registry.add("matting", matting_op);  // (render/ops_matting.cpp)
}

}  // namespace genko::render
