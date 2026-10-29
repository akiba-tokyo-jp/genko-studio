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
//   set_type (G-b, render/ops_type_set.cpp): below (type_ops::set_type).
//   type_style, replace_fonts: registered; not_yet_ported until G-c and G-d write them.
// A type layer turned, scaled or slanted (transform_layers, the page's turns and mirrors) keeps its text: the map goes
// into its matrix (native_type.transform); moved, its record moves (render/layer_transform, core/page_geometry).

namespace genko::render {

void register_type_ops(core::OpRegistry& registry);

// PC5 G-b: what render/ops_type.cpp's ops share with set_type (render/ops_type_set.cpp).
namespace type_ops {

// The text an op names — a line ("line": its line; edit: its page's lock is the bus's) or a type layer ("page" and
// "layer_id": its page and place; edit: not locked, its position's lock kept by what moves it) — or core::OpError.
struct TypeTarget {
    core::StoryLine* line = nullptr;
    std::size_t page = 0;
    std::size_t layer = 0;
};
TypeTarget target(core::Document& doc, const core::Json& op, bool edit);
// The record's native_type in its normal form for its text (none when it holds nothing), or core::OpError.
void set_native_type(core::StoryLine& record, const core::Json& raw);
// A point text's box kept round its letters as set now (nothing for other records).
void fit_point_text(core::StoryLine& record, const core::Document& doc);
// A type layer's text, or a line's set by the OpenType typesetting: at most this many graphemes (review P2-5) —
// bounded_text's core::OpError past it.
inline constexpr std::size_t kMaxTypeText = 100000;
void bounded_text(const std::string& text);
// A record without a shape (a line in its balloon) set as it is at 300 dpi — fit_point_text sets a point or box text —, or
// core::OpError where its picture would be too large (review P1-1).
void measure(const core::StoryLine& record, const core::Document& doc);
// set_type {line | page + layer_id, range? [start, end] (graphemes), char? {…}, para? {…}, any character or paragraph
// attribute by its name (core::type_text's tables), size_pt?} → {}: the attributes given (null: the record's own taken
// away) on the whole text — every range's own of them taken away, as Photoshop sets what is chosen — or on the range (its
// characters; the paragraphs it touches); the record is then set by the OpenType typesetting (native_type.shaping).
void set_type(core::OpContext& c);

}  // namespace type_ops

}  // namespace genko::render
