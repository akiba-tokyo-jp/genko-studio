#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// The pixels of a paint layer as they were, which an op names as its `from` (PC2c fade_layer's; PC3 B4 history_brush's,
// SPEC.md COLOR-06): an asset of the book ("sha256:…") of the layer's kind — an 8-bit layer's PNG, a precise layer's
// GKCR, an ink layer's GKIR — or a precise layer's tiles as project.json holds them (each tile read alone). The op reads
// only what is in the book, so it is the same wherever it is replayed. render/ops_fade.cpp.

namespace genko::render::adjust_ops {

// Their bytes (the layer's kind's: PNG, GKCR, GKIR — the tiles joined). OpError `refused()` when the tiles are not those of
// such a raster (another size, space or precision), and OpError when the book is not saved (`op` names the op), has no
// such asset, or the asset is past what one read takes.
std::string pixels_before(const core::Document& doc, const core::Page& page, const core::Layer& layer, const core::Json& from, std::string_view op,
                          const std::function<core::OpError()>& refused);

}  // namespace genko::render::adjust_ops
