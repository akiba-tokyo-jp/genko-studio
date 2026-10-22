#pragma once

#include "app/filter_menu.hpp"

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 消点… — Photoshop's window: the page as it shows, the
// planes drawn over it with their grids, and its tools — 面作成ツール (four clicks make a plane; the first one's, or one more of
// its own), 面修正ツール (a plane chosen, its corners dragged; 面を継ぎ足す tears one off its edge at an angle — as Photoshop's
// Ctrl-drag —; 面を削除 takes the last one off), スタンプツール (Alt-click sets where it copies from, a drag paints a stroke:
// 直径, 硬さ, 不透明度) and 貼り付け (a layer of the page laid on the chosen plane; dragged to move it). What the pastes and
// strokes make is shown on the page's picture (render/vanishing on it). OK sends the op vanishing_point (the planes kept on
// the page: vanishing-point@1), on the layer drawn on or a new layer (新しいレイヤーに描く); キャンセル nothing. The planes the
// page keeps are there when it opens.

namespace genko::app::vanishing_window {

void open(const FilterWindowCall& call);

}  // namespace genko::app::vanishing_window
