#pragma once

#include "core/command_bus.hpp"

// Every op by name: core's (core::OpRegistry::builtin()) and the ones that draw (render's), with the resolver of the
// richer areas. The command line, the app and the tests apply ops through this registry.

namespace genko::render {

const core::OpRegistry& ops_registry();

// Adds render's ops to a registry (each part of render registers its own).
void register_render_ops(core::OpRegistry& registry);

}  // namespace genko::render
