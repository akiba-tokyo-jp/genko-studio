#pragma once

#include <cstddef>
#include <vector>

// PC3 B4 (SPEC.md COLOR-06「修復」): gradient-domain ("Poisson") blending — the heart of the Healing Brush, the Spot
// Healing Brush and Content-Aware Fill's colour adaptation. PhotoCraft v0.2.0's algo/src/poisson.rs (MIT,
// native/third_party/photocraft/NOTICE.md), in float as it computes them.
//
// P. Pérez, M. Gangnet, A. Blake, "Poisson Image Editing", SIGGRAPH 2003: seamless cloning with the source's gradients
// as the guidance field is the source plus the membrane (harmonic) interpolant of the boundary mismatch — the healed
// pixels keep the source's texture and take the destination's colour and light. The Laplace equation is solved on the
// 4-connected grid by successive over-relaxation, cascadically (each level from a half-size one's solution, bilinearly
// prolonged), so a 300 px dab takes a few milliseconds. Buffers are interleaved `w × h × ch` floats of any colour model
// (each channel solved alone); the results do not depend on how many threads there are (there is one).

namespace genko::render::poisson {

// Δv = 0 on the `unknown` pixels with the known ones of `v` as Dirichlet values (out-of-grid neighbours ignored: a
// Neumann boundary). `v` holds the known values on entry and the solution on exit.
void solve_membrane(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v);
// The reference (tests): plain Gauss–Seidel from zero until an update is below `tol`.
void solve_membrane_reference(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v, float tol,
                              std::size_t max_iter);

// Seamless cloning: `dst` outside `mask`, inside `src` plus the membrane that removes the mismatch at the boundary. A
// masked pixel on the grid's edge sees a free boundary there (leave a one-pixel unmasked margin for a fully Dirichlet
// problem).
std::vector<float> seamless_clone(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& src, const std::vector<float>& dst,
                                  const std::vector<bool>& mask);
// The membrane fill (no texture): the known pixels harmonically interpolated into `hole`.
std::vector<float> membrane_fill(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole);

}  // namespace genko::render::poisson
