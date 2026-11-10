#pragma once

#include "app/filter_menu.hpp"

// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 描画 → 照明効果… — on the left the layer's picture (the
// selection's box, else the page) with the result laid on it a moment after a change (worked on a thread at the window's
// resolution: render/fx_render, the op's own kernel) and the lights over it — a spot light's place and where it points, a
// point light's place and its reach, an infinite light's direction — moved by their handles; on the right the lights (スポ
// ットライト・点光源・無限光: added, chosen, deleted) and the chosen one's 種類・カラー・強度・ホットスポット・円錐・高さ・半径・角度・
// 仰角, and the surface's 光沢・メタリック・露光量・環境光 and its テクスチャ (none, a colour of the layer, its transparency or
// luminance, an alpha channel of the page) with its 高さ and 白い部分を高く. OK sends the settings (filter_raster's
// render_lighting_effects; on a smart object a smart filter — its texture not a channel), キャンセル nothing.

namespace genko::app::lighting_window {

// The window from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open(const FilterWindowCall& call);

}  // namespace genko::app::lighting_window
