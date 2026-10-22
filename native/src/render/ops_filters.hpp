#pragma once

#include "core/command_bus.hpp"
#include "render/ops_adjust.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」): フィルター's kinds as filter_raster's local filters (render/ops_filters.cpp,
// by their kernels: render/fx.hpp) and the frames of PC4's new ops.

namespace genko::render {

// render_picture (D4: 炎・額縁・木) and vanishing_point (D6: 消点), refused (not_yet_ported) until their groups write them.
void register_filter_ops(core::OpRegistry& registry);

}  // namespace genko::render
