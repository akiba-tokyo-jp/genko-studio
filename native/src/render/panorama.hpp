#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "render/features.hpp"

// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」「Photomerge」): panorama registration and blending for ファイル → 自動処理 →
// Photomerge and 編集 → レイヤーを自動整列・レイヤーを自動合成. PhotoCraft v0.2.0's algo/src/panorama.rs (MIT,
// native/third_party/photocraft/NOTICE.md), after M. Brown, D. G. Lowe, "Automatic Panoramic Image Stitching using Invariant
// Features" (IJCV 2007):
//   1. features (render/features: Harris corners, steered BRIEF);
//   2. every pair matched, RANSAC's homography kept when it passes Brown–Lowe's check (inliers > 8 + 0.3·matches);
//   3. the focal length (cylindrical, spherical) from the pairs' homographies of a turning camera (Shum & Szeliski 2000) or the
//      EXIF's;
//   4. the projection and motion of each layout: perspective (a plane, a homography), cylindrical and spherical (the points on
//      the cylinder or the sphere, a rigid motion there), collage (a similarity), reposition (a translation); auto takes the
//      perspective unless it is too stretched, then the cylinder, the sphere when the panorama is tall too;
//   5. a maximum spanning tree from the reference gives the first placements, Levenberg–Marquardt then brings all the matched
//      points together (one radial distortion coefficient too with the geometric correction, the focal length of the cylinder
//      or sphere) — at most 80 points of each pair;
//   6. gains (Brown–Lowe §6) with a shared vignetting log V(r) = a·r² + b·r⁴ (Goldman 2010), linear least squares in logs;
//   7. seams: each image in turn takes its side of the cheapest path through its overlap with what is there (seam carving's
//      dynamic programming on the colour difference);
//   8. multi-band blending (Burt & Adelson 1983) with holes filled by push-pull.
// Deterministic; registration on luminance in 0..1 at a reduced scale (Placement::scaled back to full resolution).

namespace genko::render::panorama {

using features::Homography;
using features::XY;

enum class Layout { Auto, Perspective, Cylindrical, Spherical, Collage, Reposition };
std::optional<Layout> layout_from(std::string_view text);  // auto | perspective | cylindrical | spherical | collage | reposition
std::string_view layout_name(Layout layout);

enum class Projection { Plane, Cylinder, Sphere };
enum class Motion { Translation, Euclidean, Similarity, Homography };

std::optional<Homography> fit_motion(Motion motion, const std::vector<XY>& src, const std::vector<XY>& dst);
std::optional<std::pair<Homography, std::vector<std::size_t>>> ransac_motion(Motion motion, const std::vector<XY>& src, const std::vector<XY>& dst,
                                                                             std::size_t iters, double threshold, std::uint64_t seed);

// Where one image lands in the panorama: its pixels → (undistorted) → the projection → the 2-D motion.
struct Placement {
    Projection projection = Projection::Plane;
    double focal = 1.0;       // (px: the projection's radius)
    XY center{0.0, 0.0};      // (image px: the projection's centre, the image's middle)
    double k1 = 0.0;          // (radial distortion on radii over the focal length; 0: none)
    Homography h;             // (projected → panorama)

    XY project(double x, double y) const;
    std::optional<XY> unproject(double a, double b) const;
    XY forward(double x, double y) const;
    std::optional<XY> inverse(double u, double v) const;
    // the same for images k× larger (registration ran at 1/k)
    Placement scaled(double k) const;
    Placement translated(double dx, double dy) const;
    // [x0, y0, x1, y1] of a w × h image in the panorama (its edges sampled)
    std::array<double, 4> bounds(double w, double h) const;
};

// The projective matrix of a plane placement without distortion (image px → panorama px), none for the others.
std::optional<Homography> plane_matrix(const Placement& p);

struct ImageFeatures {
    std::size_t w = 0, h = 0;
    std::vector<XY> points;
    std::vector<features::Descriptor> desc;
};
ImageFeatures detect(std::size_t w, std::size_t h, const std::vector<float>& luma, const std::vector<bool>* valid, std::size_t max);

struct PairMatch {
    std::size_t a = 0, b = 0;
    std::vector<XY> pts_a, pts_b;
    Homography homography;  // (b → a, image px)
};
std::vector<PairMatch> match_pairs(const std::vector<ImageFeatures>& feats);

// The focal length (px) of a turning camera from a homography between two of its images (centred at ca, cb).
std::optional<double> focal_from_homography(const Homography& h, XY ca, XY cb);

struct AlignOptions {
    Layout layout = Layout::Auto;
    std::optional<double> focal;            // (registration px: the EXIF's; else estimated)
    std::optional<std::size_t> reference;   // (none: the best connected)
    bool geometric = false;                 // (a radial distortion coefficient fitted: 幾何学的歪曲の補正)
};

struct Alignment {
    Layout layout = Layout::Perspective;     // (auto resolved)
    std::size_t reference = 0;
    std::vector<std::optional<Placement>> placements;  // (none: an image that could not be joined)
    double focal = 0, k1 = 0;
    std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> pairs;  // (a, b, inliers)
    double rms = 0;
};
// None when fewer than two images join.
std::optional<Alignment> align(const std::vector<ImageFeatures>& feats, const std::vector<PairMatch>& matches, const AlignOptions& options);

// --- photometric -------------------------------------------------------------------------------------------------------

// One image in the panorama: a box of colours (`ch` a pixel, no alpha) and coverage.
struct RoiImage {
    int x0 = 0, y0 = 0;
    std::size_t w = 0, h = 0, ch = 1;
    std::vector<float> px;
    std::vector<float> alpha;
    std::optional<std::size_t> at(int x, int y) const;
};

struct Photometric {
    std::vector<std::vector<float>> gains;  // (each image's, each channel's)
    std::array<double, 2> vignette{0.0, 0.0};
    // What corrects a pixel of image i at a squared radius r2 (over the half diagonal).
    float factor(std::size_t i, std::size_t c, double r2) const;
};
// The gains and (with the placements and sizes) the vignetting from the overlaps.
Photometric photometric(const std::vector<RoiImage>& imgs, const std::vector<Placement>* placements,
                        const std::vector<std::pair<std::size_t, std::size_t>>* sizes, bool gain, bool vignette);

// --- seams and blending ------------------------------------------------------------------------------------------------

// The seams' labels on a w × h grid (−1: no image), the images added in `order`.
std::vector<int> seam_labels(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& luma, const std::vector<std::vector<bool>>& cover,
                             const std::vector<std::size_t>& order);
std::size_t blend_levels(std::size_t w, std::size_t h);
// The pixels of no weight (`wt` below 1) filled from around them (push-pull: upstream's, before a pyramid), so a blend draws in
// no black from outside a picture.
void fill_holes(std::size_t w, std::size_t h, std::size_t ch, std::vector<float>& px, const std::vector<float>& wt);
// The multi-band blend of `imgs` into a w × h panorama (origin 0, 0), one weight map each over its box: the colours (ch a pixel)
// and the coverage.
std::pair<std::vector<float>, std::vector<float>> multiband(std::size_t w, std::size_t h, const std::vector<RoiImage>& imgs,
                                                            const std::vector<std::vector<float>>& weights, std::size_t levels);

// The dense normal equations of a weighted least-squares problem (upstream's photo_util::Normal).
struct Normal {
    std::size_t n;
    std::vector<double> ata, atb;
    explicit Normal(std::size_t unknowns) : n(unknowns), ata(unknowns * unknowns, 0.0), atb(unknowns, 0.0) {}
    void add(const std::vector<std::pair<std::size_t, double>>& row, double rhs, double w);
    std::optional<std::vector<double>> solve() const;
};

}  // namespace genko::render::panorama
