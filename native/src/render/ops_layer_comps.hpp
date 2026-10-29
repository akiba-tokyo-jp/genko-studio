#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/layer_comps.hpp"
#include "core/model.hpp"

// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」): Photoshop's レイヤーカンプ on a page — PhotoCraft v0.2.0's
// crates/doc/src/comps.rs (capture_states, layer_position, next_comp_id, missing_layers) and engine/src/comps_cmds.rs
// (apply_comp and the layerComp.* commands), MIT: native/third_party/photocraft/NOTICE.md — the page's layers' visibility,
// position and appearance kept under a name, and put back.
//
// What a comp records of each layer of the page (core::CompLayerState): whether it is shown; where it is — the top left of
// its box (render/layer_bounds: what it shows drawn alone at the working 200 dpi, without its masks; mm) — none for a layer
// that shows nothing, a fill or a correction layer and a folder (a folder's place is its layers'); its appearance — its
// blend mode, opacity and layer style (the fill opacity in it). A comp's options choose which of the three applying puts
// back. Applying moves a layer by the difference between its box now and the one recorded, by whole working pixels (as the
// Move tool and Align do: through render/layer_transform's move_layer, each kind of layer as it moves — a locked or
// position-locked one too, and alone: not its folder's or linked layers, which have their own places in the comp); a layer
// the page no longer has is left out (Photoshop's warning triangle: `missing`). The lines of dialogue, the panels, the
// effect lines and 3D are not layers: no comp holds them.
//
// The ops (render/ops_layer_comps.cpp; the C++ build's own: not in `genko schema` or the MCP tool list, applied through
// `apply`). A comp is named by its id (an integer) or its name; none: the comp applied last, else the first.
//   new_layer_comp {page, name?, comment?, visibility?, position?, appearance?} → {comp}        新規レイヤーカンプ
//   update_layer_comp {page, comp? | "*", what?: all | visibility | position | appearance} → {updated}  レイヤーカンプを更新
//   apply_layer_comp {page, comp? | "next" | "previous" | "last_state"} → {comp, missing}       適用・次・前・最後のドキュメントの状態
//   set_layer_comp {page, comp?, name?, comment?, visibility?, position?, appearance?}          名前・コメント・オプション
//   duplicate_layer_comp {page, comp?} → {comp}                                                  レイヤーカンプを複製
//   delete_layer_comp {page, comp?}                                                              レイヤーカンプを削除
//   clear_layer_comp_warnings {page} → {cleared: [{comp, missing}]}                             レイヤーカンプの警告をクリア
// Applying a comp over the page's own state (none applied since) first keeps that state as 最後のドキュメントの状態
// (`last_state`), which "last_state" puts back whole.

namespace genko::render::layer_comps {

// What a comp records of each of the page's layers now (page order), all three.
std::vector<core::CompLayerState> capture(const core::Document& doc, const core::Page& page);
// A layer's comp position: the top left of its box (mm, to 4 decimals); none for a folder, a fill or a correction layer and
// one that shows nothing.
std::optional<std::array<double, 2>> position_of(const core::Document& doc, const core::Page& page, const core::Layer& layer);
// The layers a comp records that the page no longer has (in its order).
std::vector<std::string> missing(const core::Page& page, const std::vector<core::CompLayerState>& states);
// Those states put back on the page at `at`: what `visibility`, `position` and `appearance` choose. A state that names a
// layer the page does not have is left out. OpError for an appearance that cannot be put on its layer (a broken style).
void apply(core::Document& doc, std::size_t at, const std::vector<core::CompLayerState>& states, bool visibility, bool position, bool appearance);
// The comp an op names on the page ("comp": an id or a name; none: the one applied last, else the first). OpError for one
// the page does not have.
const core::LayerComp& named(const core::LayerComps& comps, const core::Json* comp);
// A comp's file name's part ("No/B" → "No_B", as PhotoCraft's sanitize: a path's and a file system's marks made "_").
std::string file_part(const std::string& name);
// The page's comps as `genko inspect --comps` and the panel list them: {"comps": [{id, name, comment, visibility, position,
// appearance, layers, missing: [ids]}], "last_applied", "last_state": bool}.
core::Json listing(const core::Page& page);

}  // namespace genko::render::layer_comps

namespace genko::render {

void register_layer_comp_ops(core::OpRegistry& registry);

}  // namespace genko::render
