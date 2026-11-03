#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// PC5 I-g (SPEC.md AUTO-01「自動合成」): multiresolution blending for 編集 → レイヤーを自動合成 — PhotoCraft v0.2.0's
// algo/src/pyramid.rs (MIT, native/third_party/photocraft/NOTICE.md), after P. J. Burt, E. H. Adelson, "A Multiresolution Spline
// With Application to Image Mosaics" (ACM TOG 1983): each picture a Laplacian pyramid, the weights a Gaussian one, each level
// blended with its own smoothed weights and the whole collapsed. The weights: stack_weights (focus stacking: at each pixel the
// picture with the most detail — the smoothed |∇²L| —) or panorama_weights (each pixel to the picture whose opaque part it lies
// deepest in). Buffers are interleaved w × h × ch floats of any colour model; deterministic.
//
// Genko's own (design 節 I 4.4): the blend in tiles (blend_tiled) — 1024 px a side with an overlap of 5·2^levels px, at most 6
// levels (320 px) —, so a page is never held at once in every level of every picture.

namespace genko::render::pyramid {

struct Level {
    std::size_t w = 0, h = 0;
    std::vector<float> px;
};

Level reduce(const Level& l, std::size_t ch);
std::vector<float> expand(const Level& l, std::size_t ch, std::size_t w, std::size_t h);
std::vector<Level> gaussian(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& px, std::size_t levels);
std::vector<Level> laplacian(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& px, std::size_t levels);
std::vector<float> collapse(const std::vector<Level>& pyr, std::size_t ch);
// The levels that make the coarsest a few pixels across (at most 10).
std::size_t auto_levels(std::size_t w, std::size_t h);
// `images` blended by their per-pixel weights (any, not negative: normalised at each level); `clamp`: the result kept in 0..1
// (a 32-bit page's values are not).
std::vector<float> blend(std::size_t w, std::size_t h, std::size_t ch, const std::vector<const std::vector<float>*>& images,
                         const std::vector<const std::vector<float>*>& weights, std::size_t levels, bool clamp = true);

// The tiles of blend_tiled.
inline constexpr std::size_t kTile = 1024;
inline constexpr std::size_t kMaxTiledLevels = 6;
std::size_t tile_overlap(std::size_t levels);  // 5·2^levels
// The blend worked a tile at a time: `read(i, x0, y0, w, h, out)` puts picture i's w × h pixels at (x0, y0) into `out` (ch a
// pixel) and `weight(i, …)` its weights (one a pixel); `write(x0, y0, w, h, px)` takes each tile's middle. Levels at most
// kMaxTiledLevels.
using ReadPart = std::function<void(std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::vector<float>&)>;
using WritePart = std::function<void(std::size_t, std::size_t, std::size_t, std::size_t, const std::vector<float>&)>;
void blend_tiled(std::size_t w, std::size_t h, std::size_t ch, std::size_t images, const ReadPart& read, const ReadPart& weight, std::size_t levels,
                 bool clamp, const WritePart& write);

// One-hot weights: focus stacking (the sharpest wins at each pixel; `alpha[k]` where picture k shows) and panorama.
std::vector<std::vector<float>> stack_weights(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& luma,
                                              const std::vector<std::vector<float>>& alpha);
std::vector<std::vector<float>> panorama_weights(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& alpha);

}  // namespace genko::render::pyramid
