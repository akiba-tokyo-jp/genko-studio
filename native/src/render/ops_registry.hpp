#pragma once

#include "core/command_bus.hpp"

// The ops this build can apply, all together: core's (core::OpRegistry::builtin()) and the ones that need drawing
// (render's own, added as they are ported). The app's editing sessions apply ops with this registry, so an op that
// draws (fill, filters, selections) works from the GUI as soon as render has it.

namespace genko::render {

const core::OpRegistry& ops_registry();

}  // namespace genko::render
