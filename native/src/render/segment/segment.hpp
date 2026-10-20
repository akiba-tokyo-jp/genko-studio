#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "render/selection_ext.hpp"

// PC3 B3 (SPEC.md COLOR-06「賢い選択」): smart selection — PhotoCraft v0.2.0's algo/src/segment (MIT,
// native/third_party/photocraft/NOTICE.md), classical as it is (no learned models: the user's decision 6):
//   * Graph: Boykov–Kolmogorov min-cut / max-flow (PAMI 2004);
//   * Gmm: Gaussian mixture colour models (k-means++ seeding, EM);
//   * slic: SLIC superpixels (Achanta et al., PAMI 2012);
//   * grabcut and object_select: GrabCut (Rother, Kolmogorov & Blake, SIGGRAPH 2004);
//   * quick_select: the Quick Selection brush (stroke seeds, a geodesic barrier, a contrast-sensitive cut);
//   * select_subject: superpixel saliency (Zhu et al., CVPR 2014; Achanta et al., CVPR 2009) seeding a GrabCut;
//   * select_sky: a colour prior × smoothness grown from the top, refined by a guided filter;
//   * focus_area: local Laplacian energy (sharpness) thresholded.
// Colours are straight RGB in 0..1 read through a Sampler, so a page of any depth or colour model is segmented the same
// (render/select_smart reads them); a large area is worked at a reduced resolution and its boundary cut again at full
// resolution in a narrow band. Results are 8-bit coverage Regions (render/selection_ext). Deterministic: the seeds are
// fixed, and nothing depends on threads.

namespace genko::render::segment {

using Rect = selection_ext::Rect;
using Region = selection_ext::Region;
using Rgb = std::array<float, 3>;
using Rgba = std::array<float, 4>;
// A labelling, one byte a pixel (0: no, else yes).
using Mask = std::vector<std::uint8_t>;

// --- stopping ---------------------------------------------------------------------------------------------------------

// While it lives, the segmentation and the matting on this thread stop soon after `stop` is requested, throwing
// render::Cancelled (the window's worker: Esc, or the work no longer wanted).
class StopScope {
public:
    explicit StopScope(std::stop_token stop);
    ~StopScope();
    StopScope(const StopScope&) = delete;
    StopScope& operator=(const StopScope&) = delete;

private:
    const std::stop_token* before_;
    std::stop_token stop_;
};
// render::Cancelled when this thread's work is to stop.
void check_stop();

// --- images and samplers (mod.rs) -------------------------------------------------------------------------------------

// Row-major RGB, samples in 0..1.
struct RgbImage {
    std::size_t w = 0;
    std::size_t h = 0;
    std::vector<Rgb> px;

    RgbImage() = default;
    RgbImage(std::size_t width, std::size_t height) : w(width), h(height), px(width * height, Rgb{0, 0, 0}) {}
    const Rgb& at(std::size_t x, std::size_t y) const { return px[y * w + x]; }
    // Straight RGBA to RGB over mid grey (transparency a colour the models can separate).
    static RgbImage from_rgba(const std::vector<Rgba>& px, std::size_t w, std::size_t h);
    // A box average by a whole step (ceil(w / step) × ceil(h / step)).
    RgbImage downsample(std::size_t step) const;
};

// Where the algorithms read colours: RGB over a box of the canvas's pixels (outside what it holds: mid grey).
class Sampler {
public:
    virtual ~Sampler() = default;
    virtual RgbImage rgb(const Rect& r) const = 0;
    // RGB over `r`, box-averaged by `step`, read in strips (the whole never held at full resolution).
    virtual RgbImage rgb_scaled(const Rect& r, std::size_t step) const;
};

// An image placed at `origin` (tests).
class ImageSampler : public Sampler {
public:
    ImageSampler(const RgbImage& img, std::pair<int, int> origin = {0, 0}) : img_(img), origin_(origin) {}
    RgbImage rgb(const Rect& r) const override;

private:
    const RgbImage& img_;
    std::pair<int, int> origin_;
};

// The whole step that brings `area` to about `max_px` pixels.
std::size_t scale_for(const Rect& area, std::size_t max_px);
float d2(const Rgb& a, const Rgb& b);
// GrabCut's contrast: β = 1 / (2 ⟨‖z_m − z_n‖²⟩) over the 8-neighbour pairs.
float contrast_beta(const RgbImage& img);

inline constexpr std::uint8_t kFree = 0;
inline constexpr std::uint8_t kHardFg = 1;
inline constexpr std::uint8_t kHardBg = 2;

class Gmm;

// One min cut over the 8-connected grid (Boykov & Jolly 2001): Σ D_p(l_p) + Σ [l_p ≠ l_q] γ e^{−β‖z_p − z_q‖²} / dist.
// `fixed`: kFree, kHardFg or kHardBg. Foreground: 1.
Mask grid_cut(const RgbImage& img, const std::vector<float>& cost_fg, const std::vector<float>& cost_bg, const std::vector<std::uint8_t>& fixed,
              float gamma, float beta);
// (−ln p(z | fg), −ln p(z | bg)) for every pixel.
std::pair<std::vector<float>, std::vector<float>> data_costs(const RgbImage& img, const Gmm& fg, const Gmm& bg);
// 8-connected components of `mask`: the labels (0: background) and the sizes by label.
std::pair<std::vector<std::uint32_t>, std::vector<std::size_t>> components(const Mask& mask, std::size_t w, std::size_t h);
// The components holding a seed.
Mask keep_seeded(const Mask& mask, const Mask& seeds, std::size_t w, std::size_t h);
// The components of at least `frac` of the largest, holes (4-connected, off the border) of up to `hole_frac` of the kept
// area filled.
Mask clean_mask(const Mask& mask, std::size_t w, std::size_t h, float frac, float hole_frac);
// A hard 0/255 mask's edge pixels blended with their 3×3 neighbourhood.
void antialias_u8(std::vector<std::uint8_t>& mask, std::size_t w, std::size_t h);
// The region's box shrunk to its pixels (none: empty).
std::optional<Region> trim_region(Region r);
// A working-resolution labelling of `window` (sampled by `step`) as a full-resolution region: the boundary cut again
// at full resolution in a band about a step wide (by the colour models), anti-aliased by a pixel.
std::optional<Region> finish_region(const Sampler& sampler, const Rect& window, std::size_t step, const Mask& low, std::size_t lw,
                                    std::size_t lh, const Gmm* fg, const Gmm* bg);

// xorshift, as PhotoCraft's (seeded alike, the same numbers).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ULL | 1) {}
    std::uint64_t next_u64();
    float f32();     // (uniform in [0, 1))
    float normal();  // (Box–Muller)

private:
    std::uint64_t s_;
};

// Every k-th so that at most `max` remain.
std::vector<Rgb> subsample(const std::vector<Rgb>& v, std::size_t max);

// --- max-flow (maxflow.rs) --------------------------------------------------------------------------------------------

class Graph {
public:
    Graph(std::size_t nodes, std::size_t edges);
    std::size_t node_count() const { return first_.size(); }
    // Capacities from the source and to the sink (accumulated; only the difference kept).
    void add_tweights(std::size_t i, float cap_source, float cap_sink);
    // An edge i → j of `cap` and j → i of `rev_cap`.
    void add_edge(std::size_t i, std::size_t j, float cap, float rev_cap);
    double maxflow();
    // After maxflow: on the source side of the cut (unreached nodes on the sink side).
    bool in_source(std::size_t i) const;
    double flow() const { return flow_; }
    // The residual capacity leaving the source side (0 after maxflow).
    float residual_out_of_source_side() const;

private:
    void augment(std::size_t m, std::deque<std::uint32_t>& orphans);
    void adopt(std::size_t i, std::uint32_t time, std::deque<std::uint32_t>& queue, std::deque<std::uint32_t>& orphans);

    std::vector<std::uint32_t> first_;
    std::vector<float> tr_cap_;
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint8_t> is_sink_;
    std::vector<std::uint32_t> ts_;
    std::vector<std::uint32_t> dist_;
    std::vector<std::uint8_t> active_;
    std::vector<std::uint32_t> head_;
    std::vector<std::uint32_t> next_;
    std::vector<float> r_cap_;
    double flow_ = 0;
};

// --- colour models (gmm.rs) -------------------------------------------------------------------------------------------

struct Component {
    float weight = 0;
    Rgb mean{};
    std::array<std::array<float, 3>, 3> cov{};
    std::array<std::array<double, 3>, 3> inv{};
    double log_norm = 0;  // (ln weight − ½ ln det Σ − 3/2 ln 2π)
    // ln(weight · N(z; μ, Σ))
    double log_weighted(const Rgb& z) const;
};

inline constexpr float kDefaultReg = 2e-5f;

class Gmm {
public:
    std::vector<Component> comps;
    float reg = kDefaultReg;

    // k components (fewer without enough distinct samples); none without samples.
    static std::optional<Gmm> fit(const std::vector<Rgb>& samples, std::size_t k, float reg, std::size_t em_iters = 3);
    static std::optional<Gmm> from_labels(const std::vector<Rgb>& samples, const std::vector<std::size_t>& labels, std::size_t k, float reg);
    // GrabCut's step: each sample to its likeliest component, estimated again.
    std::optional<Gmm> refit(const std::vector<Rgb>& samples) const;
    std::size_t component_of(const Rgb& z) const;
    double log_prob(const Rgb& z) const;
    // −ln p(z), clamped to −50..200.
    float neg_log(const Rgb& z) const;

private:
    std::optional<Gmm> em_step(const std::vector<Rgb>& samples) const;
};

// k-means with k-means++ seeding (a fixed seed), empty clusters dropped.
std::vector<Rgb> kmeans(const std::vector<Rgb>& samples, std::size_t k, std::size_t iters);

// --- superpixels (slic.rs) --------------------------------------------------------------------------------------------

// sRGB (0..1) to CIELAB (D65), L in 0..100.
Rgb rgb_to_lab(const Rgb& c);
struct Superpixels {
    std::size_t w = 0;
    std::size_t h = 0;
    std::vector<std::uint32_t> labels;
    std::size_t count = 0;
    float step = 1;
};
Superpixels slic(const RgbImage& img, std::size_t target, float compactness, std::size_t iters);
Superpixels slic_lab(const std::vector<Rgb>& lab, std::size_t w, std::size_t h, std::size_t target, float compactness, std::size_t iters);

// --- GrabCut and Object Selection (grabcut.rs) ------------------------------------------------------------------------

inline constexpr std::uint8_t kBg = 0;
inline constexpr std::uint8_t kFg = 1;
inline constexpr std::uint8_t kPrBg = 2;
inline constexpr std::uint8_t kPrFg = 3;

// `iters` GrabCut steps on the trimap's probable labels; the final (foreground, background) models, none when a side has
// no pixels.
std::optional<std::pair<Gmm, Gmm>> grabcut(const RgbImage& img, std::vector<std::uint8_t>& trimap, std::size_t iters);
// Object Selection (a rectangle): GrabCut from `rect` (outside background) on a working copy of about `work_px` pixels,
// the boundary cut again at full resolution; `canvas` bounds it.
std::optional<Region> object_select(const Sampler& sampler, const Rect& canvas, const Rect& rect, std::size_t work_px);

// --- Quick Selection (quick.rs) ---------------------------------------------------------------------------------------

inline constexpr std::size_t kQuickWorkPx = 160'000;
// A stroke (canvas pixels) of a brush `size` across: the region it grows to (to add or take away); none when it misses.
std::optional<Region> quick_select(const Sampler& sampler, const Rect& canvas, const std::vector<std::pair<float, float>>& points, float size,
                                   std::size_t work_px);
struct QuickCut {
    Gmm fg;
    Gmm bg;
    Mask low;
};
QuickCut quick_segment(const RgbImage& img, const Mask& seeds);
Gmm broad_model();
Mask stroke_mask(const std::vector<std::pair<float, float>>& pts, float radius, std::size_t w, std::size_t h);
std::vector<float> geodesic(const RgbImage& img, const Mask& seeds);

// --- Select Subject (subject.rs) --------------------------------------------------------------------------------------

inline constexpr std::size_t kSubjectWorkPx = 110'000;
std::optional<Region> select_subject(const Sampler& sampler, const Rect& canvas);
std::vector<float> saliency(const RgbImage& img);
float otsu(const std::vector<float>& v);

// --- Select Sky (sky.rs) ----------------------------------------------------------------------------------------------

inline constexpr std::size_t kSkyWorkPx = 160'000;
struct SkyParams {
    float threshold = 50;  // (0..100: higher, stricter)
    float softness = 50;   // (0..100: the refined edge's softness)
};
float colour_prior(const Rgb& p);
std::vector<float> sky_likelihood(const RgbImage& img);
std::optional<Mask> sky_mask_low(const RgbImage& img, float threshold);
std::optional<Region> select_sky(const Sampler& sampler, const Rect& canvas, const SkyParams& p);

// --- Focus Area (focus.rs) --------------------------------------------------------------------------------------------

inline constexpr std::size_t kFocusWorkPx = 1'000'000;
std::vector<float> sharpness(const RgbImage& img, float noise);
std::optional<Region> focus_area(const Sampler& sampler, const Rect& canvas, float range, float noise);

}  // namespace genko::render::segment
