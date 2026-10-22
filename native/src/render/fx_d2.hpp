#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/fx.hpp"

// PC4 節 D2 (SPEC.md COLOR-08「フィルター」): the pieces 変形・ピクセレート・表現手法's kernels share (render/fx_distort.cpp,
// fx_pixelate.cpp, fx_stylize.cpp, fx_oil.cpp) — their settings read, PhotoCraft v0.2.0's inverse mapping (algo/src/
// distort.rs remap), the lightness of the samples a kind of the lightness works (a page of inks' plates as 1 − ink), a
// pixel's place for a pattern in working pixels, and exact box sums. MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::fx::d2 {

double number(const core::Json& params, std::string_view key, double fallback);
std::string text(const core::Json& params, std::string_view key, std::string_view fallback);
std::uint32_t seed_of(const core::Json& params);
// A colour [r, g, b] 0..255 of the settings as straight 0..1 (none or not a colour: `fallback`).
std::array<float, 3> colour(const core::Json& params, std::string_view key, std::array<float, 3> fallback);

// Edge::Wrap, Repeat or Transparent by 未定義領域's value ("wrap", "repeat", "transparent").
Edge edge_of(const std::string& undefined);

// The base box's centre and the radius of the circle inside it (distort.rs centre).
struct Centre {
    float x = 0, y = 0, r = 0;
};
Centre centre(const Rect& b);

// Each pixel of `out` the bilinear sample of the window where `f` maps its centre (x + 0.5, y + 0.5), past the base box
// by `edge` (distort.rs remap; the window holds the base box: a kind of the whole base box).
std::vector<float> remap(const Window& src, const Rect& out, const Ctx& ctx, Edge edge, const std::function<std::pair<float, float>(float, float)>& f);

// The lightness of a pixel of samples a kind of the lightness works (FilterInks::LightnessAndColour, 0..1): Rec. 601 of
// RGB, grey's own, a Lab page's L, CMYK's from its plates as 1 − ink (the colours they make, through K), a multichannel
// page's the product of its plates' (each 1 − ink).
float light(const Ctx& ctx, const float* px);
// Is this space's colour channel `c` a lightness (RGB's, grey's, a CMYK or multichannel page's plates as 1 − ink, a Lab
// page's L) — not a Lab page's a or b (about their middle, 128/255)?
bool lightness_channel(const Ctx& ctx, std::size_t c);
inline constexpr float kLabMiddle = 128.0f / 255.0f;

// A pixel of the layer's grid as a pixel of the working 200 dpi (a pattern's place: the same on the paper at every
// resolution — design 2.2), the surface's place on the grid added (Ctx::x0, y0).
inline int working(int v, int origin, double s) { return static_cast<int>(std::floor((static_cast<double>(v) + origin) / s)); }

// The radius in the layer's pixels of a reach of `working` pixels of the working 200 dpi (at least 1 when `least`).
inline int reach(double working, double s, int least = 0) { return std::max(least, static_cast<int>(std::lround(working * s))); }

// A halo of `radius` pixels, or — a reach past what a window of a thread's tile holds well — the whole base box read once
// (what the run gives is the same either way).
inline Halo halo_of(int radius) { return radius > 1024 ? Halo::whole() : Halo::margin(radius); }

// `px` moved `w` (0..1) of the way to `other` (straight pixels of `n` samples, the alpha last), their colours weighed by
// their alpha: a clear pixel's colour never comes in (D3's review P2-1).
inline void mix(float* px, const float* other, float w, std::size_t n) {
    const float a0 = px[n - 1], a1 = other[n - 1];
    const float a = a0 + (a1 - a0) * w;
    for (std::size_t c = 0; c + 1 < n; ++c) {
        const float p0 = px[c] * a0, p1 = other[c] * a1;
        px[c] = a > 1e-7f ? (p0 + (p1 - p0) * w) / a : px[c] + (other[c] - px[c]) * w;
    }
    px[n - 1] = a;
}

// Gaussian smoothing (three box passes along the rows, then the columns: fxutil.rs gauss_blur_n) of `n` interleaved
// channels of a w × h buffer, edges clamped — its sums exact in whole units of a power of two of `largest` (the largest
// magnitude of a value: the same values whatever buffer holds them, tile independence).
void gauss_blur_exact(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, float sigma, double largest);

}  // namespace genko::render::fx::d2
