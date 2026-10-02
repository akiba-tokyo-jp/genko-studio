#pragma once

#include "core/command_bus.hpp"

// The ops of this build in one registry: the ops of core (core::register_core_ops: the book, frames, pages, pen lines,
// layers, rulers) and the ops that draw (render: tones, effect lines, 3D). The command line and every other entry point
// apply ops with it; core::OpRegistry::builtin() has core's ops only.

namespace genko::render {

// core's ops and render's.
const core::OpRegistry& ops_registry();

// The ops render adds, each module's in turn.
void register_render_ops(core::OpRegistry& registry);

// add_tone, set_tone, delete_tone (ops_tones.cpp)
void register_tone_ops(core::OpRegistry& registry);
// add_effect, edit_effect, delete_effect, effect_to_layer (ops_effects.cpp)
void register_effect_ops(core::OpRegistry& registry);
// (the 3D ops: render/ops_3d.hpp)

}  // namespace genko::render
