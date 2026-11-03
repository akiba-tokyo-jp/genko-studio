// PC5 J-0: the production's new ops as frames (render/ops_production.hpp), until their groups write them.

#include "render/ops_production.hpp"

#include <array>
#include <string>

#include "render/not_yet_ported.hpp"

namespace genko::render {

namespace {

// (J-a's レイヤーカンプ — layer-comps@1 — are written: render/ops_layer_comps; J-c's 注釈 — annotations@1 —:
// render/ops_annotations; J-d1's and J-d2's 計測 — measure@1 —: render/ops_measure; J-b's スライス — slices@1 —: render/ops_slices;
// J-e2's ビデオレイヤー — video@1 —: render/ops_video. None is a frame any more.)
constexpr std::array<std::string_view, 0> kNames{};

}  // namespace

std::span<const std::string_view> production_op_names() { return kNames; }

void register_production_ops(core::OpRegistry& registry) {
    for (const std::string_view name : kNames) {
        registry.add(std::string(name), [name](core::OpContext&) { throw NotYetPorted(std::string(name)); });
    }
}

}  // namespace genko::render
