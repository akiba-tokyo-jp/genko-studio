#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "core/model.hpp"

// groups@1 (PC5 I-b, the user's decision 2 of 2026-10-28; SPEC.md AUTO-01「グループ」, docs/cpp-migration/schema-v4.md
// 「グループ（groups@1）」): a folder marked `group` is drawn as Photoshop's group — what is in it hidden with it, its
// opacity, its blend (pass_through 通過: what is in it laid on the picture below as if the folder were not there; any
// other: what is in it composited alone over nothing, then laid as one layer), its mask and its vector mask. A folder
// without the mark (every folder of a book before it) is an order of layers only, drawn as before. A book with a group
// names groups@1; a build that does not know it opens the book read-only.
//
// What is in a group: the layers whose nearest group up their folders is it (a folder that is not a group in between is
// passed through, as it is drawn), in the page's order, drawn where the group is in that order. A folder chain that
// comes back on itself is no group's.

namespace genko::core {

inline constexpr std::string_view kGroupsFeature = "groups@1";
// A group's blend: what is in it is drawn straight on the picture below (Photoshop's 通過).
inline constexpr std::string_view kPassThrough = "pass_through";
// How deep groups nest in what this build reads (a PSD's, a .pcraft's: deeper folders are not made, what they hold put in
// the deepest one made, said).
inline constexpr std::size_t kGroupDepthMax = 64;

// Is the layer a folder drawn as a group?
bool is_group(const Layer& layer);
// Does the page hold a group?
bool has_groups(const std::vector<Layer>& layers);
// Does the book need groups@1 (a group on a page or in the contents it writes)?
bool uses_groups(const Document& doc);
// The book's groups as they must be (core::Error("value") says what is not): `group` only on a folder, the blend
// pass_through only on a group. Pages not read yet are not looked at.
void validate_groups(const Document& doc);

// Does the group change how what it holds looks (an isolated blend, its opacity below 1, a mask or vector mask in use)?
// A pass-through group of none draws what it holds as a folder does.
bool group_changes_look(const Layer& group);

// How many groups that hold a picture while they are drawn (those that change the look, and a clipped one) are nested at
// most on the page (0: none).
std::size_t group_picture_depth(const std::vector<Layer>& layers);

// For each layer, the position of the group it is drawn in (its nearest group up its folders; none: drawn on the page).
std::vector<std::optional<std::size_t>> group_owners(const std::vector<Layer>& layers);
// Is the layer drawn — itself and every group it is in shown? (`owners`: group_owners of the layers, made once by a caller
// that asks of many)
bool shown_in_groups(const std::vector<Layer>& layers, std::size_t index);
bool shown_in_groups(const std::vector<Layer>& layers, std::size_t index, const std::vector<std::optional<std::size_t>>& owners);
// The positions of what a group holds (every layer drawn in it, in groups in it too), in the page's order.
std::vector<std::size_t> group_contents(const std::vector<Layer>& layers, std::size_t group);

// How the page's layers are drawn: each layer in its place, and each group as its start (where it is drawn: what is
// below it drawn), what it holds, its end. A page without groups: its layers in their order, one step each.
struct GroupStep {
    enum class Kind { Layer, Begin, End };
    std::size_t layer = 0;  // (the layer, or the group's folder)
    Kind kind = Kind::Layer;
    std::size_t end = 0;  // (Begin: the step of its End; End: the step of its Begin)
};
std::vector<GroupStep> group_steps(const std::vector<Layer>& layers);

}  // namespace genko::core
