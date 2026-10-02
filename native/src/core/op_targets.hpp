#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// What an op works on, found as Python's ops.py finds it (_layer_by_id, _paint_target, _frame_or_fail, _ruler,
// _brush_kind), with the same messages (core::OpError, so the batch reports "ops[i] <op>: <message>").

namespace genko::core {

// Python's _layer_by_id: the position of the first layer with this id; OpError "no layer <id>".
std::size_t layer_index_by_id(const Page& page, std::string_view layer_id);

// Python's _paint_target: the layer op["layer_id"] names, else the first layer with the role op["layer"] ("ink" by
// default; the page gets one when it has none). OpError when it is locked or is not a pen, paint or tone layer;
// PyValueError for a role that is not one ("'x' is not a valid LayerRole").
Layer& paint_target(Page& page, const Json& op);

// Python's _frame_or_fail: the panel page._find(str(frame_id)) finds; OpError "no panel <frame_id>".
const Frame& frame_or_fail(const Page& page, const Json& frame_id);

// Python's _ruler: the position of the first ruler whose id == ruler_id; OpError "no ruler <ruler_id>".
std::size_t ruler_index(const Page& page, const Json& ruler_id);

// The built-in brush kinds (brushes.BRUSHES), in Python's order.
const std::vector<std::string>& builtin_brush_kinds();

// Python's _brush_kind(kind, episode): the brush a line is drawn with (an old name as its new one); OpError "kind must
// be one of …" for one that is neither built in nor defined in the book.
std::string brush_kind(const Json& kind, const Document& doc);

// Python's `episode.strict_gates` check of a drawing op that names a layer (ops._check_strict for the raster edit
// ops): printed layers change only after the name is approved.
void check_strict_raster_edit(const Document& doc, const Json& op, std::string_view name);

// The op is refused when what it would keep holds a number that is not finite (Python keeps it, and its json.dumps
// writes Infinity or NaN into project.json): OpError "<what> must be a finite number", `what` naming the key that holds
// it ("<key>.<inner key>" inside objects; an empty `what` names an object's own keys). Ops call it once they have done
// everything else, so every error Python gives comes first.
void require_finite(const Json& value, const std::string& what = {});

}  // namespace genko::core
