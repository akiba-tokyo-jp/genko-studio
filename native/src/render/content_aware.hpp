#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// PC3 B4 (SPEC.md COLOR-06「コンテンツに応じた塗りつぶし」): Edit › Content-Aware Fill — the completion of
// render/inpaint with a sampling area, rotated, scaled and mirrored copies of it, and colour adaptation. PhotoCraft
// v0.2.0's algo/src/content_aware.rs (MIT, native/third_party/photocraft/NOTICE.md), in float as it computes them.
//
// - The sampling area: what may not be copied from is handed to the completion as unknown (it copies only wholly known
//   patches) and is put back after; only the hole keeps what was made.
// - Rotation, scale and mirror: the sampling window and its turned, scaled and mirrored copies side by side (gaps wider
//   than a patch between), so the search can take a transformed patch (a small set of the Generalized PatchMatch's,
//   Barnes et al., ECCV 2010).
// - Colour adaptation: the fill's low frequencies pulled toward the membrane of the hole's edge (Pérez et al.'s), which
//   follows gradual changes of light and colour across the hole and keeps the copied texture.
// Interleaved `w × h × ch` floats of any colour model; deterministic.

namespace genko::render::content_aware {

struct FillOptions {
    float color_adaptation = 0.35f;  // 0..1 (none 0, default, high, very high)
    std::vector<float> rotations;    // extra copies turned by these degrees
    bool scale = false;              // copies at 80 % and 125 %
    bool mirror = false;             // a copy mirrored left to right
    std::uint64_t seed = 1;
    // The colour adaptation keeps values in 0..1 (false: only above 0 — a float layer's HDR kept).
    bool unit_range = true;
};

// Photoshop's levels: rotation none, low, medium, high, full; colour none, default, high, very_high.
std::vector<float> rotation_level(std::string_view level);
float color_level(std::string_view level);

// `hole` filled in `img`, copying only from where `source` is true (and not in the hole); the membrane when nothing can
// be copied. The whole buffer (as it was outside the hole).
std::vector<float> fill(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole,
                        const std::vector<bool>& source, const FillOptions& opts);

// (tests) A copy of the source turned by `deg` and scaled about its centre, mirrored; its size and which pixels are
// known.
struct Variant {
    std::size_t w = 0, h = 0;
    std::vector<float> img;
    std::vector<bool> ok;
};
Variant variant(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& ok, float deg, float scale,
                bool mirror);

}  // namespace genko::render::content_aware
