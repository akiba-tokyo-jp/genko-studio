#pragma once

#include <cstddef>
#include <vector>

// PC3 C-c (SPEC.md COLOR-07, edit.contentAwareScale): Edit › Content-Aware Scale by seam carving (Avidan & Shamir,
// SIGGRAPH 2007). PhotoCraft v0.2.0's algo/src/seam.rs (MIT, native/third_party/photocraft/NOTICE.md).
//
// A seam is an 8-connected path of one pixel a row (a vertical seam), found by dynamic programming over a gradient
// energy. Shrinking takes out the lowest seams one at a time; enlarging finds the k lowest on a shrinking copy and
// doubles them (each averaged with its right neighbour), so one seam is not stretched again and again. Heights by
// transposing. A protection map (0..1: an alpha channel, the skin tones) adds a large energy, so seams go around what
// it protects. Buffers are w × h pixels of `ch` interleaved floats of any colour and depth (an alpha carried as any
// other channel). The same input gives the same result.

namespace genko::render::seam {

// The gradient energy: |∂x| + |∂y| over the channels, forward and backward differences both (a one-pixel line has
// energy too).
std::vector<float> energy(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img);
// The lowest vertical seam: one x a row.
std::vector<std::size_t> find_seam(std::size_t w, std::size_t h, const std::vector<float>& energy);
// The picture made new_w wide by taking out or doubling vertical seams.
std::vector<float> carve_width(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<float>* protect,
                               std::size_t new_w);
// Content-aware scale to new_w × new_h (the width first, then the height by transposing).
std::vector<float> carve(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<float>* protect,
                         std::size_t new_w, std::size_t new_h);
// A plain bilinear resize (the part of Amount that is not content-aware).
std::vector<float> resize_bilinear(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, std::size_t nw, std::size_t nh);
// The skin tones (1) of RGB pixels (the first 3 of `ch` channels, 0..1): Kovač, Peer and Solina's explicit rule
// (EUROCON 2003).
std::vector<float> skin_mask(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img);

}  // namespace genko::render::seam
