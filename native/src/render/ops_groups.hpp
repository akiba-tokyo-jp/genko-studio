#pragma once

#include "core/command_bus.hpp"

// PC5 I-b (groups@1, SPEC.md AUTO-01「グループ」, core/groups.hpp): Layer › New › Frame from Layers — PhotoCraft v0.2.0
// crates/engine/src/frame_cmds.rs (1-60), MIT: native/third_party/photocraft/NOTICE.md:
//   frame_from_layers {page, ids, name?, id?} → {id, frame_mm: [x, y, w, h]}: the layers grouped (a group, 通過: as
//                     group_layers with group true), the group given a vector mask of the box of what it holds (its layers'
//                     boxes, render/layer_bounds; none: the paper) — PhotoCraft's is a pixel mask of the rectangle; a vector
//                     mask here, which does not depend on the resolution.

namespace genko::render {

void register_group_ops(core::OpRegistry& registry);

}  // namespace genko::render
