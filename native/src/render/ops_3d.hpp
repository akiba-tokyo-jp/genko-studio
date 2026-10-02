#pragma once

#include "core/command_bus.hpp"

// The 3D ops (Python's genko/threeops.py and the 3D branches of ops._apply_one): add_figure, pose_figure, add_head,
// add_hand, import_model, set_camera, set_light, render_prims, add_mannequin, pose_mannequin, add_prim3d, add_scene,
// edit_prim, delete_prim, trace_prims, ruler_from_3d and camera_from_ruler, with Python's arguments, results and
// messages.

namespace genko::render {

void register_3d_ops(core::OpRegistry& registry);

}  // namespace genko::render
