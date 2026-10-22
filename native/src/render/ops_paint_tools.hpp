#pragma once

#include "core/command_bus.hpp"

// PC4 節 E (描く道具・プリセット; SPEC.md COLOR-09): the new ops of the drawing tools, this build's own (the public list,
// `genko schema`, and the MCP tools stay as they are; MCP's and the CLI's apply take them). Each group writes its ops'
// work in its file; until then an op is registered and refused with not_yet_ported naming it and its group, so a book
// or a script that names one is told it is not in this build yet rather than that it is unknown (as PC3's frames).

namespace genko::render {

// render/ops_paint_tools.cpp (E5: dodge, burn, sponge, blur_brush, sharpen_brush, smudge_brush, mixer_brush,
// background_eraser, color_replacement — each a stroke on a paint layer's tiles: render/tiled_pixels,
// retouch::apply_dab_stroke)
void register_paint_tool_ops(core::OpRegistry& registry);
// render/ops_patterns.cpp (E3: add_pattern, rename_pattern, delete_pattern — the book's patterns, patterns@1)
void register_pattern_ops(core::OpRegistry& registry);

// A frame for an op whose work is not written yet: refused with not_yet_ported ("<op> is not in the C++ build yet (PC4
// <group>)"), nothing changed.
core::OpFunction not_yet_pc4(const char* op, const char* group);

}  // namespace genko::render
