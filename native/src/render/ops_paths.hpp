#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/command_bus.hpp"

// vector@1 (PC3 C-a, SPEC.md COLOR-07「パス」「ベクトルマスク」): the ops of a page's paths and a layer's vector mask —
// set_path, delete_path, rename_path, make_work_path, text_to_path, stroke_path, set_vector_mask (render/ops_paths.cpp;
// render/ops_vector2.cpp registers them). This build's own: `genko schema` and the MCP tools stay as they are, MCP's
// apply takes them.

namespace genko::render {

void register_path_ops(core::OpRegistry& registry);

// fill_area's "blend" (Fill Path's mode; normal and none: fill_area's picture laid on the layer, as before): the area
// (resolved: {poly} | {mask}) painted in `rgb` (sRGB) at `opacity` into the paint layer's own pixels by that blend (8
// bits, precise, inks: render/image_ops' composite; an alpha-locked layer keeps its alpha). OpError for a layer that is
// not a paint layer, a blend that is not one.
void fill_area_blended(core::OpContext& c, std::size_t at, const std::string& layer_id, const core::Json& area, const std::string& mode,
                       const std::vector<std::int64_t>& rgb, double opacity);

}  // namespace genko::render
