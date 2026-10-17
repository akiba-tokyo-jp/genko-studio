// PC2b (色の補正): the formulas of the colour corrections, filled into the table of correction kinds
// (core/adjust_kinds.hpp) — hue_saturation, vibrance, color_balance, black_white, photo_filter, channel_mixer,
// selective_color and desaturate. Not in this build yet: none filled (each is refused where it would be used,
// not_yet_ported naming it).

#include "core/adjust_kinds.hpp"

namespace genko::core {

HookTable color_hooks() { return {}; }

}  // namespace genko::core
