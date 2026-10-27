#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pat.hpp"

// PC4 E6 (8-4 (a): .asl, SPEC.md COLOR-09「スタイル」): Photoshop's styles file — not in PhotoCraft v0.2.0; read with the
// same effects descriptor as a PSD's lfx2 (render/psd_effects), after Adobe's public descriptor structure and the file's
// layout as it is commonly described (no other reader's code taken):
//
//   file    := version u16 (2), "8BSL", patterns' version u16 (3), patterns' length u32, the patterns (a Patt block's
//              data: render/pat), the styles' count u32, the styles
//   style   := length u32, then (padded to 4) a versioned descriptor of class "null" (Nm: its name, Idnt: its unique id)
//              and a versioned descriptor of class "Styl" (documentMode; Lefx: the layer effects, lfx2's descriptor;
//              blendOptions?: its blend mode and fill opacity)
//
// Bounds: a file past kMaxFileBytes and more styles than kMaxStyles are refused before anything is read into memory; each
// style's descriptors are read within its own length and render/psd_descriptor's budget, the patterns within render/pat's.

namespace genko::render::asl {

inline constexpr std::size_t kMaxFileBytes = std::size_t(256) << 20;
inline constexpr std::size_t kMaxStyles = 4096;

struct Style {
    std::string name;
    core::Json style;  // (core::layer_style_spec's form; a pattern named {"id": "asl-<index of `patterns`>", …}; null: none)
    std::optional<std::string> blend;  // (its blending options' mode, when it has them)
};

struct File {
    std::vector<pat::PsdPattern> patterns;
    std::vector<Style> styles;
    std::vector<std::string> notes;  // (what was not read as it is)
};

// The prefix of the ids the styles' pattern fills name the file's patterns by ("asl-0", "asl-1", …).
inline constexpr std::string_view kPatternPrefix = "asl-";

// An .asl's styles, its lengths the file's pixels at `dpi`. core::Error("format") for what is not an .asl or cannot be
// read, core::Error("too_large") past the bounds. A style that cannot be read is left out (said in notes).
File read(std::string_view data, double dpi);

// An .asl of these styles (each its name, its style in core::layer_style_spec's form — its patterns the book's, written
// with it — and its blend mode when it has blending options), its lengths pixels at `dpi`.
std::string write(const std::vector<Style>& styles, const core::Document& book, double dpi);

}  // namespace genko::render::asl
