#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/json.hpp"

// artboards@1 (PC5 I-c、SPEC.md AUTO-01「アートボード」・schema-v4.md「アートボード」; the user's decision 3: アートボード＝ページ):
// an artboard is a page with the artboard mark — its size the page's own paper (page-spec@1: its pixels at the book's
// resolution, its finished size the paper, no bleed, its basic frame the paper), its contents the page's layers. The page's
// `artboard`:
//   {"name": str,                                    (the artboard's name: the panel's, the exports' file names)
//    "preset"?: str,                                 (the preset it was made at — gone when its size is changed by hand)
//    "background": "white" | "black" | "transparent" | "custom", "rgb"?: [r, g, b] (custom's only),
//    "origin_px"?: [x, y]}                           (where it lies on a PSD's or a .pcraft's canvas: written back there;
//                                                     never drawn)
// white is the paper as every page's; black and custom are laid under the page's layers (the three ways of drawing:
// render::page_background); transparent is white on the screen and nothing in an export of a format with alpha
// (formats/artboards). A cover or a page of a spread is never an artboard. After PhotoCraft v0.2.0's doc/src (Artboard) and
// engine/src/artboard_cmds.rs (MIT): there an artboard is a board on one canvas; here it is a page of the book. The ops are
// render/ops_artboards, the exports formats/artboards.

namespace genko::core {

struct Document;
struct Page;

inline constexpr std::string_view kArtboardsFeature = "artboards@1";
inline constexpr std::string_view kArtboardKey = "artboard";
inline constexpr std::size_t kArtboardNameMax = 1024;      // bytes
inline constexpr std::int64_t kArtboardOriginMax = 300000;  // (a canvas's side, PhotoCraft's fit_canvas)

struct Artboard {
    std::string name;
    std::optional<std::string> preset;
    std::string background = "white";  // white | black | transparent | custom
    std::optional<std::array<int, 3>> rgb;
    std::optional<std::array<std::int64_t, 2>> origin_px;

    bool operator==(const Artboard&) const = default;
};

Json to_json(const Artboard& artboard);
// core::Error("value") says what is not so (a key not known, a value of another type or out of its range).
Artboard artboard_from_json(const Json& value);
void check_artboard(const Artboard& artboard);

bool uses_artboards(const Document& doc);
// Every page's artboard well formed, and none on a cover or a page of a spread (core::Error("value")).
void validate_artboards(const Document& doc);

// Why the page cannot be an artboard (an op's words), or none: a cover, a page of a spread.
std::optional<std::string> artboard_refusal(const Page& page);
// The colour laid under the page's layers: an artboard's black or custom background (none: the paper as every page's).
std::optional<std::array<int, 3>> artboard_underlay(const Page& page);
// An artboard whose background is transparent (written so where the format holds an alpha).
bool transparent_artboard(const Page& page);

// The artboard presets (アートボード… の プリセット): PhotoCraft's devices and Web sizes in pixels, then Genko's finished sizes
// (A4, B5, B4) in mm — their pixels at the book's resolution.
struct ArtboardPreset {
    std::string_view name;
    double width = 0;
    double height = 0;
    bool mm = false;
};
std::span<const ArtboardPreset> artboard_presets();
const ArtboardPreset* artboard_preset(std::string_view name);
// A preset's size in pixels at `dpi`.
std::array<std::int64_t, 2> preset_pixels(const ArtboardPreset& preset, double dpi);

// The name a new artboard takes: アートボード N, the first N no artboard of the book has.
std::string next_artboard_name(const Document& doc);

}  // namespace genko::core
