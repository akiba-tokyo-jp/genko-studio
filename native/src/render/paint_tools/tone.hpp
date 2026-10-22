#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "render/image.hpp"

// PC4 E5 (SPEC.md COLOR-09「画素を変える筆」): what the toning and focus brushes do to a pixel — Dodge, Burn, Sponge, Blur
// and Sharpen — as PhotoCraft v0.2.0's crates/algo/src/retouch.rs (lines 1-200: tone_curve, compress_gamut, dodge_burn,
// saturation, sponge, local_blur, local_sharpen) and crates/engine/src/retouch_cmds.rs (lines 434-456: per_dab), MIT
// (native/third_party/photocraft/NOTICE.md). The brushes (render/ops_paint_tools) decide where and how strongly a dab
// applies; these decide what happens to the pixels. Each works on straight values 0..1 (any channel count for the focus
// brushes); a colour is three of them.

namespace genko::render::paint_tools {

// The tonal range Dodge and Burn work on (Photoshop's 範囲).
enum class ToneRange { Shadows, Midtones, Highlights };

// Rec. 601 luma of a straight RGB colour.
float luma(const std::array<float, 3>& c);
// The dodge (burn false) or burn transfer of one value with a strength e (0..1): midtones a gamma bend weighted to
// peak at mid-grey (0 and 1 kept), shadows and highlights moving the end chosen most.
float tone_curve(float v, float e, ToneRange range, bool burn);
// A colour dodged or burned at strength e (0..1): with protect_tones the curve on its luma, the colour scaled to the
// new luma and brought back into 0..1 toward its grey (hue and saturation kept); else each channel through the curve.
std::array<float, 3> dodge_burn(const std::array<float, 3>& c, float e, ToneRange range, bool burn, bool protect_tones);
// HSV saturation (chroma / max).
float saturation(const std::array<float, 3>& c);
// Sponge: the colour moved away from (saturate) or toward its luma grey by amount (0..1); vibrance damps it where it
// would clip (saturating a saturated colour, desaturating a near-grey one less).
std::array<float, 3> sponge(const std::array<float, 3>& c, float amount, bool saturate, bool vibrance);

// One dab's share of a stroke's strength e, so that one pass over a point totals about e whatever the spacing (about
// 1 / spacing dabs overlap each point of the stroke's centre line).
float per_dab(float e, float spacing);

// The focus brushes' windows read from a layer's values (any channel count `ch`, the alpha at `alpha` when there is
// one: < 0 for none), each pixel by its place on the grid — what lies past it the nearest pixel of it (as upstream's
// edge clamp): `get(x, y)` gives the `ch` values of a pixel within the grid of size `grid`.
using Reader = std::function<const float*(int x, int y)>;
// The Gaussian blur (sigma) of the window `win` of the grid: the colours blurred premultiplied by the alpha, so clear
// pixels lend none (a pixel left clear keeps its colour). The window's pixels, row by row, ch values each.
std::vector<float> local_blur(const Reader& get, Size grid, int ch, int alpha, const Box& win, float sigma);
// Unsharp-mask sharpening of the window: v + amount·(v − blur(v)) at sigma 1, the alpha kept; protect_detail clamps each
// to its 3×3 neighbourhood's range (no new extremes from halos and noise).
std::vector<float> local_sharpen(const Reader& get, Size grid, int ch, int alpha, const Box& win, float amount, bool protect_detail);

}  // namespace genko::render::paint_tools
