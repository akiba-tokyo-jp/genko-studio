#pragma once

#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// 新規ガイドレイアウトを作成 (PC5 H-b, view.newGuideLayout; SPEC.md VIEW-01「ガイド」; PhotoCraft v0.2.0's file_cmds.rs
// new_guide_layout, MIT): the guides an add_guide_layout op would put on the page — its columns ({count, width_mm?,
// gutter_mm?}), rows ({count, height_mm?, gutter_mm?}), margin_mm (one number or [top, left, bottom, right]),
// center_columns, clear and from ("paper" the default — Photoshop's canvas —, "trim" the finished size, "frame" the 基本枠):
// the upright guides' x and the guides' across y (mm, rounded to 0.001), each once and none where the page has one already
// (unless clear). The op (core/ops_rulers.cpp) and the dialog's preview (app/guide_dialogs.cpp) both ask it. Throws OpError
// with the op's words for what it refuses (nothing to lay out, a value of the wrong kind).

namespace genko::core {

struct GuideLayout {
    std::vector<double> down;    // the upright guides ("v"): their x
    std::vector<double> across;  // the guides across ("h"): their y
    bool clear = false;          // the page's guides taken away first
};

GuideLayout guide_layout(const Page& page, const Json& op);

}  // namespace genko::core
