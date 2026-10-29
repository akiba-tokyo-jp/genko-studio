#pragma once

#include "core/command_bus.hpp"

// PC5 節 G (文字; type@1, SPEC.md TEXT-02, core/type_text.hpp): the ops of Photoshop's type — the C++ build's own (the
// public list, `genko schema`, and the MCP tools stay as they are; MCP's apply does not take them: they are not among
// service::agent_op's, the CLI's apply and the window's ops do). Lengths are mm, places in a text graphemes (TEXT-01).
//   add_type_layer {page, text, point: [x, y] | box: [x, y, w, h], wrap?: vertical | horizontal (vertical), style?,
//                   size_pt?, native_type?, char?, para?, char_style?, para_style?, antialias?, balloon? (none), title?,
//                   parent? (a folder), above? (a layer: just above it; none: on top), new_id?} → {layer_id}: a point text
//                   (ポイントテキスト: set unbroken from its anchor) or a box text (段落テキスト: broken at its box, what runs
//                   past it hidden); with a balloon, a box's text is set in its balloon as a line's (G-0, G-a)
//   edit_type      {line: id | page + layer_id; text? | replace? {start, end, text} | insert? {at, text}; point? (true or
//                   [x, y]: to point text — in place, or at that anchor), box? (true or [x, y, w, h]: to box text — round its
//                   letters, or that box), anchor?, wrap?, antialias?, transform? ([a, b, c, d, e, f] or null; a type
//                   layer's), title? (a type layer's)} → {} (the rubies' and ranges' report as edit_line's): ranges and
//                   rubies follow the text; a type layer named by its text is named by the new one (G-0, G-a)
//   type_info      {line | page + layer_id} → its text, graphemes, box, shape, anchor, anti-alias, matrix, native_type,
//                   paragraphs and runs with their attributes resolved, and as set (300 dpi): its size, outline and lines
//                   on the page, and whether a box text runs past its box. Changes nothing (a question).
//   line_to_type_layer {line, keep?, title?, above?, new_id?} → {layer_id}; type_layer_to_line {page, layer_id, keep?,
//                   new_id?} → {id}: a line and a type layer each other's (G-a)
//   set_type, type_style, replace_fonts: registered; not_yet_ported until G-b, G-c and G-d write them.
// A type layer turned, scaled or slanted (transform_layers, the page's turns and mirrors) keeps its text: the map goes
// into its matrix (native_type.transform); moved, its record moves (render/layer_transform, core/page_geometry).

namespace genko::render {

void register_type_ops(core::OpRegistry& registry);

}  // namespace genko::render
