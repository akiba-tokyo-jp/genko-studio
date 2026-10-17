// PC2b (色の補正): the forms of the colour corrections (hue_saturation — its ranges' band —, vibrance, color_balance,
// black_white, photo_filter — its presets —, channel_mixer — its output switch —, selective_color — its ranges —,
// desaturate) and their commands in レイヤー → 色調補正 (app/adjust_forms.hpp). Not in this build yet: no form and no
// command.

#include "app/adjust_forms.hpp"

namespace genko::app::adjust_forms {

std::vector<FormKind> color_forms() { return {}; }

void build_color_menu(QMenu*, const MenuContext&) {}

}  // namespace genko::app::adjust_forms
