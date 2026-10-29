#pragma once

#include "core/command_bus.hpp"

// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」, core/variables.hpp): イメージ → 変数 ▸ and データセットを適用… —
// PhotoCraft v0.2.0 crates/engine/src/variables_cmds.rs (MIT: native/third_party/photocraft/NOTICE.md). The book's ops (one
// step each in the history, never page by page — for_pages and a smart object's contents refuse them):
//   set_variables {defs: [{name, type: visibility|text|pixels, page: n, layer: id | line: id (text),
//                          method?: fit|fill|as_is|conform, align?: top_left…bottom_right, clip?: bool,
//                          box_mm?: [x0, y0, x1, y1]}]}
//                 → the listing (variables_listing). The book's defs, all of them (its data sets kept): visibility on any
//                   layer, text on a type layer or a line, pixels on a paint layer or a smart object. A pixels def's box is
//                   the one given, else the def of that name's when it names the same layer, else the layer's content's box
//                   now (none: the paper).
//   set_data_sets {data_sets: [{name, values: [{variable, value: bool | str | {data: base64, name?} | {asset, name?}}]}],
//                  append?: bool}
//                 → the listing. The book's data sets (append: after its own). Each value for a def of the book, of its
//                   type; a picture given as its bytes (a picture this build reads, 256 MiB at most) or as the book's asset —
//                   kept by the book (never a path); all the pictures together 4 GiB at most.
//   apply_data_set {name | index}
//                 → {applied, data_set: its index, skipped: [{variable, reason}]}. Its values in their order: a layer shown or hidden
//                   (set_layer), a line's or a type layer's text (edit_line, edit_type: its ranges followed — type@1), a
//                   smart object's contents replaced (smart_replace: its transform kept), a paint layer's pixels replaced by
//                   the picture put in the def's box by its method and align (clip: cut to the box) on the layer's own grid at
//                   its kind and depth (an 8-bit page's 8 bits, a precise page's floats, an ink page's inks). Each change as
//                   that op makes it, under the same rules (page locks, strict_gates, locked layers). Skipped (said, never
//                   refused): a def whose layer or line is gone or no longer of its kind, a value of another type, a value
//                   for no def. The book remembers it as the data set applied last (active).
// Not in MCP's agent_op (the coordinator's decision: PC5's new ops stay out).

namespace genko::render {

void register_variable_ops(core::OpRegistry& registry);

// The book's variables as genko inspect --variables and MCP's inspect target=variables show them: {"defs": [{…the def…,
// "page": its page's number (null: gone), "target": the layer's or line's name (null: gone), "missing": bool}],
// "data_sets": [{"name", "values"}], "active": index | null}.
core::Json variables_listing(const core::Document& doc);

}  // namespace genko::render
