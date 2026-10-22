#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "render/fx.hpp"

// PC4 節 D1 (SPEC.md COLOR-08「フィルター」, design 4.3): the pieces D1's kernels share (render/fx_blur.cpp, fx_blur2.cpp,
// fx_noise.cpp, fx_other.cpp; their tests) — PhotoCraft v0.2.0's hash of a pixel's place (algo/src/noise.rs) and its rank
// filters (noise.rs median, other.rs min_max, blur.rs surface), MIT: native/third_party/photocraft/NOTICE.md — by fast
// algorithms whose cost does not grow with the window's area (a histogram slid along each row, running extremes), the
// algorithms as PhotoCraft writes them kept beside them as their tests' reference. Each works the samples of a window
// (fx::Window: the output grown by the radius, cut to the layer's grid — past its edge a sample is its edge pixel's,
// Window::at) for an output rectangle, whatever the tiling: a sample's result is the same whichever window holds it.

namespace genko::render::fx {

// PhotoCraft's hash of a pixel's place to [0, 1) (noise.rs hash01): a pixel's grain, the same whatever the tiling.
float hash01(int x, int y, std::uint32_t c, std::uint32_t seed);

// How a rank filter is worked: by its fast algorithm where the samples allow it (Auto), as PhotoCraft writes it (Naive:
// the tests' reference), or fast wherever it can be (Fast; as written where it cannot).
enum class Method { Auto, Naive, Fast };

// 明るさの中間値 / ダスト＆スクラッチ (noise.rs median): each channel's (the alpha's too) median over the disc
// dx² + dy² ≤ r² + r; with `threshold` (0..1), a sample within it of the median is kept. Fast: a histogram of exact bins
// slid along each row (Huang; two levels, the median's coarse bin followed) — 256 for 8 bits, 65536 for 16 (Ctx::levels):
// the same values as written —; floats as written.
std::vector<float> median(const Window& src, const Rect& out, const Ctx& ctx, int r, std::optional<float> threshold, Method method = Method::Auto);

// 明るさの最大値・最小値 (other.rs min_max, min_max_round): each channel's largest (`max`) or least over the square of side
// 2·round(radius) + 1 (`square`, 保持: 直角度) or the disc dx² + dy² ≤ radius² (真円率). Fast: running extremes along the
// rows (and the columns: van Herk / Gil-Werman), a constant work a sample — exact, the same values as written.
std::vector<float> extreme(const Window& src, const Rect& out, double radius, bool square, bool max, Method method = Method::Auto);

// ぼかし（表面） (blur.rs surface): each colour's mean over the square of side 2r + 1 of the samples that show (alpha above
// 0), weighted by max(0, 1 − |v − v0| / t), v0 the pixel's own (its own value where none weighs); the alpha kept. Fast
// (r past 8): bins slid along each row (two levels: a coarse bin wholly to one side of v0 taken at once by its sums) — 256
// exact ones for 8 bits (the same values as written), 4096 for samples within 0..1 (their values' sums exact, each bin
// weighed at its centre: within 0.5/255 of it) —; otherwise (HDR) as written.
std::vector<float> surface(const Window& src, const Rect& out, const Ctx& ctx, int r, float t, Method method = Method::Auto);

// The lightness a kernel's samples mean, 0..1 (fxutil.rs luma): Rec. 601 of RGB, grey's and Lab's own, CMYK's and a
// multichannel page's from their inks (1 − ink, through K).
float lightness(const Ctx& ctx, const float* px);

// Each colour of each pixel that shows as f(its value, its Gaussian blur of `sigma` layer pixels), the alpha kept
// (sharpen.rs with_blur: アンシャープマスク, スマートシャープ, ハイパス; render/fx_sharpen.cpp).
std::vector<float> with_blur(const Window& src, const Rect& out, const Ctx& ctx, float sigma, const std::function<float(float, float)>& f);

}  // namespace genko::render::fx
