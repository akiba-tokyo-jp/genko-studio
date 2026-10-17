// PC2a (階調の補正・3D LUT): the formulas of the tone corrections, filled into the table of correction kinds
// (core/adjust_kinds.hpp) — levels' and a tone curve's native (the upstream formula, the channel records in their
// space), brightness_contrast's and posterize's upstream formulas, a gradient map's reverse and dither, color_lookup and
// equalize. Not in this build yet: none filled (each is refused where it would be used, not_yet_ported naming it).

#include "core/adjust_kinds.hpp"

namespace genko::core {

HookTable tone_hooks() { return {}; }

}  // namespace genko::core
