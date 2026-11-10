#pragma once

#include "app/filter_menu.hpp"

// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → ぼかしギャラリー → フィールドぼかし…・虹彩絞りぼかし…・
// チルトシフト…・パスぼかし…・スピンぼかし… — Photoshop's work over the canvas, not a window: the pins on the page (a click adds
// one, a drag moves it, Delete takes the chosen one away; 虹彩絞り's and スピン's ellipses by their handles — its radii, the
// sharp core —, チルトシフト's band by its lines and its turn, パスぼかし's paths by their points), the ぼかしツール panel beside
// it with the chosen pin's settings (ぼかし・丸み・回転・速度・先細り …), the result shown on the page a moment after a change (a
// heavy one worked on a thread at the screen's resolution), Enter or OK sends it (filter_raster's kind; on a smart object a
// smart filter), Esc or キャンセル lets it go. The pins are shares of the base box (the selection's box, else the page; a
// smart object's: the page) and move with a smart object; パスぼかし's points are the page's mm.

namespace genko::app::blur_gallery {

// The work from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open(const FilterWindowCall& call);

// (tests) Whether a work is over the canvas now, and its settings as they are.
bool working();
core::Json settings();

}  // namespace genko::app::blur_gallery
