#pragma once

#include "core/command_bus.hpp"

// All public operations through one registry: core, rulers, raster/selection, tones, effects and 3D.
// Core's builtin() has no drawing operations or richer-area resolver.
namespace genko::render {

const core::OpRegistry& ops_registry();
void register_render_ops(core::OpRegistry& registry);
void register_tone_ops(core::OpRegistry& registry);
void register_effect_ops(core::OpRegistry& registry);
// import_psd (render/import_psd.cpp)
void register_file_ops(core::OpRegistry& registry);

}  // namespace genko::render
