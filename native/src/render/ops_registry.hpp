#pragma once

#include "core/command_bus.hpp"

// The ops of this build in one registry (the book ops of core and the drawing ops of render), the one the command line
// and every other entry point apply ops with.

namespace genko::render {

// core::OpRegistry::builtin() and the ops of render.
const core::OpRegistry& ops_registry();

// The ops render adds (each module registers its own here).
void register_render_ops(core::OpRegistry& registry);

}  // namespace genko::render
