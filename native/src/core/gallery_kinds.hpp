#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "core/adjust_kinds.hpp"
#include "core/json.hpp"

// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 2.4): フィルターギャラリー — filter_raster's kind filter_gallery, a stack of
// effects laid on one after another (the first at the bottom, as the window's list shows them from the bottom up), each
// one of the 47 of Photoshop's gallery in its six folders (アーティスティック・ブラシストローク・変形・スケッチ・表現手法・
// テクスチャ). PhotoCraft v0.2.0's table (algo/src/artistic.rs 39-91: their settings, ranges and defaults — the looks are
// PhotoCraft's own approximations, not Photoshop's) and its reading of an effect (engine/src/gallery_cmds.rs 32-79), MIT:
// native/third_party/photocraft/NOTICE.md. Keys are snake_case (PhotoCraft's camelCase: pencilWidth → pencil_width), a
// choice's values PhotoCraft's own (bottomLeft, wideSharp…), colours [r, g, b] 0..255.
//
// The kind's settings (its normal form, core::normalize_filter):
//   {"effects": [{"filter": "<effect>", "params": {…its settings, every one}, "visible": true|false,
//                 "foreground"?: [r, g, b], "background"?: [r, g, b]}, …1 to 32],
//    "foreground": [r, g, b] (black), "background": [r, g, b] (white)}
// An effect hidden is not laid on (all hidden: the layer as it was). The colours an effect reads (the sketches' ink and
// paper, 光彩拡散's glow, ステンドグラス's lead…) are its own or else the stack's: the window fills the stack's with the
// main and sub colours (描画色・背景色), so a smart filter draws again as it was made. ネオン光彩's glow is its setting
// glow_color. Lengths are the working 200 dpi's pixels (render/fx_gallery.cpp scales them to a layer's grid).

namespace genko::core {

// One effect of the gallery.
struct GalleryEffectKind {
    std::string_view filter;    // its key ("colored_pencil")
    std::string_view upstream;  // PhotoCraft's ("coloredPencil": its command filter.gallery.coloredPencil)
    std::string_view name;      // Photoshop's name (色鉛筆)
    std::string_view category;  // its folder (Photoshop's name: アーティスティック…)
    std::vector<AdjustParam> params;  // its settings in PhotoCraft's order (numbers, choices — the first the default —, checks;
                                      // ネオン光彩's colour)
};

// The 47 in Photoshop's order (by folder), and one by its key (none: not an effect).
const std::vector<GalleryEffectKind>& gallery_effects();
const GalleryEffectKind* gallery_effect(std::string_view filter);
// The folders in order.
const std::vector<std::string_view>& gallery_categories();

inline constexpr std::size_t kGalleryMostEffects = 32;
// ネオン光彩's colour by default (PhotoCraft's NEON_DEFAULT, a saturated sky blue).
inline constexpr std::array<int, 3> kNeonGlowColour{0, 158, 255};

// filter_gallery's settings as its kind's table read them (an op's: the colours read, `effects` as given) in their normal
// form: each effect's filter known, its settings read as an op's (clamped, defaults filled), visible filled in (true).
// OpError for what it refuses (no effects, more than 32, an effect not known, a setting it has not).
Json read_gallery(Json read);

// The gallery's catalogue (genko inspect --filters, design 3.2; PhotoCraft's gallery_cmds catalogue): its effects in order,
// each its key, PhotoCraft's key and command, Photoshop's name, its folder and its settings (type, default, range,
// choices). (The folders' order: gallery_categories.)
Json gallery_catalogue();

}  // namespace genko::core
