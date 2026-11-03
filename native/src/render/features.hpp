#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "render/resample.hpp"

// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」): the registration of 編集 → レイヤーを自動整列 and ファイル → 自動処理 →
// Photomerge — corners, binary descriptors, matching and robust model fitting. PhotoCraft v0.2.0's algo/src/features.rs (MIT,
// native/third_party/photocraft/NOTICE.md), the same steps and constants:
//   - corners: Harris & Stephens 1988 (the structure tensor's det − 0.04·trace², two box passes for the Gaussian window), local
//     maxima above 1 % of the peak, kept at least `min_dist` apart (a coarse grid), strongest first;
//   - descriptors: steered BRIEF (ORB, Rublee et al. 2011): 256 comparisons of a smoothed 31 × 31 patch, turned by the patch's
//     intensity centroid;
//   - matching: Hamming distance, Lowe's ratio test and a mutual nearest check;
//   - fitting: RANSAC (Fischler & Bolles 1981) over a translation, a similarity or a homography (Hartley's normalised DLT), the
//     winner fitted again to all its inliers by least squares.
// Everything is deterministic (fixed seeds, one thread). Images are w × h luminance in 0..1, row-major.

namespace genko::render::features {

using Homography = resample::Homography;
using XY = std::array<double, 2>;

struct Corner {
    float x = 0, y = 0, score = 0;
};

using Descriptor = std::array<std::uint64_t, 4>;

enum class Model { Translation, Similarity, Homography };

// Corners nearer the border than this are dropped (the descriptor's patch).
inline constexpr std::size_t kPatchRadius = 15;

// SplitMix64, as upstream's (the same numbers for the same seed).
struct Rng {
    std::uint64_t s;
    std::uint64_t next();
    double unit();
    std::size_t below(std::size_t n);
};

// Two box passes of radius r (edges carried on).
std::vector<float> box_blur(std::size_t w, std::size_t h, const std::vector<float>& v, std::size_t r);

// Harris corners: at most `max`, at least `min_dist` apart, strongest first; where `valid` is given, only where it is true over
// the whole descriptor patch (sampled every 5 pixels).
std::vector<Corner> harris(std::size_t w, std::size_t h, const std::vector<float>& img, std::size_t max, float min_dist,
                           const std::vector<bool>* valid = nullptr);
// Steered-BRIEF descriptors of `corners` (`upright`: not turned).
std::vector<Descriptor> describe(std::size_t w, std::size_t h, const std::vector<float>& img, const std::vector<Corner>& corners, bool upright);
unsigned hamming(const Descriptor& a, const Descriptor& b);
// The matches (index in a, index in b) passing the ratio test and the mutual nearest check (and a distance of 80 at most).
std::vector<std::pair<std::size_t, std::size_t>> match_descriptors(const std::vector<Descriptor>& a, const std::vector<Descriptor>& b, float ratio);

// The n × n system a·x = b (row-major) by Gaussian elimination with partial pivoting; none when singular.
std::optional<std::vector<double>> solve_dense(std::size_t n, std::vector<double> a, std::vector<double> b, double tiny = 1e-12);

// The model taking src[i] to dst[i] by least squares (exact for a minimal set); none when it cannot be fitted.
std::optional<Homography> fit(Model model, const std::vector<XY>& src, const std::vector<XY>& dst);
std::size_t min_points(Model model);
// RANSAC: the model with the most correspondences within `threshold` after `iters` random minimal samples, fitted again to its
// inliers; the model and the inliers' indices.
std::optional<std::pair<Homography, std::vector<std::size_t>>> ransac(Model model, const std::vector<XY>& src, const std::vector<XY>& dst,
                                                                      std::size_t iters, double threshold, std::uint64_t seed);
// `moving` registered onto `reference` (both w × h; optional masks of where each is there): the model taking moving's
// coordinates to reference's and the inlier count.
std::optional<std::pair<Homography, std::size_t>> register_pair(std::size_t w, std::size_t h, const std::vector<float>& reference,
                                                                const std::vector<float>& moving, const std::vector<bool>* valid_ref,
                                                                const std::vector<bool>* valid_mov, Model model);

}  // namespace genko::render::features
