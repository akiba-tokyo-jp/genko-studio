#pragma once

#include "core/command_bus.hpp"

// The ops that need pictures (genko_render_ops): each group registers here, and register_render_ops adds them all.
// They join core::OpRegistry::builtin() in every program that draws pages (tones.cpp adds the registrar: a program
// that draws tone layers has the drawing ops too), so the command line's `genko apply` has them.

namespace genko::render {

// add_tone, set_tone, delete_tone (ops_tones.cpp)
void register_tone_ops(core::OpRegistry& registry);
// add_effect, edit_effect, delete_effect, effect_to_layer (ops_effects.cpp)
void register_effect_ops(core::OpRegistry& registry);

// Every op of genko_render_ops.
void register_render_ops(core::OpRegistry& registry);

// core::OpRegistry::builtin() with the ops of genko_render_ops (for a program or test that wants them whether or not
// it draws pages).
const core::OpRegistry& registry_with_render_ops();

}  // namespace genko::render
