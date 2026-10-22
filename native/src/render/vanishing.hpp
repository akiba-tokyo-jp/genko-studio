#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "render/image_ops.hpp"
#include "render/resample.hpp"

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」, schema-v4.md「消点」): フィルター → 消点… — PhotoCraft v0.2.0's
// algo/src/vanishing.rs and engine/src/vp_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md).
//
// A plane is a quad laid on a flat surface of the picture (corners clockwise from its top left, the page's mm). Its two
// vanishing points (where its opposite edges meet) give the camera's focal length for a centre at the page's middle,
// f² = −(v₁ − c)·(v₂ − c) (R. Hartley, A. Zisserman, Multiple View Geometry, §8.8), which lifts the quad to 3-D: its metric
// sides (a picture pasted keeps its proportions) and planes torn off an edge — turned about it in 3-D, projected back.
// Pasting maps a picture's box into a plane's metric coordinates through its homography, clipped to the plane; the
// perspective stamp copies, for each place a stroke covers, the pixels at the same metric offset from its source, so the
// texture shrinks with the distance as in the picture (PC3's Clone Stamp's way: the stroke's coverage built first,
// render/retouch_dabs, then one picture laid through it).
//
// The op vanishing_point (render/ops_filters registers it): {"page", "id"?: a paint layer, "new_layer"?: bool, "planes"?
// (none: the page's — core/vanishing), "focal_mm"?, "keep_planes"?: true, "paste"?: [{"plane", "layer"?: id | "data"?: a
// PNG's base64, "at": [u, v], "width"}], "clone"?: [{"source": [x, y], "points": [[x, y]…] (mm), "size_px" (the working
// 200 dpi), "hardness", "opacity" (0..100)}], "area"?} → {"id", "new_layer", "planes": [{"corners_mm", "aspect"}],
// "focal_mm", "pasted", "dabs", "box_mm"}. Every kind of a paint layer's pixels (8 bits, precise, inks); inside the
// selection; one Undo. The planes kept on the page (vanishing-point@1) unless keep_planes is false.

namespace genko::render::vanishing {

using XY = std::array<double, 2>;
using V3 = std::array<double, 3>;
using Quad = std::array<XY, 4>;

enum class Edge { Top, Right, Bottom, Left };

// A plane lifted to 3-D (the camera at the origin looking along +z, the plane at unit distance along its normal).
struct Lifted {
    double focal = 1;
    XY centre{0, 0};
    std::array<V3, 4> pts{};
    std::optional<XY> project(const V3& x) const;
    // The metric sides: along the top edge (u) and the left edge (v).
    std::pair<double, double> lengths() const;
};

// The unit square (u, v) → the quad.
std::optional<resample::Homography> homography(const Quad& corners);
// The focal length its two vanishing points imply for a camera centred at c (none when one is at infinity or they do
// not fit one).
std::optional<double> focal(const Quad& corners, const XY& c);
Lifted lift(const Quad& corners, double f, const XY& c);

struct Frame {
    resample::Homography h;  // the unit square → the quad
    double lu = 1, lv = 1;   // the metric width and height
};

// The planes and their camera.
struct Scene {
    std::vector<Quad> planes;
    double focal = 1;
    XY centre{0, 0};
    // The focal length from the first plane (or `focal` when given, else `fallback`: a plane seen straight on).
    static Scene make(const Quad& first, const XY& centre, std::optional<double> focal, double fallback);
    Lifted lifted(std::size_t i) const;
    // A plane torn off the edge of `from`, turned by `angle` degrees about it (90: perpendicular), reaching `depth` times
    // the parent's side across it: its index, none when it would lie behind the camera.
    std::optional<std::size_t> tear_off(std::size_t from, Edge edge, double angle, double depth);
    std::optional<Frame> frame(std::size_t i) const;
    // The last plane whose quad holds p.
    std::optional<std::size_t> plane_at(const XY& p) const;
};

// The scene of planes in their normal form (core::read_vanishing) on a page of width × height mm (its centre the
// camera's; a plane seen straight on: the page's long side). OpError for a plane torn off behind the camera or one that
// is degenerate.
Scene scene_of(const core::Json& planes, double width_mm, double height_mm);

// What a stroke of the perspective stamp lays: its coverage and the dabs it made (on a grid of `per_mm` pixels a mm).
struct Stamp {
    std::vector<float> cov;  // over `box`
    Box box;
    std::size_t dabs = 0;
    // the metric offset to the source and the planes (none: nothing laid)
    std::optional<std::size_t> from, to;
    XY offset{0, 0};
};
Stamp stamp_coverage(const Scene& scene, const XY& source_mm, const std::vector<XY>& points_mm, double size_px, double hardness, double per_mm,
                     const Box& grid);
// The place (grid pixels) a stamp copies a destination pixel's centre (grid pixels) from.
std::optional<XY> stamp_source(const Scene& scene, const Stamp& stamp, const XY& at_px, double per_mm);

// A picture (pixels of its own kind and the target's, over its box `rect`) pasted onto plane i at its (u, v) and `width`
// (of the plane's width, its height by the plane's metric aspect), on a grid of `per_mm` pixels a mm: the pixels over
// `out` of that grid (clear past the plane), none when nothing lands there.
std::optional<resample::Plane> pasted(const Scene& scene, std::size_t i, const resample::Plane& picture, const XY& at, double width, double per_mm,
                                      const Box& out);
// The box of the grid a paste can reach (the plane's quad's), within `grid`.
Box paste_box(const Scene& scene, std::size_t i, double per_mm, const Box& grid);

// The op.
void vanishing_point(core::OpContext& c);

}  // namespace genko::render::vanishing
