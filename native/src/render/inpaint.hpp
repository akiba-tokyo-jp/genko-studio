#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// PC3 B4 (SPEC.md COLOR-06「修復」): filling a hole from what is around it — the Spot Healing Brush's three ways and
// Content-Aware Fill's completion. PhotoCraft v0.2.0's algo/src/inpaint.rs (MIT, native/third_party/photocraft/NOTICE.md),
// in float as it computes them.
//
// - complete: Content-Aware completion — the multi-scale EM of Y. Wexler, E. Shechtman, M. Irani ("Space-Time Completion
//   of Video", TPAMI 2007) with the nearest-neighbour field of C. Barnes et al.'s PatchMatch (SIGGRAPH 2009: random
//   start, propagation, an exponentially shrinking random search). Each EM step finds, for every patch over the hole, its
//   most similar wholly known patch and votes the hole's pixels from the matches, weighted by how alike they are; coarse
//   to fine over a pyramid (the coarsest level started by the membrane), so a big hole stays cheap.
// - best_offset: Proximity Match — the one displacement whose surroundings best match the ring around the hole.
// - synthesize: Create Texture — blocks of the surrounding texture chosen among random candidates by how well they agree
//   with what is filled already (A. Efros, W. Freeman, "Image Quilting", SIGGRAPH 2001; a feathered overlap).
//
// Buffers are interleaved `w × h × ch` floats of any colour model and a `w × h` hole; each result equals the input
// outside the hole. All are deterministic (a seeded hash generator; the same result however many threads there are).

namespace genko::render::inpaint {

struct CompleteParams {
    std::size_t patch_radius = 3;  // (patches are 2r + 1 square)
    // EM iterations at the coarsest level (finer levels fewer, down to min_em_iters).
    std::size_t em_iters = 8;
    std::size_t min_em_iters = 2;
    std::size_t pm_iters = 2;  // PatchMatch passes per EM iteration
    // A level where the hole is wider than this (its pixels) only refines the match field it was given (one local pass,
    // one vote): the structure is decided coarse, the fine levels add the detail.
    std::size_t refine_extent = 96;
    std::uint64_t seed = 1;
};

// Content-aware completion of `hole`; none when there is no wholly known patch to copy from (the caller can fall back to
// poisson::membrane_fill).
std::optional<std::vector<float>> complete(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img,
                                           const std::vector<bool>& hole, const CompleteParams& p = {});
// Proximity match: the displacement (within max_radius) whose surroundings best match the hole's ring of `ring` pixels,
// the hole moved by it wholly inside the grid and outside the hole; none when none fits.
std::optional<std::pair<int, int>> best_offset(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img,
                                               const std::vector<bool>& hole, std::size_t ring, int max_radius);
// Create texture: the hole filled with blocks (`block` pixels) of the texture around it.
std::vector<float> synthesize(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole,
                              std::size_t block, std::uint64_t seed);

// The box of the hole's pixels (x0, y0, x1, y1); none when it has none.
struct HoleBox {
    std::size_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::optional<HoleBox> hole_box(std::size_t w, std::size_t h, const std::vector<bool>& hole);

// SplitMix64, the small deterministic generator the fills use (tests make their noise with it too).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ^ 0x9E3779B97F4A7C15ULL) {}
    std::uint64_t next();
    // A whole number in lo..=hi.
    int range(int lo, int hi);

private:
    std::uint64_t state_;
};

}  // namespace genko::render::inpaint
