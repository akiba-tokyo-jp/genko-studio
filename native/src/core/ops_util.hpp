#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pyops.hpp"

// Helpers shared by the op implementations of every module (Python's small functions in genko/ops.py): what an op
// works on, found as Python finds it, with Python's messages (OpError: the batch reports "ops[i] <op>: <message>").

namespace genko::core {

// LayerRole(name): PyValueError "'xyz' is not a valid LayerRole".
LayerRole role_from(const Json& value);

// ops._layer_by_id: the first layer with this id. OpError "no layer <id>".
std::size_t layer_by_id(const Page& page, std::string_view layer_id);

// Page._layer(role): the first layer with the role, made (with a new id) and appended when there is none.
std::size_t layer_for_role(Page& page, LayerRole role);

// ops._paint_target: the layer op["layer_id"] names, else the first layer with the role op["layer"] ("ink" by
// default; the page gets one when it has none). OpError when it is locked or is not a pen, paint or tone layer;
// PyValueError for a role that is not one ("'x' is not a valid LayerRole").
Layer& paint_target(Page& page, const Json& op);

// ops._frame_or_fail: the panel page._find(str(frame_id)) finds; OpError "no panel <frame_id>".
const Frame& frame_or_fail(const Page& page, const Json& frame_id);

// ops._ruler: the position of the first ruler whose id == ruler_id; OpError "no ruler <ruler_id>".
std::size_t ruler_index(const Page& page, const Json& ruler_id);

// ops._screen_spec: レイヤーのトーン化 ({pattern, lpi, angle, black, white, shape?, offset_mm?}); OpError for what it
// does not take.
Json screen_spec(const Json& raw);

// The op is refused when what it would keep holds a number that is not finite (Python keeps it, and its json.dumps
// writes Infinity or NaN into project.json): OpError "<what> must be a finite number", `what` naming the key that holds
// it ("<key>.<inner key>" inside objects; an empty `what` names an object's own keys). Ops call it once they have done
// everything else, so every error Python gives comes first.
void require_finite(const Json& value, const std::string& what = {});

// The role a stroke op's "layer" names: ink, name, or any other role by its name (LayerRole(name)).
LayerRole stroke_role(const std::string& layer_name);

// ops._gated: strict gates or a studio (agents' books) enforce the name → art → finish order.
bool gated(const Document& doc);

// ops._rgb3: three ints 0..255. OpError "<what> is [r, g, b]".
std::vector<std::int64_t> rgb3(const Json& value, std::string_view what);

// ops._blend_mode: OpError "unknown blend mode <mode>".
std::string blend_mode(const Json& value);

// ops._gradient_extras(g): 多色 (stops), 楕円 (ratio), 繰り返し (repeat) of a gradient, checked (core/ops_layers.cpp; also
// gradient_fill's). OpError with Python's words.
Json gradient_extras(const Json& g);

// The JSON of a list of ints.
Json ints_json(const std::vector<std::int64_t>& values);

// models.stroke_to_dict: {"id", "points", "pressure", "width_mm", "kind"} and "rgb", "color_rgb", "opacity" when set.
Json stroke_to_dict(const Stroke& stroke);

// io._layer_to_dict(layer) (with its strokes as models.stroke_to_dict): what studio.orphans keeps of a layer.
Json layer_to_dict(const Layer& layer);

// The panel ids of a page (pre-order, with duplicates as they are).
std::vector<std::string> frame_ids(const Page& page);

// Register the ops of M2-O1 (frames, pages, strokes, layers, brush).
void register_frame_ops(OpRegistry& registry);
void register_page_ops(OpRegistry& registry);
void register_stroke_ops(OpRegistry& registry);
void register_layer_ops(OpRegistry& registry);
void register_color_ops(OpRegistry& registry);
// Register the ops of M3-A that draw nothing (move_layers, group_layers, set_paper, set_timelapse, store_area,
// forget_area: core/ops_arrange.cpp). The ones that draw are render's (render/ops_registry.hpp).
void register_arrange_ops(OpRegistry& registry);

// Image.open(BytesIO(bytes)).verify() for a picture an op is given as base64 (a line's style.picture and
// style.fill_png): render's (render/raster_ops.hpp). Throws PyValueError with Pillow's words for what Python raises as
// OSError, ValueError or IndexError there, and PyUncaught for the others.
using PictureCheck = std::function<void(const std::string& bytes)>;

// Register the line ops of M4 (add_line, edit_line, move_line, delete_line, reorder_lines, cut_balloon,
// set_balloon_path, replace_text: core/ops_lines.cpp). Without `check` a picture in a line's style is refused with
// not_yet_ported (as an area that needs resolving is): render::ops_registry registers them again with its check.
void register_line_ops(OpRegistry& registry, PictureCheck check = {});

}  // namespace genko::core
