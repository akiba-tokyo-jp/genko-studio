// PC2a (階調の補正・3D LUT・補正のパネル): the forms of the tone corrections (levels, curve, brightness_contrast,
// exposure, posterize, threshold, invert, gradient_map, color_lookup, equalize, the auto ones) and their commands in
// レイヤー → 色調補正 (app/adjust_forms.hpp). Not in this build yet: no form and no command.

#include "app/adjust_forms.hpp"

namespace genko::app::adjust_forms {

std::vector<FormKind> tone_forms() { return {}; }

void build_tone_menu(QMenu*, const MenuContext&) {}

}  // namespace genko::app::adjust_forms
