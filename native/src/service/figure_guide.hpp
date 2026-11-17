#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "render/image.hpp"

// The generation guides of a panel's people (SPEC FIGURE-03, this build's own): guides/figures.png, the panel's 3D
// figures, heads and hands as smooth shaded meshes on black, each in its own material colour (as the 3D-mesh pose
// controls of image tools are trained: shaded body meshes on black, a colour per person, highlights and shadows showing
// shape and overlap; no lines, no marks), and guides/regions.png, a solid colour per person (the same colours, in the
// same order) and black where nobody is (the regional controls' format). Drawn with the page's camera and light over
// the generation box, at exactly the size the image is generated at. Only + − × ÷, sqrt, min and max: the same bytes on
// every run.

namespace genko::service::figure_guide {

using core::Json;
using Box = std::array<double, 4>;  // x, y, w, h (mm)

// The page area (mm) an image of px[0] × px[1] covers (each axis its own scale).
struct View {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::array<int, 2> px{};
};

// The view of an image of `px` over the box: the box itself (each axis scaled to fit), or, with `cover`, the box grown
// about its centre to the image's aspect (one scale; an image tool's size whose middle is the box).
View view_of(const Box& box, std::array<int, 2> px, bool cover);

// The panel's people: the figure, head and hand prims set in it (frame_id), or set in no panel with their middle
// inside it (as the page's camera shows them), in the page's order.
std::vector<Json> panel_prims(const core::Page& page, const core::Frame& frame);

// A person's place from the brief: the box of their head and body together, and the middle of their head across.
struct Place {
    Box box{};
    double middle = 0;
};
// The cast's places in a panel (blocking's figures, in the brief's order).
std::vector<Place> cast_places(const Json& panel, const std::array<core::Num, 4>& rect);

// The material colour of the i-th person (a fixed palette: warm beige, lavender, olive, teal, salmon, …; it repeats
// after twelve).
std::array<int, 3> colour(std::size_t i);

// guides/figures.png: the prims shaded on black (RGB), 3 × 3 samples a pixel.
render::Image figures(const core::Page& page, std::span<const Json> prims, const View& view);
// guides/regions.png: each prim's own silhouette (the nearest where they overlap) made one clean shape — the slits and
// notches narrower than 2 × `grow` px closed: between an arm and the body beside it — and grown by `grow` px, in its
// colour; black elsewhere (RGB). grow < 0: about 1 % of the image's long side; 0: the silhouette as it is.
render::Image regions(const core::Page& page, std::span<const Json> prims, const View& view, int grow = -1);
// The same from the cast's places when a panel has people but no 3D figures (where places overlap, the person whose
// head is nearest across; the first of equals).
render::Image place_regions(std::span<const Place> places, const View& view);

}  // namespace genko::service::figure_guide
