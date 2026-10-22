#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/model.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"
#include "render/path_raster.hpp"

// pen-shapes@1 (PC3 C-b, the user's decision 7, SPEC.md COLOR-07「シェイプ」): a pen layer's shape parts — each a path
// filled and stroked as one of Photoshop's shape layers is (PhotoCraft v0.2.0's vector/src/lib.rs CompiledShape and
// Paint::from_fill, MIT: native/third_party/photocraft/NOTICE.md): the fill's coverage laid on its paint (a colour, or
// a gradient as a fill layer's: render/composite's), the stroke's outline over it (inside: clipped by the fill, outside:
// outside it), the part's opacity on the whole. A layer draws its patches, then its parts that are not `over`, then its
// lines, then the `over` parts (render/page). And what the ops do to a part: its outline and its parts' operations
// baked into plain outlines (the coverage traced again), an area cutting it in two, the eraser taking a band out of it,
// a warp bending it, its bounds.

namespace genko::render::shape_parts {

using core::vector::Path;
using core::vector::ShapePart;

// Does the layer hold shape parts?
inline bool any(const core::Layer& layer) { return layer.shapes && !layer.shapes->items.empty(); }

// The colour a part's fill or stroke of {"rgb": null} (or a stroke's null fill) is drawn in: the layer's ink, as a pen
// line without a colour of its own.
inline const std::vector<std::int64_t> kInkColour{20, 20, 20};

// A part made ready to draw at a resolution (the page's grid: pixel (x, y) covers [x, x + 1) × [y, y + 1) of the page
// at `dpi`). `colour`: every colour drawn in this one (a name or draft layer's guide colour), its alphas kept.
class Compiled {
public:
    // `book`: the book whose patterns a pattern fill names (patterns@1, PC4 E3; none: a pattern fill draws nothing).
    Compiled(const ShapePart& part, double dpi, const std::vector<std::int64_t>* colour = nullptr, const core::Document* book = nullptr);
    ~Compiled();
    Compiled(Compiled&&) noexcept;
    Compiled& operator=(Compiled&&) noexcept;
    // The pixels it may cover (none: nothing).
    std::optional<Box> box() const;
    // Straight RGBA (sRGB, 0..1) of every pixel of `box`, row by row: the fill, the stroke over it, the part's opacity.
    std::vector<std::array<float, 4>> rgba(const Box& box) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The parts (in their order) laid normal over `out` ("RGBA", the box `area` of the page at `dpi`), each in bands of
// rows. `colour` as Compiled's.
void draw(Image& out, const Box& area, int dpi, const std::vector<const ShapePart*>& parts, const std::vector<std::int64_t>* colour = nullptr,
          const core::Document* book = nullptr);
// The same over nothing: an "RGBA" picture of `area`.
Image picture(const Box& area, int dpi, const std::vector<const ShapePart*>& parts, const std::vector<std::int64_t>* colour = nullptr,
              const core::Document* book = nullptr);

// --- geometry (mm on the page) ------------------------------------------------------------------------------------

// What the part's drawing covers on the page, [x0, y0, x1, y1] in mm: its fill's outline, its stroke's (inside,
// centred or outside; its caps, joins and dashes); none when it draws nothing. (Its control points' box is the path's
// own: core::vector::Path::control_bounds.)
std::optional<std::array<double, 4>> drawn_bounds(const ShapePart& part);
// The part's first anchor (where panel_each keeps it: the panel it begins in); none for a part of no knot.
std::optional<std::pair<double, double>> first_anchor(const ShapePart& part);
// Is every subpath of the path closed (a part that can be cut by an area and taken a band out of)?
bool closed(const Path& path);

// The operations of a path baked into plain outlines (Merge Shape Components): its coverage drawn at `dpi` and its 0.5
// contour fitted within `tolerance_mm` (path_trace: the first outline combines, the others exclude). An empty path when
// it covers nothing. OpError when it is too large to draw at once.
inline constexpr double kBakeDpi = 800.0;            // (4 × the selection's 200 dpi, as PhotoCraft traces at 4×)
inline constexpr double kBakeTolerance = 0.0127;     // (0.1 px at 200 dpi)
Path baked(const Path& path, double tolerance_mm = kBakeTolerance, double dpi = kBakeDpi);
// The path's coverage times `area`'s (inside), or times its complement (inside = false), baked as above.
Path cut(const Path& path, const Path& area, bool inside, double tolerance_mm = kBakeTolerance, double dpi = kBakeDpi);
// The path less the band `polys` covers (outline pieces in mm, filled non-zero: path_raster::stroke_polygons of an
// eraser's line), baked as above.
Path without(const Path& path, const std::vector<std::vector<path_raster::XY>>& polys, double tolerance_mm = kBakeTolerance,
             double dpi = kBakeDpi);

// The path bent by a map of the page (mm → mm, not affine): each segment cut into pieces of at most `step_mm` (the same
// curve, de Casteljau), every anchor and handle mapped (as a pen line's points are: render/warp's warp_stroke).
using MapFunction = std::function<std::pair<double, double>(double, double)>;
Path bent(const Path& path, const MapFunction& go, double step_mm = 1.5);

// The part through an affine map of the page (x' = a·x + c·y + e, y' = b·x + d·y + f). Its live shape stays when the map
// keeps the axes (PhotoCraft's transform_shape: a move, a scale, a flip, a quarter turn — a polygon only a move or a
// scale); `scale_stroke`: its stroke's width by √|det| (as a pen line's, through a selection's transform).
ShapePart transformed(const ShapePart& part, const core::vector::Affine& m, bool scale_stroke);

// A fill as the drawing reads it: none (null), a colour, the layer's ink ({"rgb": null}), a gradient, a pattern
// (patterns@1).
bool fills(const core::Json& fill);

// How much of the path's area (mm², its fill's coverage at `dpi`) lies inside `area`'s and outside it.
std::pair<double, double> shares(const Path& path, const Path& area, double dpi = 200.0);

// The part as a picture patch ({"mode": "image"}, at the patches' 300 dpi), or none when it draws nothing (Layer ›
// Rasterize › Shape; a part laid on a layer that is not a pen layer). An inverted part as far as the page (none without
// one).
std::optional<core::Patch> as_patch(const core::Page* page, const ShapePart& part, const core::Document* book = nullptr);
// Every part of the layer as patches, in the order they are drawn (those under the lines, then those over them), after
// its own (the lines kept as they are: a part over a line would go under it — Layer › Rasterize › Shape draws such a layer
// in whole, render/ops_smart).
void rasterize_parts(const core::Page& page, core::Layer& layer, const core::Document* book = nullptr);

}  // namespace genko::render::shape_parts
