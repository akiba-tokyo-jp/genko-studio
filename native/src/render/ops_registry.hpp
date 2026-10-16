#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/command_bus.hpp"

// All public operations through one registry: core, rulers, raster/selection, tones, effects and 3D.
// Core's builtin() has no drawing operations or richer-area resolver.
namespace genko::render {

const core::OpRegistry& ops_registry();
void register_render_ops(core::OpRegistry& registry);
void register_tone_ops(core::OpRegistry& registry);
void register_effect_ops(core::OpRegistry& registry);
// import_psd (render/import_psd.cpp)
void register_file_ops(core::OpRegistry& registry);
// channels@1: add_channel, duplicate_channel, delete_channel, rename_channel, move_channel, set_channel,
// merge_spot_channel, split_channels, merge_channels (render/ops_channels.cpp)
void register_channel_ops(core::OpRegistry& registry);
// set_color_mode and set_inks (color-mode@1, render/ops_ink.cpp)
void register_ink_ops(core::OpRegistry& registry);

// channels@1, color-model@1 (the user's decision 2026-10-16: as PhotoCraft flattens): the layer merge_visible's flatten
// keeps the page's pixels in (the first shown), and the page's pixel layers flattened into it (Genko's 画像をフラット化:
// its frames, lines and balloons kept as they are, and its name and draft layers; its hidden layers discarded), under
// a studio book's rules for a flatten too. The flattened layer's id, or none when no layer shows (nothing done); the
// hidden layers holding something it discarded added to `discarded`. render/ops_channels.cpp.
std::optional<std::string> first_shown_layer(const core::Page& page);
std::optional<std::string> flatten_page(core::OpContext& c, std::size_t at, std::int64_t* discarded = nullptr);
// What that flatten would do to the page: the layers holding something (pixels, lines, patches, a fill, a correction)
// it merges into one — those shown — and the hidden ones it discards (none when no layer shows). The window asks
// before a conversion that flattens with this.
struct FlattenCount {
    int merged = 0;
    int discarded = 0;
};
FlattenCount flatten_count(const core::Page& page);

}  // namespace genko::render
