#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"

// pen-shapes@1 (PC3 C-b, the user's decision 7, SPEC.md COLOR-07「シェイプ」): the ops of a pen layer's shape parts —
// add_shape_part, edit_shape_part, combine_shape_parts, move_shape_parts (render/ops_shapes.cpp; render/ops_vector2.cpp
// registers them) — and what the other ops do to a part (erase, vector_edit; the selection's, the merges', rasterize's
// in their own files). PhotoCraft v0.2.0's engine/src/vector_cmds.rs (shape.*: 300-480, 1011-1050) in mm on a pen
// layer (MIT, native/third_party/photocraft/NOTICE.md). This build's own: `genko schema` and the MCP tools stay as they
// are, MCP's apply takes them.

namespace genko::render {

void register_shape_ops(core::OpRegistry& registry);

// add_shape_part's work (also text_to_path's "shape" and select.convertToShape's area): a part of `path` (and the live
// shape it was made from; `kind` names it for a new layer's title) with the op's fill, stroke, opacity, over, name and id
// placed on the pen layer the op names (layer_id) or on a new pen layer (new_layer: {name?, id?, after?}). Its report
// ({"layer", "part", "shape", "bounds"}).
core::Json place_shape(core::OpContext& c, std::size_t at, core::vector::Path path, std::optional<core::vector::LiveShape> live,
                       const std::string& kind, const core::Json& default_fill);

}  // namespace genko::render
