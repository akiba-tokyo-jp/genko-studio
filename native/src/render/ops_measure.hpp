#pragma once

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): イメージ → 解析's 計測スケールを設定, the カウントツール's groups and marks,
// the ものさしツール's line and レイヤーの角度補正 on a page — PhotoCraft v0.2.0's crates/engine/src/analysis_cmds.rs
// (image.analysis.setMeasurementScale, count.*, image.analysis.rulerTool, image.analysis.straightenLayer,
// image.analysis.info; MIT: native/third_party/photocraft/NOTICE.md) with the places in mm and the lengths in the page's
// pixels (its resolution: core::page_px_per_mm).
//
// The ops (the C++ build's own: not in `genko schema` or the MCP tool list, applied through `apply` — the window,
// `genko apply`, HTTP; not the MCP apply_ops: service::agent_op does not list them, as PC3's and PC4's own ops and J-a's,
// J-c's). Each change is one step of the history (Photoshop keeps the ruler out of it; Genko's ops are all undone alike):
//   set_measurement_scale {page, preset?: "custom" | "default" | "paper_mm", pixel_length?, logical_length?, units?}
//       → {scale}  (custom: the lengths and units given over the page's; default: 1 pixels = 1 pixels — none kept;
//                   paper_mm: the page's pixels a mm = 1 mm)                                    計測スケールを設定
//   add_count {page, at_mm: [x, y], group?: index} → {number, group, count}  (the active group; a page without groups
//       gets カウントグループ 1)                                                                  カウント
//   remove_count {page, index, group?} | {page, at_mm, radius_mm?, group?} → {count}  (the nearest visible mark within
//       radius_mm — 6 of the page's pixels by default)                                         カウントを削除
//   move_count {page, index, group?, to: [x, y]} | {page, at_mm, radius_mm?, group?, to} → {count}   カウントを移動
//   clear_count {page, group?: index | "all"} → {count}  (the active group by default)          カウントをクリア
//   new_count_group {page, name?, color?: [r, g, b] | "#rrggbb"} → {group, count}  (made active)  新規カウントグループ
//   delete_count_group {page, group?} → {count}                                                カウントグループを削除
//   set_count_group {page, group?, name?, color?, marker_size?: 1..10, label_size?: 8..72, visible?, active?} → {count}
//       (a group named alone is made active, as PhotoCraft's)                                  カウントグループのオプション
//   set_ruler_line {page, start: [x, y], end: [x, y], protractor?: [x, y] | null} | {page, protractor: [x, y] | null}
//       | {page, clear: true} → {ruler, info}                                                     ものさしツール
//   straighten_layer {page, ids: [layer id…], interp?} | {page, crop: true, interp?, trim?, bleed_mm?, margins?} (one of the two
//       given: refused without)
//       → {angle, cropped, …}  (the line laid level or upright, whichever is nearer — core::straighten_turn —: the layers
//       turned about the page's middle by transform_layers, or the page turned and cut to the largest upright rectangle
//       inside it by crop_page — one op, the same functions as theirs on the three ways —; the line cleared after, as
//       Photoshop's)                                                                           レイヤーの角度補正
// and the question (OpRegistry::add_query: nothing changed, journalled or saved):
//   measure_info {page} → {page, dpi, scale, ruler: {start, end, protractor, info} | null, count, notes}   image.analysis.info

namespace genko::render::measure {

// The page's measuring as measure_info and `genko inspect --measure` answer it.
core::Json listing(const core::Page& page);

}  // namespace genko::render::measure

namespace genko::render {

void register_measure_ops(core::OpRegistry& registry);

}  // namespace genko::render
