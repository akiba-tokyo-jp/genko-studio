#pragma once

#include "core/command_bus.hpp"

// PC5 J-e2 (video@1, SPEC.md PROD-01「ビデオレイヤー」, render/ops_video.cpp): the video layers' ops — add_video_layer,
// video_insert_blank, video_duplicate, video_delete, video_restore, video_restore_all, video_alter, set_video_layer,
// replace_footage, rasterize_video, footage_to_layers. Their footage is the book's pictures (render/footage read_footage
// puts them there); the timeline's frame is `at`. The C++ build's own: not in `genko schema` or the MCP tool list, not an
// agent's (service::agent_op).

namespace genko::render {

void register_video_ops(core::OpRegistry& registry);

}  // namespace genko::render
