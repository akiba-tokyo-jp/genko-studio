#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "render/segment/segment.hpp"

// PC3 B3 (SPEC.md COLOR-06「選択とマスク」「マッティング」): the edge refined (選択とマスク… / 境界の調整) and the colours
// decontaminated — PhotoCraft v0.2.0's algo/src/matting.rs (MIT, native/third_party/photocraft/NOTICE.md):
//   * soft edges from the guided filter (K. He, J. Sun and X. Tang, "Guided Image Filtering", ECCV 2010 / PAMI 2013): the
//     hard selection filtered with the picture as guide takes the picture's soft transitions within `radius` of its edge;
//     Smart Radius widens or narrows that band to the transition the picture shows;
//   * then, in Photoshop's order: Smooth, Feather (a Gaussian of σ = feather / 2), Contrast and Shift Edge (grey dilation
//     or erosion);
//   * Decontaminate Colors: the fringe's colours moved to the normalised-convolution average of the fully selected ones
//     near them, by `amount` and the fringe's transparency;
// and Layer › Matting's Defringe and Remove Black / White Matte (engine/src/layer_menu_cmds.rs). Lengths are pixels of the
// grid worked on (the callers turn mm into them). Worked tile by tile near the boundary only.

namespace genko::render::matting {

using segment::Mask;
using segment::Rect;
using segment::Region;
using segment::RgbImage;
using segment::Rgba;

// Select and Mask's settings (Photoshop's units).
struct RefineParams {
    float radius = 0;         // (the band's radius in pixels; 0: no edge detection)
    bool smart_radius = false;
    float smooth = 0;         // (0..100)
    float feather = 0;        // (pixels)
    float contrast = 0;       // (0..100)
    float shift_edge = 0;     // (−100..100: a percentage of max(radius, 4 px))

    std::size_t gf_radius() const;
    float smooth_sigma() const;
    float feather_sigma() const;
    int shift_px() const;
    bool uses_image() const { return radius > 0; }
    // the pixels read around one put out, and how far the refined mask may reach past the first
    int halo() const;
    int spread() const;
};

// The guided filter's regulariser (a colour guide, samples in 0..1).
inline constexpr float kEps = 1e-4f;

// A box mean of radius r (the window clipped to the picture, divided by its pixels).
std::vector<float> box_mean(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r);
std::vector<float> guided_filter_gray(const std::vector<float>& guide, const std::vector<float>& p, std::size_t w, std::size_t h, std::size_t r,
                                      float eps);
// A colour guide (He et al., eq. 19–21: a_k = (Σ_k + εU)⁻¹ cov_k(I, p)).
std::vector<float> guided_filter_color(const RgbImage& guide, const std::vector<float>& p, std::size_t r, float eps);
// A separable Gaussian, the edges repeated (nothing below σ 0.1).
std::vector<float> gaussian_blur(const std::vector<float>& src, std::size_t w, std::size_t h, float sigma);
// Grey dilation (`grow`) or erosion by about r pixels: 3×3 squares and crosses in turn (an octagon).
std::vector<float> morph(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r, bool grow);
// A running max (or min) over a square of radius r, edge-clipped (van Herk / Gil–Werman, separable).
std::vector<float> max_square(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r, bool max);
// Each pixel's transition width (pixels) of the guide's luminance: the local range over the local largest gradient.
std::vector<float> edge_width(const RgbImage& img, std::size_t r);

// A soft mask `m` (w × h) refined, with `img` as the guide (needed when radius > 0).
std::vector<float> refine_buffer(const RgbImage* img, const std::vector<float>& m, std::size_t w, std::size_t h, const RefineParams& p);
// A selection refined: `mask(r)` its coverage over r (row-major), `content` the box of what it covers; the result clipped
// to `canvas`; `sampler` the guide (read only when radius > 0). None when nothing stays selected.
using MaskReader = std::function<std::vector<float>(const Rect&)>;
std::optional<Region> refine_mask(const segment::Sampler& sampler, const MaskReader& mask, const Rect& content, const Rect& canvas,
                                  const RefineParams& p);
// A reader over a mask of the whole canvas (w × h, 0 outside).
MaskReader buffer_reader(const std::vector<float>& m, std::size_t w, std::size_t h);
// Quick Selection's Auto-Enhance: a light refinement flowing the edge to the picture's.
RefineParams enhance_params();

// --- Layer › Matting, on straight RGBA (0..1) of w × h ------------------------------------------------------------------

// Decontaminate Colors: each pixel of the soft fringe of `matte` (0..1) moved toward the average colour of the fully
// selected pixels near it (a box of radius max(radius, 3), ×4 where none is near), by amount % × its transparency. The
// colours alone change.
void decontaminate(std::vector<Rgba>& px, std::size_t w, std::size_t h, const std::vector<float>& matte, float radius, float amount);
// Defringe: the pixels within `width` of transparency take the colour of those further inside, grown outward a pixel at a
// time. The alpha is kept.
void defringe(std::vector<Rgba>& px, std::size_t w, std::size_t h, std::size_t width);
// Remove Black / White Matte: colours composited over black or white unpremultiplied (the half-clear pixels).
void remove_matte(std::vector<Rgba>& px, bool white);

}  // namespace genko::render::matting
