#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/preset_store.hpp"

// PC4 E4 (SPEC.md COLOR-09「グラデーションのプリセット」, design section E 2.3, 2.4 and 3.5): the gradients of this machine —
// the settings folder's gradients.json, in render/preset_store's form — as Photoshop's グラデーション panel holds them, and
// the built-in groups (PhotoCraft v0.2.0's engine/src/presets/gradients.rs builtin(), MIT: its own colours, the names in
// Japanese). A preset is {"name", "stops": [[position 0..1, [r, g, b] | "foreground" | "background"], …] (2 to 16),
// "transparency"?: [[position 0..1, opacity 0..1], …] (2 to 16)} — "foreground" and "background" the 描画色 and 背景色
// when it is used (Photoshop's 描画色から背景色へ). The ops never read the library (PC1 9 章): the window and the CLI give
// a preset's stops to gradient_fill or a gradient fill layer.

namespace genko::render::gradient_presets {

// gradients.json: its built-in groups, how an item is checked.
const preset_store::Kind& kind();

// A preset made of what is given: {"stops": [[position, colour], …] or a list of colours (spaced evenly), "transparency"?}
// with each colour [r, g, b], "#rrggbb", "foreground" or "background" (upstream's form too: "transparency" opacities
// 0..100 when `percent`), named `name`. core::Error("value") says what is wrong.
core::Json item_of(const core::Json& params, std::string_view name, bool percent = false);

// A preset's colours for gradient_fill or a gradient fill layer: {"stops": [[position, colour, 1], …], "transparency"?}
// with "foreground" and "background" given these colours (each [r, g, b] or a precise colour {"precision", "values"}:
// gradient_fill takes both, a fill layer [r, g, b] only). `transparency`: false leaves its transparency out (the gradient
// tool's 透明部分 off).
core::Json stops_for(const core::Json& item, const core::Json& foreground, const core::Json& background, bool transparency = true);

// The colour of a stop at a place (0..1) of a preset, the transparency at it too: for its swatch in the panel (8-bit
// sRGB and an alpha 0..255), "foreground" and "background" given.
std::array<int, 4> sample(const core::Json& item, double t, const std::array<int, 3>& foreground, const std::array<int, 3>& background);

}  // namespace genko::render::gradient_presets
