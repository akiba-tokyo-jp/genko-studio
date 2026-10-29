#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/slice_layout.hpp"

// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」): Web 用に保存's slices on a page — the スライスツール (slice.new, スライスを
// ガイドから作成 slice.fromGuides), the スライス選択ツール (slice.set = スライスオプション, slice.promote = ユーザー定義スライスに
// 変換, slice.divide = スライスを分割, slice.delete), レイヤー → 新規レイヤーベースのスライス (layer.newLayerBasedSlice) and 表示 →
// スライスを消去 (view.clearSlices) — PhotoCraft v0.2.0's crates/engine/src/slice_cmds.rs (MIT: native/third_party/
// photocraft/NOTICE.md) with the rects in mm (core/slice_layout: their pixels at the page's resolution, core::page_px_per_mm).
// スライスをロック is the window's (not the book's: view.lockSlices), as are 表示・非表示 → スライス and スナップ先 → スライス.
//
// A layer-based slice's rect is made again each time it is looked at — the layer's box (render/layer_bounds::of_layer: what
// it shows, its panels cutting it) grown by its style's effects and by the slice's outsets (the page's pixels) —, so it
// follows the layer; the rect it keeps is the last one an op saw. A layer gone takes its slices with it: after any op
// (render/ops_registry, follow_layers).
//
// The ops (the C++ build's own: not in `genko schema` or the MCP tool list, applied through `apply` — the window, `genko
// apply`, HTTP; not the MCP apply_ops: service::agent_op does not list them, as J-a's, J-c's and J-d1's). A slice is named by
// its id (`slice`) or its number as shown (`number`: an auto slice's made a user slice first, in the same step). The options:
// name, kind (image | noImage | table), url, target, message, alt, cell_text, cell_text_is_html, align [h, v] (0..4),
// background ("#rrggbb" | null | "none"), outsets [t, l, b, r] (page pixels; a layer-based slice's). A rect is rect_mm
// [x, y, w, h] or rect_px (the page's pixels).
//   add_slice {page, rect_mm | rect_px, …options} | {page, layer, outsets?, …options} → {slice, number, rect_mm}
//       (cut to the paper; a layer's: its slice following it — none for a fill or an adjustment layer, an empty one, or a
//       layer that has one)                                                    スライスツール, 新規レイヤーベースのスライス
//   set_slice {page, slice | number, promote?: true, rect_mm? | rect_px?, …options} → the slice as slices_info lists it
//       (a rect given or promote: a layer-based slice made a user one)          スライスオプション, ユーザー定義スライスに変換
//   delete_slices {page, slices: [id…] | "all"} → {deleted}                     スライスを削除, スライスを消去
//   divide_slice {page, slice | number, rows?: 1..1000 (down), columns?: 1..1000 (across)} → {slices, count}
//       (the first part keeps the slice's id and options; the others are fresh)  スライスを分割
//   slices_from_guides {page} → {slices}  (every slice replaced by the cells of the page's guides)   ガイドからスライスを作成
// and the question (OpRegistry::add_query: nothing changed, journalled or saved):
//   slices_info {page} → {page, dpi, width_px, height_px, group, base, slices: [{number, id, origin, name, kind, rect_px,
//       rect_mm, layer, url, target, message, alt, cell_text, cell_text_is_html, align, background, outsets}]}   slice.list

namespace genko::render::slices {

// The page's pixels (its resolution) and its paper in them.
core::SliceGrid page_grid(const core::Page& page);
// The base of the made names: the slices' group, else the page's file name (p001), file-name safe.
std::string base_name(const core::Page& page);
// A layer-based slice's rect now (mm): none when its layer is gone or shows nothing.
std::optional<std::array<double, 4>> layer_rect_mm(const core::Document& doc, const core::Page& page, const core::Slice& slice);
// Each user and layer-based slice's rect now (mm), in the list's order (a layer-based one showing nothing left out).
std::vector<std::pair<std::int64_t, std::array<double, 4>>> rects_now(const core::Document& doc, const core::Page& page);
// Every slice — the auto ones made around them — in `grid`'s pixels, numbered (`rects`: rects_now's, none: made here).
std::vector<core::ResolvedSlice> resolve(const core::Document& doc, const core::Page& page, const core::SliceGrid& grid,
                                         const std::vector<std::pair<std::int64_t, std::array<double, 4>>>* rects = nullptr);
// One resolved slice as slices_info lists it.
core::Json slice_json(const core::Page& page, const core::ResolvedSlice& r, const core::SliceGrid& grid);
// slices_info's answer (and `genko inspect --slices`).
core::Json listing(const core::Document& doc, const core::Page& page);
// After an op: the layer-based slices whose layer is gone taken away, on the pages it changed (`before`: the book's pages
// before it).
void follow_layers(core::Document& doc, const std::vector<core::PagePtr>& before);

}  // namespace genko::render::slices

namespace genko::render {

void register_slice_ops(core::OpRegistry& registry);

}  // namespace genko::render
