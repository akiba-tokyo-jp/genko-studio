#pragma once

#include <QString>

#include <string_view>

#include "app/filter_menu.hpp"

// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 描画 → 炎…・額縁…・木… — Photoshop's windows: 木's 34
// types and 額縁's 16 designs each with a small picture of it (made on a thread when the window opens), the settings
// (core::picture_kinds' table in Photoshop's words), 炎's path (the page's saved paths, the work path; none: upstream's arc
// across the lower part of the trim), 新しいレイヤーに描く, the result shown on the page a moment after a change (on a thread),
// OK sends render_picture (its frame the selection's box, else the trim), キャンセル nothing.

namespace genko::app::render_dialogs {

// The windows from フィルター's commands (app/filter_menu.cpp's filter_windows), by the item's kind: flame, picture_frame, tree.
void open(const FilterWindowCall& call);

// Photoshop's word for a tree type or a frame design (core::tree_type_names / frame_style_names, numbered from 1).
QString tree_words(int type);
QString frame_words(int design);

}  // namespace genko::app::render_dialogs
