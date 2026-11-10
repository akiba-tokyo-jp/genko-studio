#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "render/fx.hpp"

// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 3.2): フィルター → 描画 → 炎…・額縁…・木… — the op render_picture.
// PhotoCraft v0.2.0's algo/src/render2.rs (1-1530: the shapes — tapered capsules and shaded ellipses, laid normally,
// added or lightened —, the random stream, the flame along a path, the 34 trees, the 16 picture frames) and
// engine/src/render_cmds.rs (1-183), MIT: native/third_party/photocraft/NOTICE.md. The shapes are made in pixels of the
// working 200 dpi over the page (the same picture on the paper at every resolution: design 4.2), then laid on the layer's
// grid on all three ways — an 8-bit layer's RGBA, a precise layer's samples, an ink layer's inks by the page's rules —
// through render/fx's engine (tile by tile, on its threads, in the imaging budget, stopped by 中止), inside the selection.

namespace genko::render::picture {

using XY = std::pair<float, float>;

// One shape (render2.rs Prim): a capsule from a to b whose radius goes ra → rb, or an ellipse about c (radii rx, ry, turned
// `angle` radians); its colour c0 → c1 (along the capsule, centre to rim of the ellipse; straight RGBA 0..1), its edge's
// softness in pixels, and how it lies on what is under it.
struct Prim {
    enum class Shape : std::uint8_t { Capsule, Ellipse };
    enum class Blend : std::uint8_t { Normal, Add, Lighten };
    Shape shape = Shape::Capsule;
    Blend blend = Blend::Normal;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // (a and b; an ellipse's centre at x0, y0)
    float r0 = 0, r1 = 0;                  // (ra and rb; an ellipse's rx and ry)
    float angle = 0;
    std::array<float, 4> c0{}, c1{};
    float soft = 1;
    bool operator==(const Prim&) const = default;
};

// A box of the page in pixels of the working 200 dpi (x0, y0, width, height).
struct Canvas {
    float x0 = 0, y0 = 0, width = 1, height = 1;
};

// The shapes of each kind from its settings (core::normalize_picture's normal form), in working pixels. 炎's paths:
// polylines (each a path's subpath flattened; a closed one ending where it starts). OpError past kPictureMostShapes.
std::vector<Prim> flame(const core::Json& spec, const std::vector<std::vector<XY>>& paths);
std::vector<Prim> tree(const core::Json& spec, const Canvas& canvas);
std::vector<Prim> picture_frame(const core::Json& spec, const Canvas& canvas);
// 炎's path when the page has none (render2.rs default_flame_path): an arc across the lower part of the canvas.
std::vector<XY> default_flame_path(const Canvas& canvas);
// How many shapes 炎 makes at most on these paths (before they are made: refused past kPictureMostShapes).
std::uint64_t flame_shapes_at_most(const core::Json& spec, const std::vector<std::vector<XY>>& paths);

// The op: {"op": "render_picture", "page", "id"? (the paint layer drawn on), "new_layer"?, "kind": flame | picture_frame |
// tree, …its settings, "path"? (炎), "area"?}. A new paint layer when asked, when no layer is named or the one named is not a
// paint layer (above it). The frame of 木 and 額縁 (and 炎's arc) is the selection's box, else the page's trim. Its report:
// {"id", "new_layer", "bounds_mm" ([x, y, w, h] of what it drew, or null), "primitives", and "used_path" (炎), "tree_type" (木)
// or "frame" (額縁)}.
// The shapes (on the layer's grid: working pixels × s) laid over `surface` within `clip` — mixed by a coverage, or none —,
// tiled and threaded by the engine (`tiling`: tests), the colours the layer's pixels mean; what they reach within the clip
// (none: nothing laid). The cells' table and each tile's list held in the imaging budget.
fx::Rect lay(fx::Surface& surface, std::vector<Prim> shapes, const fx::Rect& clip, const std::string* cover, double s, const fx::Tiling& tiling = {});

void render_picture(core::OpContext& c);

}  // namespace genko::render::picture
