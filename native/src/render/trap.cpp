// PC2c (トラッピング): the op trap on a CMYK page's inks (design 3.4, PhotoCraft's algo/trap.rs; render/ops_adjust.hpp).
// Not in this build yet: refused, not_yet_ported naming it.

#include "render/ops_adjust.hpp"

namespace genko::render::adjust_ops {

void trap(core::OpContext&) { core::adjust_not_yet("the op trap", "PC2c"); }

}  // namespace genko::render::adjust_ops
