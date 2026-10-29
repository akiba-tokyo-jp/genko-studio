// PC5 J-0: the production's new ops as frames (render/ops_production.hpp), until their groups write them.

#include "render/ops_production.hpp"

#include <array>
#include <string>

#include "render/not_yet_ported.hpp"

namespace genko::render {

namespace {

// (J-a's レイヤーカンプ — layer-comps@1 — are written: render/ops_layer_comps; J-c's 注釈 — annotations@1 —:
// render/ops_annotations; J-d1's 計測 — measure@1 —: render/ops_measure)
constexpr std::array<std::string_view, 17> kNames{
    // J-b: スライス (slices@1)
    "add_slice", "set_slice", "delete_slices", "divide_slice", "slices_from_guides",
    // J-d2: 目盛りマーカー
    "place_scale_marker",
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
