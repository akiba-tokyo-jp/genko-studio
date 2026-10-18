#pragma once

#include "core/command_bus.hpp"

// PC3 (選択・マスク・修復・パス・変形・判型; SPEC.md COLOR-05, COLOR-06): the new ops, this build's own (the public list,
// `genko schema`, and the MCP tools stay as they are; MCP's apply takes them). Each group writes its ops' work in its
// file; until then an op is registered and refused with not_yet_ported naming it and its group, so a book or a script
// that names one is told it is not in this build yet rather than that it is unknown.

namespace genko::render {

// render/ops_select.cpp (節 B: B1 quick_mask, magic_erase; B2 outline_area, apply_layer_mask, mask_from_transparency;
// B3 matting)
void register_select_ops(core::OpRegistry& registry);
// render/ops_retouch.cpp (節 B: B4 clone_stamp, healing_brush, spot_healing, history_brush, content_aware_fill)
void register_retouch_ops(core::OpRegistry& registry);
// render/ops_vector2.cpp (節 C: C-a set_path, delete_path, rename_path, make_work_path, text_to_path, stroke_path,
// set_vector_mask; C-b add_shape_part, edit_shape_part, combine_shape_parts, move_shape_parts)
void register_vector2_ops(core::OpRegistry& registry);
// render/ops_transform.cpp (節 C: C-c transform_layers, content_aware_scale; C-d align_layers, distribute_layers,
// arrange_layers, link_layers)
void register_transform_ops(core::OpRegistry& registry);
// render/ops_pagesize.cpp (節 C: C-e resize_pages, set_canvas_size, crop_page, trim_page, reveal_all, rotate_pages)
void register_pagesize_ops(core::OpRegistry& registry);

// A frame for an op whose work is not written yet: refused with not_yet_ported ("<op> is not in the C++ build yet (PC3
// <group>)"), nothing changed.
core::OpFunction not_yet(const char* op, const char* group);

}  // namespace genko::render
