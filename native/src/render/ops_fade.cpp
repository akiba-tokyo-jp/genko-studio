// PC2c (フェード): the op fade_layer — a layer's pixels and those before the last filter, correction or paint mixed by a
// blend and an opacity (design 3.4, PhotoCraft's edit_menu_cmds; render/ops_adjust.hpp). Not in this build yet:
// refused, not_yet_ported naming it.

#include "render/ops_adjust.hpp"

namespace genko::render::adjust_ops {

void fade_layer(core::OpContext&) { core::adjust_not_yet("the op fade_layer", "PC2c"); }

}  // namespace genko::render::adjust_ops
