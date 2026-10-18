// PC3 節 C (SPEC.md COLOR-06): the paths', vector masks' and shape parts' ops — registered here; C-a and C-b write their
// work in this file (the paths' JSON and checks: core/vector; their coverage: render/path_raster).

#include "render/ops_pc3.hpp"

namespace genko::render {

void register_vector2_ops(core::OpRegistry& registry) {
    // C-a: the page's paths ({page, name?: "work" | a name, path, op?}, {page, name}, {page, name, to}), a work path from an
    // area ({page, area, tolerance_mm?}), a line of dialogue's letters as a path or a shape ({id, to, …}), a path stroked as
    // pen lines ({page, layer_id, name? | path?, tool, kind?, width_mm?, rgb?, opacity?, simulate_pressure?}), a layer's
    // vector mask ({page, id, path? | path_name?, reveal_all? | hide_all?, enabled?, linked?, density?, feather_mm?,
    // invert?, delete?})
    registry.add("set_path", not_yet("set_path", "C-a"));
    registry.add("delete_path", not_yet("delete_path", "C-a"));
    registry.add("rename_path", not_yet("rename_path", "C-a"));
    registry.add("make_work_path", not_yet("make_work_path", "C-a"));
    registry.add("text_to_path", not_yet("text_to_path", "C-a"));
    registry.add("stroke_path", not_yet("stroke_path", "C-a"));
    registry.add("set_vector_mask", not_yet("set_vector_mask", "C-a"));
    // C-b: a pen layer's shape parts (決定 7, pen-shapes@1)
    registry.add("add_shape_part", not_yet("add_shape_part", "C-b"));
    registry.add("edit_shape_part", not_yet("edit_shape_part", "C-b"));
    registry.add("combine_shape_parts", not_yet("combine_shape_parts", "C-b"));
    registry.add("move_shape_parts", not_yet("move_shape_parts", "C-b"));
}

}  // namespace genko::render
