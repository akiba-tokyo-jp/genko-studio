#pragma once

#include <span>
#include <string_view>

#include "core/command_bus.hpp"

// PC5 節 J (SPEC.md PROD-01「制作の補助」): the production's new ops, registered by J-0 as frames (each refused with
// not_yet_ported naming it) until its group writes it in its own file. The C++ build's own: not in `genko schema` or the
// MCP tool list (applied through `apply`; the MCP `apply_ops` takes the ones service::agent_op lets an agent use). No new op
// has `frame` in its name (Genko's ops' frame is a panel — コマ —, a timeline's frame is not): a video op names the
// timeline's frame `at`.
//
//   J-a (render/ops_layer_comps.cpp, layer-comps@1): new_layer_comp, update_layer_comp, apply_layer_comp, set_layer_comp,
//       duplicate_layer_comp, delete_layer_comp, clear_layer_comp_warnings — written (not frames any more)
//   J-b (render/ops_slices.cpp, slices@1): add_slice, set_slice, delete_slices, divide_slice, slices_from_guides
//   J-c (render/ops_annotations.cpp, annotations@1): add_annotation, set_annotation, delete_annotations
//   J-d1 (render/ops_measure.cpp, measure@1): set_measurement_scale, add_count, remove_count, move_count, clear_count,
//       new_count_group, delete_count_group, set_count_group, set_ruler_line, straighten_layer; J-d2: place_scale_marker
//   J-e2 (render/ops_video.cpp, video@1): add_video_layer, video_insert_blank, video_duplicate, video_delete,
//       video_restore, video_restore_all, video_alter, set_video_layer, replace_footage, rasterize_video, footage_to_layers
// (J-e1 widens set_animation — timeline@1 —, J-f filter_raster's plugin kinds: ops there are.)

namespace genko::render {

// The production's ops still frames (the groups' own not written yet, in the order above).
std::span<const std::string_view> production_op_names();
// Each of them as a frame (not_yet_ported naming it). A group's ops written take their frames' places: its register_* is
// called after this one in render/ops_registry.cpp (OpRegistry::add puts the later function in place).
void register_production_ops(core::OpRegistry& registry);

}  // namespace genko::render
