#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model.hpp"

// PC5 I-a (SPEC.md AUTO-01「レイヤーの基本」, docs/cpp-migration/schema-v4.md「ロックとチャンネル」): what the rest of Photoshop's
// layer basics keep on a layer, and what the layer ops of I-a share.
//
// layer-locks@1: a layer's `lock_all` (すべてをロック: nothing drawn on it, moved, its look changed or deleted — its other
// locks as they are, shown under it) and `lock_nest` (アートボードの内外への自動ネストを防ぐ: kept for PSD's lspf and a
// .pcraft's locks; Genko's artboards are pages, so it does nothing here). blend-channels@1: a layer's `blend_channels`
// (混合オプション → チャンネル: one flag for each colour channel of its page, R G B / C M Y K / L a b / grey; a channel left
// out keeps what is under the layer). A book names each while a layer has it; a build that does not know them opens the
// book read-only.

namespace genko::core {

inline constexpr std::string_view kLayerLocksFeature = "layer-locks@1";
inline constexpr std::string_view kBlendChannelsFeature = "blend-channels@1";

// The colour channels blend_channels names on the page: RGB 3 (an 8-bit page, one of precise colour), CMYK 4, Lab 3,
// grey 1; 0 on a page whose mode has none to leave out here (bitmap, indexed, duotone, multichannel).
unsigned blend_channel_count(const Page& page);
// Does the layer leave a colour channel out?
bool leaves_channels_out(const Layer& layer);
// The pages a book writes — its own and its smart objects' contents — with a layer of the kind.
bool uses_layer_locks(const Document& doc);
bool uses_blend_channels(const Document& doc);
// The book's locks and channels as they must be (core::Error("value") says what is not): a layer's blend_channels one
// flag for each colour channel of its page, on a page whose mode has them, not all on (that is no blend_channels).
void validate_layer_basics(const Document& doc);
// After an op that changed a page's mode (set_color_mode, a merge of channels…): the layers whose blend_channels no
// longer match their page's channels blend every channel again (Photoshop's mode change does the same).
void settle_blend_channels(Document& doc);

// すべてをロック, kept by the command bus around each op (core/command_bus.cpp): the layers locked so before it, and after it
// each of them still there as it was — what it holds, where it is, how it looks (its visibility, name, folder, place in
// the order, links and locks may change) — unless the op works on whole pages (image size, canvas size, mode, a layer comp
// applied: kLockAllPageOps). OpError names the first that is not.
struct LockedLayer {
    std::string page;  // (the page's id)
    Layer layer;
};
std::vector<LockedLayer> locked_layers(const Document& doc);
void check_locked_layers(const std::vector<LockedLayer>& before, const Document& after, std::string_view op);
// Is the layer's look (what it holds, where it is, its opacity, blend, style, mask…) the same — what すべてをロック keeps?
bool same_locked_look(const Layer& a, const Layer& b);

// The layers of the core roles (ネーム, ペン入れ, 背景, 仕上げ), which are never deleted.
bool core_layer(const Layer& layer);
// Does the layer hold nothing — no lines, shape parts, pictures, pixels, fill or correction, no mask or style — or is it
// a folder whose layers all hold nothing? (A paint layer whose pixels are there is not, even when they are all clear.)
bool empty_layer(const std::vector<Layer>& layers, std::size_t index);
// What a folder holds, every level down (positions, in the page's order; a folder chain that comes back on itself is cut).
std::vector<std::size_t> folder_contents(const std::vector<Layer>& layers, std::size_t folder);
// What on the book names the page's layers `gone` (deleted): a line set under one (its style's below_layer) and a ruler
// kept to one (layer_id) let go, an animation's cel of one emptied and its light table without it. Layer comps and
// variables keep naming them (they say so when applied: layer-comps@1, variables@1); notes and the measuring name no
// layer.
void forget_layers(Document& doc, std::size_t page, const std::set<std::string>& gone);

}  // namespace genko::core
