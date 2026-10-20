#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "render/layer_bounds.hpp"

// PC3 C-d (SPEC.md COLOR-07「移動・整列」): Layer › Align and Distribute, Arrange and Link Layers as ops — PhotoCraft
// v0.2.0 crates/engine/src/layer_multi_cmds.rs (196-420: links, moves, align, distribute; 485-505: reverse) and
// commands.rs (516-523, 1028: arrange), MIT: native/third_party/photocraft/NOTICE.md.
//
// What is aligned or distributed is the layers named (a folder with its layers; a layer named inside a folder that is
// named too goes with the folder) and the lines of dialogue named — the user's 調整役の判断: layers and balloons by one op
// —, each by its box (render/layer_bounds). A layer moves with its folder's layers and its linked layers by whole
// working pixels (200 dpi), through render/layer_transform (C-c's move_layer: its pixels exactly, its lines, its parts,
// its linked masks); a line moves by the exact amount. Arrange works among siblings (the layers of one folder, or of
// none), a folder with its layers.

namespace genko::render::layer_align {

using Box = layer_bounds::Box;

enum class Edge { Top, VMiddle, Bottom, Left, HMiddle, Right };
// top | vcenter | bottom | left | hcenter | right
std::optional<Edge> edge_from(std::string_view name);

// One thing aligned or distributed: its box (mm) and whether it moves by whole working pixels (a layer) or exactly (a
// line).
struct Item {
    Box box{0, 0, 0, 0};
    bool whole = true;
};

// How far each item moves (dx, dy in mm) to put its edge on the target's (PhotoCraft's: a half pixel goes up or left), a
// layer by whole pixels of the page's working grid (layer_bounds::grid: its pixels a mm across and down).
using Grid = std::array<double, 2>;
std::vector<std::array<double, 2>> align_moves(const std::vector<Item>& items, Edge edge, const Box& target,
                                               const Grid& grid = {layer_bounds::kPx, layer_bounds::kPx});
// How far each item moves to spread them out (three or more): by an edge or a middle (the outermost two stay, the rest
// at equal steps between) or "horizontal" / "vertical" (equal gaps between them).
std::vector<std::array<double, 2>> distribute_moves(const std::vector<Item>& items, std::string_view by,
                                                    const Grid& grid = {layer_bounds::kPx, layer_bounds::kPx});

// Arrange among siblings: the layers (page order) moved to "front", "forward", "backward", "back", or "reverse" (their
// places swapped end for end); a folder moves with its layers. False when nothing moves (the layers stay as they were).
bool arrange(std::vector<core::Layer>& layers, const std::vector<std::string>& ids, std::string_view to);
// The ids without those inside a folder also named (the folder carries them), page order.
std::vector<std::string> top_level(const core::Page& page, const std::vector<std::string>& ids);
// The layers linked to any of these (transform@1: the same link), these first.
std::vector<std::string> with_links(const core::Page& page, const std::vector<std::string>& ids);
// Link Layers' toggle: would the layers be unlinked (they are one link group already)?
bool unlinks(const core::Page& page, const std::vector<std::string>& ids);

// The ops: align_layers ({page, ids, lines?, edge, to?, area?, frame_id?}), distribute_layers ({page, ids, lines?, by}),
// arrange_layers ({page, ids, to}), link_layers ({page, ids, link?}).
void align_layers(core::OpContext& c);
void distribute_layers(core::OpContext& c);
void arrange_layers(core::OpContext& c);
void link_layers(core::OpContext& c);

}  // namespace genko::render::layer_align
