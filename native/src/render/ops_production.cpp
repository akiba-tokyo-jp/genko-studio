// PC5 J-0: the production's new ops as frames (render/ops_production.hpp), until their groups write them.

#include "render/ops_production.hpp"

#include <array>
#include <string>

#include "render/not_yet_ported.hpp"

namespace genko::render {

namespace {

constexpr std::array<std::string_view, 37> kNames{
    // J-a: レイヤーカンプ (layer-comps@1)
    "new_layer_comp", "update_layer_comp", "apply_layer_comp", "set_layer_comp", "duplicate_layer_comp", "delete_layer_comp",
    "clear_layer_comp_warnings",
    // J-b: スライス (slices@1)
    "add_slice", "set_slice", "delete_slices", "divide_slice", "slices_from_guides",
    // J-c: 注釈 (annotations@1)
    "add_annotation", "set_annotation", "delete_annotations",
    // J-d1: 計測スケール・カウント・ものさし・角度補正 (measure@1); J-d2: 目盛りマーカー
    "set_measurement_scale", "add_count", "remove_count", "move_count", "clear_count", "new_count_group", "delete_count_group",
    "set_count_group", "set_ruler_line", "straighten_layer", "place_scale_marker",
    // J-e2: ビデオレイヤー (video@1)
    "add_video_layer", "video_insert_blank", "video_duplicate", "video_delete", "video_restore", "video_restore_all", "video_alter",
    "set_video_layer", "replace_footage", "rasterize_video", "footage_to_layers"};

}  // namespace

std::span<const std::string_view> production_op_names() { return kNames; }

void register_production_ops(core::OpRegistry& registry) {
    for (const std::string_view name : kNames) {
        registry.add(std::string(name), [name](core::OpContext&) { throw NotYetPorted(std::string(name)); });
    }
}

}  // namespace genko::render
