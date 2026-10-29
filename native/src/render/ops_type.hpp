#pragma once

#include "core/command_bus.hpp"

// PC5 節 G (文字; type@1, SPEC.md TEXT-02, core/type_text.hpp): the ops of Photoshop's type — the C++ build's own (the
// public list, `genko schema`, and the MCP tools stay as they are; MCP's apply does not take them yet: they are not among
// service::agent_op's, the CLI's apply and the window's ops do). G-0 registers all of them:
//   add_type_layer {page, text, box: [x, y, w, h] mm, wrap?: vertical | horizontal (vertical), style?, native_type?,
//                   balloon? (none), title?, new_id?} → {layer_id}: a type layer on top of the page (G-0; point text, the
//                   character and paragraph attributes, the styles, the anti-alias, size_pt and parent: G-a)
//   edit_type      {line: id | page + layer_id, text} → {} (the rubies' report as edit_line's): the text replaced whole, its
//                   rubies and ranges followed (G-0; replace, insert, point, box, anchor, transform, antialias, title: G-a)
//   type_info      {line | page + layer_id} → its text, graphemes, box, shape, native_type, paragraphs and runs with their
//                   attributes resolved (G-0; the set lines and outline: G-a, G-b). Changes nothing.
//   set_type, type_style, line_to_type_layer, type_layer_to_line, replace_fonts: registered; not_yet_ported until G-a, G-b,
//                   G-c and G-d write them.
// A type layer turned, scaled or bent (transform_layers) is G-a's; moved, it takes the move (render/layer_transform).

namespace genko::render {

void register_type_ops(core::OpRegistry& registry);

}  // namespace genko::render
