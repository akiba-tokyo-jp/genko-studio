#pragma once

#include "core/figure_shape.hpp"

// Surface Nets over a field (SPEC FIGURE-01): the field is sampled on a grid of step h whose points sit at
// ((i + ½) h, (j + ½) h, (k + ½) h) — so a field symmetric about a plane x = 0 is sampled symmetrically —, in bricks of
// 8 × 8 × 8 cells; a brick far from the surface is left out (the field changes by at most 1.3 × the step: no surface
// can be inside), and in the others only the parts near enough are evaluated (the others cannot change any value that
// shapes the mesh: the same numbers as evaluating them all). Each cell the surface crosses gets one point, the mean of
// the crossings on its edges, then moved onto the surface by two Newton steps (kept within half a cell of the cell);
// each crossed grid edge makes a quad of the four cells around it, split along its shorter diagonal (the first one on
// a tie), turned so that it faces outward. The normal at a point is the field's gradient (central differences),
// normalised. Bricks, cells and edges are taken in a fixed order: the same bytes on every run.

namespace genko::core::surface_nets {

// The surface of `field` (within its bounds, two steps more), on a grid of step h. A point is marked
// no_contour when the part nearest to it is; `part` says which part that is.
figure_shape::Smooth mesh(const figure_shape::Field& field, double h);

}  // namespace genko::core::surface_nets
