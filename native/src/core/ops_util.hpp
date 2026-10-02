#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pyops.hpp"

// Helpers shared by the op implementations (Python's small functions in genko/ops.py).

namespace genko::core {

// LayerRole(name): PyValueError "'xyz' is not a valid LayerRole".
LayerRole role_from(const Json& value);

// ops._layer_by_id: the first layer with this id. OpError "no layer <id>".
std::size_t layer_by_id(const Page& page, std::string_view layer_id);

// Page._layer(role): the first layer with the role, made (with a new id) and appended when there is none.
std::size_t layer_for_role(Page& page, LayerRole role);

// The role a stroke op's "layer" names: ink, name, or any other role by its name (LayerRole(name)).
LayerRole stroke_role(const std::string& layer_name);

// ops._gated: strict gates or a studio (agents' books) enforce the name → art → finish order.
bool gated(const Document& doc);

// ops._rgb3: three ints 0..255. OpError "<what> is [r, g, b]".
std::vector<std::int64_t> rgb3(const Json& value, std::string_view what);

// ops._blend_mode: OpError "unknown blend mode <mode>".
std::string blend_mode(const Json& value);

// The JSON of a list of ints.
Json ints_json(const std::vector<std::int64_t>& values);

// io._layer_to_dict(layer) (with its strokes as models.stroke_to_dict): what studio.orphans keeps of a layer.
Json layer_to_dict(const Layer& layer);

// The panel ids of a page (pre-order, with duplicates as they are).
std::vector<std::string> frame_ids(const Page& page);

// Register the ops of M2-O1 (frames, pages, strokes, layers, brush).
void register_frame_ops(OpRegistry& registry);
void register_page_ops(OpRegistry& registry);
void register_stroke_ops(OpRegistry& registry);
void register_layer_ops(OpRegistry& registry);

}  // namespace genko::core
