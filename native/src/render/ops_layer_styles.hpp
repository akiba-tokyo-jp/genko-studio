#pragma once

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」): the ops of a layer's style and the book's global light
// (render/ops_layer_styles.cpp). The C++ build's own: not in `genko schema` or the MCP tool list (applied through `apply`).
//
//   set_layer_style {page, id | ids, style: {…} | null, add?: bool, blend?: mode}   スタイルを当てる・貼る・消す
//   add_layer_effect {page, id | ids, effect: {kind, …}, add?: bool}                 レイヤースタイル → ドロップシャドウ… など
//   set_global_light {angle?, altitude?}                                             包括光源…
//   scale_layer_style {page, id | ids, scale: 1..1000}                               効果を拡大・縮小…
//   show_layer_effects {page, show: bool, ids?}                                      すべての効果を隠す・表示
//   layer_style_to_layers {page, id}                                                 レイヤーを作成
//
// A style's patterns may be given as add_pattern takes a picture ({"png" | "gkcr" | "gkir" | "asset", …} with their
// placement): put into the book first. A style changed leaves what the layer kept of a PSD's effects (psd-keep@1).

namespace genko::render {

void register_layer_style_ops(core::OpRegistry& registry);

// The style ({"effects", …}) with each pattern given as a picture put into the book (fill_pattern) and named by its id.
core::Json style_with_patterns(core::Document& doc, const core::Json& style);
// A layer's style changed: the PSD's effects it kept (lfx2, lfxs, lmfx, lrFX) are not its own any more.
void forget_psd_effects(core::Layer& layer);

}  // namespace genko::render
